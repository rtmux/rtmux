/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/** ***********************************************************************
 *************************************************************************
 *
 * This file may be overridden in a CPU specific directory with code optimized
 * for the particular machine.
 */

#include <limits.h>
#include <unistd.h>
#include <setjmp.h>
#include <core/globals.h>

/* use xfer_fault_handlers_xferlen, in case that xfer_fault is for no setjmp
 * version
 */
static void xfer_fault(thread_t *thp, CPU_REGISTERS *regs, unsigned flags)
{
    xfer_longjmp(*GET_XFER_ENV(), -1, regs);
}

static const struct fault_handlers xfer_fault_handlers_xferlen = {
    .fault = xfer_fault,
    .restart = 0,
};

/* The total length of a vector, or cap once the entries add up to that: a
 * transfer of at most cap bytes does not read a longer vector to its end. The
 * sum is clamped at cap, so one entry far past it cannot carry the result over
 * the int return range and wrap to a short length.
 */
int xferlen_upto(thread_t *thp, iov_t *iov, int parts, size_t cap)
{
    size_t len;
    int status;
    jmp_buf env;

    if (parts < 0) {
        return (int) PARTS_TO_LEN(parts);
    }
#ifndef NDEBUG
    /* Make sure iov's address space is accessable */
    if (thp->as_prp && thp->as_prp != pcpu_array[get_cpunum()].pc_as_prp) {
        crash();
    }
#endif

    SET_XFER_ENV(&env);
    if ((status = xfer_setjmp(*GET_XFER_ENV()))) {
        /* A fault longjmps here with the fault handler still armed. */
        SET_XFER_HANDLER(0);
        return status;
    }

    SET_XFER_HANDLER(&xfer_fault_handlers_xferlen);

    len = 0;
    while (parts > 0 && len < cap) {
        size_t n = GETIOVLEN(iov);

        len += n < cap - len ? n : cap - len;
        ++iov;
        --parts;
    }

    SET_XFER_HANDLER(0);

    /* The result is an int, and no message can be longer than INT_MAX, so a
     * total past it is clamped rather than wrapped to a small or negative
     * length.
     */
    return len > (size_t) INT_MAX ? INT_MAX : (int) len;
}

int xferlen(thread_t *thp, iov_t *iov, int parts)
{
    return xferlen_upto(thp, iov, parts, SIZE_MAX);
}
