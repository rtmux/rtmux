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

static int xfer_cpy(thread_t *thp, char *dst, char *src, size_t nbytes)
{
    size_t size;
    int status;
    jmp_buf env;

    SET_XFER_ENV(&env);
    if ((status = xfer_setjmp(*GET_XFER_ENV()))) {
        return status;
    }

    do {
        xfer_memcpy(dst, src, size = min_size(nbytes, XFER_CHUNKSIZE));
        thp->args.ms.msglen += (uint32_t) size;
        src += size;
        dst += size;
    } while (nbytes -= size);

    return 0;
}

int(xferiov)(thread_t *sthp,
             iov_t *dst,
             iov_t *src,
             int dparts,
             int sparts,
             unsigned doff,
             unsigned soff)
{
    char *daddr, *saddr;
    size_t dlen, slen;
    int ret;

#ifndef NDEBUG
    if (doff > GETIOVLEN(dst))
        crash();
#endif
    daddr = (char *) GETIOVBASE(dst) + doff;
    dlen = GETIOVLEN(dst) - doff;

#ifndef NDEBUG
    if (soff > GETIOVLEN(src))
        crash();
#endif
    saddr = (char *) GETIOVBASE(src) + soff;
    slen = GETIOVLEN(src) - soff;

    /* Now we move the data. */
    for (;;) {
        if (slen < dlen) {
            ret = xfer_cpy(sthp, daddr, saddr, slen);
            if ((--sparts == 0) || ret) {
                break;
            }
            daddr += slen;
            dlen -= slen;
            ++src;
            saddr = (char *) GETIOVBASE(src);
            slen = GETIOVLEN(src);
        } else if (dlen < slen) {
            ret = xfer_cpy(sthp, daddr, saddr, dlen);
            if ((--dparts == 0) || ret) {
                break;
            }
            saddr += dlen;
            slen -= dlen;
            ++dst;
            daddr = (char *) GETIOVBASE(dst);
            dlen = GETIOVLEN(dst);
        } else {
            ret = xfer_cpy(sthp, daddr, saddr, slen);
            if ((--dparts == 0) || (--sparts == 0) || ret) {
                break;
            }
            ++src;
            saddr = (char *) GETIOVBASE(src);
            slen = GETIOVLEN(src);
            ++dst;
            daddr = (char *) GETIOVBASE(dst);
            dlen = GETIOVLEN(dst);
        }
    }
    return ret;
}
