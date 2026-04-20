/*
 * GICv3 implementation for the HVF build (HOST_IF=hvf).
 *
 * Stage 3 scope: full GICv3 init at EL2 using the system-register
 * interface (ICC_*), plus CNTHP armed and polled. The vector-driven
 * delivery path is known-blocked on HVF nested virt (see note below),
 * so this stage uses polled delivery: after arming CNTHP, we spin
 * checking ICC_HPPIR1_EL1, and when INTID 26 shows up we IAR + EOI
 * it via the sysreg path. Same exit criterion as the plan described.
 *
 * Note on vector delivery: with ICC_SRE_EL2.SRE=1, ICH_HCR_EL2.En=1,
 * ICC_PMR_EL1=0xFF, ICC_IGRPEN1_EL1=1, DAIF.I=0, HCR_EL2=0x80000000,
 * and an IRQ visible at ICC_HPPIR1_EL1, the EL2h IRQ vector at
 * VBAR_EL2 + 0x280 still doesn't fire on this Mac. Polled IAR+EOI
 * via ICC_IAR1_EL1 / ICC_EOIR1_EL1 work correctly. Root cause is
 * probably nested-virt-specific behaviour in Apple's HVF; the port
 * can proceed with polled delivery until it's understood.
 *
 * Under GIC_VERSION == 2 this file compiles to nothing; gic.c provides
 * the GICv2 equivalent.
 */

#include "gic.h"
#include "uart.h"
#include "exceptions.h"

#if GIC_VERSION == 3

/* IPA layout matches host/hvf_main.c. */
#define GICD_BASE          0x08000000UL
#define GICR_BASE          0x080A0000UL     /* per-CPU redistributor RD_base */
#define GICR_SGI_OFFSET    0x00010000UL     /* RD_base + 64 KiB = SGI/PPI frame */

/* GICD_CTLR comes from gic.h. Local redistributor offsets: */
#define GICR_WAKER         0x0014

#define GICR_WAKER_PROCESSOR_SLEEP  (1u << 1)
#define GICR_WAKER_CHILDREN_ASLEEP  (1u << 2)

/* SGI/PPI frame offsets (relative to GICR_BASE + GICR_SGI_OFFSET). */
#define GICR_IGROUPR0      0x0080
#define GICR_ISENABLER0    0x0100
#define GICR_IPRIORITYR    0x0400

#define IRQ_HTIMER_PPI     26              /* CNTHP */

/* ---- MMIO helpers ---- */

static inline unsigned mmio_r(unsigned long addr) {
    return *(volatile unsigned *)addr;
}
static inline void mmio_w(unsigned long addr, unsigned v) {
    *(volatile unsigned *)addr = v;
}
static inline void mmio_wb(unsigned long addr, unsigned char v) {
    *(volatile unsigned char *)addr = v;
}

/* ---- ICC sysreg accessors (GICv3 CPU interface) ---- */

static inline void write_icc_sre_el2(unsigned long v) {
    __asm__ volatile ("msr S3_4_C12_C9_5, %0" :: "r"(v));
    __asm__ volatile ("isb");
}
static inline void write_icc_pmr_el1(unsigned long v) {
    __asm__ volatile ("msr S3_0_C4_C6_0, %0" :: "r"(v));
}
static inline void write_icc_igrpen1_el1(unsigned long v) {
    __asm__ volatile ("msr S3_0_C12_C12_7, %0" :: "r"(v));
    __asm__ volatile ("isb");
}
static inline unsigned long read_icc_iar1_el1(void) {
    unsigned long v;
    __asm__ volatile ("mrs %0, S3_0_C12_C12_0" : "=r"(v));
    __asm__ volatile ("dsb sy; isb" ::: "memory");
    return v;
}
static inline void write_icc_eoir1_el1(unsigned long v) {
    __asm__ volatile ("msr S3_0_C12_C12_1, %0" :: "r"(v));
    __asm__ volatile ("isb");
}
static inline unsigned long read_icc_hppir1_el1(void) {
    unsigned long v;
    __asm__ volatile ("mrs %0, S3_0_C12_C12_2" : "=r"(v));
    return v;
}

/* ---- redistributor setup ---- */

static void wake_redistributor(void) {
    unsigned w = mmio_r(GICR_BASE + GICR_WAKER);
    w &= ~GICR_WAKER_PROCESSOR_SLEEP;
    mmio_w(GICR_BASE + GICR_WAKER, w);

    unsigned spins = 0;
    while (mmio_r(GICR_BASE + GICR_WAKER) & GICR_WAKER_CHILDREN_ASLEEP) {
        if (++spins > 1000000) {
            uart_puts("[gic-v3] redistributor wake timeout\n");
            break;
        }
    }
}

static void enable_ppi(unsigned intid, unsigned char priority) {
    unsigned long sgi = GICR_BASE + GICR_SGI_OFFSET;
    mmio_wb(sgi + GICR_IPRIORITYR + intid, priority);
    unsigned grp = mmio_r(sgi + GICR_IGROUPR0);
    grp |= (1u << intid);
    mmio_w(sgi + GICR_IGROUPR0, grp);
    mmio_w(sgi + GICR_ISENABLER0, (1u << intid));
}

/* ---- CNTHP helpers ---- */

static void cnthp_arm_ms(unsigned ms) {
    unsigned long freq, now;
    __asm__ volatile ("mrs %0, cntfrq_el0" : "=r"(freq));
    __asm__ volatile ("mrs %0, cntpct_el0" : "=r"(now));
    unsigned long cval = now + (freq * (unsigned long)ms) / 1000UL;
    __asm__ volatile ("msr cnthp_cval_el2, %0" :: "r"(cval));
    __asm__ volatile ("msr cnthp_ctl_el2,  %0" :: "r"(1UL));
    __asm__ volatile ("isb" ::: "memory");
}

static void cnthp_disable(void) {
    __asm__ volatile ("msr cnthp_ctl_el2, xzr" ::: "memory");
    __asm__ volatile ("isb" ::: "memory");
}

/* ---- init ---- */

void gic_init_el2(void) {
    uart_puts("[gic-v3] init: SRE, redistributor, distributor, CPU interface\n");

    /* Enable the system-register interface at EL2.
     *   SRE=1 (bit 0): ICC_*_EL1 use sysregs, not MMIO to GICC
     *   Enable=1 (bit 3): EL1 can use the sysreg interface too */
    write_icc_sre_el2(0x9);

    wake_redistributor();

    /* Distributor: affinity routing + Group 1 NS. */
    mmio_w(GICD_BASE + GICD_CTLR, 0x12);

    /* Enable the ICH (hypervisor) interface at EL2. Stage 4 will use
     * ICH_LRn for virtual IRQ injection into the guest. */
    __asm__ volatile ("msr S3_4_C12_C11_0, %0" :: "r"(1UL));   /* ICH_HCR_EL2.En */
    __asm__ volatile ("isb");

    /* CPU interface: PMR + Group 1 enable. */
    write_icc_pmr_el1(0xFF);
    write_icc_igrpen1_el1(0x1);

    /* Enable CNTHP (PPI 26) at this redistributor. */
    enable_ppi(IRQ_HTIMER_PPI, 0xA0);
    uart_puts("[gic-v3] PPI 26 (CNTHP) enabled\n");

    /* Arm CNTHP for ~500 ms, then poll for it. Vector delivery at EL2
     * is blocked on HVF nested virt (see top-of-file note); polling
     * via ICC_HPPIR1_EL1 + ICC_IAR1_EL1 is the Stage 3 workaround. */
    cnthp_arm_ms(500);
    uart_puts("[gic-v3] CNTHP armed, polling for delivery...\n");

    for (unsigned long i = 0; i < 10000000UL; i++) {
        unsigned long hppir = read_icc_hppir1_el1();
        if ((hppir & 0xFFFFFF) == IRQ_HTIMER_PPI) {
            unsigned long iar = read_icc_iar1_el1();
            cnthp_disable();
            write_icc_eoir1_el1(iar);
            uart_puts("[gic-v3] CNTHP IRQ picked up (polled IAR = ");
            uart_put_hex(iar);
            uart_puts("), Stage 3 exit criterion met\n");
            goto done;
        }
    }
    uart_puts("[gic-v3] Stage 3 polling window expired without CNTHP\n");

done:
    for (;;) __asm__ volatile ("wfe");
}

/* ---- vector-based IRQ dispatch (not reached on HVF yet, see note) ---- */

void gic_handle_physical_irq(struct trap_frame *tf) {
    (void)tf;
    unsigned long iar   = read_icc_iar1_el1();
    unsigned      intid = (unsigned)(iar & 0xFFFFFF);

    if (intid >= 1020) return;                 /* 1020..1023 special/spurious */

    if (intid == IRQ_HTIMER_PPI) {
        cnthp_disable();
        write_icc_eoir1_el1(iar);
        return;
    }

    uart_puts("[gic-v3] unexpected INTID ");
    uart_put_hex(intid);
    uart_puts("\n");
    write_icc_eoir1_el1(iar);
}

#endif /* GIC_VERSION == 3 */
