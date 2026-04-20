/*
 * Minimal EL1 guest used by the HVF / GICv3 build for Stage 4.
 *
 * Runs at EL1h with MMU off. Sets up the GICv3 CPU interface via
 * system registers, unmasks IRQs, and WFEs. When our EL2 injects an
 * IRQ via ICH_LRn_EL2, the CPU's virtual CPU interface delivers it
 * through VBAR_EL1; the handler reads ICC_IAR1_EL1, prints a marker
 * over PL011, EOIs via ICC_EOIR1_EL1, and halts.
 *
 * PL011 accesses fault at stage-2 (HVF) and are emulated by the host
 * launcher — same path that produces all of our hypervisor output.
 */

#if GIC_VERSION == 3

    .section .text, "ax", @progbits

    .globl guest_v3_start
guest_v3_start:
    /* Set our own SP_EL1 — the EL2 code didn't initialize it, and
     * our IRQ handler immediately does stp [sp, #0]. */
    adr     x0, guest_v3_stack_top
    mov     sp, x0

    /* VBAR_EL1 -> our vector table. */
    adr     x0, guest_v3_vectors
    msr     vbar_el1, x0
    isb

    /* Enable the GICv3 system-register interface at EL1:
     *   ICC_SRE_EL1.SRE = 1 (S3_0_C12_C12_5). */
    mov     x0, #1
    msr     S3_0_C12_C12_5, x0
    isb

    /* Priority mask: accept everything. ICC_PMR_EL1 = S3_0_C4_C6_0. */
    mov     x0, #0xFF
    msr     S3_0_C4_C6_0, x0

    /* Enable Group 1 IRQs. ICC_IGRPEN1_EL1 = S3_0_C12_C12_7. */
    mov     x0, #1
    msr     S3_0_C12_C12_7, x0
    isb

    /* Print "guest-el1 up\n". */
    bl      guest_v3_banner

    /* Unmask DAIF.I. */
    msr     daifclr, #0x2

    /* Wait for the IRQ the hypervisor injected via ICH_LR. WFE
     * blocks until an event (including our virtual IRQ). */
1:  wfe
    b       1b


/* --- helpers: write a byte / string to PL011 DR (0x09000000) --- */

/* X0 = char, trashes X1 */
guest_v3_putc:
    mov     x1, #0x09000000
    str     w0, [x1]
    ret

/* X0 = zero-terminated string, trashes X1..X3 and X30 */
guest_v3_puts:
    mov     x2, x0                /* save string ptr */
    mov     x3, x30               /* save link register */
    mov     x1, #0x09000000
1:  ldrb    w0, [x2], #1
    cbz     w0, 2f
    str     w0, [x1]
    b       1b
2:  mov     x30, x3
    ret

guest_v3_banner:
    mov     x29, x30
    adr     x0, .Lguest_banner
    bl      guest_v3_puts
    mov     x30, x29
    ret

.Lguest_banner:
    .asciz "[guest-el1] up, waiting for injected IRQ\n"

    .balign 4
/* --- IRQ handler: read IAR, print marker, EOI, halt if real IRQ --- */
guest_v3_irq_entry:
    sub     sp, sp, #64
    stp     x0, x1,   [sp, #0]
    stp     x2, x3,   [sp, #16]
    stp     x29, x30, [sp, #32]

    /* Read ICC_IAR1_EL1 (S3_0_C12_C12_0). Under HCR_EL2.IMO=1 this
     * is virtualized to ICV_IAR1_EL1, which returns the pending
     * INTID from our populated ICH_LR, or 1023 if spurious. */
    mrs     x19, S3_0_C12_C12_0
    and     x20, x19, #0xFFFFFF

    /* 1023 = spurious: silently EOI + ERET back to WFE. */
    mov     x21, #1023
    cmp     x20, x21
    b.eq    1f

    adr     x0, .Lguest_got_irq
    bl      guest_v3_puts
    and     x20, x19, #0xFF
    lsr     x0, x20, #4
    bl      guest_v3_hex_nibble
    and     x0, x20, #0xF
    bl      guest_v3_hex_nibble
    mov     x0, #'\n'
    bl      guest_v3_putc

    /* EOI the real IRQ, then halt — Stage 4 done. */
    msr     S3_0_C12_C12_1, x19
    isb
    adr     x0, .Lguest_done
    bl      guest_v3_puts
    msr     daifset, #0xF             /* mask everything */
2:  wfe
    b       2b

1:  /* Spurious path: EOI and return. */
    msr     S3_0_C12_C12_1, x19
    isb
    ldp     x29, x30, [sp, #32]
    ldp     x2, x3,   [sp, #16]
    ldp     x0, x1,   [sp, #0]
    add     sp, sp, #64
    eret

/* x0 = nibble (0..15) -> PL011 */
guest_v3_hex_nibble:
    mov     x2, x30
    cmp     x0, #9
    b.hi    1f
    add     x0, x0, #'0'
    b       2f
1:  add     x0, x0, #('a' - 10)
2:  mov     x1, #0x09000000
    str     w0, [x1]
    mov     x30, x2
    ret

.Lguest_got_irq:
    .asciz "[guest-el1] IAR=0x"
    .balign 4
.Lguest_done:
    .asciz "[guest-el1] IRQ handled, Stage 4 complete\n"
    .balign 4


/* --- EL1 vector table (2 KiB aligned) --- */

/* Macro: record which slot fired in w21, branch to guest_v3_bad. */
.macro BAD_SLOT n
    mov     w21, #\n
    b       guest_v3_bad
.endm

.balign 2048
    .globl guest_v3_vectors
guest_v3_vectors:

    .balign 128
    BAD_SLOT 0                      /* EL1t sync */
    .balign 128
    BAD_SLOT 1                      /* EL1t irq  */
    .balign 128
    BAD_SLOT 2                      /* EL1t fiq  */
    .balign 128
    BAD_SLOT 3                      /* EL1t serr */

    .balign 128
    BAD_SLOT 4                      /* EL1h sync */
    .balign 128
    b       guest_v3_irq_entry      /* EL1h irq  (slot 5) */
    .balign 128
    BAD_SLOT 6                      /* EL1h fiq  */
    .balign 128
    BAD_SLOT 7                      /* EL1h serr */

    .balign 128
    BAD_SLOT 8
    .balign 128
    BAD_SLOT 9
    .balign 128
    BAD_SLOT 10
    .balign 128
    BAD_SLOT 11

    .balign 128
    BAD_SLOT 12
    .balign 128
    BAD_SLOT 13
    .balign 128
    BAD_SLOT 14
    .balign 128
    BAD_SLOT 15

guest_v3_bad:
    /* w21 = slot index. Print header + ESR_EL1 + slot + ELR_EL1. */
    adr     x0, .Lguest_bad_msg
    bl      guest_v3_puts
    mov     x0, x21
    lsr     x0, x0, #4
    and     x0, x0, #0xF
    bl      guest_v3_hex_nibble
    mov     x0, x21
    and     x0, x0, #0xF
    bl      guest_v3_hex_nibble
    adr     x0, .Lguest_bad_esr
    bl      guest_v3_puts
    mrs     x22, esr_el1
    lsr     x0, x22, #28
    and     x0, x0, #0xF
    bl      guest_v3_hex_nibble
    lsr     x0, x22, #24
    and     x0, x0, #0xF
    bl      guest_v3_hex_nibble
    lsr     x0, x22, #20
    and     x0, x0, #0xF
    bl      guest_v3_hex_nibble
    lsr     x0, x22, #16
    and     x0, x0, #0xF
    bl      guest_v3_hex_nibble
    lsr     x0, x22, #12
    and     x0, x0, #0xF
    bl      guest_v3_hex_nibble
    lsr     x0, x22, #8
    and     x0, x0, #0xF
    bl      guest_v3_hex_nibble
    lsr     x0, x22, #4
    and     x0, x0, #0xF
    bl      guest_v3_hex_nibble
    and     x0, x22, #0xF
    bl      guest_v3_hex_nibble
    mov     x0, #'\n'
    bl      guest_v3_putc
1:  wfe
    b       1b

.Lguest_bad_msg:
    .asciz "[guest-el1] bad-slot 0x"
    .balign 4
.Lguest_bad_esr:
    .asciz " ESR_EL1=0x"
    .balign 4

    /* Stack for the EL1 guest. 4 KiB is plenty — nested exception
     * chains are short. */
    .balign 16
    .globl guest_v3_stack_bottom
guest_v3_stack_bottom:
    .skip 4096
    .globl guest_v3_stack_top
guest_v3_stack_top:

#endif /* GIC_VERSION == 3 */
