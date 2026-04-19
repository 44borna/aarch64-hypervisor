/*
 * AArch64 EL2 exception vector table.
 *
 * 16 entries of 128 bytes each, whole table aligned to 2 KB.
 * Installed into VBAR_EL2 by boot.S.
 *
 * Each entry saves the full trap_frame (see include/exceptions.h),
 * tail-calls the C dispatcher handle_exception(tf, kind), and — if the
 * handler returns — restores and ERETs. The Stage-2 handler always
 * halts, so the return path is defensive.
 */

#define FRAME_SIZE 272

.macro SAVE_CTX
    sub     sp, sp, #FRAME_SIZE
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
    mrs     x9,  elr_el2
    mrs     x10, spsr_el2
    stp     x30, x9,  [sp, #0xF0]
    str     x10, [sp, #0x100]
.endm

.macro RESTORE_CTX
    ldr     x10, [sp, #0x100]
    ldp     x30, x9,  [sp, #0xF0]
    msr     spsr_el2, x10
    msr     elr_el2,  x9
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
    add     sp, sp, #FRAME_SIZE
.endm

.macro VEC_ENTRY kind
    SAVE_CTX
    mov     x0, sp
    mov     x1, #\kind
    b       vector_tail
.endm

    .section .vectors, "ax", @progbits
    .balign 2048
    .globl  vector_table_el2
vector_table_el2:
    /* Current EL with SP_EL0 (EL2t) — we never run in SP_EL0 mode */
    .balign 128;  VEC_ENTRY 0      /* sync    */
    .balign 128;  VEC_ENTRY 1      /* irq     */
    .balign 128;  VEC_ENTRY 2      /* fiq     */
    .balign 128;  VEC_ENTRY 3      /* serror  */

    /* Current EL with SP_ELx (EL2h) — faults we take ourselves */
    .balign 128;  VEC_ENTRY 4
    .balign 128;  VEC_ENTRY 5
    .balign 128;  VEC_ENTRY 6
    .balign 128;  VEC_ENTRY 7

    /* Lower EL, AArch64 — exceptions from a guest at EL1 (Stage 3+) */
    .balign 128;  VEC_ENTRY 8
    .balign 128;  VEC_ENTRY 9
    .balign 128;  VEC_ENTRY 10
    .balign 128;  VEC_ENTRY 11

    /* Lower EL, AArch32 — unused; we don't run AArch32 guests */
    .balign 128;  VEC_ENTRY 12
    .balign 128;  VEC_ENTRY 13
    .balign 128;  VEC_ENTRY 14
    .balign 128;  VEC_ENTRY 15

    /* Shared tail: call C, then (defensively) restore and ERET */
    .balign 16
vector_tail:
    bl      handle_exception
    RESTORE_CTX
    eret
