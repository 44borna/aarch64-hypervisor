/*
 * Software GICC for guests that won't cooperate with our HCR_EL2.VI
 * shortcut (read: Linux). Pending is a 64-bit bitmap over INTIDs 0..63
 * (SGIs + PPIs + low SPIs including PL011 at 33); active is a single
 * slot (no nested preemption in a UP guest). GICD stays passthrough;
 * only the CPU interface is emulated here.
 *
 * Forwarded SPIs (vgic_forward_spi) defer their physical EOI until
 * the guest completes its virtual EOI cycle — otherwise we'd ack
 * PL011 at EL2 before Linux drained the FIFO, the line would stay
 * high, and we'd re-trap instantly in a storm.
 */
#include "vgic.h"
#include "vcpu.h"
#include "uart.h"
#include "gic.h"

/* Linux's GIC driver clears GICD_ISENABLER bits for PPIs it doesn't
 * know about, and our EL2 scheduler tick (PPI 26, CNTHP) isn't in the
 * guest DT. Re-assert it whenever we see activity at the CPU interface
 * so the scheduler doesn't silently freeze after the first slice. */
static inline void reassert_htimer_ppi(void) {
    *(volatile unsigned *)(GICD_BASE_PA + GICD_ISENABLER) =
        (1u << IRQ_HTIMER);
}

/* Linux's pl011 driver enables UART011_IMSC.{RXIM|RTIM} in startup()
 * and clears them in shutdown(). Whatever boot sequence we're running
 * (rdinit=/init with a minimal overlay) doesn't hold the tty open
 * consistently, so the mask gets torn down and RX SPIs stop firing.
 * Re-assert IMSC + the GIC distributor bits on every guest GIC touch
 * — cheap, idempotent, and means Linux never loses the console-RX
 * path even if it thinks it closed the tty. */
static inline void reassert_pl011_rx(void) {
    /* PL011 IMSC: RTIM (bit 6) + RXIM (bit 4) */
    *(volatile unsigned *)(0x09000000UL + 0x38) = 0x50;
    /* GICD ISENABLER1 bit 1 = SPI 33. ITARGETSR[33] = CPU 0. */
    *(volatile unsigned *)(GICD_BASE_PA + 0x104) = (1u << (IRQ_PL011 - 32));
    *(volatile unsigned char *)(GICD_BASE_PA + 0x800 + IRQ_PL011) = 0x01;
    *(volatile unsigned char *)(GICD_BASE_PA + 0x400 + IRQ_PL011) = 0xA0;
}

#define NO_INTID     0x3FFu

/* Field ordering matches the Device-nGnRnE alignment constraint at
 * EL2 (SCTLR_EL2.M=0): gcc pairs adjacent 4-byte stores into 8-byte
 * stores, so each pair must land on an 8-byte boundary. struct size
 * must also be a multiple of 8 so array element N starts 8-aligned. */
typedef struct vgic {
    unsigned long pending_mask;  /*  0 — one bit per INTID (0..63) */
    unsigned      ctlr;          /*  8 */
    unsigned      pmr;           /* 12 */
    unsigned      bpr;           /* 16 */
    unsigned      active;        /* 20 */
    unsigned      phys_iar;      /* 24 — deferred IAR for fwd'd SPI */
    unsigned      has_phys_iar;  /* 28 */
} vgic_t;

static vgic_t vgics[4];

/* ---- HCR_EL2.VI helpers ------------------------------------------------ */

static inline void set_vi(int on) {
    unsigned long hcr;
    __asm__ volatile ("mrs %0, hcr_el2" : "=r"(hcr));
    if (on) hcr |=  (1UL << 7);
    else    hcr &= ~(1UL << 7);
    __asm__ volatile ("msr hcr_el2, %0" :: "r"(hcr));
}

/* Lowest set bit as INTID, or NO_INTID if empty. Lower INTID = higher
 * priority under GICv2 tie-break rules, matching the real hardware. */
static inline unsigned lowest_pending(unsigned long mask) {
    if (!mask) return NO_INTID;
    return (unsigned)__builtin_ctzl(mask);
}

/* ---- public entry points ---------------------------------------------- */

void vgic_init(unsigned id) {
    vgic_t *v = &vgics[id];
    v->pending_mask = 0;
    v->ctlr         = 0;
    v->pmr          = 0;
    v->bpr          = 0;
    v->active       = NO_INTID;
    v->phys_iar     = 0;
    v->has_phys_iar = 0;
}

void vgic_inject(unsigned id, unsigned intid) {
    vgic_t *v = &vgics[id];
    if (intid >= 64) return;        /* outside our bitmap window */
    v->pending_mask |= (1UL << intid);
    set_vi(1);
}

void vgic_forward_spi(unsigned id, unsigned intid, unsigned phys_iar) {
    vgic_t *v = &vgics[id];
    v->phys_iar     = phys_iar;
    v->has_phys_iar = 1;
    vgic_inject(id, intid);
}

int vgic_has_pending_phys_iar(unsigned id) {
    return vgics[id].has_phys_iar != 0;
}

void vgic_ack_all(unsigned id) {
    vgic_t *v = &vgics[id];
    v->pending_mask = 0;
    v->active       = NO_INTID;
    /* If a physical SPI was pending deferred-EOI when we got here
     * (HVC_IRQ_DONE path from the bare-metal guest), complete its
     * physical cycle so we don't leave the real GIC with a stuck
     * active entry. */
    if (v->has_phys_iar) {
        *(volatile unsigned *)(GICC_BASE_PA + 0x10 /*GICC_EOIR*/) = v->phys_iar;
        v->has_phys_iar = 0;
    }
    set_vi(0);
}

/* ---- MMIO emulation ---------------------------------------------------- */

static unsigned read_reg(vgic_t *v, unsigned off) {
    switch (off) {
    case 0x00: return v->ctlr;
    case 0x04: return v->pmr;
    case 0x08: return v->bpr;
    case 0x0C: {                     /* IAR — pops pending into active */
        unsigned intid = lowest_pending(v->pending_mask);
        if (intid == NO_INTID) return 1023;
        v->pending_mask &= ~(1UL << intid);
        v->active = intid;
        /* Clear VI now; if more pending remain we'll re-raise it below. */
        set_vi(v->pending_mask != 0);
        return intid;
    }
    case 0x14: return 0;             /* RPR  — no priority tracking */
    case 0x18: {                     /* HPPIR */
        unsigned intid = lowest_pending(v->pending_mask);
        return (intid == NO_INTID) ? 1023 : intid;
    }
    case 0xFC: return 0x0200143B;    /* IIDR — mimic GICv2 */
    default:   return 0;
    }
}

static void write_reg(vgic_t *v, unsigned off, unsigned val) {
    switch (off) {
    case 0x00: v->ctlr = val & 0x1F; break;
    case 0x04: v->pmr  = val & 0xFF; break;
    case 0x08: v->bpr  = val & 0x07; break;
    case 0x10: {                     /* EOIR — deactivate */
        unsigned intid = val & 0x3FF;
        if (intid == v->active) v->active = NO_INTID;
        /* If this INTID came from a forwarded SPI, complete the
         * physical cycle now. Linux has already drained the device
         * (PL011 ICR write happened in its ISR before EOIR), so the
         * IRQ line is low and the next physical SPI can latch. */
        if (v->has_phys_iar && (v->phys_iar & 0x3FF) == intid) {
            *(volatile unsigned *)(GICC_BASE_PA + 0x10) = v->phys_iar;
            v->has_phys_iar = 0;
        }
        break;
    }
    default: break;
    }
}

int vgic_mmio(trap_frame_t *tf, unsigned long ipa, unsigned long iss) {
    int isv = (int)((iss >> 24) & 1);
    if (!isv) {
        uart_puts("  [vgic] abort w/o ISV at IPA="); uart_put_hex(ipa);
        uart_puts("\n");
        return 0;
    }

    unsigned sas = (unsigned)((iss >> 22) & 3);
    unsigned srt = (unsigned)((iss >> 16) & 0x1F);
    int      wnr = (int)((iss >> 6) & 1);

    vcpu_t *cur = current_vcpu();
    vgic_t *v   = &vgics[cur->id];
    unsigned val;

    if (wnr) val = (srt == 31) ? 0u : (unsigned)tf->x[srt];
    else     val = 0;

    if (ipa >= GICD_EMU_BASE && ipa < GICD_EMU_LIMIT) {
        /* GICD: forward to real distributor. Honor the guest's access
         * size — Linux writes ITARGETSR/IPRIORITYR as bytes, and an
         * unaligned word access into the real GICD faults at EL2. */
        unsigned long addr = GICD_BASE_PA + (ipa - GICD_EMU_BASE);
        switch (sas) {
        case 0:
            if (wnr) *(volatile unsigned char  *)addr = (unsigned char)val;
            else     val = *(volatile unsigned char  *)addr;
            break;
        case 1:
            if (wnr) *(volatile unsigned short *)addr = (unsigned short)val;
            else     val = *(volatile unsigned short *)addr;
            break;
        default:
            if (wnr) *(volatile unsigned *)addr = val;
            else     val = *(volatile unsigned *)addr;
            break;
        }
    } else {
        /* GICC: pure emulation. */
        unsigned off = (unsigned)(ipa - GICC_EMU_BASE);
        if (wnr) write_reg(v, off, val);
        else     val = read_reg(v, off);
    }

    if (!wnr && srt != 31) tf->x[srt] = val;
    reassert_htimer_ppi();
    reassert_pl011_rx();
    return 1;
}
