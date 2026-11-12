
/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "externs.h"
#include <fcntl.h>
#include "pinode.h"

static struct pinfo {
    const char *path;
    int idx;
    int path_id;
    dev_t devno;
} pinfo[3] = {
    {"/dev/stdin", STDIN_FILENO},
    {"/dev/stdout", STDOUT_FILENO},
    {"/dev/stderr", STDERR_FILENO},
};


static int std_stat(ioserv_context_t *ctp, io_stat_t *msg, void *ocb)
{
    struct pinfo *pp;

    pp = ocb;

    memset(&msg->o, 0x00, sizeof msg->o);
    msg->o.st_ino = PINO_STDIN + (unsigned) pp->idx;
    msg->o.st_size = 0;
    msg->o.st_ctime = msg->o.st_mtime = msg->o.st_atime = time(0);
    msg->o.st_mode = 0666 | S_IFCHR;
    msg->o.st_nlink = 1;
    msg->o.st_dev = (ctp->info.srcnd << RTMUX_NODE_BITS) | path_devno;
    msg->o.st_rdev = (ctp->info.srcnd << RTMUX_NODE_BITS) | pp->devno;
    return IOSERV_PTR(ctp, &msg->o, sizeof msg->o);
}

static int std_open(ioserv_context_t *ctp,
                    io_open_t *msg,
                    void *hdl,
                    void *extra)
{
    struct _io_connect_link_reply *linkp;
    struct _server_info *info;
    io_dup_t *new;
    unsigned eflag;
    struct pinfo *pp;

    linkp = (void *) msg;
    info = (void *) (linkp + 1);
    new = (void *) (info + 1);

    pp = hdl;

    if ((msg->connect.ioflag & (_IO_FLAG_RD | _IO_FLAG_WR)) == 0) {
        if (ioserv_open_bind(ctp, pp, 0) == -1) {
            return errno;
        }
        return EOK;
    }

    if (ctp->info.nd != RTMUX_LOCAL_NODE) {
        return EREMOTE;
    }

    eflag = msg->connect.eflag;

    if (connect_server_info(ctp->info.pid, (uint64_t) pp->idx, info) !=
        pp->idx) {
        return EBADF;
    }

    memset(new, 0x00, sizeof(*new));
    new->i.type = _IO_DUP;
    new->i.combine_len = sizeof(new->i);
    new->i.info.nd = RTMUX_LOCAL_NODE;
    new->i.info.pid = ctp->info.pid;
    new->i.info.chid = info->chid;
    new->i.info.scoid = info->scoid;
    new->i.info.coid = pp->idx;

    _IO_SET_CONNECT_RET(ctp, _IO_CONNECT_RET_MSG);
    memset(linkp, 0x00, sizeof *linkp);
    linkp->eflag = (uint8_t) eflag;
    linkp->path_len = sizeof *new;
    return IOSERV_PTR(ctp, msg, sizeof *linkp + sizeof *info + sizeof *new);
}

static ioserv_connect_ops_t std_connect_funcs = {
    .nfuncs = IOSERV_CONNECT_NFUNCS,
    .open = std_open,
};

static ioserv_ops_t std_io_funcs = {
    .nfuncs = IOSERV_IO_NFUNCS,
    .stat = std_stat,
};

void devstd_init(void)
{
    int i;
    struct pinfo *pp;

    for (i = 0; i < sizeof(pinfo) / sizeof(pinfo[0]); i++) {
        pp = &pinfo[i];
        pp->path_id = ioserv_attach(dpp, NULL, pp->path, _FTYPE_ANY, 0,
                                    &std_connect_funcs, &std_io_funcs, pp);
        hwres_proc_devno("dev", &pp->devno, -1, 0);
    }
}
