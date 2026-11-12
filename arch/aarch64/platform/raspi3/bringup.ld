/* SPDX-License-Identifier: Apache-2.0 */
OUTPUT_ARCH(aarch64)
ENTRY(_start)
SECTIONS {
    . = 0x80000;
    __text_start = .;
    .text : { KEEP(*(.text.boot)) *(.text.vectors) *(.text .text.*) }
    . = ALIGN(4096);
    __text_end = .;
    __rodata_start = .;
    .rodata : { *(.rodata .rodata.*) }
    . = ALIGN(4096);
    __rodata_end = .;
    .data : { *(.data .data.*) }
    . = ALIGN(4096);
    __bss_start = .;
    .bss (NOLOAD) : { *(.bss .bss.*) *(COMMON) }
    . = ALIGN(4096);
    __bss_end = .;
    __image_end = .;
    ASSERT(__image_end < 0x200000, "bootstrap overlaps EL0 fixture")
    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}
