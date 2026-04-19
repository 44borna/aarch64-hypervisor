/*
 * GICv3 implementation for the HVF build (HOST_IF=hvf).
 *
 * Stage 1 stub: links cleanly, prints a marker, halts quietly on the
 * first IRQ. Real init lives in Stage 3 of HVF_PLAN.md.
 *
 * Under GIC_VERSION==2 this file compiles to nothing; gic.c provides
 * the equivalent functions.
 */

#include "gic.h"
#include "uart.h"
#include "exceptions.h"

#if GIC_VERSION == 3

void gic_init_el2(void) {
    uart_puts("[gic-v3] stub: gic_init_el2 not implemented yet (HVF_PLAN Stage 3)\n");
}

void gic_handle_physical_irq(struct trap_frame *tf) {
    (void)tf;
    /* No IRQs should reach us yet; if one does, say so and keep going. */
    uart_puts("[gic-v3] stub: IRQ ignored\n");
}

#endif /* GIC_VERSION == 3 */
