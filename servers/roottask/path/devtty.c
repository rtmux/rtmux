/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "externs.h"
#include "root-process.h"
#include <fcntl.h>
#include "pinode.h"
#include <private/root-proc-info.h>

static dev_t tty_devno;

static int tty_read(ioserv_context_t *ctp, io_read_t *msg, void *ocb)
{
    switch (msg->i.xtype & _IO_XTYPE_MASK) {
    case _IO_XTYPE_NONE:
        break;
    default:
        /* We don't handle this xtype, return error */
        return EINVAL;
    }
    _IO_SET_READ_NBYTES(ctp, 0);
    return EOK;
}

static int tty_write(ioserv_context_t *ctp, io_write_t *msg, void *ocb)
{
    switch (msg->i.xtype & _IO_XTYPE_MASK) {
    case _IO_XTYPE_NONE:
        break;
    default:
        /* We don't handle this xtype, return error */
        return EINVAL;
    }
    _IO_SET_WRITE_NBYTES(ctp, msg->i.nbytes);
    return EOK;
}

static int tty_stat(ioserv_context_t *ctp, io_stat_t *msg, void *ocb)
{
    memset(&msg->o, 0x00, sizeof msg->o);
    msg->o.st_ino = PINO_TTY;
    msg->o.st_size = 0;
    msg->o.st_ctime = msg->o.st_mtime = msg->o.st_atime = time(0);
    msg->o.st_mode = 0666 | S_IFCHR;
    msg->o.st_nlink = 1;
    msg->o.st_dev = (ctp->info.srcnd << RTMUX_NODE_BITS) | path_devno;
    msg->o.st_rdev = (ctp->info.srcnd << RTMUX_NODE_BITS) | tty_devno;
    return IOSERV_PTR(ctp, &msg->o, sizeof msg->o);
}

static const ioserv_ops_t tty_io_funcs = {
    .nfuncs = IOSERV_IO_NFUNCS,
    .read = tty_read,
    .write = tty_write,
    .stat = tty_stat,
};

static int tty_open(ioserv_context_t *ctp,
                    io_open_t *msg,
                    void *extra,
                    void *reserved)
{
    struct _io_connect_link_reply *linkp = (void *) msg;
    struct _server_info *info = (void *) (linkp + 1);
    io_openfd_t *new = (void *) (info + 1);
    unsigned eflag;
    unsigned ioflag;
    unsigned sflag;
    process_t *prp;
    root_proc_info_t qinfo;
    int sess_fd;

    if (((ioflag = msg->connect.ioflag) & (_IO_FLAG_RD | _IO_FLAG_WR)) == 0) {
        if (ioserv_open_bind(ctp, 0, 0) == -1) {
            return errno;
        }
        return EOK;
    }

    if (ctp->info.nd != RTMUX_LOCAL_NODE) {
        return EREMOTE;
    }

    /* prp lives in the kernel heap (PTE_U=0); U-mode roottask cannot deref
     * prp->session / sep->fd directly. Snapshot the session fd via
     * process_query_info, which runs in S-mode where the kernel heap (and the
     * kernel-heap-allocated session_t reachable through prp->session) is
     * mapped.
     */
    if (!(prp = root_proc_lock(ctp->info.pid))) {
        return EL2HLT;
    }
    if (process_query_info(ctp->info.pid, &qinfo) != EOK) {
        return proc_error(ESRCH, ctp->info.pid);
    }
    sess_fd = qinfo.sess_fd;
    if (!qinfo.session || sess_fd < 0) {
        return proc_error(ENXIO, ctp->info.pid);
    }
    root_proc_unlock(ctp->info.pid);
    _IO_SET_CONNECT_RET(ctp, _IO_CONNECT_RET_MSG);
    eflag = msg->connect.eflag;
    sflag = msg->connect.sflag;
    memset(linkp, 0x00, sizeof *linkp);
    linkp->eflag = (uint8_t) eflag;
    linkp->path_len = (uint16_t) sizeof *new;
    if (connect_server_info(0, (uint64_t) sess_fd, info) == -1) {
        return ENXIO;
    }
    memset(new, 0x00, sizeof *new);
    new->i.type = _IO_OPENFD;
    new->i.combine_len = sizeof new->i;
    new->i.sflag = (uint16_t) sflag;
    new->i.ioflag = ioflag;
    new->i.xtype = _IO_OPENFD_NONE;
    new->i.info.nd = ctp->info.srcnd;
    new->i.info.pid = getpid();
    new->i.info.chid = info->chid;
    new->i.info.scoid = info->scoid;
    new->i.info.coid = info->coid;
    SETIOV(ctp->iov + 0, new, offsetof(io_openfd_t, i.key));

    /* The key is the opener's thread secret over these bytes; a second open by
     * the same thread replaces it, so open and follow the link in turn.
     */
    if (msg_key_data_r(ctp->rcvid, RTMUX_KEYDATA_CALCULATE, 0, &new->i.key,
                       ctp->iov, 1) != EOK)
        return EIO;
    return IOSERV_PTR(ctp, msg, sizeof *linkp + sizeof *info + sizeof *new);
}

static const ioserv_connect_ops_t tty_connect_funcs = {
    .nfuncs = IOSERV_CONNECT_NFUNCS,
    .open = tty_open,
};

void devtty_init(void)
{
    ioserv_attach(dpp, NULL, "/dev/tty", 0, 0, &tty_connect_funcs,
                  &tty_io_funcs, 0);
    hwres_proc_devno("dev", &tty_devno, -1, 0);
}
