/*
 * Guest EL1 vector table.
 *
 * Only the "current EL with SP_ELx, IRQ" slot (index 5, EL1h IRQ) is live;
 * every other vector traps to a panic loop we can spot from the hypervisor
 * trace. This keeps the guest-side code tiny — it only needs to service
 * virtual timer IRQs.
 *
 * guest_irq_handler() (in guest.c) does the actual work; this file just
 * saves enough state to call it safely and return with ERET.
 */

#define GFRAME 272

.macro GSAVE
    sub     sp, sp, #GFRAME
    stp     x0,  x1,  [sp, #0x00]
    stp     x2,  x3,  [sp, #0x10]
    stp     x4,  x5,  [sp, #0x20]
    stp     x6,  x7,  [sp, #0x30]
    stp     x8,  x9,  [sp, #0x40]
    stp     x10, x11, [sp, #0x50]
    stp     x12, x13, [sp, #0x60]
    stp     x14, x15, [sp, #0x70]
    stp     x16, x17, [sp, #0x80]
    stp     x18, x19, [sp, #0x90]
    stp     x20, x21, [sp, #0xA0]
    stp     x22, x23, [sp, #0xB0]
    stp     x24, x25, [sp, #0xC0]
    stp     x26, x27, [sp, #0xD0]
    stp     x28, x29, [sp, #0xE0]
    mrs     x9,  elr_el1
    mrs     x10, spsr_el1
    stp     x30, x9,  [sp, #0xF0]
    str     x10, [sp, #0x100]
.endm

.macro GRESTORE
    ldr     x10, [sp, #0x100]
    ldp     x30, x9,  [sp, #0xF0]
    msr     spsr_el1, x10
    msr     elr_el1,  x9
    ldp     x28, x29, [sp, #0xE0]
    ldp     x26, x27, [sp, #0xD0]
    ldp     x24, x25, [sp, #0xC0]
    ldp     x22, x23, [sp, #0xB0]
    ldp     x20, x21, [sp, #0xA0]
    ldp     x18, x19, [sp, #0x90]
    ldp     x16, x17, [sp, #0x80]
    ldp     x14, x15, [sp, #0x70]
    ldp     x12, x13, [sp, #0x60]
    ldp     x10, x11, [sp, #0x50]
    ldp     x8,  x9,  [sp, #0x40]
    ldp     x6,  x7,  [sp, #0x30]
    ldp     x4,  x5,  [sp, #0x20]
    ldp     x2,  x3,  [sp, #0x10]
    ldp     x0,  x1,  [sp, #0x00]
    add     sp, sp, #GFRAME
.endm

.macro GPANIC id
1:  wfi
    b       1b
.endm

    .section .guest_vectors, "ax", @progbits
    .balign  2048
    .globl   guest_vector_table_el1
guest_vector_table_el1:
    /* EL1t — we never use SP_EL0 at EL1 */
    .balign 128;  GPANIC 0
    .balign 128;  GPANIC 1
    .balign 128;  GPANIC 2
    .balign 128;  GPANIC 3

    /* EL1h — the only vectors that matter for our guest */
    .balign 128
    b       .                /* sync — we don't expect any at EL1 */
    .balign 128
    GSAVE
    bl      guest_irq_handler
    GRESTORE
    eret
    .balign 128
    GPANIC 6                 /* fiq */
    .balign 128
    GPANIC 7                 /* serror */

    /* Lower-EL slots — irrelevant from EL1 */
    .balign 128;  GPANIC 8
    .balign 128;  GPANIC 9
    .balign 128;  GPANIC 10
    .balign 128;  GPANIC 11
    .balign 128;  GPANIC 12
    .balign 128;  GPANIC 13
    .balign 128;  GPANIC 14
    .balign 128;  GPANIC 15
