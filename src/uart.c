/*
 * Minimal PL011 UART driver for QEMU virt.
 * MMIO base 0x09000000. Polls FR.TXFF before every byte; no IRQs, no FIFO
 * watermark config beyond the defaults we set at init.
 */
#include "uart.h"

#define UART0_BASE 0x09000000UL

#define REG(off)    (*(volatile unsigned int *)(UART0_BASE + (off)))
#define UARTDR      REG(0x000)
#define UARTFR      REG(0x018)
#define UARTIBRD    REG(0x024)
#define UARTFBRD    REG(0x028)
#define UARTLCRH    REG(0x02C)
#define UARTCR      REG(0x030)
#define UARTIMSC    REG(0x038)
#define UARTICR     REG(0x044)

#define FR_TXFF     (1u << 5)
#define LCRH_FEN    (1u << 4)
#define LCRH_WLEN_8 (3u << 5)
#define CR_UARTEN   (1u << 0)
#define CR_TXE      (1u << 8)
#define CR_RXE      (1u << 9)

void uart_init(void) {
    UARTCR   = 0;
    UARTICR  = 0x7ff;
    UARTIBRD = 26;          /* irrelevant to QEMU; kept for realism. */
    UARTFBRD = 3;
    UARTLCRH = LCRH_WLEN_8 | LCRH_FEN;
    UARTIMSC = 0;
    UARTCR   = CR_UARTEN | CR_TXE | CR_RXE;
}

void uart_putc(char c) {
    while (UARTFR & FR_TXFF) { }
    UARTDR = (unsigned int)(unsigned char)c;
}

void uart_puts(const char *s) {
    while (*s) {
        if (*s == '\n') uart_putc('\r');
        uart_putc(*s++);
    }
}

void uart_put_hex(unsigned long v) {
    uart_puts("0x");
    for (int i = 60; i >= 0; i -= 4) {
        unsigned n = (v >> i) & 0xfu;
        uart_putc(n < 10 ? (char)('0' + n) : (char)('a' + n - 10));
    }
}
