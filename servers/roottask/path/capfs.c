/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * capfs — read-only capability introspection mounted at /cap.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <align.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/iomsg.h>
#include <sys/mount.h>
#include <sys/pool-canary.h>
#include <sys/rtmux-core.h>
#include <sys/stat.h>
#include <devctl/common.h>
#include <cap-table.h>
#include "externs.h"

#define CAPFS_MAX_OCBS 8
#define CAPFS_SELF_BUFSZ 1024

static dev_t capfs_devno;

extern int ProcessCapStats(pid_t pid, rtmux_cap_stats_t *stats);
extern int ProcessEndpointCap(pid_t pid,
                              uint32_t coid,
                              rtmux_cap_t *cap,
                              uint32_t *rights);

struct capfs_ocb {
    POOL_CANARY_HEAD
    uint32_t mode;
    off_t pos;
    char data[CAPFS_SELF_BUFSZ];
    size_t size;
    POOL_CANARY_TAIL
};

struct capfs_counts {
    unsigned handle_count;
    unsigned fd_count;
    unsigned side_channel_count;
    unsigned endpoint_cap_count;
    rtmux_cap_t first_endpoint_cap;
    uint32_t first_endpoint_rights;
};

static struct capfs_ocb capfs_ocbs[CAPFS_MAX_OCBS];
static int capfs_ocb_inuse[CAPFS_MAX_OCBS];
static pthread_mutex_t capfs_ocb_lock = PTHREAD_MUTEX_INITIALIZER;

static struct capfs_ocb *capfs_ocb_alloc(void)
{
    int i;

    pthread_mutex_lock(&capfs_ocb_lock);
    for (i = 0; i < CAPFS_MAX_OCBS; i++) {
        if (!capfs_ocb_inuse[i]) {
            pool_canary_check(&capfs_ocbs[i], "capfs_ocb", i);
            capfs_ocb_inuse[i] = 1;
            memset(&capfs_ocbs[i], 0, sizeof(capfs_ocbs[i]));
            pool_canary_arm(&capfs_ocbs[i]);
            pthread_mutex_unlock(&capfs_ocb_lock);
            return &capfs_ocbs[i];
        }
    }
    pthread_mutex_unlock(&capfs_ocb_lock);
    return NULL;
}

static void capfs_ocb_free(struct capfs_ocb *ocb)
{
    int i;

    pthread_mutex_lock(&capfs_ocb_lock);
    for (i = 0; i < CAPFS_MAX_OCBS; i++) {
        if (&capfs_ocbs[i] == ocb) {
            pool_canary_check(ocb, "capfs_ocb", i);
            capfs_ocb_inuse[i] = 0;
            pthread_mutex_unlock(&capfs_ocb_lock);
            return;
        }
    }
    pthread_mutex_unlock(&capfs_ocb_lock);
}

static int capfs_resolve(const char *path)
{
    if (path[0] == '\0' || (path[0] == '/' && path[1] == '\0'))
        return 1;
    if (path[0] == '/')
        path++;
    return !strcmp(path, "self") ? 2 : 0;
}

static void capfs_count_handles(pid_t pid, struct capfs_counts *counts)
{
    struct _server_info info;
    int coid;
    int found;

    memset(counts, 0, sizeof(*counts));
    for (coid = 0;
         (found = connect_server_info(pid, (uint64_t) coid, &info)) != -1;) {
        rtmux_cap_t cap = 0;
        uint32_t rights = 0;

        counts->handle_count++;
        if (ProcessEndpointCap(pid, (uint32_t) found, &cap, &rights) == EOK &&
            cap != 0) {
            counts->endpoint_cap_count++;
            if (counts->first_endpoint_cap == 0) {
                counts->first_endpoint_cap = cap;
                counts->first_endpoint_rights = rights;
            }
        }
        if ((unsigned) found & RTMUX_SIDE_CHANNEL) {
            counts->side_channel_count++;
            coid = (int) (((unsigned) found & ~RTMUX_SIDE_CHANNEL) + 1U) |
                   (int) RTMUX_SIDE_CHANNEL;
        } else {
            counts->fd_count++;
            coid = found + 1;
        }
    }
}

static size_t capfs_synth_self(char *buf, size_t cap, pid_t pid)
{
    struct capfs_counts counts;
    rtmux_cap_stats_t stats;
    int n;

    capfs_count_handles(pid, &counts);
    if (ProcessCapStats(pid, &stats) != EOK)
        memset(&stats, 0, sizeof(stats));
    n = snprintf(
        buf, cap,
        "version 1\n"
        "pid %d\n"
        "scope self\n"
        "endpoint_rights send receive grant\n"
        "native_cap_slots 3\n"
        "transfer_max %u\n"
        "handle_count %u\n"
        "handle_fd_count %u\n"
        "handle_side_channel_count %u\n"
        "endpoint_cap_count %u\n"
        "first_endpoint_cap %llu\n"
        "first_endpoint_rights %#x\n"
        "cap_alloc_count %llu\n"
        "cap_delegate_count %llu\n"
        "cap_transfer_count %llu\n"
        "cap_revoke_count %llu\n"
        "cap_revoke_stale_count %llu\n"
        "cap_lookup_denied_count %llu\n"
        "retired_slots %llu\n"
        "last_denied %d\n"
        "redaction self-only\n",
        (int) pid, RTMUX_CAP_TRANSFER_MAX, counts.handle_count, counts.fd_count,
        counts.side_channel_count, counts.endpoint_cap_count,
        (unsigned long long) counts.first_endpoint_cap,
        counts.first_endpoint_rights, (unsigned long long) stats.alloc_count,
        (unsigned long long) stats.delegate_count,
        (unsigned long long) stats.transfer_count,
        (unsigned long long) stats.revoke_count,
        (unsigned long long) stats.revoke_stale_count,
        (unsigned long long) stats.lookup_denied_count,
        (unsigned long long) stats.retired_slots, stats.last_denied);
    if (n < 0)
        return 0;
    if ((size_t) n >= cap)
        return cap - 1;
    return (size_t) n;
}

static int capfs_open(ioserv_context_t *ctp,
                      io_open_t *msg,
                      void *extra,
                      void *reserved)
{
    struct capfs_ocb *ocb;
    int kind;

    (void) extra;
    (void) reserved;

    kind = capfs_resolve(msg->connect.path);
    if (kind == 0) {
        /* Non-existent path: creation cannot happen on this read-only
         * filesystem (EROFS with O_CREAT), otherwise it simply does not exist.
         * capfs has a flat namespace, so there is no prefix to distinguish --
         * ENOENT is the only lookup failure.
         */
        return (msg->connect.ioflag & O_CREAT) ? EROFS : ENOENT;
    }

    /* O_CREAT|O_EXCL on an existing path is EEXIST regardless of the read-only
     * medium (the file already exists, so no create is attempted).
     */
    if ((msg->connect.ioflag & (O_CREAT | O_EXCL)) == (O_CREAT | O_EXCL))
        return EEXIST;

    /* capfs is read-only, so any write intent -- a write access mode or O_TRUNC
     * (write intent even under O_RDONLY) -- fails at open with the
     * type-specific errno: the directory (kind 1) takes the more specific
     * EISDIR, a regular file EROFS.
     */
    if (msg->connect.ioflag & (_IO_FLAG_WR | O_TRUNC))
        return (kind == 1) ? EISDIR : EROFS;

    ocb = capfs_ocb_alloc();
    if (!ocb)
        return ENOMEM;

    if (kind == 1) {
        ocb->mode = S_IFDIR | 0555;
    } else {
        ocb->mode = S_IFREG | 0444;
        ocb->size =
            capfs_synth_self(ocb->data, sizeof(ocb->data), ctp->info.pid);
    }

    if (ioserv_open_bind(ctp, ocb, 0) == -1) {
        capfs_ocb_free(ocb);
        return errno;
    }
    return EOK;
}

static int capfs_readdir(ioserv_context_t *ctp, io_read_t *msg, void *handle)
{
    struct capfs_ocb *ocb = handle;
    struct dirent *de = (struct dirent *) (void *) msg;
    size_t nlen = sizeof("self") - 1;
    size_t reclen =
        lib_align_up_size(offsetof(struct dirent, d_name) + nlen + 1, 8);

    if (ocb->pos != 0 || msg->i.nbytes < reclen) {
        _IO_SET_READ_NBYTES(ctp, 0);
        return EOK;
    }

    memset(de, 0, reclen);
    de->d_ino = 1;
    de->d_reclen = (uint16_t) reclen;
    de->d_namelen = (uint16_t) nlen;
    memcpy(de->d_name, "self", nlen + 1);
    ocb->pos = 1;
    _IO_SET_READ_NBYTES(ctp, reclen);
    SETIOV(ctp->iov, de, reclen);
    return IOSERV_NPARTS(1);
}

static int capfs_read(ioserv_context_t *ctp, io_read_t *msg, void *handle)
{
    struct capfs_ocb *ocb = handle;
    size_t nbytes;

    if (!ocb)
        return EBADF;
    if (S_ISDIR(ocb->mode))
        return capfs_readdir(ctp, msg, handle);
    if ((msg->i.xtype & _IO_XTYPE_MASK) != _IO_XTYPE_NONE)
        return ENOSYS;
    if (ocb->pos >= (off_t) ocb->size) {
        _IO_SET_READ_NBYTES(ctp, 0);
        return EOK;
    }

    nbytes = ocb->size - (size_t) ocb->pos;
    if (nbytes > msg->i.nbytes)
        nbytes = msg->i.nbytes;
    SETIOV(ctp->iov, ocb->data + ocb->pos, nbytes);
    ocb->pos += (off_t) nbytes;
    _IO_SET_READ_NBYTES(ctp, nbytes);
    return IOSERV_NPARTS(1);
}

static int capfs_close_ocb(ioserv_context_t *ctp, void *reserved, void *handle)
{
    (void) ctp;
    (void) reserved;
    if (handle)
        capfs_ocb_free(handle);
    return EOK;
}

static void capfs_fill_stat(ioserv_context_t *ctp,
                            struct capfs_ocb *ocb,
                            struct stat *st)
{
    memset(st, 0, sizeof(*st));
    st->st_mode = ocb->mode;
    st->st_size = (off_t) ocb->size;
    st->st_nlink = 1;
    st->st_dev = (ctp->info.srcnd << RTMUX_NODE_BITS) | path_devno;
    st->st_rdev = (ctp->info.srcnd << RTMUX_NODE_BITS) | capfs_devno;
}

static int capfs_stat(ioserv_context_t *ctp, io_stat_t *msg, void *handle)
{
    struct capfs_ocb *ocb = handle;

    if (!ocb)
        return EBADF;
    capfs_fill_stat(ctp, ocb, &msg->o);
    return IOSERV_PTR(ctp, &msg->o, sizeof(msg->o));
}

static int capfs_native_msg(ioserv_context_t *ctp, io_msg_t *msg, void *handle)
{
    struct capfs_ocb *ocb = handle;
    struct _io_native_hdr *hdr = (struct _io_native_hdr *) (void *) msg;

    if (!ocb)
        return EBADF;
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
        capfs_fill_stat(ctp, ocb, st);
        return IOSERV_PTR(ctp, st, sizeof(*st));
    }
    case _IO_NATIVE_READ: {
        struct _io_native_read *rd = (struct _io_native_read *) (void *) msg;
        size_t nbytes;

        if (ctp->size < (int) sizeof(*rd))
            return EINVAL;
        if (S_ISDIR(ocb->mode))
            return EISDIR;
        if (rd->offset >= ocb->size) {
            SETIOV(ctp->iov, NULL, 0);
            return IOSERV_NPARTS(1);
        }
        nbytes = ocb->size - (size_t) rd->offset;
        if (nbytes > rd->nbytes)
            nbytes = rd->nbytes;
        SETIOV(ctp->iov, ocb->data + rd->offset, nbytes);
        return IOSERV_NPARTS(1);
    }
    case _IO_NATIVE_WRITE:
    case _IO_NATIVE_CTL_WRITE:
        return EROFS;
    default:
        return ENOSYS;
    }
}

static int capfs_devctl(ioserv_context_t *ctp, io_devctl_t *msg, void *handle)
{
    (void) ctp;
    (void) handle;

    if (msg->i.devctl_cmd == DEVCTL_SET_FLAGS)
        return EOK;
    if (msg->i.devctl_cmd == DEVCTL_GET_MOUNT_FLAGS) {
        int flags = _MOUNT_READONLY | _MOUNT_NOEXEC;

        memset(&msg->o, 0, sizeof(msg->o));
        msg->o.ret_val = EOK;
        msg->o.nbytes = sizeof(flags);
        memcpy(&msg->o + 1, &flags, sizeof(flags));
        return IOSERV_PTR(ctp, &msg->o, sizeof(msg->o) + sizeof(flags));
    }
    return ENOSYS;
}

static int capfs_sync(ioserv_context_t *ctp, io_sync_t *msg, void *ocb)
{
    (void) ctp;
    (void) msg;
    (void) ocb;

    /* capfs is read-only and RAM-backed: nothing to flush, so the sync is
     * trivially complete.
     */
    return EOK;
}

static int capfs_fdinfo(ioserv_context_t *ctp, io_fdinfo_t *msg, void *handle)
{
    struct capfs_ocb *ocb = handle;
    char *path = (char *) (&msg->o + 1);
    int len;
    size_t cap;

    (void) ctp;
    if (!ocb)
        return EBADF;

    cap = (size_t) msg->i.path_len;
    memset(&msg->o, 0, sizeof msg->o);
    msg->o.info.mode = ocb->mode;
    msg->o.info.offset = (uint64_t) ocb->pos;
    msg->o.info.size = (uint64_t) ocb->size;
    len = snprintf(path, cap, S_ISDIR(ocb->mode) ? "/cap" : "/cap/self");
    if (len < 0)
        return EINVAL;
    _IO_SET_FDINFO_LEN(ctp, len + 1);
    return IOSERV_PTR(ctp, &msg->o,
                      sizeof msg->o + (unsigned) min(cap, (size_t) len + 1));
}

static const ioserv_ops_t capfs_io_funcs = {
    .nfuncs = IOSERV_IO_NFUNCS,
    .read = capfs_read,
    .close_ocb = capfs_close_ocb,
    .stat = capfs_stat,
    .devctl = capfs_devctl,
    .fdinfo = capfs_fdinfo,
    .msg = capfs_native_msg,
    .sync = capfs_sync,
};

static const ioserv_connect_ops_t capfs_connect_funcs = {
    .nfuncs = IOSERV_CONNECT_NFUNCS,
    .open = capfs_open,
};

int capfs_init(void)
{
    ioserv_attr_t rattr;

    pool_canary_init(capfs_ocbs, CAPFS_MAX_OCBS);
    memset(&rattr, 0, sizeof(rattr));
    if (ioserv_attach(dpp, &rattr, "/cap", 0, IOSERV_FLAG_DIR,
                      &capfs_connect_funcs, &capfs_io_funcs, NULL) == -1) {
        root_log("capfs: ioserv_attach failed\n");
        return -1;
    }

    hwres_proc_devno("cap", &capfs_devno, -1, 0);
    return 0;
}
