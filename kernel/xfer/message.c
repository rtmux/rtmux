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
#include <core/globals.h>

#define XFER_CHUNKSIZE 4096

unsigned(xferiov_pos)(CPU_REGISTERS *regs)
{
    /*  uintptr_t           *ip = (uintptr_t *)REGIP(regs); */

    return 0;
}

static void xfer_cpy(thread_t *thp, char *dst, char *src, size_t nbytes)
{
    size_t size;

    do {
        xfer_memcpy(dst, src, size = min_size(nbytes, XFER_CHUNKSIZE));
        thp->args.ms.msglen += (uint32_t) size;
        src += size;
        dst += size;
    } while (nbytes -= size);
}

/* Read the entry at e once, into a snapshot, so the base and the length that
 * are checked and used are the pair that was read: another thread sharing the
 * vector can rewrite an entry at any time. The entry must lie inside bound (0:
 * no check) and its offset off must not pass its end.
 *
 * Returns 0 or fault.
 */
static int xfer_entry(const iov_t *e,
                      uintptr_t bound,
                      size_t off,
                      int fault,
                      char **addr,
                      size_t *len)
{
    iov_t snap = *e;
    uintptr_t base = (uintptr_t) GETIOVBASE(&snap), last;
    size_t n = GETIOVLEN(&snap);

    if (off > n)
        return fault;
    if (bound && n) {
        last = base + n - 1;
        if (last < base || !WITHIN_BOUNDARY(base, last, bound))
            return fault;
    }
    *addr = (char *) base;
    *len = n;
    return 0;
}

/* Move data until a vector runs out, or a bounded walk has done its step of
 * work. A fault longjmps to the env xferiov armed. On XFER_YIELD the walk holds
 * the position to resume from and msglen counts the bytes moved.
 */
static int xferiov_walk(thread_t *sthp, struct xfer_walk *w)
{
    char *dbase, *sbase;
    size_t dtot, stot, n;
    struct xfer_spent *sp = w->spent;
    int ret;

    if ((ret = xfer_entry(w->dst, w->dbound, w->doff, XFER_DST_FAULT, &dbase,
                          &dtot)) ||
        (ret = xfer_entry(w->src, w->sbound, w->soff, XFER_SRC_FAULT, &sbase,
                          &stot)))
        return ret;

    for (;;) {
        bool dnext, snext;

        n = min_size(dtot - w->doff, stot - w->soff);
        if (sp)
            n = min_size(n, XFER_STEP_BYTES - sp->bytes);
        xfer_cpy(sthp, dbase + w->doff, sbase + w->soff, n);
        w->doff += n;
        w->soff += n;
        if (sp)
            sp->bytes += (unsigned) n;

        dnext = w->doff == dtot;
        snext = w->soff == stot;
        if (dnext) {
            ++w->dst;
            w->doff = 0;
            if (--w->dparts == 0)
                return 0;
        }
        if (snext) {
            ++w->src;
            w->soff = 0;
            if (--w->sparts == 0)
                return 0;
        }
        if (sp) {
            sp->ents += dnext + snext;
            if (sp->ents >= XFER_STEP_ENTRIES || sp->bytes >= XFER_STEP_BYTES)
                return XFER_YIELD;
        }
        if (dnext && (ret = xfer_entry(w->dst, w->dbound, 0, XFER_DST_FAULT,
                                       &dbase, &dtot)))
            return ret;
        if (snext && (ret = xfer_entry(w->src, w->sbound, 0, XFER_SRC_FAULT,
                                       &sbase, &stot)))
            return ret;
    }
}

/* Copy between two iov vectors from the position in w. The caller has armed the
 * fault handlers for the user side; the fault env is armed here, around the
 * whole walk, so a fault in an entry load or in the copy returns its status.
 */
int(xferiov)(thread_t *sthp, struct xfer_walk *w)
{
    jmp_buf env, *old_env = GET_XFER_ENV();
    int status;

    SET_XFER_ENV(&env);
    if ((status = xfer_setjmp(*GET_XFER_ENV()))) {
        SET_XFER_ENV(old_env);
        return status;
    }
    status = xferiov_walk(sthp, w);
    SET_XFER_ENV(old_env);
    return status;
}
