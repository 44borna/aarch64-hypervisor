/*
 * Linker script for AArch64 QEMU virt machine.
 * RAM starts at 0x40000000; we load 512 KiB in at 0x40080000
 * (the same offset Linux uses for its Image header on arm64).
 */

ENTRY(_start)

SECTIONS
{
    . = 0x40080000;

    .text : ALIGN(4) {
        KEEP(*(.text.boot))
        *(.text .text.*)
    }

    .vectors : ALIGN(2048) {
        __vectors_start = .;
        KEEP(*(.vectors))
        __vectors_end = .;
    }

    .guest_vectors : ALIGN(2048) {
        __guest_vectors_start = .;
        KEEP(*(.guest_vectors))
        __guest_vectors_end = .;
    }

    .rodata : ALIGN(8) {
        *(.rodata .rodata.*)
    }

    .data : ALIGN(8) {
        *(.data .data.*)
    }

    .bss : ALIGN(16) {
        __bss_start = .;
        *(.bss .bss.*)
        *(COMMON)
        __bss_end = .;
    }

    . = ALIGN(16);
    __stack_bottom = .;
    . += 0x4000;          /* 16 KiB hypervisor stack (EL2) */
    __stack_top = .;

    . = ALIGN(16);
    __guest_stack_bottom_a = .;
    . += 0x4000;          /* 16 KiB guest A stack (SP_EL1) */
    __guest_stack_top_a = .;

    . = ALIGN(16);
    __guest_stack_bottom_b = .;
    . += 0x4000;          /* 16 KiB guest B stack (SP_EL1) */
    __guest_stack_top_b = .;

    /DISCARD/ : { *(.comment) *(.note.*) *(.eh_frame) }
}
