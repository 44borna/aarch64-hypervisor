/*
 * Minimal AArch64 boot stub.
 * QEMU virt -kernel entry: x0 = DTB phys addr, EL = 2 (with -machine virt,virtualization=on).
 * All other secondary CPUs are parked here too; only CPU 0 proceeds.
 */

    .section .text.boot, "ax", @progbits
    .globl _start
_start:
    mrs     x1, mpidr_el1
    and     x1, x1, #0xff
    cbnz    x1, park

    ldr     x1, =__stack_top
    mov     sp, x1

    ldr     x1, =vector_table_el2
    msr     vbar_el2, x1
    isb

    ldr     x1, =__bss_start
    ldr     x2, =__bss_end
1:  cmp     x1, x2
    b.hs    2f
    str     xzr, [x1], #8
    b       1b
2:
    bl      kmain

park:
    wfe
    b       park
