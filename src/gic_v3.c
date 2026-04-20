/*
 * GICv3 implementation for the HVF build (HOST_IF=hvf).
 *
 * Vector-driven delivery now works with the launcher bridging
 * GIC-pending to vCPU-pending via hv_vcpu_set_pending_interrupt.
 * HVF's hv_gic updates ICC/GIC state on hv_gic_set_spi and
 * timer-related events, but does NOT automatically cause the vCPU
 * to take the exception. The launcher reads ICC_HPPIR1_EL1 from
 * outside the vCPU before each hv_vcpu_run and pends IRQ at the
 * vCPU level if anything is present. Then the vector fires normally.
 *
 * Under GIC_VERSION == 2 this file compiles to nothing; gic.c provides
 * the GICv2 equivalent.
 */

#include "gic.h"
#include "uart.h"
#include "exceptions.h"

#if GIC_VERSION == 3

#define GICD_BASE          0x08000000UL
#define GICR_BASE          0x080A0000UL
#define GICR_SGI_OFFSET    0x00010000UL

#define GICR_WAKER         0x0014
#define GICR_WAKER_PROCESSOR_SLEEP  (1u << 1)
#define GICR_WAKER_CHILDREN_ASLEEP  (1u << 2)

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

/* ---- ICC sysreg accessors ---- */

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

/* ---- redistributor ---- */

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

/* ---- CNTHP ---- */

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

volatile unsigned long gic_v3_cnthp_count;

void gic_init_el2(void) {
    uart_puts("[gic-v3] init: SRE, redistributor, distributor, CPU interface\n");

    write_icc_sre_el2(0x9);                         /* SRE + Enable */
    wake_redistributor();
    mmio_w(GICD_BASE + GICD_CTLR, 0x12);            /* EnableGrp1NS | ARE_NS */
    __asm__ volatile ("msr S3_4_C12_C11_0, %0" :: "r"(1UL));  /* ICH_HCR_EL2.En */
    __asm__ volatile ("isb");
    write_icc_pmr_el1(0xFF);
    write_icc_igrpen1_el1(0x1);

    enable_ppi(IRQ_HTIMER_PPI, 0xA0);
    uart_puts("[gic-v3] PPI 26 (CNTHP) enabled\n");

    /* Unmask IRQs at EL2 and arm CNTHP. The launcher pends IRQ at the
     * vCPU level every run; real INTIDs land here via the vector, and
     * spurious (1023) IARs return quickly. */
    __asm__ volatile ("msr daifclr, #0x2" ::: "memory");
    cnthp_arm_ms(500);
    uart_puts("[gic-v3] CNTHP armed; entering WFI loop\n");

    for (unsigned i = 0; i < 1000; i++) {
        __asm__ volatile ("wfi");
        if (gic_v3_cnthp_count) {
            uart_puts("[gic-v3] CNTHP vector fire observed, Stage 3 exit criterion met\n");
            for (;;) __asm__ volatile ("wfe");
        }
    }
    uart_puts("[gic-v3] WFI loop exhausted without CNTHP fire\n");
    for (;;) __asm__ volatile ("wfe");
}

void gic_handle_physical_irq(struct trap_frame *tf) {
    (void)tf;
    unsigned long iar   = read_icc_iar1_el1();
    unsigned      intid = (unsigned)(iar & 0xFFFFFF);

    if (intid >= 1020) return;                     /* spurious, silent return */

    if (intid == IRQ_HTIMER_PPI) {
        cnthp_disable();
        write_icc_eoir1_el1(iar);
        gic_v3_cnthp_count++;
        return;
    }

    uart_puts("[gic-v3] unexpected INTID ");
    uart_put_hex(intid);
    uart_puts("\n");
    write_icc_eoir1_el1(iar);
}

#endif /* GIC_VERSION == 3 */
