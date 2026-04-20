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

/* Stage 4: inject a virtual IRQ into an EL1 guest via ICH_LRn_EL2.
 * Does not return (ERETs into the guest, guest loops WFE). */
extern void guest_v3_start(void);

static inline void write_ich_hcr_el2(unsigned long v) {
    __asm__ volatile ("msr S3_4_C12_C11_0, %0" :: "r"(v));
    __asm__ volatile ("isb");
}
static inline void write_ich_vmcr_el2(unsigned long v) {
    __asm__ volatile ("msr S3_4_C12_C11_7, %0" :: "r"(v));
    __asm__ volatile ("isb");
}
static inline void write_ich_lr0_el2(unsigned long v) {
    __asm__ volatile ("msr S3_4_C12_C12_0, %0" :: "r"(v));
    __asm__ volatile ("isb");
}

/* Compose an ICH_LRn_EL2 value for a Group 1 pending virtual INTID.
 *   bits  31:0    — vINTID (we use 23:0)
 *   bits 48:41    — priority (we pack into bit 48+8 per arch)
 *   bit 60        — Group (1 = Group 1)
 *   bits 63:62    — State: 01 = pending
 * Using ARMv8 GICv3 encoding:
 *   vINTID[31:0], pINTID[41:32], priority[55:48], group[60], state[63:62]
 */
static unsigned long make_lr(unsigned intid, unsigned char prio, int group1) {
    unsigned long v = 0;
    v |= (unsigned long)(intid & 0xFFFFFF);
    v |= ((unsigned long)prio) << 48;
    if (group1) v |= (1UL << 60);
    v |= (1UL << 62);                        /* State = Pending (01) */
    return v;
}

static __attribute__((noreturn)) void stage4_eret_to_guest(void) {
    /* Set HCR_EL2.IMO = 1 so the guest's ICC_*_EL1 accesses are
     * virtualized to ICV (our LRn state), and physical IRQs continue
     * to route to our EL2 vector. Without this, guest IAR reads go to
     * the physical CPU interface (empty) and return spurious. */
    {
        unsigned long hcr;
        __asm__ volatile ("mrs %0, hcr_el2" : "=r"(hcr));
        hcr |= (1UL << 4);                               /* IMO */
        __asm__ volatile ("msr hcr_el2, %0" :: "r"(hcr));
        __asm__ volatile ("isb");
    }

    /* Enable the virtual CPU interface for our EL1 guest. */
    write_ich_hcr_el2(1);                                /* ICH_HCR_EL2.En = 1 */

    /* ICH_VMCR_EL2: populate the guest-visible ICV control state.
     *   VPMR (bits 31:24) = 0xFF  → accept all priorities
     *   VENG1 (bit 1)   = 1       → virtual Group 1 IRQs enabled
     *   VBPR1 (bits 20:18) = 0    → finest grouping
     *   VEOIM (bit 9)   = 0       → combined EOI
     */
    unsigned long vmcr = ((unsigned long)0xFF << 24) | (1UL << 1);
    write_ich_vmcr_el2(vmcr);

    /* Inject INTID 42 as pending, Group 1, priority 0xA0. SW-triggered
     * (HW=0). Virtual CPU interface will present this to EL1 guest's
     * ICV_IAR1_EL1 when it reads. */
    write_ich_lr0_el2(make_lr(42, 0xA0, 1));

    uart_puts("[gic-v3] LR0 loaded (INTID 42 pending), ERETing to EL1 guest\n");

    /* ERET state:
     *   ELR_EL2  = guest_v3_start
     *   SPSR_EL2 = EL1h (M = 0b0101) with DAIF fully masked
     *              (the guest will daifclr #2 itself once VBAR is live).
     */
    unsigned long spsr = 0x3C5UL;
    __asm__ volatile ("msr elr_el2,  %0" :: "r"((unsigned long)guest_v3_start));
    __asm__ volatile ("msr spsr_el2, %0" :: "r"(spsr));
    __asm__ volatile ("isb; eret" ::: "memory");
    __builtin_unreachable();
}

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

    /* Stage 4: load ICH_LR0_EL2 with a pending virtual IRQ and ERET
     * into a minimal EL1 guest. The guest handles the IRQ through
     * the CPU's virtual CPU interface — no vgic_mmio trap needed. */
    stage4_eret_to_guest();
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
