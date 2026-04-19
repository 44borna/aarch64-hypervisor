/*
 * Round-robin scheduler on top of the EL2 physical hypervisor timer
 * (CNTHP, PPI 26). One slice = SLICE_MS ms. On each CNTHP tick, the
 * GIC IRQ handler calls sched_tick(tf) — we save the outgoing vCPU's
 * state from `tf` + sysregs, restore the incoming vCPU into `tf` +
 * sysregs, and re-arm CNTHP. When handle_exception() returns,
 * vector_tail's RESTORE_CTX pops the (now-swapped) frame and ERETs
 * into the new guest.
 *
 * Guests are both entered at guest_entry_common(id) on first run.
 * Per-guest state lives in guest_states[]; each vCPU's TPIDR_EL1
 * points at its own slot, so the shared guest text can use
 *   mrs x, tpidr_el1
 * to find its own mutable state without touching the other VM's BSS.
 */
#include "vcpu.h"
#include "uart.h"
#include "stage2.h"
#include "vgic.h"

/* NVCPU is a build-time knob. The Linux-only path (default) runs one
 * vCPU and leaves the scheduler tick disabled — cheapest for 8c. Build
 * with `make NVCPU=2` to restore the Stage 7 layout (vCPU 0 = Linux,
 * vCPU 1 = bare-metal guest) and re-enable CNTHP round-robin.
 *
 * Caveat for NVCPU=2 under QEMU TCG: the first few slices trade cleanly
 * (Linux boots, guest B boots + ticks, vtimer forwards), but CNTHP
 * eventually stops asserting ISTATUS despite CVAL being in the past.
 * The distributor shows `ispend=0` for PPI 26 while `cntpct > cval` —
 * TCG's generic-timer model doesn't re-raise the line cleanly after
 * an arm/disable cycle. KVM doesn't have this problem. Good enough
 * to prove the scheduler, not good enough for an uptime benchmark. */
#ifndef NVCPU
#define NVCPU     1
#endif
#define SLICE_MS  50

/* Linux AArch64 boot protocol lands us at the Image's first byte.
 * QEMU's -device loader drops it here (see Makefile GUEST_KERNEL_ADDR)
 * and the DTB at GUEST_DTB_ADDR. */
#define LINUX_ENTRY_IPA  0x48200000UL
#define LINUX_DTB_IPA    0x4C000000UL

#define HCR_VM    (1UL << 0)
#define HCR_IMO   (1UL << 4)
#define HCR_TWI   (1UL << 13)
#define HCR_RW    (1UL << 31)
/* TID3 dropped for 8b: Linux reads dozens of ID registers during arch
 * init; letting them through untrapped is far cheaper than faking each
 * one, and the CPU reports its real (emulated a72) features. */
#define HCR_FLAGS (HCR_RW | HCR_TWI | HCR_IMO | HCR_VM)

/* SPSR: M[3:0]=0b0101 (EL1h); DAIF[9:6]=1111 (mask all async exceptions) */
#define SPSR_EL1H_DAIF 0x3C5UL

/* Per-guest mutable state; pointed at by each vCPU's TPIDR_EL1. The
 * shared guest code reaches this through tpidr_el1, so "guest A" never
 * sees "guest B"'s counters even though all host memory is identity
 * mapped into both VMs. */
typedef struct guest_state {
    unsigned      id;           /* 0 or 1                */
    unsigned      _pad;
    unsigned long ticks;        /* vtimer interrupts taken by this guest */
    unsigned long tick_period;  /* cntfrq/10 = 100 ms    */
} guest_state_t;

static guest_state_t guest_states[NVCPU];

extern char __guest_stack_top_a[];
extern char __guest_stack_top_b[];
extern void guest_entry_common(unsigned long id);

static vcpu_t  vcpus[NVCPU];
static unsigned current;

vcpu_t *current_vcpu(void) { return &vcpus[current]; }

/* ---- CNTHP (EL2 physical timer) --------------------------------------- */

static void arm_hyp_timer_ms(unsigned ms) {
    unsigned long freq, now;
    __asm__ volatile ("mrs %0, cntfrq_el0" : "=r"(freq));
    __asm__ volatile ("mrs %0, cntpct_el0" : "=r"(now));
    unsigned long cval = now + (freq * (unsigned long)ms) / 1000UL;
    __asm__ volatile ("msr cnthp_cval_el2, %0" :: "r"(cval));
    __asm__ volatile ("msr cnthp_ctl_el2,  %0" :: "r"(1UL));
    __asm__ volatile ("isb" ::: "memory");
}

/* CNTHP fires every SLICE_MS regardless of NVCPU. With NVCPU>1 the
 * handler swaps vCPUs; with NVCPU=1 it's a pure heartbeat used to
 * reassert PL011 RXIM + GIC SPI 33 while Linux sits in a tickless
 * idle waiting for console input. */
void sched_rearm_tick(void) {
    arm_hyp_timer_ms(SLICE_MS);
}

/* ---- save / restore --------------------------------------------------- */

#define MRS(reg)                                                \
    ({ unsigned long _v; __asm__ volatile ("mrs %0, " #reg      \
        : "=r"(_v)); _v; })
#define MSR(reg, val)                                           \
    __asm__ volatile ("msr " #reg ", %0" :: "r"((unsigned long)(val)))

static void save_vcpu(vcpu_t *v, trap_frame_t *tf) {
    for (int i = 0; i < 31; i++) v->x[i] = tf->x[i];
    v->elr_el2        = tf->elr_el2;
    v->spsr_el2       = tf->spsr_el2;

    v->vttbr_el2      = MRS(vttbr_el2);
    v->hcr_el2        = MRS(hcr_el2);
    v->cntv_cval      = MRS(cntv_cval_el0);
    v->cntv_ctl       = MRS(cntv_ctl_el0);

    v->sctlr_el1      = MRS(sctlr_el1);
    v->ttbr0_el1      = MRS(ttbr0_el1);
    v->ttbr1_el1      = MRS(ttbr1_el1);
    v->tcr_el1        = MRS(tcr_el1);
    v->mair_el1       = MRS(mair_el1);
    v->amair_el1      = MRS(amair_el1);
    v->vbar_el1       = MRS(vbar_el1);
    v->contextidr_el1 = MRS(contextidr_el1);
    v->cpacr_el1      = MRS(cpacr_el1);
    v->elr_el1        = MRS(elr_el1);
    v->spsr_el1       = MRS(spsr_el1);
    v->sp_el1         = MRS(sp_el1);
    v->sp_el0         = MRS(sp_el0);
    v->tpidr_el0      = MRS(tpidr_el0);
    v->tpidrro_el0    = MRS(tpidrro_el0);
    v->tpidr_el1      = MRS(tpidr_el1);
    v->esr_el1        = MRS(esr_el1);
    v->far_el1        = MRS(far_el1);
    v->par_el1        = MRS(par_el1);
}

static void load_vcpu(vcpu_t *v, trap_frame_t *tf) {
    for (int i = 0; i < 31; i++) tf->x[i] = v->x[i];
    tf->elr_el2       = v->elr_el2;
    tf->spsr_el2      = v->spsr_el2;

    MSR(vttbr_el2,     v->vttbr_el2);
    MSR(hcr_el2,       v->hcr_el2);
    MSR(cntv_cval_el0, v->cntv_cval);
    MSR(cntv_ctl_el0,  v->cntv_ctl);

    /* Disable EL1 MMU before swapping TTBR/TCR/MAIR, then let the
     * incoming vCPU's SCTLR_EL1 decide whether to re-enable. Skipping
     * this step means the CPU can fetch through the wrong tables
     * between the TTBR0 write and the final SCTLR write. */
    MSR(sctlr_el1,      0UL);
    __asm__ volatile ("isb" ::: "memory");

    MSR(ttbr0_el1,      v->ttbr0_el1);
    MSR(ttbr1_el1,      v->ttbr1_el1);
    MSR(tcr_el1,        v->tcr_el1);
    MSR(mair_el1,       v->mair_el1);
    MSR(amair_el1,      v->amair_el1);
    MSR(vbar_el1,       v->vbar_el1);
    MSR(contextidr_el1, v->contextidr_el1);
    MSR(cpacr_el1,      v->cpacr_el1);
    MSR(elr_el1,        v->elr_el1);
    MSR(spsr_el1,       v->spsr_el1);
    MSR(sp_el1,         v->sp_el1);
    MSR(sp_el0,         v->sp_el0);
    MSR(tpidr_el0,      v->tpidr_el0);
    MSR(tpidrro_el0,    v->tpidrro_el0);
    MSR(tpidr_el1,      v->tpidr_el1);
    MSR(esr_el1,        v->esr_el1);
    MSR(far_el1,        v->far_el1);
    MSR(par_el1,        v->par_el1);
    MSR(sctlr_el1,      v->sctlr_el1);

    /* Flush any stage-1/2 TLB entries tagged for the *outgoing* VMID
     * so the new VM sees its own translations. Coarse but correct. */
    __asm__ volatile ("dsb ish              \n"
                      "tlbi vmalls12e1is    \n"
                      "dsb ish              \n"
                      "isb                  \n" ::: "memory");
}

/* ---- init / start / tick --------------------------------------------- */

static void init_vcpu(unsigned i, unsigned vmid, char *stack_top) {
    vcpu_t        *v = &vcpus[i];
    guest_state_t *s = &guest_states[i];

    for (int k = 0; k < 31; k++) v->x[k] = 0;
    s->id          = i;
    s->ticks       = 0;
    s->tick_period = 0;

    v->id        = i;
    v->vmid      = vmid;
    v->vttbr_el2 = stage2_alloc_vm(i, vmid);
    v->sp_el1    = (unsigned long)stack_top;
    v->elr_el2   = (unsigned long)guest_entry_common;
    v->spsr_el2  = SPSR_EL1H_DAIF;
    v->hcr_el2   = HCR_FLAGS;
    v->cntv_cval = 0;
    v->cntv_ctl  = 0;
    v->tpidr_el1 = (unsigned long)s;
    v->x[0]      = (unsigned long)i;   /* first-entry arg */

    vgic_init(i);
}

void sched_init(void) {
    stage2_global_init();
    init_vcpu(0, /*vmid*/1, __guest_stack_top_a);

    /* Stage 8a: vCPU 0 boots Linux instead of guest_entry_common.
     * Linux expects x0 = DTB IPA, x1..x3 = 0, MMU off, DAIF masked. */
    vcpus[0].elr_el2 = LINUX_ENTRY_IPA;
    vcpus[0].x[0]    = LINUX_DTB_IPA;
    vcpus[0].x[1]    = 0;
    vcpus[0].x[2]    = 0;
    vcpus[0].x[3]    = 0;

#if NVCPU > 1
    init_vcpu(1, /*vmid*/2, __guest_stack_top_b);
#endif
    current = 0;
    uart_puts("[sched] initialised "); uart_put_hex(NVCPU);
    uart_puts(" vCPU(s), slice="); uart_put_hex(SLICE_MS);
    uart_puts(" ms; vCPU0 -> Linux@"); uart_put_hex(LINUX_ENTRY_IPA);
    uart_puts(" DTB@");               uart_put_hex(LINUX_DTB_IPA);
    uart_puts("\n");
}

__attribute__((noreturn))
void sched_start(void) {
    vcpu_t *v = &vcpus[current];

    MSR(hcr_el2,       v->hcr_el2);
    MSR(sp_el1,        v->sp_el1);
    MSR(vttbr_el2,     v->vttbr_el2);
    MSR(cntv_cval_el0, v->cntv_cval);
    MSR(cntv_ctl_el0,  v->cntv_ctl);
    MSR(tpidr_el1,     v->tpidr_el1);
    MSR(elr_el2,       v->elr_el2);
    MSR(spsr_el2,      v->spsr_el2);

    /* Always arm CNTHP — even with NVCPU=1, we use the tick as a
     * periodic heartbeat to re-assert PL011 RXIM + GIC SPI 33 enable
     * (Linux's pl011 driver tears them down when it thinks the tty
     * closed, and there's no other periodic EL2 entry while the
     * guest sits in a tickless idle). */
    arm_hyp_timer_ms(SLICE_MS);

    uart_puts("[sched] ERET into vCPU 0\n");
    /* Linux AArch64 boot protocol: x1..x3 must be zero at entry.
     * Pin them via register vars so the inline asm sees the intended
     * values instead of whatever the C call path left in those regs. */
    register unsigned long x0 __asm__("x0") = v->x[0];
    register unsigned long x1 __asm__("x1") = v->x[1];
    register unsigned long x2 __asm__("x2") = v->x[2];
    register unsigned long x3 __asm__("x3") = v->x[3];
    __asm__ volatile ("isb; eret" :: "r"(x0), "r"(x1), "r"(x2), "r"(x3) : "memory");
    __builtin_unreachable();
}

void sched_tick(trap_frame_t *tf) {
#if NVCPU > 1
    save_vcpu(&vcpus[current], tf);
    current = (current + 1) % NVCPU;
    load_vcpu(&vcpus[current], tf);
    arm_hyp_timer_ms(SLICE_MS);
#else
    (void)tf;
#endif
}
