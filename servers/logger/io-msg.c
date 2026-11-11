/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * system logger service
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "externs.h"

#include <sys/iomsg.h>

static size_t logger_status(char *buf, size_t cap, struct slogdev *trp)
{
    int readers;
    int waiters;
    struct ocbs *ocb;
    struct waiting *wait;
    int n;

    readers = 0;
    for (ocb = trp->ocbs; ocb; ocb = ocb->next)
        readers++;
    waiters = 0;
    for (wait = trp->waiting; wait; wait = wait->next)
        waiters++;

    n = snprintf(buf, cap,
                 "version 1\n"
                 "buffer_ints %d\n"
                 "used_ints %u\n"
                 "readers %d\n"
                 "waiters %d\n",
                 NumInts, trp->cnt, readers, waiters);
    if (n < 0)
        return 0;
    return (size_t) n < cap ? (size_t) n : cap - 1;
}

int io_msg(ioserv_context_t *ctp, io_msg_t *msg, struct slog_ocb *ocb)
{
    struct _io_native_hdr *hdr = (struct _io_native_hdr *) (void *) msg;
    struct slogdev *trp = ocb->dev;

    if (msg->i.protocol != _IOMSG_RTMUX_NATIVE)
        return ENOSYS;
    if (ctp->size < (int) sizeof(*hdr) || hdr->version != _IO_NATIVE_VERSION ||
        hdr->nxfers > _IO_NATIVE_CAP_TRANSFER_MAX)
        return EINVAL;
    switch (msg->i.subtype) {
    case _IO_NATIVE_STAT: {
        struct stat *st = (struct stat *) (void *) msg;

        if (ctp->size < (int) sizeof(struct _io_native_stat))
            return EINVAL;
        memset(st, 0, sizeof(*st));
        st->st_mode = S_IFCHR | 0666;
        st->st_nlink = 1;
        st->st_size = (off_t) (trp->cnt * sizeof(int));
        return IOSERV_PTR(ctp, st, sizeof(*st));
    }
    case _IO_NATIVE_READ: {
        struct _io_native_read *rd = (struct _io_native_read *) (void *) msg;
        static char status[128];
        size_t len;
        size_t nbytes;

        if (ctp->size < (int) sizeof(*rd))
            return EINVAL;
        len = logger_status(status, sizeof(status), trp);
        if (rd->offset >= len) {
            SETIOV(ctp->iov, NULL, 0);
            return IOSERV_NPARTS(1);
        }
        nbytes = len - (size_t) rd->offset;
        if (nbytes > rd->nbytes)
            nbytes = rd->nbytes;
        SETIOV(ctp->iov, status + rd->offset, nbytes);
        return IOSERV_NPARTS(1);
    }
    case _IO_NATIVE_WRITE:
    case _IO_NATIVE_CTL_WRITE:
        return EROFS;
    default:
        return ENOSYS;
    }
}
