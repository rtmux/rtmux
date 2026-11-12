/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2019 Western Digital Corporation or its affiliates.
 *
 * Authors:
 *   Anup Patel <anup.patel@wdc.com>
 */

OUTPUT_ARCH(aarch64)
ENTRY(_start)

KERNEL_VIRT_BASE = 0xFFFFFFC000000000;

SECTIONS
{
	. = 0xFFFFFFC000080000;

	. = ALIGN(0x1000);

	/* Beginning of the code section */

	.text : AT(ADDR(.text) - KERNEL_VIRT_BASE)
	{
		PROVIDE(_text_start = .);
		KEEP(*(.text.boot))
		*(.text .text.*)
		PROVIDE(_text_end = .);
	}

	. = ALIGN(0x1000); /* Ensure next section is page aligned */

	/* End of the code sections */

	/* Beginning of the read-only data sections */

	. = ALIGN(0x1000); /* Ensure next section is page aligned */

	.rodata : AT(ADDR(.rodata) - KERNEL_VIRT_BASE)
	{
		PROVIDE(_rodata_start = .);
		*(.srodata .srodata.*)
		*(.rodata .rodata.*)
		. = ALIGN(8);
		PROVIDE(_rodata_end = .);
	}

	/* End of the read-only data sections */

	/* Beginning of the read-write data sections */

	. = ALIGN(0x1000); /* Ensure next section is page aligned */

	.data : AT(ADDR(.data) - KERNEL_VIRT_BASE)
	{
		PROVIDE(_data_start = .);
		*(.sdata .sdata.*)
		*(.data .data.*)
		*(.readmostly.data)
		*(*.data)
		. = ALIGN(8);

		PROVIDE(_data_end = .);
	}

	. = ALIGN(0x1000); /* Ensure next section is page aligned */

	.bss : AT(ADDR(.bss) - KERNEL_VIRT_BASE)
	{
		PROVIDE(_bss_start = .);
		*(.sbss .sbss.*)
		*(.bss .bss.*)
		. = ALIGN(8);
		PROVIDE(_bss_end = .);
	}

	/* End of the read-write data sections */

	. = ALIGN(0x1000);

	. = ALIGN(0x1000); /* Need this to create proper sections */

	PROVIDE(_end = .);
}
