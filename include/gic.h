#pragma once

/* QEMU virt, GICv2 */
#define GICD_BASE_PA      0x08000000UL
#define GICC_BASE_PA      0x08010000UL
#define GICH_BASE_PA      0x08030000UL
#define GICV_BASE_PA      0x08040000UL

/* Address the guest thinks its "GICC" lives at — stage-2 sends it to GICV */
#define GUEST_GICC_IPA    0x08010000UL

#define IRQ_VTIMER        27   /* PPI 11 = INTID 27, guest virtual timer */
#define IRQ_HTIMER        26   /* PPI 10 = INTID 26, EL2 hypervisor timer (CNTHP) */
#define IRQ_PL011         33   /* SPI 1  = INTID 33, UART RX/TX on QEMU virt   */

/* GICD */
#define GICD_CTLR         0x000
#define GICD_ISENABLER    0x100     /* + n*4 */
#define GICD_IPRIORITYR   0x400     /* byte per IRQ */
#define GICD_IGROUPR      0x080     /* + n*4 */

/* GICC (physical, used by EL2) */
#define GICC_CTLR         0x000
#define GICC_PMR          0x004
#define GICC_IAR          0x00C
#define GICC_EOIR         0x010

/* GICH (EL2 hypervisor interface) */
#define GICH_HCR          0x000
#define GICH_VTR          0x004
#define GICH_LR0          0x100     /* + n*4 */

/* Spurious interrupt id */
#define INTID_SPURIOUS    1023

struct trap_frame;

void gic_init_el2(void);
void gic_handle_physical_irq(struct trap_frame *tf);
