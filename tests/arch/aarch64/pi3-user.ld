/* SPDX-License-Identifier: Apache-2.0 */
OUTPUT_ARCH(aarch64)
ENTRY(_start)
PHDRS {
    text PT_LOAD FLAGS(5);
    data PT_LOAD FLAGS(6);
}
SECTIONS {
    . = 0x200000;
    .text : { *(.text) } :text
    .rtmux : { KEEP(*(.rtmux)) } :text
    ASSERT(. <= 0x201000, "user test text exceeds one page")
    . = 0x201000;
    .data : { *(.data) } :data
    ASSERT(. <= 0x202000, "user test data exceeds one page")
    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}
