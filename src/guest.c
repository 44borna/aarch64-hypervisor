/*
 * Stage 7 guest: one image, two instances.
 *
 * Each vCPU is entered via guest_entry_common(id). The scheduler sets
 * TPIDR_EL1 to point at this guest's private state (allocated in the
 * hypervisor's BSS), so all mutable state is looked up through
 *   mrs x, tpidr_el1
 * and never collides with the other VM. The UART is shared (passthrough
 * via stage-2), but each guest reads a distinct byte from IPA
 * VM_PRIVATE_IPA that's backed by a per-VM host page — proof of
 * stage-2 isolation at a single IPA.
 *
 * IRQ delivery: physical vtimer fires -> EL2 disables CNTV and pends
 * HCR_EL2.VI. Guest takes EL1 IRQ, prints, rearms, HVC_IRQ_DONE to
 * clear VI.
 */
#include "guest.h"
#include "hvc.h"
#include "stage2.h"

typedef struct guest_state {
    unsigned      id;
    unsigned      _pad;
    unsigned long ticks;
    unsigned long tick_period;
} guest_state_t;

/* Legacy stage-4/5 dashboard — unused in Stage 7 but kept exported so
 * the hypervisor symbol table stays stable. */
volatile unsigned long guest_counter;
volatile unsigned long guest_current_el;
volatile unsigned long guest_pfr0;

extern char guest_vector_table_el1[];

/* ---- UART (stage-2 passthrough, shared) -------------------------------- */

#define UART_DR   (*(volatile unsigned *)0x09000000UL)
#define UART_FR   (*(volatile unsigned *)0x09000018UL)

static void guest_putc(char c) {
    while (UART_FR & (1u << 5)) { /* TXFF */ }
    UART_DR = (unsigned)c;
}
static void guest_puts(const char *s) { while (*s) guest_putc(*s++); }
static void guest_put_dec(unsigned long v) {
    char buf[24]; int i = 0;
    if (!v) { guest_putc('0'); return; }
    while (v) { buf[i++] = '0' + (v % 10); v /= 10; }
    while (i--) guest_putc(buf[i]);
}

/* ---- per-guest state pointer ------------------------------------------ */

static inline guest_state_t *gs(void) {
    guest_state_t *p;
    __asm__ volatile ("mrs %0, tpidr_el1" : "=r"(p));
    return p;
}

/* ---- virtual timer ----------------------------------------------------- */

static inline unsigned long read_cntvct(void) {
    unsigned long v; __asm__ volatile ("mrs %0, cntvct_el0" : "=r"(v)); return v;
}
static inline unsigned long read_cntfrq(void) {
    unsigned long v; __asm__ volatile ("mrs %0, cntfrq_el0" : "=r"(v)); return v;
}
static inline void write_cntv_cval(unsigned long v) {
    __asm__ volatile ("msr cntv_cval_el0, %0" :: "r"(v));
}
static inline void write_cntv_ctl(unsigned long v) {
    __asm__ volatile ("msr cntv_ctl_el0,  %0" :: "r"(v));
}

static void timer_rearm(guest_state_t *s) {
    write_cntv_cval(read_cntvct() + s->tick_period);
    write_cntv_ctl(1);
}

/* ---- HVC helper -------------------------------------------------------- */

static inline unsigned long hvc3(unsigned long num,
                                 unsigned long a1,
                                 unsigned long a2) {
    register unsigned long x0 __asm__("x0") = num;
    register unsigned long x1 __asm__("x1") = a1;
    register unsigned long x2 __asm__("x2") = a2;
    __asm__ volatile ("hvc #0" : "+r"(x0) : "r"(x1), "r"(x2) : "memory");
    return x0;
}

/* ---- IRQ handler (called from guest_vectors.S) ------------------------ */

void guest_irq_handler(void);
void guest_irq_handler(void) {
    guest_state_t *s = gs();
    s->ticks++;

    guest_puts("[guest "); guest_putc('A' + (char)s->id); guest_puts("] tick ");
    guest_put_dec(s->ticks); guest_putc('\n');

    timer_rearm(s);
    hvc3(HVC_IRQ_DONE, 0, 0);
}

/* ---- guest entry ------------------------------------------------------- */

void guest_entry_common(unsigned long id);
void guest_entry_common(unsigned long id) {
    guest_state_t *s = gs();
    s->id          = (unsigned)id;
    s->ticks       = 0;
    s->tick_period = read_cntfrq() / 10;    /* 100 ms */

    /* Read our per-VM private page — each VM sees a distinct byte at
     * the same IPA, so printing it proves stage-2 is per-VM. */
    unsigned char priv = *(volatile unsigned char *)VM_PRIVATE_IPA;

    __asm__ volatile ("msr vbar_el1, %0" :: "r"(guest_vector_table_el1));
    __asm__ volatile ("isb");

    guest_puts("[guest "); guest_putc('A' + (char)id);
    guest_puts("] booted, priv=");          guest_putc((char)priv);
    guest_puts(", cntfrq/10="); guest_put_dec(s->tick_period);
    guest_putc('\n');

    timer_rearm(s);
    __asm__ volatile ("msr daifclr, #0x2" ::: "memory");

    for (;;) {
        __asm__ volatile ("yield");
    }
}

/* Stage 3-compat stub so linkers that still reference guest_entry don't
 * break; no longer called. */
void guest_entry(void) { for (;;) __asm__ volatile ("wfe"); }
