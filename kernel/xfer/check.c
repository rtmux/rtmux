/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * check.c General function to check an iov range to make sure it is inside the
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

/* Total and check the caller's own iov array before a transfer uses it: the
 * array and every entry that is not empty must lie inside the caller's
 * boundary, the array is no longer than XFER_IOV_LIMIT, and the lengths add up
 * without wrapping. An empty array totals 0; a flat buffer (parts < 0) has no
 * entries to read and is not for this function.
 *
 * A kernel entry spends at most its step on it (xfer_piece_done), then
 * restarts the call. The prefix done so far is kept in act->scan[slot] (an
 * XFER_SCAN_* slot), keyed
 * by the array and its length, so the restart goes on where this entry
 * stopped, and a call that restarts later for another reason (its copy
 * yielded) does not walk the array again. The entries are read under a fault
 * handler; another thread can rewrite the array afterwards, so the transfer
 * still checks each entry it uses.
 *
 * Returns EOK with the total in *bytes, EFAULT, EOVERFLOW for a total that
 * wraps (not looked for when bytes is NULL: the caller wants the check only),
 * or XFER_CALL_RESTART when the call was restarted: the caller undoes what it
 * holds and returns ENOERROR.
 */
int xfer_iov_sum(thread_t *act,
                 const iov_t *iov,
                 int parts,
                 unsigned slot,
                 size_t *bytes)
{
    struct xfer_iov_scan *scan = &act->scan[slot];
    uintptr_t bound = act->process->boundary_addr;
    uintptr_t base = (uintptr_t) iov, last;
    jmp_buf env, *old_env = GET_XFER_ENV();
    void *old_handler = (void *) GET_XFER_HANDLER();
    struct xfer_spent spent;
    int rc = EOK;

    if (parts == 0) {
        if (bytes)
            *bytes = 0;
        return EOK;
    }
    if (parts < 0 || parts > XFER_IOV_LIMIT)
        return EFAULT;

    /* The array itself must lie in the caller's space: a fault-recovering
     * read still succeeds on a mapped kernel address.
     */
    last = base + (size_t) parts * sizeof(iov_t) - 1;
    if (!base || base > last || !WITHIN_BOUNDARY(base, last, bound))
        return EFAULT;
    if (scan->vector != iov || scan->parts != (uint32_t) parts)
        *scan =
            (struct xfer_iov_scan) {.vector = iov, .parts = (uint32_t) parts};

    /* A piece at a time: xfer_memchk() checks its entries against the
     * boundary (it is the function the fuzz and Frama-C harnesses cover, and
     * returns through its own fault env), then the lengths are added up under
     * the env armed here. The two passes read an entry twice; the total is
     * for the caller's accounting only, and the transfer checks every entry
     * it uses again.
     */
    xfer_step_begin(&spent);
    SET_XFER_HANDLER(&xfer_src_handlers);
    while (rc == EOK && scan->done < scan->parts) {
        uint32_t n = scan->parts - scan->done;

        if (n > XFER_SUM_ENTRIES)
            n = XFER_SUM_ENTRIES;
        if (xfer_memchk(bound, iov + scan->done, n)) {
            rc = EFAULT; /* an entry out of range, or a page not there */
            break;
        }
        SET_XFER_ENV(&env);
        if (xfer_setjmp(*GET_XFER_ENV())) {
            rc = EFAULT;
            break;
        }
        for (uint32_t i = 0; bytes && i < n; i++) {
            size_t len = GETIOVLEN(iov + scan->done + i);

            if (len > SIZE_MAX - scan->bytes) {
                rc = EOVERFLOW;
                break;
            }
            scan->bytes += len;
        }
        if (rc != EOK)
            break;
        scan->done += n;
        spent.ents = XFER_SUM_ENTRIES;
        if (scan->done < scan->parts && xfer_piece_done(&spent))
            rc = XFER_CALL_RESTART;
    }
    SET_XFER_HANDLER(old_handler);
    SET_XFER_ENV(old_env);
    if (rc == XFER_CALL_RESTART) {
        KCALL_RESTART(act);
    } else if (rc != EOK) {
        scan->vector = NULL;
    } else if (bytes) {
        *bytes = scan->bytes;
    }
    return rc;
}
