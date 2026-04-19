/*
 * GICv3 implementation for the HVF build (HOST_IF=hvf).
 *
 * Stage 2 scope: prove the ICC sysreg interface works at EL2 under
 * HVF. Wake the redistributor, enable the distributor + CPU
 * interface, read ICC_IAR1_EL1 once, print the value, then halt.
 *
 * Real IRQ dispatch + vtimer forwarding comes in Stages 3 and 4.
 *
 * Under GIC_VERSION==2 this file compiles to nothing; gic.c provides
 * the equivalent functions.
 */

#include "gic.h"
#include "uart.h"
#include "exceptions.h"

#if GIC_VERSION == 3

/* IPA layout matches host/hvf_main.c. */
#define GICD_BASE          0x08000000UL
#define GICR_BASE          0x080A0000UL     /* per-CPU redistributor */
#define GICR_SGI_OFFSET    0x00010000UL     /* RD_base + 64 KiB = SGI frame */

/* GICD_CTLR is inherited from gic.h (offset 0). Local GICR offsets: */
#define GICR_WAKER         0x0014

#define GICR_WAKER_PROCESSOR_SLEEP  (1u << 1)
#define GICR_WAKER_CHILDREN_ASLEEP  (1u << 2)

/* ICC system register accessors.
 * Apple's toolchain understands the named form for most GICv3 sysregs
 * on modern ARMv8.x; fall back to the raw S3_<op1>_<CRn>_<CRm>_<op2>
 * form where needed for portability with older GCC. */

static inline unsigned mmio_r(unsigned long addr) {
    return *(volatile unsigned *)addr;
}
static inline void mmio_w(unsigned long addr, unsigned v) {
    *(volatile unsigned *)addr = v;
}

/* ICC_SRE_EL2: S3_4_C12_C9_5. Bit 0 = SRE (system-register interface),
 * bit 3 = Enable (lower-EL can use the sysreg interface too). */
static inline void write_icc_sre_el2(unsigned long v) {
    __asm__ volatile ("msr S3_4_C12_C9_5, %0" :: "r"(v));
    __asm__ volatile ("isb");
}
static inline unsigned long read_icc_sre_el2(void) {
    unsigned long v;
    __asm__ volatile ("mrs %0, S3_4_C12_C9_5" : "=r"(v));
    return v;
}

/* ICC_PMR_EL1: S3_0_C4_C6_0. Priority mask, all 0xFF = accept all. */
static inline void write_icc_pmr_el1(unsigned long v) {
    __asm__ volatile ("msr S3_0_C4_C6_0, %0" :: "r"(v));
}

/* ICC_IGRPEN1_EL1: S3_0_C12_C12_7. Bit 0 = enable Group 1 NS IRQs. */
static inline void write_icc_igrpen1_el1(unsigned long v) {
    __asm__ volatile ("msr S3_0_C12_C12_7, %0" :: "r"(v));
    __asm__ volatile ("isb");
}

/* ICC_IAR1_EL1: S3_0_C12_C12_0. Acknowledge a pending IRQ. */
static inline unsigned long read_icc_iar1_el1(void) {
    unsigned long v;
    __asm__ volatile ("mrs %0, S3_0_C12_C12_0" : "=r"(v));
    __asm__ volatile ("dsb sy; isb" ::: "memory");
    return v;
}

static void wake_redistributor(void) {
    /* Clear ProcessorSleep in GICR_WAKER, then poll ChildrenAsleep
     * until it reads back as 0. Standard GICv3 wake-up sequence. */
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

void gic_init_el2(void) {
    uart_puts("[gic-v3] init: SRE + redistributor wake + CPU interface\n");

    /* 1. Enable the system-register interface at EL2.
     *    SRE=1 makes ICC_*_EL1 accesses use sysregs, not MMIO at GICC.
     *    Enable (bit 3) lets lower ELs use the sysreg interface too. */
    write_icc_sre_el2(0x9);

    /* 2. Wake the redistributor for this vCPU. */
    wake_redistributor();

    /* 3. Enable the distributor (Group 1 NS, affinity routing on).
     *    Bit 1 = EnableGrp1NS, bit 4 = ARE_NS (HVF's GIC is always
     *    affinity-routed but the bit is required on real hw). */
    mmio_w(GICD_BASE + GICD_CTLR, 0x12);

    /* 4. CPU interface: accept all priorities, enable Group 1. */
    write_icc_pmr_el1(0xFF);
    write_icc_igrpen1_el1(0x1);

    /* 5. Read IAR1 once. With no IRQ pending the GIC must return
     *    1023 (spurious). Any other value here indicates either a
     *    config bug or a leaked pending state. */
    unsigned long iar = read_icc_iar1_el1();
    uart_puts("[gic-v3] ICC_IAR1_EL1 = ");
    uart_put_hex(iar);
    uart_puts(iar == 1023 ? "  (spurious, as expected)\n"
                          : "  (unexpected, investigate)\n");

    uart_puts("[gic-v3] Stage 2 complete, halting.\n");
    for (;;) __asm__ volatile ("wfe");
}

void gic_handle_physical_irq(struct trap_frame *tf) {
    (void)tf;
    uart_puts("[gic-v3] IRQ received (no dispatch wired yet)\n");
}

#endif /* GIC_VERSION == 3 */
