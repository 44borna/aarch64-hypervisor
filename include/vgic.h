#pragma once

/*
 * Minimal software GICC emulator. QEMU TCG's GICv2 LR path does not
 * drive GICV_IAR correctly (see memory/qemu_tcg_gicv2_virt.md), so we
 * unmap GICC in stage-2 and trap every guest access here. GICD is
 * still passthrough; only the CPU interface is emulated.
 *
 * Model: pending is a 64-bit bitmap over INTIDs 0..63 (covers all
 * SGIs + PPIs + the first SPIs including PL011 at 33), active is a
 * single slot (uniprocessor Linux doesn't nest). PPIs are handled
 * inline at EL2 (we ack the physical IAR immediately because we
 * control the source). SPIs are forwarded via vgic_forward_spi(),
 * which defers the physical EOI until the guest writes its emulated
 * GICC_EOIR — essential for level-triggered devices like PL011 where
 * only the guest driver can lower the IRQ line.
 */
#include "exceptions.h"

#define GICC_EMU_BASE   0x08010000UL
#define GICC_EMU_LIMIT  0x08020000UL   /* first 64 KiB of the "GICC" window */
#define GICD_EMU_BASE   0x08000000UL
#define GICD_EMU_LIMIT  0x08010000UL

void vgic_init(unsigned vcpu_id);
void vgic_inject(unsigned vcpu_id, unsigned intid);

/* Forward a physical SPI into the guest with deferred physical EOI.
 * phys_iar is what we got from the real GICC_IAR. The physical IRQ
 * stays "active" at the CPU interface until the guest writes its
 * emulated GICC_EOIR, at which point vgic_mmio completes the cycle
 * with a write to the real GICC_EOIR. */
void vgic_forward_spi(unsigned vcpu_id, unsigned intid, unsigned phys_iar);

/* Called from stage-2 abort handler when IPA falls in the GICC window.
 * Returns 1 if handled (ELR should be advanced by caller), 0 otherwise. */
int  vgic_mmio(trap_frame_t *tf, unsigned long ipa,
               unsigned long esr_iss);

/* Bare-metal guest path: HVC_IRQ_DONE also clears vGIC pending/active so
 * Stage 7 keeps working without touching GICC_EOIR. */
void vgic_ack_all(unsigned vcpu_id);

/* True if the vCPU already has a forwarded SPI awaiting guest EOI;
 * used by handle_wfi to skip re-IAR while one's in flight. */
int  vgic_has_pending_phys_iar(unsigned vcpu_id);
