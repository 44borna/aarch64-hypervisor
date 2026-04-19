#include "uart.h"
#include "exceptions.h"
#include "hvc.h"
#include "gic.h"
#include "vgic.h"
#include "vcpu.h"

/* Silence per-trap prints during Linux boot (hundreds of ID-reg MRS,
 * WFIs in the idle loop, etc.). Flip to 1 when debugging EL2 traps. */
#define HYP_DEBUG_TRAPS 0

/* ---- VM-exit ring buffer ---------------------------------------------- *
 *
 * A fixed-size circular log of the most recent traps into EL2. Every
 * call to handle_exception() appends one record; dump_and_halt() prints
 * the last EXIT_RING_SIZE entries so a crash shows its own prelude
 * instead of requiring us to bisect with single-stepping.
 *
 * Kept deliberately small: two cache lines per entry is overkill for a
 * post-mortem buffer we only read on fatal paths.
 */
#define EXIT_RING_SIZE 32

typedef struct exit_record {
    unsigned long esr;
    unsigned long elr;
    unsigned long far;
    unsigned long kind;
    unsigned      vcpu;
    unsigned      _pad;
} exit_record_t;

static exit_record_t exit_ring[EXIT_RING_SIZE];
static unsigned      exit_ring_head;   /* index of next slot to write */
static unsigned long exit_ring_count;  /* total records ever appended */

static void exit_ring_push(unsigned long kind, unsigned long esr,
                           unsigned long elr, unsigned long far,
                           unsigned vcpu) {
    exit_record_t *r = &exit_ring[exit_ring_head];
    r->esr  = esr;
    r->elr  = elr;
    r->far  = far;
    r->kind = kind;
    r->vcpu = vcpu;
    exit_ring_head = (exit_ring_head + 1) & (EXIT_RING_SIZE - 1);
    exit_ring_count++;
}

/* PSCI-over-HVC (arm,psci-0.2). IDs are SMC Calling Convention fast
 * calls in the Standard Secure Service range 0x84xx/0xC4xx. */
#define PSCI_VERSION_FN        0x84000000U
#define PSCI_CPU_OFF_FN        0x84000002U
#define PSCI_CPU_ON_FN_32      0x84000003U
#define PSCI_CPU_ON_FN_64      0xC4000003U
#define PSCI_AFFINITY_INFO_32  0x84000004U
#define PSCI_AFFINITY_INFO_64  0xC4000004U
#define PSCI_MIGRATE_INFO_TYPE 0x84000006U
#define PSCI_SYSTEM_OFF_FN     0x84000008U
#define PSCI_SYSTEM_RESET_FN   0x84000009U
#define PSCI_FEATURES_FN       0x8400000AU

#define PSCI_RET_SUCCESS        0
#define PSCI_RET_NOT_SUPPORTED  (-1L)

static int is_psci_fid(unsigned long fid) {
    unsigned high = (unsigned)(fid >> 24);
    return high == 0x84 || high == 0xC4;
}

__attribute__((noreturn))
static void psci_halt(const char *why) {
    uart_puts("  [PSCI] "); uart_puts(why); uart_puts(" — halting\n");
    for (;;) __asm__ volatile ("wfe");
}

static int handle_psci(trap_frame_t *tf, unsigned long fid) {
    switch ((unsigned)fid) {
    case PSCI_VERSION_FN:
        tf->x[0] = 0x00010000UL;          /* PSCI 1.0 */
        return 1;
    case PSCI_MIGRATE_INFO_TYPE:
        tf->x[0] = 2;                     /* Trusted OS not present */
        return 1;
    case PSCI_AFFINITY_INFO_32:
    case PSCI_AFFINITY_INFO_64:
        /* Uniprocessor: affinity 0 is ON, everything else is OFF. */
        tf->x[0] = (tf->x[1] == 0) ? 0UL /* ON */ : 1UL /* OFF */;
        return 1;
    case PSCI_FEATURES_FN: {
        unsigned fn = (unsigned)tf->x[1];
        switch (fn) {
        case PSCI_VERSION_FN:
        case PSCI_CPU_OFF_FN:
        case PSCI_AFFINITY_INFO_32:
        case PSCI_AFFINITY_INFO_64:
        case PSCI_MIGRATE_INFO_TYPE:
        case PSCI_SYSTEM_OFF_FN:
        case PSCI_SYSTEM_RESET_FN:
        case PSCI_FEATURES_FN:
            tf->x[0] = PSCI_RET_SUCCESS;
            return 1;
        default:
            tf->x[0] = (unsigned long)PSCI_RET_NOT_SUPPORTED;
            return 1;
        }
    }
    case PSCI_CPU_OFF_FN:
        psci_halt("guest CPU_OFF");
    case PSCI_SYSTEM_OFF_FN:
        psci_halt("guest SYSTEM_OFF");
    case PSCI_SYSTEM_RESET_FN:
        psci_halt("guest SYSTEM_RESET");
    case PSCI_CPU_ON_FN_32:
    case PSCI_CPU_ON_FN_64:
        tf->x[0] = (unsigned long)PSCI_RET_NOT_SUPPORTED;  /* UP only */
        return 1;
    default:
        tf->x[0] = (unsigned long)PSCI_RET_NOT_SUPPORTED;
        return 1;
    }
}

static inline unsigned long read_esr_el2(void) {
    unsigned long v;
    __asm__ volatile ("mrs %0, esr_el2" : "=r"(v));
    return v;
}

static inline unsigned long read_far_el2(void) {
    unsigned long v;
    __asm__ volatile ("mrs %0, far_el2" : "=r"(v));
    return v;
}

static inline unsigned long read_hpfar_el2(void) {
    unsigned long v;
    __asm__ volatile ("mrs %0, hpfar_el2" : "=r"(v));
    return v;
}

static const char *ec_name(unsigned ec) {
    switch (ec) {
    case 0x00: return "Unknown reason";
    case 0x01: return "Trapped WFI/WFE";
    case 0x07: return "SIMD/FP access trapped";
    case 0x15: return "SVC from AArch64";
    case 0x16: return "HVC from AArch64";
    case 0x17: return "SMC from AArch64";
    case 0x18: return "Trapped MSR/MRS/system";
    case 0x20: return "Instruction Abort, lower EL";
    case 0x21: return "Instruction Abort, same EL";
    case 0x22: return "PC alignment fault";
    case 0x24: return "Data Abort, lower EL";
    case 0x25: return "Data Abort, same EL";
    case 0x26: return "SP alignment fault";
    case 0x2f: return "SError";
    case 0x30: return "HW Breakpoint, lower EL";
    case 0x31: return "HW Breakpoint, same EL";
    case 0x3c: return "BRK instruction (AArch64)";
    default:   return "unknown EC";
    }
}

static const char *vec_name(unsigned long kind) {
    static const char * const names[16] = {
        "EL2t sync",   "EL2t irq",   "EL2t fiq",   "EL2t serror",
        "EL2h sync",   "EL2h irq",   "EL2h fiq",   "EL2h serror",
        "Lower64 sync","Lower64 irq","Lower64 fiq","Lower64 serror",
        "Lower32 sync","Lower32 irq","Lower32 fiq","Lower32 serror",
    };
    return (kind < 16) ? names[kind] : "invalid";
}

static void put_hex_line(const char *label, unsigned long v) {
    uart_puts(label); uart_put_hex(v); uart_puts("\n");
}

static void dump_exit_ring(void) {
    unsigned long total = exit_ring_count;
    if (!total) { uart_puts("  (exit ring empty)\n"); return; }

    unsigned depth = (total < EXIT_RING_SIZE) ? (unsigned)total
                                              : EXIT_RING_SIZE;
    uart_puts("\n--- last "); uart_put_hex(depth);
    uart_puts(" VM exits (newest first, of ");
    uart_put_hex(total); uart_puts(" total) ---\n");

    unsigned idx = exit_ring_head;        /* next-to-write */
    for (unsigned i = 0; i < depth; i++) {
        idx = (idx + EXIT_RING_SIZE - 1) & (EXIT_RING_SIZE - 1);
        exit_record_t *r = &exit_ring[idx];
        unsigned ec = (unsigned)((r->esr >> 26) & 0x3f);
        uart_puts("  ["); uart_put_hex(i); uart_puts("] v=");
        uart_put_hex(r->vcpu);
        uart_puts(" kind="); uart_puts(vec_name(r->kind));
        uart_puts(" EC=");   uart_put_hex(ec);
        uart_puts(" (");     uart_puts(ec_name(ec)); uart_puts(")");
        uart_puts(" ELR=");  uart_put_hex(r->elr);
        uart_puts(" FAR=");  uart_put_hex(r->far);
        uart_puts("\n");
    }
}

static void dump_and_halt(trap_frame_t *tf, unsigned long kind,
                          unsigned long esr) {
    unsigned      ec  = (unsigned)((esr >> 26) & 0x3f);
    unsigned long iss = esr & 0x1ffffffUL;

    uart_puts("\n=== UNHANDLED EL2 EXCEPTION ===\n");
    uart_puts("  vector  : "); uart_puts(vec_name(kind));
    uart_puts(" (kind="); uart_put_hex(kind); uart_puts(")\n");
    put_hex_line("  ESR_EL2 : ", esr);
    uart_puts("    EC    : "); uart_put_hex(ec);
    uart_puts("  ("); uart_puts(ec_name(ec)); uart_puts(")\n");
    put_hex_line("    ISS   : ", iss);
    put_hex_line("  FAR_EL2 : ", read_far_el2());
    put_hex_line("  ELR_EL2 : ", tf->elr_el2);
    put_hex_line("  SPSR_EL2: ", tf->spsr_el2);
    dump_exit_ring();
    uart_puts("===============================\nhalting.\n");
    for (;;) __asm__ volatile ("wfe");
}

/* ---- lower-EL synchronous: trap handlers ------------------------------- */

static void handle_hvc(trap_frame_t *tf, unsigned long esr) {
    unsigned imm = (unsigned)(esr & 0xffff);
    unsigned long num = tf->x[0];

#if HYP_DEBUG_TRAPS
    /* Stage 7: IRQ_DONE is on the hot path (every guest vtimer tick) —
     * don't trace it. Trace everything else so stage 4/5 regressions
     * are still visible. */
    if (num != HVC_IRQ_DONE) {
        uart_puts("  [HVC #"); uart_put_hex(imm);
        uart_puts("]  x0="); uart_put_hex(num);
        uart_puts("  x1="); uart_put_hex(tf->x[1]);
        uart_puts("  x2="); uart_put_hex(tf->x[2]);
        uart_puts("\n");
    }
#else
    (void)imm;
#endif

    if (is_psci_fid(num)) {
        handle_psci(tf, num);
        return;
    }

    switch (num) {
    case HVC_PING:
        tf->x[0] = 0;
        break;
    case HVC_PUTS: {
        const char *s = (const char *)tf->x[1];
        uart_puts("  [hypervisor relaying guest string] ");
        uart_puts(s);
        tf->x[0] = 0;
        break;
    }
    case HVC_IRQ_DONE:
        /* Stage 7 bare-metal guests skip GICC_EOIR; clear the vGIC
         * pending/active slots ourselves so the next inject is clean. */
        vgic_ack_all(current_vcpu()->id);
        tf->x[0] = 0;
        break;
    default:
#if HYP_DEBUG_TRAPS
        uart_puts("  (unknown hypercall — returning -1)\n");
#endif
        tf->x[0] = ~0UL;
        break;
    }
    /* ELR_EL2 already points past the HVC — do not advance. */
}

static void handle_wfi(trap_frame_t *tf, unsigned long esr) {
#if HYP_DEBUG_TRAPS
    int is_wfe = (int)(esr & 1);
    uart_puts("  [WFI trap] guest executed ");
    uart_puts(is_wfe ? "WFE" : "WFI");
    uart_puts(" — logging and skipping\n");
#else
    (void)esr;
#endif
    /* Linux's idle path hits WFI constantly. We use it as the
     * console-RX "heartbeat":
     *  1. Keep PL011 IMSC + GIC SPI 33 enable hot so the line
     *     stays armed even when Linux's pl011 driver tore it down.
     *  2. Clear any stale SGI pending so a latched SGI at priority 0
     *     doesn't block SPI 33 at the CPU interface (the same
     *     head-of-line problem we hit in 8c).
     *  3. Poll GICC_HPPIR and actively IAR-forward SPI 33 to the
     *     guest — TCG's GICv2 doesn't reliably assert the IRQ line
     *     to the CPU when using HCR_EL2.IMO routing, so the outer
     *     delivery path never fires. Pulling at WFI side-steps it. */
    *(volatile unsigned *)(0x09000000UL + 0x38) = 0x50;              /* PL011 IMSC */
    *(volatile unsigned *)(GICD_BASE_PA + 0x104) = (1u << (33 - 32));
    *(volatile unsigned *)(GICD_BASE_PA + 0x820) = 0x00010000u;      /* ITARGETSR[33]=1 */
    *(volatile unsigned *)(GICD_BASE_PA + 0x420) = 0x00A00000u;      /* IPRIORITYR[33]=0xA0 */
    *(volatile unsigned *)(GICD_BASE_PA + 0x280) = 0x0000FFFFu;      /* clear SGI pending */

    unsigned hppir = *(volatile unsigned *)(GICC_BASE_PA + 0x18);
    if ((hppir & 0x3FF) == 33 && !vgic_has_pending_phys_iar(current_vcpu()->id)) {
        unsigned iar = *(volatile unsigned *)(GICC_BASE_PA + 0x0C);
        if ((iar & 0x3FF) == 33)
            vgic_forward_spi(current_vcpu()->id, 33, iar);
    }

    tf->elr_el2 += 4;
}

static void handle_stage2_abort(trap_frame_t *tf, unsigned long esr) {
    unsigned long far   = read_far_el2();
    unsigned long hpfar = read_hpfar_el2();
    unsigned long ipa   = ((hpfar & ~0xFUL) << 8) | (far & 0xFFFUL);
    unsigned long iss   = esr & 0x1ffffffUL;

    if (ipa >= GICD_EMU_BASE && ipa < GICC_EMU_LIMIT) {
        vgic_mmio(tf, ipa, iss);
        tf->elr_el2 += 4;
        return;
    }

#if HYP_DEBUG_TRAPS
    unsigned dfsc = (unsigned)(iss & 0x3f);
    int      wnr  = (int)((iss >> 6) & 1);
    uart_puts("\n  [STAGE-2 DATA ABORT from guest]\n");
    uart_puts("    FAR_EL2   (guest VA) = "); uart_put_hex(far);  uart_puts("\n");
    uart_puts("    HPFAR_EL2 (guest IPA)= "); uart_put_hex(ipa);  uart_puts("\n");
    uart_puts("    WnR="); uart_putc(wnr ? 'W' : 'R');
    uart_puts(" DFSC=");  uart_put_hex(dfsc); uart_puts("\n");
    uart_puts("    skipping faulting instruction; guest continues\n");
#endif
    tf->elr_el2 += 4;
}

static void handle_sysreg(trap_frame_t *tf, unsigned long esr) {
    unsigned iss = (unsigned)(esr & 0x1ffffff);
    unsigned rt  = (iss >> 5) & 0x1f;
    unsigned dir = iss & 1;                 /* 0=write, 1=read */
    unsigned op0 = (iss >> 20) & 0x3;
    unsigned op2 = (iss >> 17) & 0x7;
    unsigned op1 = (iss >> 14) & 0x7;
    unsigned crn = (iss >> 10) & 0xf;
    unsigned crm = (iss >>  1) & 0xf;

#if HYP_DEBUG_TRAPS
    uart_puts("  [sysreg trap] ");
    uart_puts(dir ? "MRS " : "MSR ");
    uart_puts("S"); uart_putc((char)('0' + op0));
    uart_puts("_"); uart_putc((char)('0' + op1));
    uart_puts("_C"); uart_putc((char)('0' + crn));
    uart_puts("_C"); uart_putc((char)('0' + crm));
    uart_puts("_"); uart_putc((char)('0' + op2));
    uart_puts("  Rt=x"); uart_put_hex(rt); uart_puts("\n");
#else
    (void)op0; (void)op1; (void)op2; (void)crn; (void)crm;
#endif

    if (dir == 1 && rt != 31) {
        /* Return all-zero for any trapped ID read — safe default for Stage 4. */
        tf->x[rt] = 0;
    }
    tf->elr_el2 += 4;
}

/* ---- top-level dispatch ------------------------------------------------- */

void handle_exception(trap_frame_t *tf, unsigned long kind) {
    unsigned long esr = read_esr_el2();
    unsigned      ec  = (unsigned)((esr >> 26) & 0x3f);

    exit_ring_push(kind, esr, tf->elr_el2, read_far_el2(),
                   current_vcpu()->id);

    /* IRQs (from any EL): hand off to the GIC path. */
    if (kind == VEC_L64_IRQ || kind == VEC_EL2H_IRQ) {
        gic_handle_physical_irq(tf);
        return;
    }

    if (kind == VEC_L64_SYNC) {
        switch (ec) {
        case 0x16: handle_hvc          (tf, esr); return;
        case 0x01: handle_wfi          (tf, esr); return;
        case 0x18: handle_sysreg       (tf, esr); return;
        case 0x24: handle_stage2_abort (tf, esr); return;
        default: break;
        }
    }

    dump_and_halt(tf, kind, esr);
}
