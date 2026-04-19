#pragma once
#include "exceptions.h"

/*
 * Per-vCPU state. On every VM exit the vector tail has already pushed a
 * trap_frame on the EL2 stack; sched_tick copies the GP/ELR/SPSR portion
 * into/out of the current vcpu and swaps the EL1-visible sysregs by
 * hand (SP_EL1, VTTBR_EL2, HCR_EL2, CNTV_*, TPIDR_EL1).
 *
 * HCR_EL2 is per-vCPU because HCR_EL2.VI (our virtual-IRQ pending bit)
 * is the only workable injection path on QEMU TCG GICv2.
 */
typedef struct vcpu {
    unsigned long x[31];       /* x0..x30                               */
    unsigned long elr_el2;     /* guest PC at last exit                 */
    unsigned long spsr_el2;    /* guest PSTATE at last exit             */

    /* EL2-owned per-vCPU state */
    unsigned long vttbr_el2;   /* stage-2 root + VMID in bits [55:48]   */
    unsigned long hcr_el2;     /* includes VI injection state           */
    unsigned long cntv_cval;   /* guest vtimer deadline                 */
    unsigned long cntv_ctl;    /* guest vtimer control                  */

    /* EL1 stage-1 translation + exception context. All shared hardware
     * between vCPUs, so must be saved/restored on swap once any guest
     * (read: Linux) enables its MMU — otherwise the other guest runs
     * with the wrong translation tables and lands in foreign memory. */
    unsigned long sctlr_el1;
    unsigned long ttbr0_el1;
    unsigned long ttbr1_el1;
    unsigned long tcr_el1;
    unsigned long mair_el1;
    unsigned long amair_el1;
    unsigned long vbar_el1;
    unsigned long contextidr_el1;
    unsigned long cpacr_el1;
    unsigned long elr_el1;
    unsigned long spsr_el1;
    unsigned long sp_el1;
    unsigned long sp_el0;
    unsigned long tpidr_el0;
    unsigned long tpidrro_el0;
    unsigned long tpidr_el1;   /* = &guest_states[id] — per-guest TLS   */
    unsigned long esr_el1;
    unsigned long far_el1;
    unsigned long par_el1;

    unsigned      id;
    unsigned      vmid;
} vcpu_t;

void          sched_init(void);
__attribute__((noreturn)) void sched_start(void);
void          sched_tick(trap_frame_t *tf);
vcpu_t       *current_vcpu(void);
