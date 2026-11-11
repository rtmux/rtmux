/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* check.c General function to check an iov range to make sure it is inside the
 * process boundary.
 */
/** ***********************************************************************
 *************************************************************************
 *
 * This file may be overridden in a CPU specific directory with code optimized
 * for the particular machine.
 */

#include <core/globals.h>
#include <unistd.h>

/* framac-verify: xfer_memchk framac-assume: get_cpunum framac-scan:
 * kernel/xfer/check.c arch/riscv/include/runtime.h framac-min-goals: 14
 */

/* Return from transfer with a error code stating the side that had the fault
 * (src or dest).
 *
 * Note: the fault handlers defined here cover this module, core.c,
 * 	message.c and copy.c.
 * If those modules are not optimized together in a specific platform, special
 * care need to be taken on fault handlers.
 */

static void xfer_dst_fault(thread_t *thp, CPU_REGISTERS *regs, unsigned flags)
{
    xfer_longjmp(*GET_XFER_ENV(),
                 FAULT_ISWRITE(flags) ? XFER_SRC_FAULT : XFER_DST_FAULT, regs);
}

static void xfer_src_fault(thread_t *thp, CPU_REGISTERS *regs, unsigned flags)
{
    xfer_longjmp(*GET_XFER_ENV(),
                 FAULT_ISWRITE(flags) ? XFER_DST_FAULT : XFER_SRC_FAULT, regs);
}

const struct fault_handlers xfer_dst_handlers = {
    .fault = xfer_dst_fault,
    .restart = xfer_restart,
};

const struct fault_handlers xfer_src_handlers = {
    .fault = xfer_src_fault,
    .restart = xfer_restart,
};

const struct fault_handlers xfer_async_handlers = {
    .fault = xfer_src_fault,
    .restart = xfer_async_restart,
};

/* The postcondition is the property this function exists to provide: a zero
 * return means every non-empty range in the vector lies inside the caller's
 * boundary, so the transfer paths that follow may dereference it. Stated over
 * all of iov rather than "the loop finished", because the second is what the
 * code does and the first is what the callers rely on.
 *
 * A zero-length entry is exempt by design: base + 0 - 1 underflows to base - 1,
 * so the "base > last" arm catches it and the code then chooses to let it pass.
 * Saying so in the contract keeps that deliberate hole visible rather than
 * buried in pointer arithmetic.
 */
/*@ requires iov_len == 0 || \valid_read(iov + (0 .. iov_len - 1));
    ensures \result == 0 ==>
            \forall integer k; 0 <= k < \old(iov_len) ==>
              \old(iov)[k].iov_len == 0 ||
              WITHIN_BOUNDARY((uintptr_t) \old(iov)[k].iov_base,
                              (uintptr_t) \old(iov)[k].iov_base
                                  + \old(iov)[k].iov_len - 1,
                              \old(bound));
 */
int(xfer_memchk)(uintptr_t bound, const iov_t *iov, size_t iov_len)
{
    uintptr_t base, last;
    size_t len;
    int status;
    jmp_buf env;

    SET_XFER_ENV(&env);
    if ((status = xfer_setjmp(*GET_XFER_ENV()))) {
        return status;
    }

    /* The invariant is written over \at(..., Pre) rather than over C locals
     * holding the original iov and count. Locals for the sole use of a
     * specification are dead code to every compiler, and this tree builds
     * warning-clean.
     */
    /*@ loop invariant 0 <= iov_len <= \at(iov_len, Pre);
        loop invariant iov == \at(iov, Pre) + (\at(iov_len, Pre) - iov_len);
        loop invariant \forall integer k;
              0 <= k < \at(iov_len, Pre) - iov_len ==>
              \at(iov, Pre)[k].iov_len == 0 ||
              WITHIN_BOUNDARY((uintptr_t) \at(iov, Pre)[k].iov_base,
                              (uintptr_t) \at(iov, Pre)[k].iov_base
                                  + \at(iov, Pre)[k].iov_len - 1,
                              bound);
        loop assigns iov, iov_len, base, last, len;
        loop variant iov_len;
     */
    while (iov_len) {
        base = (uintptr_t) GETIOVBASE(iov);
        len = GETIOVLEN(iov++);
        last = base + len - 1;
        if (base > last || !WITHIN_BOUNDARY(base, last, bound)) {
            if (len != 0) {
                return -1;
            }
        }
        iov_len--;
    }

    return 0;
}
