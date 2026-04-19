/*
 * Stage-2 translation setup.
 *
 * Per-VM tables: each VM gets its own s2_l1 / s2_l2_low / s2_l3_gic /
 * s2_l3_priv plus a private 4 KiB scratch page. Identity maps cover
 *   - guest RAM   : IPA 0x40000000..0x7FFFFFFF (1 GiB block, Normal WB)
 *   - UART        : IPA 0x09000000..0x091FFFFF (2 MiB block, Device)
 *   - GICD        : IPA 0x08000000..0x0800FFFF pages -> real GICD
 *   - "GICC"      : IPA 0x08010000..0x0801FFFF pages -> real GICV
 * Plus a *per-VM* 2 MiB region at IPA 0x50000000 where one 4 KiB page
 * points at this VM's private scratch. Each VM sees a different host
 * page at the same IPA — enough to prove stage-2 isolation.
 */
#include "stage2.h"
#include "uart.h"
#include "gic.h"

/* ---- descriptor bits (stage-2 VMSAv8-64) ------------------------------- */

#define DESC_VALID      (1UL << 0)
#define DESC_BLOCK      (DESC_VALID | (0UL << 1))
#define DESC_TABLE      (DESC_VALID | (1UL << 1))
#define DESC_PAGE       (DESC_VALID | (1UL << 1))

#define S2_MEM_DEV      (0x0UL << 2)
#define S2_MEM_NORMAL   (0xFUL << 2)
#define S2_AP_RW        (3UL  << 6)
#define S2_SH_NSH       (0UL  << 8)
#define S2_SH_ISH       (3UL  << 8)
#define S2_AF           (1UL  << 10)
#define S2_XN           (1UL  << 54)

/* ---- VTCR_EL2 fields --------------------------------------------------- */

#define VTCR_T0SZ(n)    ((unsigned long)(n) & 0x3f)
#define VTCR_SL0_L1     (1UL << 6)
#define VTCR_IRGN0_WBWA (1UL << 8)
#define VTCR_ORGN0_WBWA (1UL << 10)
#define VTCR_SH0_ISH    (3UL << 12)
#define VTCR_TG0_4K     (0UL << 14)
#define VTCR_PS_40BIT   (2UL << 16)

/* ---- per-VM storage ---------------------------------------------------- */

#define MAX_VMS 2

static __attribute__((aligned(4096))) unsigned long s2_l1     [MAX_VMS][512];
static __attribute__((aligned(4096))) unsigned long s2_l2_low [MAX_VMS][512];
static __attribute__((aligned(4096))) unsigned long s2_l3_gic [MAX_VMS][512];
static __attribute__((aligned(4096))) unsigned long s2_l3_priv[MAX_VMS][512];
static __attribute__((aligned(4096))) unsigned char vm_priv_page[MAX_VMS][4096];

void stage2_global_init(void) {
    /* Fresh virtual counter — match physical on first read. */
    __asm__ volatile ("msr cntvoff_el2, xzr" ::: "memory");

    unsigned long vtcr = VTCR_T0SZ(32)
                       | VTCR_SL0_L1
                       | VTCR_IRGN0_WBWA
                       | VTCR_ORGN0_WBWA
                       | VTCR_SH0_ISH
                       | VTCR_TG0_4K
                       | VTCR_PS_40BIT;
    __asm__ volatile ("msr vtcr_el2, %0" :: "r"(vtcr));
    __asm__ volatile ("dsb ish; isb" ::: "memory");

    uart_puts("[stage2] VTCR_EL2  = "); uart_put_hex(vtcr);  uart_puts("\n");
}

unsigned long stage2_alloc_vm(unsigned id, unsigned vmid) {
    if (id >= MAX_VMS) { uart_puts("[stage2] VM id out of range\n"); for(;;); }

    unsigned long *l1      = s2_l1[id];
    unsigned long *l2_low  = s2_l2_low[id];
    unsigned long *l3_gic  = s2_l3_gic[id];
    unsigned long *l3_priv = s2_l3_priv[id];

    for (int i = 0; i < 512; i++) {
        l1[i] = 0; l2_low[i] = 0; l3_gic[i] = 0; l3_priv[i] = 0;
    }

    /* --- UART: 2 MiB block at L2[0x48] -------------------------------- */
    const unsigned long uart_base = 0x09000000UL;
    unsigned           l2_uart   = (unsigned)((uart_base >> 21) & 0x1ff);
    l2_low[l2_uart] = (uart_base & ~((1UL << 21) - 1))
                    | DESC_BLOCK
                    | S2_AF | S2_SH_NSH | S2_AP_RW | S2_MEM_DEV | S2_XN;

    /* --- GIC region: LEFT UNMAPPED ------------------------------------ *
     *
     *  Both GICD (IPA 0x08000000..) and GICC (IPA 0x08010000..) are
     *  unmapped so every guest access traps to src/vgic.c. We emulate
     *  GICC entirely; GICD is forwarded straight through to the real
     *  distributor. Trapping GICD lets us watch Linux's IRQ-enable
     *  writes and fix up any PPIs we care about. */
    const unsigned long gic_base   = 0x08000000UL;
    unsigned           l2_gic     = (unsigned)((gic_base >> 21) & 0x1ff);
    l2_low[l2_gic] = (unsigned long)l3_gic | DESC_TABLE;
    (void)l3_gic;

    /* --- L1[0] -> L2 table (covers 0x00000000..0x3FFFFFFF) ----------- */
    l1[0] = (unsigned long)l2_low | DESC_TABLE;

    /* --- L1[1] -> 1 GiB block for guest RAM, identity ---------------- */
    l1[1] = 0x40000000UL
          | DESC_BLOCK
          | S2_AF | S2_SH_ISH | S2_AP_RW | S2_MEM_NORMAL;

    /* --- Private page at IPA 0x50000000 (per-VM backing) ------------- *
     * IPA 0x50000000 sits in L1[1]'s 1 GiB block; but we want page
     * granularity so each VM gets a distinct host page. Split L1[1]:
     *  - leave the 1 GiB block for identity RAM
     *  - but additionally install L1[1]-sibling? No — the 1 GiB block
     *    at L1[1] already covers 0x50000000. A block descriptor and
     *    table descriptor can't coexist at one L1 slot.
     *
     * So instead, we put the private page at IPA 0x0A000000 (inside
     * L1[0]'s L2 coverage, at L2 index 0x50), via an L3 table. That
     * keeps the guest-RAM block intact and still demonstrates per-VM
     * backing at a known IPA.
     */
    const unsigned long priv_ipa = 0x0A000000UL;       /* 2 MiB-aligned */
    unsigned           l2_priv  = (unsigned)((priv_ipa >> 21) & 0x1ff);
    const unsigned long pg_norm = DESC_PAGE | S2_AF | S2_SH_ISH | S2_AP_RW
                                | S2_MEM_NORMAL;
    l3_priv[0] = ((unsigned long)vm_priv_page[id]) | pg_norm;
    l2_low[l2_priv] = (unsigned long)l3_priv | DESC_TABLE;

    /* Seed the private page so the guest can read a distinct byte. */
    vm_priv_page[id][0] = (unsigned char)('A' + id);

    unsigned long vttbr = (unsigned long)l1 | ((unsigned long)vmid << 48);

    __asm__ volatile ("dsb ish        \n"
                      "tlbi vmalls12e1\n"
                      "dsb ish        \n"
                      "isb            \n" ::: "memory");

    uart_puts("[stage2] VM "); uart_put_hex(id);
    uart_puts(" VMID "); uart_put_hex(vmid);
    uart_puts(" VTTBR= "); uart_put_hex(vttbr);
    uart_puts(" priv@"); uart_put_hex((unsigned long)vm_priv_page[id]);
    uart_puts("\n");
    return vttbr;
}
