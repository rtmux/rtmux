/*
 * Copyright (c) The FreeBSD Project. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <machine/asm.h>
 
/* func_ptr_type
 * ldso_bootstrap(Elf_Addr *sp, func_ptr_type *exit_proc, Obj_Entry **objp)
 */

/* Mark ldso_bootstrap and ldso_bind as hidden so the linker emits direct
 * PC-relative calls instead of routing through the PLT/GOT.
 * This is essential: before self-relocation, GOT entries are unrelocated
 * (base-0), so PLT calls would jump to address 0.
 */
.hidden ldso_bootstrap
.hidden ldso_bind

ENTRY(.ldso_start)
	mv	s1, sp		/* Save the stack pointer */

#ifdef LDSO_VERBOSE
	/* Debug: trace entry via raw ecall (no GOT/PLT) */
	lla	a0, 1f
	li	a1, 20
	li	a7, 3		/* __KSYS_DBGPRINT */
	ecall
#endif

	addi	sp, sp, -16	/* Make room for obj_main & exit proc */

	mv	a0, s1		/* Pass initial SP as first arg to ldso_bootstrap */
	mv	a1, sp		/* exit_proc */
	addi	a2, a1, 8	/* obj_main */
	call	ldso_bootstrap		/* Call the loader */
	mv	s2, a0		/* Backup the entry point in callee-saved reg */

#ifdef LDSO_VERBOSE
	/* Debug: trace ldso_bootstrap returned */
	lla	a0, 2f
	li	a1, 20
	li	a7, 3
	ecall
#endif

	mv	t0, s2		/* Restore entry point */

	ld	a2, 0(sp)	/* Load cleanup */
	ld	a1, 8(sp)	/* Load obj_main */
	mv	a0, s1		/* Pass initial SP to main program */
	mv	sp, s1		/* Restore the stack pointer */
	jr	t0		/* Jump to the entry point */

#ifdef LDSO_VERBOSE
	.section .rodata
1:	.asciz	"ldso: entry point\n"
	.balign 4
2:	.asciz	"ldso: ldso_bootstrap done\n"
	.balign 4
	.text
#endif
END(.ldso_start)

/* t0 = obj pointer
 * t1 = reloc offset
 */
ENTRY(ldso_bind_start)
	/* Save the arguments and ra */
	/* We require 17 dwords, but the stack must be aligned to 16-bytes */
	addi	sp, sp, -(8 * 18)
	sd	a0, (8 * 0)(sp)
	sd	a1, (8 * 1)(sp)
	sd	a2, (8 * 2)(sp)
	sd	a3, (8 * 3)(sp)
	sd	a4, (8 * 4)(sp)
	sd	a5, (8 * 5)(sp)
	sd	a6, (8 * 6)(sp)
	sd	a7, (8 * 7)(sp)
	sd	ra, (8 * 8)(sp)

#ifdef __riscv_float_abi_double
	/* Save any floating-point arguments */
	fsd	fa0, (8 * 9)(sp)
	fsd	fa1, (8 * 10)(sp)
	fsd	fa2, (8 * 11)(sp)
	fsd	fa3, (8 * 12)(sp)
	fsd	fa4, (8 * 13)(sp)
	fsd	fa5, (8 * 14)(sp)
	fsd	fa6, (8 * 15)(sp)
	fsd	fa7, (8 * 16)(sp)
#endif

	/* Reloc offset is 3x of the .got.plt offset */
	slli	a1, t1, 1	/* Mult items by 2 */
	add	a1, a1, t1	/* Plus item */

	/* Load obj */
	mv	a0, t0

	/* Call into ldso */
	call	ldso_bind

	/* Backup the address to branch to */
	mv	t0, a0

	/* Restore the arguments and ra */
	ld	a0, (8 * 0)(sp)
	ld	a1, (8 * 1)(sp)
	ld	a2, (8 * 2)(sp)
	ld	a3, (8 * 3)(sp)
	ld	a4, (8 * 4)(sp)
	ld	a5, (8 * 5)(sp)
	ld	a6, (8 * 6)(sp)
	ld	a7, (8 * 7)(sp)
	ld	ra, (8 * 8)(sp)

#ifdef __riscv_float_abi_double
	/* Restore floating-point arguments */
	fld	fa0, (8 * 9)(sp)
	fld	fa1, (8 * 10)(sp)
	fld	fa2, (8 * 11)(sp)
	fld	fa3, (8 * 12)(sp)
	fld	fa4, (8 * 13)(sp)
	fld	fa5, (8 * 14)(sp)
	fld	fa6, (8 * 15)(sp)
	fld	fa7, (8 * 16)(sp)
#endif
	addi	sp, sp, (8 * 18)

	/* Call into the correct function */
	jr	t0
END(ldso_bind_start)
