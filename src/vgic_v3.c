/*
 * vgic API stubs for the HVF build (HOST_IF=hvf).
 *
 * Under GIC_VERSION==3 the guest sees a native GICv3 via HVF's hv_gic;
 * there is no software CPU-interface emulator. These stubs exist only
 * so exceptions.c / sched.c / gic_v3.c can link cleanly. Stage 4 of
 * HVF_PLAN.md replaces the call sites with ICH_LRn injection and
 * most of this goes away.
 *
 * Under GIC_VERSION==2 this file compiles to nothing; vgic.c provides
 * the real implementations.
 */

#include "vgic.h"
#include "uart.h"

#if GIC_VERSION == 3

void vgic_init(unsigned id) { (void)id; }

void vgic_inject(unsigned id, unsigned intid) {
    (void)id; (void)intid;
    /* Silent under v3: the HCR_EL2.VI path doesn't apply here. Real
     * injection happens via ICH_LRn_EL2 in Stage 4; call sites will
     * migrate off vgic_inject when that lands. */
}

void vgic_forward_spi(unsigned id, unsigned intid, unsigned phys_iar) {
    (void)id; (void)intid; (void)phys_iar;
}

void vgic_ack_all(unsigned id) { (void)id; }

int vgic_mmio(trap_frame_t *tf, unsigned long ipa, unsigned long iss) {
    (void)tf; (void)ipa; (void)iss;
    /* No guest MMIO trap-and-emulate under v3: HVF + hv_gic provide a
     * real CPU interface visible to the guest. */
    return 0;
}

int vgic_has_pending_phys_iar(unsigned id) { (void)id; return 0; }

#endif /* GIC_VERSION == 3 */
