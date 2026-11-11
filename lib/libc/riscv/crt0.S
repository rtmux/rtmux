/*
 * crt0.S — C runtime startup for RTMux user-space programs (RISC-V 64)
 *
 * Entry point: _start
 *
 * When invoked by the dynamic linker (ldso):
 *   a0 = initial_sp  — stack pointer as set up by the kernel
 *   a1 = obj_main    — ldso-internal Obj_Entry *, not used here
 *   a2 = cleanup     — exit hook (may be NULL), not used here
 *
 * Stack layout at initial_sp (ELF/LP64 convention):
 *   0(sp): argc         (unsigned long, 8 bytes)
 *   8(sp): argv[0] ... argv[argc-1], NULL
 *          envp[0] ... envp[nenv-1], NULL
 *          auxv entries
 *
 * SPDX-License-Identifier: Apache-2.0
 */

    /*
     * .rtmux binary identification section (32 bytes, fixed layout).
     * Mirrors struct rtmux_ident in internal/include/sys/rtmux-ident.h. Every
     * RTMux userland binary that links crt0.o ends up with this section so
     * initramfs_elf accepts it ("executable (recorded)" rather than
     * "skipping"). Sources that need a different type (e.g. SHLIB)
     * still declare their own RTMUX_IDENT(...) — the linker concatenates
     * .rtmux sections and the kernel reads the first 32-byte struct.
     */
    .section .rtmux, "a"
    /* 4-byte align matches struct rtmux_ident's natural alignment
     * (largest member is uint32_t). Keeping it at 4 avoids the linker
     * sorting crt0.o's EXEC ident ahead of a C-defined RTMUX_IDENT()
     * with a different type. */
    .align 2
    .long 0x4d555800              /* qi_magic = RTMUX_MAGIC "MUX\0" */
    .short 1                      /* qi_version */
    .short 1                      /* qi_type = RTMUX_TYPE_EXEC */
    .long 0                       /* qi_flags */
    .long 1                       /* qi_abi_version */
    .fill 16, 1, 0                /* qi_reserved[16] */

    .section .text
    .globl _start
    .type _start, @function
_start:
    /* Clear frame pointer for stack-unwinding */
    li      s0, 0

    /*
     * Restore sp to the initial stack pointer that the kernel built.
     * ldso may have adjusted sp during its own operation.
     */
    mv      sp, a0

    /* Decode argc, argv, envp from the stack */
    ld      a0, 0(sp)           /* argc (64-bit long) */
    addi    a1, sp, 8           /* argv = &sp[1] */
    slli    t0, a0, 3           /* argc * 8 */
    add     a2, a1, t0          /* &argv[argc] — NULL terminator slot */
    addi    a2, a2, 8           /* envp = address after that NULL */

    /* Save argc/argv/envp across the syschan init call */
    mv      s1, a0
    mv      s2, a1
    mv      s3, a2

    /* Save envp in the POSIX environment table before main(). It comes
     * before __rtmux_syschan_init, which finds the auxiliary vector (for
     * issetugid()) just past the end of this table. */
    la      t0, environ
    sd      s3, 0(t0)

    /* Register the per-process system channel with roottask */
    call    __rtmux_syschan_init

    /* Restore argc/argv/envp and call main */
    mv      a0, s1
    mv      a1, s2
    mv      a2, s3
    call    main

    /* main returned — call exit() with its return value */
    call    exit

    /* Should never reach here */
    ebreak

    .size _start, . - _start

    .section .bss
    .align 3
    .weak environ
    .type environ, @object
    .size environ, 8
environ:
    .zero 8
