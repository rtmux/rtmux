/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <fcntl.h>
#include <devctl/common.h>
#include "externs.h"
#include "pinode.h"

static dev_t null_devno;

static int null_read(ioserv_context_t *ctp, io_read_t *msg, void *ocb)
{
    uintptr_t ioflag = (uintptr_t) ocb;

    if (!(ioflag & _IO_FLAG_RD)) {
        return EBADF;
    }
    switch (msg->i.xtype & _IO_XTYPE_MASK) {
    case _IO_XTYPE_NONE:
    case _IO_XTYPE_OFFSET:
        break;
    default:
        /* We don't handle this xtype, return error */
        return ENOSYS;
    }
    _IO_SET_READ_NBYTES(ctp, 0);
    return EOK;
}

static int null_write(ioserv_context_t *ctp, io_write_t *msg, void *ocb)
{
    uintptr_t ioflag = (uintptr_t) ocb;

    if (!(ioflag & _IO_FLAG_WR)) {
        return EBADF;
    }
    switch (msg->i.xtype & _IO_XTYPE_MASK) {
    case _IO_XTYPE_NONE:
    case _IO_XTYPE_OFFSET:
        break;
    default:
        /* We don't handle this xtype, return error */
        return ENOSYS;
    }
    _IO_SET_WRITE_NBYTES(ctp, msg->i.nbytes);
    return EOK;
}

static int null_stat(ioserv_context_t *ctp, io_stat_t *msg, void *ocb)
{
    memset(&msg->o, 0x00, sizeof msg->o);
    msg->o.st_ino = PINO_NULL;
    msg->o.st_size = 0;
    msg->o.st_ctime = msg->o.st_mtime = msg->o.st_atime = time(0);
    msg->o.st_mode = 0666 | S_IFCHR;
    msg->o.st_nlink = 1;
    msg->o.st_dev = (ctp->info.srcnd << RTMUX_NODE_BITS) | path_devno;
    msg->o.st_rdev = (ctp->info.srcnd << RTMUX_NODE_BITS) | null_devno;
    return IOSERV_PTR(ctp, &msg->o, sizeof msg->o);
}

static int null_devctl(ioserv_context_t *ctp, io_devctl_t *msg, void *ocb)
{
    union {
        int oflag;
        int mountflag;
    } *data = (void *) (msg + 1);
    uintptr_t ioflag = (uintptr_t) ocb;
    unsigned nbytes = 0;

    switch ((unsigned) msg->i.devctl_cmd) {
    case DEVCTL_GET_FLAGS:
        data->oflag = (int) ((ioflag & ~(uintptr_t) O_ACCMODE) |
                             ((ioflag - 1) & O_ACCMODE));
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

static int null_openfd(ioserv_context_t *ctp, io_openfd_t *msg, void *ocb)
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

static const ioserv_ops_t null_io_funcs = {
    .nfuncs = IOSERV_IO_NFUNCS,
    .read = null_read,
    .write = null_write,
    .stat = null_stat,
    .devctl = null_devctl,
    .openfd = null_openfd,
};

static int null_open(ioserv_context_t *ctp,
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

static const ioserv_connect_ops_t null_connect_funcs = {
    .nfuncs = IOSERV_CONNECT_NFUNCS,
    .open = null_open,
};

void devnull_init(void)
{
    ioserv_attach(dpp, NULL, "/dev/null", 0, 0, &null_connect_funcs,
                  &null_io_funcs, 0);
    hwres_proc_devno("dev", &null_devno, -1, 0);
}
