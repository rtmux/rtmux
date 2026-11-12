/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <unistd.h>
#include <setjmp.h>
#include <core/globals.h>

/* Fault-safe user-memory probe.
 *
 * The probe macros used at kcall entry (RD_PROBE_INT / WR_PROBE_INT) touch the
 * caller's memory directly, and the range check that precedes them only bounds
 * the address from above: WITHIN_BOUNDARY is `first <= boundary && last <=
 * boundary`, so NULL and any unmapped address below the boundary pass it. Most
 * kcalls arm no fault handler, so the probe's own fault reached the crash dump
 * and any process could halt the machine with, for example, __timer_settime(id,
 * 0, NULL, NULL).
 *
 * This routes the probe through the same recovery the short-message copy path
 * uses. When the caller has already armed a handler (the IPC transfer paths do)
 * nothing changes: no env is installed and a fault unwinds to that handler
 * exactly as before. When none is armed, a private env turns the fault into an
 * error return.
 *
 * Bounded by what the trap handler recovers: RISCV_EXC_IS_PAGE_FAULT only, so a
 * misaligned user pointer is covered on any target that resolves misaligned
 * accesses in hardware or M-mode (QEMU virt and OpenSBI both do, and
 * tests/userland/user-ptr-negative.c checks it survives). A target that traps
 * RISCV_EXC_LOAD_MISALIGNED to S-mode would need that recovery widened before
 * this is fault-safe there. Parameters:
 *   loc   : user address to probe
 *   words : number of 32-bit words
 *   write : nonzero to probe for write (read-back-write) rather than read
 * Return value: 0 if the whole extent is accessible, otherwise the fault status
 */
int xfer_probe(void *loc, unsigned words, int write)
{
    void *old_handler;
    volatile uint32_t *p = (volatile uint32_t *) loc;
    jmp_buf *old_env = NULL;
    jmp_buf env;
    unsigned i;

    if (!(old_handler = (void *) GET_XFER_HANDLER())) {
        int status;

        old_env = GET_XFER_ENV();
        SET_XFER_ENV(&env);
        if ((status = xfer_setjmp(*GET_XFER_ENV()))) {
            SET_XFER_HANDLER(old_handler);
            SET_XFER_ENV(old_env);
            return status;
        }
        SET_XFER_HANDLER(&xfer_src_handlers);
    }

    /* Branch hoisted out of the loop: `write` is loop-invariant and a probe can
     * span a whole struct.
     */
    if (write) {
        for (i = 0; i < words; i++)
            p[i] = p[i];
    } else {
        for (i = 0; i < words; i++)
            (void) p[i];
    }
    SET_XFER_HANDLER(old_handler);
    if (!old_handler) {
        /* Restore the env too, not just the handler. Nothing reachable today
         * longjmps through a stale env -- every entry into the xfer machinery
         * (message.c, check.c, notification.c, length.c, copy.c) installs its
         * own before arming a handler that can longjmp -- but leaving
         * xfer_env[cpu] pointing at this dead frame makes correctness depend on
         * that holding at every future call site, and the probe now runs on
         * every kcall that touches user memory rather than only the copy paths.
         */
        SET_XFER_ENV(old_env);
    }

    return 0;
}

/* Fault-safe atomic fetch-and-OR on a user word.
 *
 * The same recovery as xfer_probe, but the write is a single atomic RMW. A
 * plain read-back-write would clobber a concurrent owner/count update from
 * another hart (see kernel/sync/core.c), so sync_t probes must use this form,
 * and a caller that needs the word's old value gets it from the same access.
 * OR-ing 0 probes writability without changing the word. Unlike xfer_probe this
 * cannot rely on the platform to emulate a misaligned access: an AMO to a
 * misaligned address traps and is not recovered, so a misaligned address is
 * refused here. Parameters:
 *   uaddr : user address of the word
 *   bits  : bits to OR in
 *   oldp  : receives the value before the OR; may be NULL
 * Return value: 0 on success, otherwise the fault status (EFAULT)
 */
int xfer_fetch_or_user(volatile unsigned *uaddr, unsigned bits, unsigned *oldp)
{
    void *old_handler;
    jmp_buf *old_env = NULL;
    jmp_buf env;
    unsigned old;

    if ((uintptr_t) uaddr & (sizeof(unsigned) - 1))
        return EFAULT;
    if (!(old_handler = (void *) GET_XFER_HANDLER())) {
        int status;

        old_env = GET_XFER_ENV();
        SET_XFER_ENV(&env);
        if ((status = xfer_setjmp(*GET_XFER_ENV()))) {
            SET_XFER_HANDLER(old_handler);
            SET_XFER_ENV(old_env);
            return status;
        }
        SET_XFER_HANDLER(&xfer_src_handlers);
    }

    old = __atomic_fetch_or(uaddr, bits, __ATOMIC_SEQ_CST);

    SET_XFER_HANDLER(old_handler);
    if (!old_handler)
        SET_XFER_ENV(old_env);
    if (oldp)
        *oldp = old;
    return 0;
}

int xfer_atomic_or_user(volatile unsigned *uaddr, unsigned bits)
{
    return xfer_fetch_or_user(uaddr, bits, NULL);
}

/* This routine is used to do memcpy between a kernel buffer and a user buffer
 * during short msg transfer. It will guranteed to return an error msg when the
 * user buffer can not be accessed.
 */
int xfer_memcpy(void *dst, const void *src, size_t len)
{
    void *old_handler;
    register uint8_t *d;
    register const uint8_t *s;

    if (!(old_handler = (void *) GET_XFER_HANDLER())) {
        jmp_buf env;
        jmp_buf *old_env = GET_XFER_ENV();
        int status;

        /* Setup the jump buffer for all exiting of this function */
        SET_XFER_ENV(&env);
        if ((status = xfer_setjmp(*GET_XFER_ENV()))) {
            /* Fault path: the copy longjmp'd back here. Restore the handler and
             * env we replaced, so a standalone caller (as-memcpy, dbgprint)
             * does not leave xfer_src_handlers installed with a now-dead env
             * frame for the next per-hart xfer to trip over.
             */
            SET_XFER_HANDLER(old_handler);
            SET_XFER_ENV(old_env);
            return status;
        }
        SET_XFER_HANDLER(&xfer_src_handlers);
    }
    d = dst;
    s = src;
    while (len--) {
        *d++ = *s++;
    }
    SET_XFER_HANDLER(old_handler);
    return 0;
}

/* This routine is used to do memcpy from a kernel buffer to a user buffer or
 * iov set during short msg transfer. It will guranteed to return an error msg
 * when the user buffer or iov set can not be accessed.
 */
int xfer_cpy_diov(thread_t *thpd,
                  iov_t *dst,
                  uint8_t *saddr,
                  int dparts,
                  size_t slen)
{
    jmp_buf env;
    int status;
    char *daddr;
    size_t dlen;


    /* Every exit clears the handler, not just the successful ones.
     *
     * The failure returns below used to leave xfer_dst_handlers (or whatever
     * was armed at fault time) installed while this frame returned, so the
     * per-CPU handler outlived the env it belongs to. Anything that later
     * called into the xfer machinery on the same hart saw a non-NULL handler,
     * skipped installing its own env (both xfer_memcpy and xfer_probe only
     * install one when none is armed), and would longjmp into this dead frame
     * on a fault instead of recovering locally. specialret() is the concrete
     * path: its short-message arm calls here, continues its drain loop after a
     * failure, and the nanosleep and join arms then copy out through
     * xfer_memcpy.
     *
     * No caller reads the handler after a non-zero return, and there is no
     * valid env to preserve once this frame is gone, so clearing is strictly
     * the safer state.
     */
    SET_XFER_ENV(&env);
    if ((status = xfer_setjmp(*GET_XFER_ENV()))) {
        SET_XFER_HANDLER(0);
        return status;
    }

    if (dparts < 0) {
        if (!WR_PROBE_PTR(thpd, dst, PARTS_TO_LEN(dparts))) {
            SET_XFER_HANDLER(0);
            return XFER_DST_FAULT;
        }
        SET_XFER_HANDLER(&xfer_src_handlers);
        xfer_memcpy(dst, saddr, min_size(slen, PARTS_TO_LEN(dparts)));
        SET_XFER_HANDLER(0);
        return 0;
    } else if (dparts == 0)
        return 0;

    SET_XFER_HANDLER(&xfer_dst_handlers);
    daddr = (char *) GETIOVBASE(dst);
    dlen = GETIOVLEN(dst);
    if (!WR_PROBE_PTR(thpd, daddr, dlen)) {
        SET_XFER_HANDLER(0);
        return XFER_DST_FAULT;
    }

    SET_XFER_HANDLER(&xfer_src_handlers);

    /* Now we move the data. */
    if (slen <= dlen) {
        xfer_memcpy(daddr, saddr, slen);
    } else {
        /* Rare case. The two returns above leave dparts >= 1 here, and the loop
         * breaks the moment the decrement reaches zero, so --dparts cannot go
         * negative. Stated because the guard is a break inside the body rather
         * than the loop condition, which a prover does not read as a bound.
         */
        /*@ loop invariant dparts >= 1; */
        while (1) {
            xfer_memcpy(daddr, saddr, dlen);
            if (--dparts == 0) {
                break;
            }
            saddr += dlen;
            slen -= dlen;
            ++dst;
            SET_XFER_HANDLER(&xfer_dst_handlers);
            daddr = (char *) GETIOVBASE(dst);
            dlen = GETIOVLEN(dst);
            if (!WR_PROBE_PTR(thpd, daddr, dlen)) {
                SET_XFER_HANDLER(0);
                return XFER_DST_FAULT;
            }
            SET_XFER_HANDLER(&xfer_src_handlers);
            if (slen <= dlen) {
                xfer_memcpy(daddr, saddr, slen);
                break;
            }
        }
    }

    SET_XFER_HANDLER(0);
    return 0;
}
