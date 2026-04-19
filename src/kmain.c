#include "uart.h"
#include "gic.h"
#include "vcpu.h"

static inline unsigned long read_current_el(void) {
    unsigned long v;
    __asm__ volatile ("mrs %0, CurrentEL" : "=r"(v));
    return v;
}

void kmain(void) {
    uart_init();

    unsigned long el = (read_current_el() >> 2) & 0x3UL;
    uart_puts("Hello from EL");
    uart_putc((char)('0' + el));
    uart_puts("\n");

    gic_init_el2();
    sched_init();
    sched_start();
}
