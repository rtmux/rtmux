/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <fcntl.h>
#include <devctl/common.h>
#include "externs.h"
#include "path-object.h"
#include "pinode.h"

static dev_t zero_devno;


static int zero_write(ioserv_context_t *ctp, io_write_t *msg, void *ocb)
{
    unsigned ioflag = (unsigned) (uintptr_t) ocb;

    if (!(ioflag & _IO_FLAG_WR)) {
        return EBADF;
    }

    switch (msg->i.xtype & _IO_XTYPE_MASK) {
    case _IO_XTYPE_NONE:
    case _IO_XTYPE_OFFSET:
        break;
    default:
        /* We don't handle this xtype, return error */
        return EINVAL;
    }
    _IO_SET_WRITE_NBYTES(ctp, msg->i.nbytes);
    return EOK;
}


static int zero_read(ioserv_context_t *ctp, io_read_t *msg, void *ocb)
{
    unsigned ioflag = (unsigned) (uintptr_t) ocb;
    unsigned nbytes;
    unsigned size;
    char *buff;

    if (!(ioflag & _IO_FLAG_RD)) {
        return EBADF;
    }
    /* Find size and start of msg buffer */
    size = (ctp->msg_max_size - (unsigned) ctp->offset) -
           (unsigned) sizeof(msg->i);
    buff = (char *) (&msg->i + 1);

    /* Start writing zeros to client */
    memset(buff, 0x00, nbytes = min_uint(size, (unsigned) msg->i.nbytes));
    SETIOV(ctp->iov + 0, buff, nbytes);
    if ((unsigned) msg->i.nbytes > size) {
        unsigned wr_offset;

        nbytes = (unsigned) msg->i.nbytes;
        wr_offset = 0;
        while (nbytes > size) {
            if ((nbytes -= size) < size) {
                SETIOV(ctp->iov + 0, buff, nbytes);
            }
            wr_offset += size;

            /* use I/O service as it takes care of combined message offsets. */
            if (ioserv_msgwritev(ctp, ctp->iov + 0, 1, (int) wr_offset) == -1) {
                return errno;
            }
        }
        SETIOV(ctp->iov + 0, buff, size);
    }

    _IO_SET_READ_NBYTES(ctp, msg->i.nbytes);
    return IOSERV_NPARTS(1);
}


static int zero_stat(ioserv_context_t *ctp, io_stat_t *msg, void *ocb)
{
    memset(&msg->o, 0x00, sizeof msg->o);
    msg->o.st_ino = PINO_ZERO;
    msg->o.st_size = 0;
    msg->o.st_ctime = msg->o.st_mtime = msg->o.st_atime = time(0);
    msg->o.st_mode = 0666 | S_IFCHR;
    msg->o.st_nlink = 1;
    msg->o.st_dev = (ctp->info.srcnd << RTMUX_NODE_BITS) | path_devno;
    msg->o.st_rdev = (ctp->info.srcnd << RTMUX_NODE_BITS) | zero_devno;
    return IOSERV_PTR(ctp, &msg->o, sizeof msg->o);
}


static int zero_devctl(ioserv_context_t *ctp, io_devctl_t *msg, void *ocb)
{
    union {
        int oflag;
        int mountflag;
    } *data = (void *) (msg + 1);
    unsigned ioflag = (unsigned) (uintptr_t) ocb;
    unsigned nbytes = 0;

    switch ((unsigned) msg->i.devctl_cmd) {
    case DEVCTL_GET_FLAGS:
        data->oflag = (int) (ioflag & ~(unsigned) O_ACCMODE) |
                      (int) ((ioflag - 1) & (unsigned) O_ACCMODE);
        nbytes = (unsigned) sizeof data->oflag;
        break;

    case DEVCTL_SET_FLAGS:
        break;

    default:
        return ENOSYS;
    }

    if (nbytes) {
        msg->o.ret_val = 0;
        return IOSERV_PTR(ctp, &msg->o, sizeof msg->o + nbytes);
    }
    return EOK;
}

static int zero_openfd(ioserv_context_t *ctp, io_openfd_t *msg, void *ocb)
{
    uintptr_t source = (uintptr_t) ocb;
    uintptr_t requested = msg->i.ioflag;

    if (msg->i.xtype != _IO_OPENFD_NONE)
        return ENOSYS;
    if ((requested & _IO_FLAG_MASK & ~source) != 0)
        return EACCES;
    if (ioserv_open_bind(ctp, (void *) requested, 0) == -1)
        return errno;
    return EOK;
}

static const ioserv_ops_t zero_io_funcs = {
    .nfuncs = IOSERV_IO_NFUNCS,
    .read = zero_read,
    .write = zero_write,
    .stat = zero_stat,
    .devctl = zero_devctl,
    .openfd = zero_openfd,
};


static int zero_open(ioserv_context_t *ctp,
                     io_open_t *msg,
                     void *extra,
                     void *reserved)
{
    if (ioserv_open_bind(ctp, (void *) (uintptr_t) msg->connect.ioflag, 0) ==
        -1) {
        return errno;
    }
    return EOK;
}


static const ioserv_connect_ops_t zero_connect_funcs = {
    .nfuncs = IOSERV_CONNECT_NFUNCS,
    .open = zero_open,
};


int devzero_check(const ioserv_ops_t *funcs,
                  mem_map_t *msg,
                  void *handle,
                  path_object_t **pobp)
{
    unsigned ioflag = (unsigned) (uintptr_t) handle;

    if (funcs != &zero_io_funcs) {
        return -1;
    }
    *pobp = NULL;
    if (msg) {
        if ((ioflag & _IO_FLAG_RD) == 0) {
            return EACCES;
        }
        if (((msg->i.flags & MAP_TYPE) == MAP_SHARED) &&
            ((ioflag & _IO_FLAG_WR) == 0)) {
            if (msg->i.prot & PROT_WRITE) {
                /* RUSH3: Is this test needed anymore - as_mmap() should catch
                 * it now
                 */
                return EACCES;
            }
            /* Indicate that we can't allow PROT_WRITE later... */
            msg->i.flags |= IMAP_OCB_RDONLY;
        }
        /* Turn it into anonymous memory. */
        msg->i.flags |= MAP_ANON;
    }
    return EOK;
}


void devzero_init(void)
{
    /* allocate zero directory entry */
    ioserv_attach(dpp, NULL, "/dev/zero", 0, 0, &zero_connect_funcs,
                  &zero_io_funcs, 0);
    hwres_proc_devno("dev", &zero_devno, -1, 0);
}
