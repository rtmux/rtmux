/**
 * \file head.S
 *
 * First instructions executed in the startup
 *
 * \author Anup Patel <anup.patel@wdc.com>
 * \copyright (c) 2019 Western Digital Corporation or its affiliates.
 *
 * \license SPDX-License-Identifier: BSD-2-Clause
 *
 */

#include <asmmacro.h>
#include <asm/image.h>

/* Supervisor interrupt enable register */
#define CSR_SIE		0x104

/* Supervisor trap handler base address */
#define CSR_STVEC	0x105

/* Supervisor interrupt pending register */
#define CSR_SIP		0x144

	.text
	.align 3

ENTRY(_start)

	.global _start
_start:

	j _head_start

	/*
	 * Image header
	 */
	.word 0			/* reserved */
	.balign 8

	.dword 0x200000		/* Offset from the start of RAM to load us */
	.dword _end - _start	/* Effective size of kernel image */
	.dword RISCV_IMAGE_FLAG_LE
	.word RISCV_HEADER_VERSION
	.word 0
	.dword 0
	.ascii RISCV_IMAGE_MAGIC
	.balign 4
	.ascii RISCV_IMAGE_MAGIC2
	.word 0

_head_start:
	/* Pick one hart to run the main boot sequence */
	la	a3, _hart_lottery
	li	a2, 1
	amoadd.w a3, a2, (a3)
	bnez	a3, _start_hang

	/* Save a0 and a1 */
	la	a3, _boot_a0
	sd	a0, 0(a3)
	la	a3, _boot_a1
	sd	a1, 0(a3)

	/* Zero-out BSS */
	la	a4, _bss_start
	la	a5, _bss_end
_bss_zero:
	sd	zero, (a4)
	add	a4, a4, __SIZEOF_POINTER__
	blt	a4, a5, _bss_zero

_start_warm:
	/* Disable and clear all interrupts */
	csrw	CSR_SIE, zero
	csrw	CSR_SIP, zero

	/* Setup exception vectors */
	la	a3, _start_hang
	csrw	CSR_STVEC, a3

	/* Setup stack (16KB above BSS end) */
	la	a3, _end
	li	a4, 0x4000
	add	sp, a3, a4

	/* Jump to startup_entry() */
	la	a3, _boot_a0
	ld	a0, 0(a3)
	la	a3, _boot_a1
	ld	a1, 0(a3)
	call	startup_entry

	/* We don't expect to reach here hence just hang */
	j	_start_hang

	.text
	.align 3
	.globl _start_hang
_start_hang:
	wfi
	j	_start_hang

	.text
	.align	3
	.global _boot_a0, _boot_a1
_hart_lottery:
	.dword	0
_boot_a0:
	.dword	0
_boot_a1:
	.dword	0
