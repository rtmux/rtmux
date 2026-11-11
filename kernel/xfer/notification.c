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

static void xfer_fault(thread_t *thp, CPU_REGISTERS *regs, unsigned flags)
{
    xfer_longjmp(*GET_XFER_ENV(), -1, regs);
}

const struct fault_handlers xfer_fault_handlers = {
    .fault = xfer_fault,
    .restart = 0,
};

int xfernotification(thread_t *dthp,
                     iov_t *dst,
                     int parts,
                     uint32_t code,
                     uintptr_t value,
                     int32_t scoid)
{
    struct _notification *addr;
    size_t len;
    uintptr_t last;
    int status;
    jmp_buf env;

#ifndef NDEBUG
    /* Make sure iov's address space is accessable */
    if (dthp->as_prp && dthp->as_prp != pcpu_array[get_cpunum()].pc_as_prp) {
        crash();
    }
#endif

    SET_XFER_ENV(&env);
    if ((status = xfer_setjmp(*GET_XFER_ENV()))) {
        return status;
    }

    SET_XFER_HANDLER(&xfer_fault_handlers);

    /* Special case for 1 part messages */
    if (parts < 0) {
        addr = (struct _notification *) dst;
        len = PARTS_TO_LEN(parts);
    } else {
        addr = (struct _notification *) GETIOVBASE(dst);
        len = GETIOVLEN(dst);
    }

    /* Make dest will hold a notification */
    if (len < sizeof(*addr)) {
        SET_XFER_HANDLER(0);
        return XFER_DST_FAULT;
    }

    /* Make sure address is within range for process */
    last = (uintptr_t) addr + len - 1;
    if ((uintptr_t) addr > last ||
        !WITHIN_BOUNDARY((uintptr_t) addr, last,
                         dthp->process->boundary_addr)) {
        SET_XFER_HANDLER(0);
        return XFER_DST_FAULT;
    }

    /* Write notification */
    addr->type = _NOTIFICATION_TYPE;
    addr->subtype = _NOTIFICATION_SUBTYPE;
    addr->code = (int8_t) code;
    addr->value.sival_ptr = (void *) value;
    addr->scoid = scoid;

    SET_XFER_HANDLER(0);
    return 0;
}
