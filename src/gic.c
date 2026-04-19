/*
 * GICv2 setup + virtual-IRQ injection path.
 *
 * QEMU virt with gic-version=2:
 *   GICD at 0x08000000, GICC at 0x08010000,
 *   GICH at 0x08030000, GICV at 0x08040000.
 *
 * PPIs we enable at EL2:
 *   - IRQ_HTIMER (26, CNTHP)  -> scheduler tick
 *   - IRQ_VTIMER (27, CNTV)   -> forwarded to current guest as VIRQ
 *
 * For injecting a virtual IRQ into the guest we use HCR_EL2.VI (bit 7)
 * to directly pend a VIRQ to the current EL1. Why not GICH_LRn with
 * HW=1? QEMU TCG's GICv2 virtualisation does not reliably drive the
 * virtual CPU interface from the list registers — LRn will latch a
 * pending entry but GICV_IAR stays spurious. HCR_EL2.VI works
 * everywhere and is enough to prove the Stage 6/7 exit criteria at the
 * cost of a single hypercall on guest EOI.
 */
#include "gic.h"
#include "uart.h"
#include "exceptions.h"
#include "vcpu.h"
#include "vgic.h"

/* TCG/GICv2 build only. Under HOST_IF=hvf (GIC_VERSION==3) this whole
 * file compiles to nothing; the v3 counterpart lives in gic_v3.c. */
#if GIC_VERSION == 2

/* Keep PL011 RXIM + GIC SPI 33 hot on every CNTHP heartbeat — Linux's
 * pl011 driver resets these shortly after boot. Mirrors the helper in
 * vgic.c; duplicated here so the heartbeat path doesn't need to pull
 * in vgic internals. */
static inline void pl011_rx_reassert(void) {
    *(volatile unsigned *)(0x09000000UL + 0x38) = 0x50;   /* IMSC RXIM|RTIM */
    *(volatile unsigned *)(GICD_BASE_PA + 0x104) = (1u << (IRQ_PL011 - 32));
    *(volatile unsigned char *)(GICD_BASE_PA + 0x800 + IRQ_PL011) = 0x01;
    *(volatile unsigned char *)(GICD_BASE_PA + 0x400 + IRQ_PL011) = 0xA0;
}

extern void sched_rearm_tick(void);

static inline void     mmio_w(unsigned long base, unsigned off, unsigned v) {
    *(volatile unsigned *)(base + off) = v;
}
static inline unsigned mmio_r(unsigned long base, unsigned off) {
    return *(volatile unsigned *)(base + off);
}

void gic_init_el2(void) {
    /* Distributor: configure while disabled, then enable. */
    mmio_w(GICD_BASE_PA, GICD_CTLR, 0x0);
    mmio_w(GICD_BASE_PA, GICD_IGROUPR + 0, 0xffffffff);   /* all SGI/PPI group 1 */
    mmio_w(GICD_BASE_PA, GICD_IGROUPR + 4, 0xffffffff);   /* SPIs 32..63 group 1 */
    /* SGIs default to priority 0 (highest) and can block our timer PPIs at
     * the CPU interface. Clear any stale SGI pending state and drop PPI 27
     * to priority 0 so it always wins arbitration against an SGI that some
     * Linux boot path re-pends before we ever route IRQs. */
    mmio_w(GICD_BASE_PA, 0x280 /* GICD_ICPENDR0 */, 0x0000FFFFu);
    *(volatile unsigned char *)(GICD_BASE_PA + GICD_IPRIORITYR + IRQ_VTIMER) = 0x00;
    *(volatile unsigned char *)(GICD_BASE_PA + GICD_IPRIORITYR + IRQ_HTIMER) = 0x00;
    /* Wire up PL011 (SPI 33) to CPU 0 with a mid priority so Linux's
     * later ITARGETSR/IPRIORITYR/ISENABLER writes are additive, not
     * the thing that first brings the line up. */
    *(volatile unsigned char *)(GICD_BASE_PA + GICD_IPRIORITYR + IRQ_PL011) = 0xA0;
    *(volatile unsigned char *)(GICD_BASE_PA + 0x800 /*ITARGETSR*/ + IRQ_PL011) = 0x01;
    mmio_w(GICD_BASE_PA, GICD_ISENABLER + 0,
           (1u << IRQ_VTIMER) | (1u << IRQ_HTIMER));
    mmio_w(GICD_BASE_PA, GICD_ISENABLER + 4, (1u << (IRQ_PL011 - 32)));
    mmio_w(GICD_BASE_PA, GICD_CTLR, 0x3);

    /* Physical CPU interface: enable both groups, allow NS ack of Grp0
     * (QEMU's GICv2 tags our PPIs as Grp0 regardless of IGROUPR). */
    mmio_w(GICC_BASE_PA, GICC_PMR,  0xff);
    mmio_w(GICC_BASE_PA, GICC_CTLR, 0x3 | (1u << 2));

    /* Force-enable PL011 RX interrupts. Linux's pl011_startup() only
     * runs when a userspace process opens /dev/ttyAMA0 via the full
     * tty driver path, and with our rdinit=/init shell that doesn't
     * reliably happen. Setting UART011_IMSC.{RXIM|RTIM} here lets
     * QEMU raise SPI 33 on incoming bytes; Linux's IRQ handler reads
     * DR and clears ICR as usual — it doesn't care who set IMSC. */
    *(volatile unsigned *)(0x09000000UL + 0x38 /*IMSC*/) = 0x50;  /* RXIM | RTIM */

    uart_puts("[gic] GICv2 up; PPIs 26 (htimer) + 27 (vtimer), SPI 33 (pl011)\n");
}

static unsigned long htimer_ticks, vtimer_ticks, other_ticks;

void gic_handle_physical_irq(struct trap_frame *tf) {
    unsigned iar   = mmio_r(GICC_BASE_PA, GICC_IAR);
    unsigned intid = iar & 0x3ff;

    if (intid == INTID_SPURIOUS) return;

    if (intid == IRQ_HTIMER) {
        htimer_ticks++;
        /* Disable CNTHP so it stops pinging while we schedule /
         * heartbeat-reassert, then re-arm and EOI. */
        __asm__ volatile ("msr cnthp_ctl_el2, xzr" ::: "memory");
        pl011_rx_reassert();
        sched_tick(tf);
        sched_rearm_tick();
        mmio_w(GICC_BASE_PA, GICC_EOIR, iar);
        return;
    }

    if (intid == IRQ_VTIMER) {
        vtimer_ticks++;
        /* Disable the physical vtimer so it stops re-firing until the
         * guest re-arms it, then pend INTID 27 to the current vCPU
         * through the vGIC (sets HCR_EL2.VI). Guest acks via GICC_EOIR
         * (Linux) or HVC_IRQ_DONE (bare-metal). */
        __asm__ volatile ("msr cntv_ctl_el0, xzr" ::: "memory");
        vgic_inject(current_vcpu()->id, IRQ_VTIMER);
        mmio_w(GICC_BASE_PA, GICC_EOIR, iar);
        return;
    }

    if (intid == IRQ_PL011) {
        uart_puts("[gic] pl011 rx -> vcpu"); uart_put_hex(current_vcpu()->id);
        uart_puts("\n");
        /* Forward the PL011 RX IRQ into the guest. Do NOT ack the
         * physical GIC here — the PL011 line stays high until Linux
         * reads DR and writes ICR from its ISR. If we EOI at EL2 now,
         * the GIC re-pends immediately and we storm. Keep the physical
         * IAR active; vgic_mmio completes the cycle on guest EOIR. */
        vgic_forward_spi(current_vcpu()->id, IRQ_PL011, iar);
        return;
    }

    other_ticks++;
    mmio_w(GICC_BASE_PA, GICC_EOIR, iar);
    uart_puts("[gic] unexpected phys IRQ "); uart_put_hex(intid); uart_puts("\n");
}

#endif /* GIC_VERSION == 2 */
