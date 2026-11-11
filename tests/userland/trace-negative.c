/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Malformed-argument conformance for the trace kcall.
 *
 * ksys_trace_event takes a pointer to a command array whose length is declared
 * by the top nibble of its opcode. The kernel runs with SSTATUS.SUM set, so an
 * unchecked command pointer is a read of whatever the caller names, kernel
 * memory included; and RTMUX_TRACE_ALLOCBUFFER writes a physical address back
 * through a pointer the caller supplies inside that array.
 *
 * Every case here must come back EFAULT (or EPERM where the capability check
 * fires first) and must not fault the kernel. The suite passing at all is half
 * the assertion: a kernel that dereferenced these would not reach the next
 * test.
 */

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/trace.h>

#include "test.h"

/* A refusal is a pass. EFAULT is the argument check; EPERM is the capability
 * check where it runs first; ENOTSUP is a kernel built without
 * CONFIG_INSTRUMENTATION, where the whole trace surface is compiled out. What
 * must never happen is acceptance, or a kernel fault (the suite would not reach
 * the next test).
 *
 * expect_efault is for the cases only the argument check can refuse;
 * expect_any_refusal is for the cases another check legitimately fires first.
 */
static void expect_fail(unsigned *data, const char *name, int efault_only)
{
    int rc;

    errno = 0;
    rc = trace_event(data);
    if (rc != -1) {
        test_fail(name, "accepted a malformed command array");
        return;
    }
    if (errno == EFAULT ||
        (!efault_only && (errno == EPERM || errno == ENOTSUP))) {
        test_ok(name);
        return;
    }
    test_fail(name, strerror(errno));
}

static void expect_efault(unsigned *data, const char *name)
{
    expect_fail(data, name, 1);
}

static void expect_any_refusal(unsigned *data, const char *name)
{
    expect_fail(data, name, 0);
}

/* Payload walks of the three pointer-carrying insert opcodes. The kernel
 * copies the caller buffer before taking any lock, so every malformed payload
 * must come back EFAULT or EINVAL without a kernel fault, and a well-formed one
 * must succeed whether or not the event mask is armed. The stub kernel answers
 * ENOTSUP to all of it, so the strict checks run only when QUERYSUPPORT reports
 * instrumentation.
 */
static int expect_rc(unsigned *cmd, int want, const char *name)
{
    int rc;

    errno = 0;
    rc = trace_event(cmd);
    if (want == 0 ? rc == 0 : (rc == -1 && errno == want)) {
        test_ok(name);
        return 1;
    }
    test_fail(name, rc == -1 ? strerror(errno) : "unexpected success");
    return 0;
}

static void set_ptr(unsigned *w, uint64_t p)
{
    w[0] = TRACE_PTR_LO(p);
    w[1] = TRACE_PTR_HI(p);
}

static void trace_payload_cases(void)
{
    unsigned cmd[8] __attribute__((aligned(8)));
    unsigned q[2] = {RTMUX_TRACE_QUERYSUPPORT, 0};
    char *edge = test_unmapped_user_addr();
    char *good, *high;
    /* Larger than the kernel's TRACE_USRSTR_MAX (256) by a wide margin. */
    char str[1024];
    int instr;

    errno = 0;
    instr = trace_event(q) != RTMUX_TRACE_NOINSTRSUPP;
    if (!instr || !edge) {
        test_ok("trace payload cases skipped (stub kernel)");
        return;
    }

    /* Valid string: low pointer words round-trip. */
    strcpy(str, "trace-payload");
    memset(cmd, 0, sizeof cmd);
    cmd[0] = RTMUX_TRACE_INSERTUSRSTREVENT;
    cmd[1] = 1;
    set_ptr(&cmd[2], (uintptr_t) str);
    expect_rc(cmd, 0, "trace usrstr valid string");

    /* Same pointer with the high word set names a different (unmapped or
     * out-of-range) address. A kernel that dropped the high word would accept.
     */
    set_ptr(&cmd[2], (uintptr_t) str | (1ULL << 40));
    expect_rc(cmd, EFAULT, "trace usrstr high word honoured");

    /* Exactly 4 GiB above a mapped page: a kernel that kept only the low word
     * would read the mapped string and return 0. User space ends at 1 GiB
     * (CPU_USER_VADDR_END), so no page above 4 GiB can be mapped to round-trip
     * a high pointer; the alias is the sharpest truncation probe available.
     */
    set_ptr(&cmd[2], (uintptr_t) str | (1ULL << 32));
    expect_rc(cmd, EFAULT, "trace usrstr 4 GiB alias honoured");
    errno = 0;
    high = mmap((void *) (1UL << 33), 4096, PROT_READ | PROT_WRITE,
                MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
    check(high == MAP_FAILED && errno == EINVAL,
          "trace usrstr no user page above 4 GiB to map",
          "the user address space grew past 4 GiB: map a real high page here");

    /* Unmapped in-range string. */
    set_ptr(&cmd[2], (uintptr_t) edge);
    expect_rc(cmd, EFAULT, "trace usrstr unmapped string");

    /* String ending exactly at the mapping edge must not over-read. */
    good = edge - 4;
    memcpy(good, "abc", 4);
    set_ptr(&cmd[2], (uintptr_t) good);
    expect_rc(cmd, 0, "trace usrstr terminator at mapping edge");

    /* Unterminated: far more non-NUL bytes than the kernel cap. */
    memset(str, 'x', sizeof str);
    set_ptr(&cmd[2], (uintptr_t) str);
    expect_rc(cmd, EINVAL, "trace usrstr unterminated string");

    /* Unterminated and running off the end of the mapping. */
    memset(edge - 8, 'x', 8);
    set_ptr(&cmd[2], (uintptr_t) (edge - 8));
    expect_rc(cmd, EFAULT, "trace usrstr unterminated into unmapped");

    /* INSERTCCLASSEVENT: { op, -, code, ptr_lo, ptr_hi, nwords }. */
    memset(cmd, 0, sizeof cmd);
    cmd[0] = RTMUX_TRACE_INSERTCCLASSEVENT;
    cmd[2] = 1;
    set_ptr(&cmd[3], (uintptr_t) str);
    cmd[5] = 4;
    expect_rc(cmd, 0, "trace cclass valid payload");
    set_ptr(&cmd[3], (uintptr_t) edge);
    expect_rc(cmd, EFAULT, "trace cclass unmapped payload");
    set_ptr(&cmd[3], (uintptr_t) (edge - 4));
    cmd[5] = 2;
    expect_rc(cmd, EFAULT, "trace cclass payload crossing into unmapped");
    set_ptr(&cmd[3], (uintptr_t) str | (1ULL << 40));
    cmd[5] = 4;
    expect_rc(cmd, EFAULT, "trace cclass high word honoured");
    set_ptr(&cmd[3], (uintptr_t) str | (1ULL << 32));
    cmd[5] = 4;
    expect_rc(cmd, EFAULT, "trace cclass 4 GiB alias honoured");
    set_ptr(&cmd[3], (uintptr_t) str);
    cmd[5] = sizeof str / sizeof(unsigned); /* 1 KiB, over the cap */
    expect_rc(cmd, EINVAL, "trace cclass oversize payload");

    /* INSERTCUSEREVENT: { op, code, ptr_lo, ptr_hi, nwords }. */
    memset(cmd, 0, sizeof cmd);
    cmd[0] = RTMUX_TRACE_INSERTCUSEREVENT;
    cmd[1] = 1;
    set_ptr(&cmd[2], (uintptr_t) edge);
    cmd[4] = 4;
    expect_rc(cmd, EFAULT, "trace cuser unmapped payload");
    cmd[4] = 4;
    set_ptr(&cmd[2], (uintptr_t) str);
    expect_rc(cmd, 0, "trace cuser valid payload");
    set_ptr(&cmd[2], (uintptr_t) str | (1ULL << 32));
    expect_rc(cmd, EFAULT, "trace cuser 4 GiB alias honoured");
    set_ptr(&cmd[2], (uintptr_t) str | (1ULL << 40));
    expect_rc(cmd, EFAULT, "trace cuser high word honoured");
    set_ptr(&cmd[2], (uintptr_t) str);
    cmd[4] = sizeof str / sizeof(unsigned); /* over the 64-word cap */
    expect_rc(cmd, EINVAL, "trace cuser oversize payload");
}

void test_trace_negative(void)
{
    /* aligned(8): case 4 writes a pointer across cmd[2..3]. */
    unsigned cmd[8] __attribute__((aligned(8)));
    void *edge;
    uint64_t paddr_out = 0;

    /* 1. NULL command array. */
    expect_efault(NULL, "trace null command array");

    /* 2. Command array in the supervisor half. */
    expect_efault((unsigned *) TEST_KERNEL_ADDR,
                  "trace kernel-range command array");

    /* 3. A multi-word opcode whose first word is readable but whose tail is
     * not. Placing an ADDCLASS command in the last word of a mapping leaves
     * word 0 readable and in range while word 1 falls in the unmapped page
     * after it, which is the only case that exercises the opcode-derived length
     * rather than the one-word entry check. A kernel-range pointer would not:
     * that is rejected by the first word's range test, as case 2 already shows.
     */
    edge = test_unmapped_user_addr();
    if (!edge) {
        test_fail("trace command array with unmapped tail", strerror(errno));
    } else {
        /* Land the opcode in the last readable word so word 1 is in the
         * unmapped page.
         */
        unsigned *cmd_edge = (unsigned *) edge - 1;

        *cmd_edge = RTMUX_TRACE_ADDCLASS; /* two words; the second is gone */
        /* Not strict: only an instrumented kernel sizes a multi-word copy and
         * so reaches the missing tail. The stub reads the opcode word alone and
         * answers ENOTSUP, which is a refusal too.
         */
        expect_any_refusal(cmd_edge, "trace command array with unmapped tail");
    }

    /* 4. ALLOCBUFFER naming a kernel address as its paddr-out destination. The
     * capability check runs before the pointer check, so an unprivileged caller
     * legitimately sees EPERM, and a kernel without CONFIG_INSTRUMENTATION sees
     * ENOTSUP. A caller holding RTMUX_PRIV_CAP_TRACE on an instrumented kernel
     * gets EFAULT. All three are refusals, and none may write through the
     * pointer.
     */
    memset(cmd, 0, sizeof cmd);
    cmd[0] = RTMUX_TRACE_ALLOCBUFFER;
    cmd[1] = 1;
    *(uint64_t *) &cmd[2] = (uint64_t) TEST_KERNEL_ADDR;
    expect_any_refusal(cmd, "trace allocbuffer kernel-range paddr-out");

    /* 5. Same, with a null destination. */
    memset(cmd, 0, sizeof cmd);
    cmd[0] = RTMUX_TRACE_ALLOCBUFFER;
    cmd[1] = 1;
    expect_any_refusal(cmd, "trace allocbuffer null paddr-out");

    /* 5b. user_slice range rules on the paddr-out destination: a wrapping and
     * a boundary-straddling eight-byte slot. Same authority caveat as case 4.
     */
    memset(cmd, 0, sizeof cmd);
    cmd[0] = RTMUX_TRACE_ALLOCBUFFER;
    cmd[1] = 1;
    *(uint64_t *) &cmd[2] = (uint64_t) -4L;
    expect_any_refusal(cmd, "trace allocbuffer wrapping paddr-out");
    *(uint64_t *) &cmd[2] = (uint64_t) ((1UL << 37) - 4);
    expect_any_refusal(cmd, "trace allocbuffer boundary-straddling paddr-out");
    if (edge) {
        *(uint64_t *) &cmd[2] = (uint64_t) (uintptr_t) edge;
        expect_any_refusal(cmd, "trace allocbuffer unmapped paddr-out");
    }

    /* 6. A well-formed, fully-valid command must be refused on authority, not
     * on its arguments. This is the control that proves the cases above fail
     * because of their pointers rather than because the command array is never
     * read.
     *
     * ALLOCBUFFER with a real destination, deliberately: its
     * RTMUX_PRIV_CAP_TRACE check runs unconditionally at the top of the case,
     * so an unprivileged caller always sees EPERM. DEALLOCBUFFER would be the
     * wrong choice -- its capability check sits inside `if
     * (privateptr->tracebuf)`, so with no buffer allocated it returns EOK, and
     * the case would pass or fail depending on whether the boot-time -T tracer
     * happened to allocate one.
     */
    memset(cmd, 0, sizeof cmd);
    cmd[0] = RTMUX_TRACE_ALLOCBUFFER;
    cmd[1] = 1;
    *(uint64_t *) &cmd[2] = (uint64_t) (uintptr_t) &paddr_out;
    errno = 0;
    if (trace_event(cmd) == -1 && (errno == EPERM || errno == ENOTSUP)) {
        test_ok("trace well-formed command refused on authority");
    } else {
        test_fail("trace well-formed command refused on authority",
                  strerror(errno));
    }

    /* 7. INSERTUSRSTREVENT with a null string. This is the reproduced panic:
     * the kernel ran the string with bare loads to find its terminator and only
     * then checked where the string lived, so a null pointer faulted an
     * instrumented kernel with the check still ahead of it (scause 13, stval 0)
     * from a process that had just been denied RTMUX_PRIV_CAP_TRACE.
     */
    memset(cmd, 0, sizeof cmd);
    cmd[0] = RTMUX_TRACE_INSERTUSRSTREVENT;
    expect_any_refusal(cmd, "trace usrstr null string");

    trace_payload_cases();

    /* ALLOCBUFFER's buffer-count bound (zero, and counts near 0x40001 that
     * wrapped the 32-bit size while the header-init loop walked past the
     * allocation) deliberately has no case here. Its RTMUX_PRIV_CAP_TRACE
     * check fires several lines ahead of the count check, and this binary is
     * an unprivileged process, so any case written against it would be
     * satisfied by the EPERM and would still pass with the count check deleted
     * entirely. A test that cannot fail is worse than an acknowledged gap.
     * Covering it needs a trace-capable process, which this suite does not
     * have; case 6 above is the honest version, asserting only the authority
     * refusal that is actually reachable from here.
     */
}
