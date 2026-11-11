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

int xferlen(thread_t *thp, iov_t *iov, int parts)
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
        return status;
    }

    SET_XFER_HANDLER(&xfer_fault_handlers_xferlen);

    len = 0;
    while (parts > 0) {
        len += GETIOVLEN(iov);
        ++iov;
        --parts;
    }

    SET_XFER_HANDLER(0);

    return (int) len;
}
