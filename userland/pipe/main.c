/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * Pipe I/O service.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

struct pipe_ocb;
#define IOSERV_OCB_T struct pipe_ocb
struct pipe;
#define IOSERV_HANDLE_T struct pipe

#include <stdbool.h>
#include <limits.h>
#include <minmax.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <endian.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/poll.h>
#include <devctl/tty.h>
#include <devctl/common.h>
#include <sys/ioserv.h>
#include <sys/ftype.h>
#include <sys/iomsg.h>
#include <sys/node.h>
#include <sys/rtmux-core.h>
#include <sys/siginfo.h>
#include <sys/stat.h>

#define PIPE_NAME "/dev/pipe"
#define PIPE_ATOMIC ((size_t) max(PIPE_BUF, _POSIX_PIPE_BUF))
#define PIPE_SIZE PIPE_ATOMIC
#define PIPE_STACKSIZE (12 * 1024)

#define OCB2PIPE(_ocb) (pipe_t *) ((_ocb)->attr)
#define LOCK(_hdr) pipe_attr_lock(&((_hdr)->attr))
#define UNLOCK(_hdr) pipe_attr_unlock(&((_hdr)->attr))
#define TRYLOCK(_hdr) pipe_attr_trylock(&((_hdr)->attr))
#define ATTR_DESTROY 0x00010000
#define PIPE_ATTR_ATIME 0x00000001
#define PIPE_ATTR_MTIME 0x00000002
#define PIPE_ATTR_CTIME 0x00000004
#define PIPE_NOTIFY_INPUT 0
#define PIPE_NOTIFY_OUTPUT 1
#define PIPE_NOTIFY_OBAND 2
#define PIPE_NOTIFY_COUNT 3
#define PIPE_OCB_LAST_INUSE 0x0001
#define PIPE_OCB_LAST_READER 0x0002
#define PIPE_OCB_LAST_WRITER 0x0004
#define PIPE_PC_CHOWN_RESTRICTED 0x0001
#define PIPE_PC_NO_TRUNC 0x0002
#define PIPE_PC_SYNC_IO 0x0004
#define PIPE_PC_LINK_DIR 19

static const char *program_name = "pipe";

typedef struct pipe_mount {
    dev_t dev;
    unsigned conf;
    int32_t blocksize;
} pipe_mount_t;

typedef struct pipe_attr {
    pipe_mount_t *mount;
    uint32_t flags;
    mode_t mode;
    uid_t uid;
    gid_t gid;
    nlink_t nlink;
    dev_t rdev;
    ino_t inode;
    off_t nbytes;
    uint16_t count;
    uint16_t rcount;
    uint16_t wcount;
    time_t atime;
    time_t mtime;
    time_t ctime;
    pthread_mutex_t mutex;
} pipe_attr_t;

typedef struct pipe_notify_event {
    struct pipe_notify_event *next;
    int rcvid;
    int scoid;
    int coid;
    int cnt;
    unsigned flags;
    struct sigevent event;
} pipe_notify_event_t;

typedef struct pipe_notify {
    int cnt;
    pipe_notify_event_t *list;
} pipe_notify_t;

struct pipe_ocb {
    pipe_attr_t *attr;
    int32_t ioflag;
    off_t offset;
};

typedef struct blocked {
    int rcvid;
    int scoid;
    int coid;
    size_t offset;
    size_t size;
    size_t nbytes;
    struct blocked *link;
} blocked_t;

typedef struct pipe {
    pipe_attr_t attr;
    pipe_notify_t notify[PIPE_NOTIFY_COUNT];
    struct blocked *waiting[PIPE_NOTIFY_COUNT];
    char *name;
    int fd;
    dev_t devno;
    struct pipe *link;
    size_t rd;
    size_t wr;
    char *buffer;
} pipe_t;

static void *Dispatch;
static thread_pool_t *Pool;
static pipe_mount_t Mount;
static pipe_t Pipes, Fifos;
static ioserv_connect_ops_t PipeConnectFuncs, FifoConnectFuncs;
static ioserv_ops_t PipeIoFuncs;
static ioserv_attr_t IoServiceAttr;
static thread_pool_attr_t PoolAttr;
static pthread_attr_t thread_attr;
static struct sigevent Event;
static size_t BufferSize, AtomicSize;
static int SelWrite1;
static int NotifyCounts[PIPE_NOTIFY_COUNT];

static int pipe_attr_lock(pipe_attr_t *attr)
{
    return pthread_mutex_lock(&attr->mutex);
}

static int pipe_attr_trylock(pipe_attr_t *attr)
{
    int status;

    status = pthread_mutex_trylock(&attr->mutex);
    return status == EOK ? EOK : EBUSY;
}

static int pipe_attr_unlock(pipe_attr_t *attr)
{
    return pthread_mutex_unlock(&attr->mutex);
}

static void pipe_notify_init(pipe_notify_t *notify)
{
    int i;

    for (i = 0; i < PIPE_NOTIFY_COUNT; ++i) {
        notify[i].cnt = INT_MAX;
        notify[i].list = NULL;
    }
}

static void pipe_attr_init(pipe_attr_t *attr,
                           mode_t mode,
                           pipe_attr_t *dattr,
                           struct _client_info *info)
{
    memset(attr, 0, sizeof(*attr));
    attr->mount = dattr ? dattr->mount : NULL;
    attr->flags = PIPE_ATTR_ATIME | PIPE_ATTR_MTIME | PIPE_ATTR_CTIME;
    attr->uid = info ? info->cred.euid : getuid();
    attr->mode = mode;
    if (dattr && (dattr->mode & S_ISGID)) {
        attr->gid = dattr->gid;
        if (S_ISDIR(mode))
            attr->mode |= S_ISGID;
    } else {
        attr->gid = info ? info->cred.egid : getgid();
    }
    attr->nlink = 1;
    pthread_mutex_init(&attr->mutex, NULL);
}

static int pipe_client_info(ioserv_context_t *ctp,
                            int ioflag,
                            struct _client_info *info)
{
    int status;

    status = connect_client_info_r(ctp->info.scoid, info, NGROUPS_MAX);
    if (status != EOK)
        return status;
    if (ioflag & O_REALIDS) {
        uid_t uid;
        gid_t gid;

        uid = info->cred.ruid;
        info->cred.ruid = info->cred.euid;
        info->cred.euid = uid;
        gid = info->cred.rgid;
        info->cred.rgid = info->cred.egid;
        info->cred.egid = gid;
    }
    return EOK;
}

static int pipe_check_access(const pipe_attr_t *attr,
                             mode_t check,
                             const struct _client_info *info)
{
    mode_t mode;
    unsigned n;

    if (!info)
        return ENOSYS;
    if (info->cred.euid == 0)
        return EOK;
    if ((check & S_ISUID) && info->cred.euid == attr->uid)
        return EOK;
    if (check & S_ISGID) {
        if (info->cred.egid == attr->gid)
            return EOK;
        for (n = 0; n < info->cred.ngroups; n++) {
            if (info->cred.grouplist[n] == attr->gid)
                return EOK;
        }
    }
    check &= S_IRWXU;
    if (check == 0)
        return EPERM;
    if (info->cred.euid == attr->uid) {
        mode = attr->mode;
    } else if (info->cred.egid == attr->gid) {
        mode = attr->mode << 3;
    } else {
        mode = attr->mode << 6;
        for (n = 0; n < info->cred.ngroups; n++) {
            if (info->cred.grouplist[n] == attr->gid) {
                mode = attr->mode << 3;
                break;
            }
        }
    }
    return (mode & check) == check ? EOK : EACCES;
}

static int pipe_time_update(pipe_attr_t *attr)
{
    if (attr->flags & (PIPE_ATTR_ATIME | PIPE_ATTR_MTIME | PIPE_ATTR_CTIME)) {
        time_t now;

        now = time(NULL);
        if (attr->flags & PIPE_ATTR_ATIME)
            attr->atime = now;
        if (attr->flags & PIPE_ATTR_MTIME)
            attr->mtime = now;
        if (attr->flags & PIPE_ATTR_CTIME)
            attr->ctime = now;
        attr->flags &=
            ~(uint32_t) (PIPE_ATTR_ATIME | PIPE_ATTR_MTIME | PIPE_ATTR_CTIME);
    }
    return EOK;
}

static int pipe_open_check(ioserv_context_t *ctp,
                           io_open_t *msg,
                           pipe_attr_t *attr,
                           struct _client_info *info)
{
    unsigned ioflag =
        msg->connect.ioflag & (~(unsigned) _IO_FLAG_MASK | msg->connect.access);
    if ((ioflag & (O_SYNC | O_DSYNC | O_RSYNC)) &&
        (!attr->mount || (attr->mount->conf & PIPE_PC_SYNC_IO) == 0))
        return EINVAL;
    if ((ioflag & (O_CREAT | O_EXCL)) == (O_CREAT | O_EXCL))
        return EEXIST;
    if ((ioflag & (_IO_FLAG_WR | O_TRUNC)) == O_TRUNC)
        msg->connect.ioflag &= ~(unsigned) O_TRUNC;
    if ((ioflag & _IO_FLAG_WR) && S_ISDIR(attr->mode))
        return EISDIR;
    if (info) {
        mode_t mode = 0;
        if (ioflag & _IO_FLAG_RD)
            mode |= S_IRUSR;
        if (ioflag & _IO_FLAG_WR)
            mode |= S_IWUSR;
        if (mode != 0) {
            int status = pipe_check_access(attr, mode, info);
            if (status != EOK)
                return status;
        }
    }
    return EOK;
}

static int pipe_ocb_attach(ioserv_context_t *ctp,
                           io_open_t *msg,
                           pipe_attr_t *attr,
                           const ioserv_ops_t *io_funcs)
{
    struct pipe_ocb *ocb;

    ocb = calloc(1, sizeof(*ocb));
    if (!ocb)
        return errno;
    ocb->ioflag = (int32_t) (msg->connect.ioflag &
                             (~(unsigned) _IO_FLAG_MASK | msg->connect.access));
    ocb->attr = attr;
    if (ioserv_open_bind(ctp, ocb, io_funcs) == -1) {
        int status;

        status = errno;
        free(ocb);
        return status;
    }
    if (ocb->ioflag & _IO_FLAG_WR)
        attr->wcount++;
    if (ocb->ioflag & _IO_FLAG_RD)
        attr->rcount++;
    attr->count++;
    return EOK;
}

static int pipe_ocb_detach(struct pipe_ocb *ocb)
{
    pipe_attr_t *attr;
    unsigned flags;

    attr = ocb->attr;
    pipe_time_update(attr);
    flags = 0;
    if ((ocb->ioflag & _IO_FLAG_WR) && --attr->wcount == 0)
        flags |= PIPE_OCB_LAST_WRITER;
    if ((ocb->ioflag & _IO_FLAG_RD) && --attr->rcount == 0)
        flags |= PIPE_OCB_LAST_READER;
    if (--attr->count == 0)
        flags |= PIPE_OCB_LAST_INUSE;
    return (int) flags;
}

static pipe_notify_event_t *pipe_notify_lookup(int rcvid, pipe_notify_t *notify)
{
    pipe_notify_event_t *event;

    for (event = notify->list; event; event = event->next) {
        if (event->rcvid == rcvid)
            return event;
    }
    event = malloc(sizeof(*event));
    if (!event)
        return NULL;
    event->next = notify->list;
    notify->list = event;
    return event;
}

static int pipe_notify_input_check(pipe_notify_t *notify, int cnt, int trans)
{
    return notify[PIPE_NOTIFY_INPUT].cnt <= cnt &&
           (notify[PIPE_NOTIFY_INPUT].cnt || trans);
}

static int pipe_notify_output_check(pipe_notify_t *notify, int cnt)
{
    return notify[PIPE_NOTIFY_OUTPUT].cnt <= cnt;
}

static int pipe_notify(ioserv_context_t *ctp,
                       io_notify_t *msg,
                       pipe_notify_t *notify,
                       int trig,
                       int hup,
                       const int *notifycnts)
{
    pipe_notify_event_t *event;
    unsigned msgflags, trig_u, ext;
    int i, action;

    trig_u = (unsigned) trig;
    trig_u |= trig_u << 28;
    trig_u &= ~(unsigned) _NOTIFY_COND_EXTEN;
    trig_u |= trig_u >> 28;

    action = msg->i.action;
    msgflags = msg->i.flags;
    ext = msgflags & (unsigned) _NOTIFY_COND_EXTEN;
    msgflags &= ~(unsigned) _NOTIFY_COND_EXTEN;

    if (action != _NOTIFY_ACTION_TRANARM) {
        trig_u &= msgflags;
    } else {
        for (event = notify[PIPE_NOTIFY_INPUT].list; event;
             event = event->next) {
            if (SIGEV_GET_TYPE(&event->event) != SIGEV_NONE) {
                if (event->scoid != ctp->info.scoid ||
                    SIGEV_GET_TYPE(&msg->i.event) != SIGEV_NONE)
                    return EBUSY;
            }
        }
        trig_u = 0;
    }

    msgflags |= msgflags >> 28;
    msgflags &= ((1u << _NOTIFY_MAXCOND) - 1u);
    for (i = 0; i < PIPE_NOTIFY_COUNT && msgflags; i++, msgflags >>= 1) {
        if ((msgflags & 1u) == 0)
            continue;
        event = pipe_notify_lookup(ctp->rcvid, &notify[i]);
        if (!event)
            return ENOMEM;
        event->rcvid = ctp->rcvid;
        event->scoid = ctp->info.scoid;
        event->coid = ctp->info.coid;
        event->flags = ext;
        if (trig_u == 0 && action != _NOTIFY_ACTION_POLL) {
            event->event = msg->i.event;
            event->cnt = action == _NOTIFY_ACTION_TRANARM ? 0 : notifycnts[i];
            if (event->cnt < notify[i].cnt)
                notify[i].cnt = event->cnt;
        } else {
            SIGEV_NONE_INIT(&event->event);
            event->cnt = INT_MAX;
        }
    }

    /* Report hangup/error as extended (POLL*) flags via flags_exten -- they do
     * not fit the four-bit _NOTIFY_COND nibble. A poll client reads them as
     * output-only revents (POLLHUP/POLLERR), which is why a set hup also makes
     * the descriptor "ready" here regardless of the requested conditions.
     */
    msg->o.flags_exten = (uint32_t) hup;
    if ((msg->o.flags = (uint32_t) trig_u) != 0)
        msg->o.flags |= (uint32_t) ext;
    if (action & _NOTIFY_ACTION_POLL)
        return IOSERV_PTR(ctp, &msg->o, sizeof(msg->o));
    if (trig_u || hup || action != _NOTIFY_ACTION_CONDARM)
        return IOSERV_PTR(ctp, &msg->o, sizeof(msg->o));
    return EAGAIN;
}

static void pipe_notify_remove(ioserv_context_t *ctp, pipe_notify_t *notify)
{
    pipe_notify_event_t *prev, *event;
    int i;

    for (i = 0; i < PIPE_NOTIFY_COUNT; ++i) {
        prev = (pipe_notify_event_t *) &notify[i].list;
        while ((event = prev->next)) {
            if (!ctp || (event->scoid == ctp->info.scoid &&
                         event->coid == ctp->info.coid)) {
                prev->next = event->next;
                free(event);
            } else {
                prev = event;
            }
        }
    }
}

static void pipe_notify_trigger(pipe_notify_t *notify, int cnt, int index)
{
    pipe_notify_event_t *event;

    notify += index;
    notify->cnt = INT_MAX;
    for (event = notify->list; event; event = event->next) {
        if (SIGEV_GET_TYPE(&event->event) == SIGEV_NONE)
            continue;
        if (event->cnt <= cnt) {
            if (SIGEV_GET_TYPE(&event->event) >= SIGEV_SIGNAL &&
                SIGEV_GET_TYPE(&event->event) <= SIGEV_NOTIFICATION &&
                event->event.sigev_code == SI_NOTIFY) {
                event->event.sigev_value.sival_int |=
                    (int) ((unsigned) _NOTIFY_COND_INPUT << index);
                if (event->flags & _NOTIFY_COND_EXTEN) {
                    event->event.sigev_value.sival_int |=
                        (int) _NOTIFY_COND_EXTEN;
                    event->event.sigev_code =
                        (short) -(int) (_NOTIFY_CONDE_RDNORM << index);
                }
            }
            (void) msg_deliver_event(event->rcvid, &event->event);
            event->event.sigev_notify = SIGEV_NONE;
            event->cnt = INT_MAX;
        }
        if (notify->cnt > event->cnt)
            notify->cnt = event->cnt;
    }
}

static int pipe_read_verify(io_read_t *msg, struct pipe_ocb *ocb, int *nonblock)
{
    if ((ocb->ioflag & _IO_FLAG_RD) == 0)
        return EBADF;
    *nonblock = (ocb->ioflag & O_NONBLOCK) ||
                ((msg->i.xtype & (_IO_XFLAG_NONBLOCK | _IO_XFLAG_BLOCK)) ==
                 _IO_XFLAG_NONBLOCK);
    return EOK;
}

static int pipe_write_verify(io_write_t *msg,
                             struct pipe_ocb *ocb,
                             int *nonblock)
{
    if ((ocb->ioflag & _IO_FLAG_WR) == 0)
        return EBADF;
    if (S_ISDIR(ocb->attr->mode))
        return EISDIR;
    *nonblock = (ocb->ioflag & O_NONBLOCK) ||
                ((msg->i.xtype & (_IO_XFLAG_NONBLOCK | _IO_XFLAG_BLOCK)) ==
                 _IO_XFLAG_NONBLOCK);
    return EOK;
}

static int pipe_stat(ioserv_context_t *ctp, pipe_attr_t *attr, struct stat *st)
{
    st->st_ctime = attr->ctime;
    st->st_mtime = attr->mtime;
    st->st_atime = attr->atime;
    st->st_gid = attr->gid;
    st->st_uid = attr->uid;
    st->st_nlink = attr->nlink;
    st->st_dev = attr->mount ? attr->mount->dev : 0;
    st->st_blocksize = attr->mount && attr->mount->blocksize > 0
                           ? (blksize_t) attr->mount->blocksize
                           : 1;
    if (st->st_dev == 0) {
        if (!S_ISNAM(attr->mode))
            st->st_dev = attr->rdev;
        if (st->st_dev == 0)
            (void) ioserv_devino(ctp->id, &st->st_dev, &attr->inode);
    }
    st->st_ino = attr->inode;
    st->st_dev = st->st_dev ? (ctp->info.srcnd << RTMUX_NODE_BITS) |
                                  (st->st_dev & RTMUX_NODE_MASK)
                            : 0;
    st->st_rdev = (attr->rdev && (S_ISCHR(attr->mode) || S_ISBLK(attr->mode)))
                      ? (ctp->info.srcnd << RTMUX_NODE_BITS) |
                            (attr->rdev & RTMUX_NODE_MASK)
                      : attr->rdev;
    st->st_mode = attr->mode;
    st->st_nblocks =
        (int32_t) (((blksize_t) attr->nbytes + st->st_blocksize - 1) /
                   st->st_blocksize);
    st->st_blksize = max_ulong(st->st_blocksize, 512);
    st->st_blocks = (blkcnt_t) (attr->nbytes + 512 - 1) / 512;
    st->st_size = S_ISBLK(attr->mode) ? (off_t) st->st_blocks : attr->nbytes;
    return EOK;
}

static int pipe_devctl(ioserv_context_t *ctp,
                       io_devctl_t *msg,
                       struct pipe_ocb *ocb)
{
    union {
        int oflag;
        int mountflag;
    } *data;
    unsigned nbytes;

    data = (void *) (msg + 1);
    nbytes = 0;
    switch ((unsigned) msg->i.devctl_cmd) {
    case (unsigned) DEVCTL_GET_FLAGS:
        data->oflag =
            (ocb->ioflag & O_SETFLAG) | ((ocb->ioflag - 1) & O_ACCMODE);
        nbytes = sizeof(data->oflag);
        break;
    case (unsigned) DEVCTL_SET_FLAGS:
        if (data->oflag & (O_SYNC | O_DSYNC | O_RSYNC)) {
            if (!ocb->attr->mount ||
                (ocb->attr->mount->conf & PIPE_PC_SYNC_IO) == 0)
                return EINVAL;
        }
        ocb->ioflag = (ocb->ioflag & ~O_SETFLAG) | (data->oflag & O_SETFLAG);
        break;
    case (unsigned) DEVCTL_GET_MOUNT_FLAGS:
        data->mountflag = 0;
        nbytes = sizeof(data->mountflag);
        break;
    default:
        return IOSERV_DEFAULT;
    }
    if (nbytes) {
        msg->o.ret_val = EOK;
        return IOSERV_PTR(ctp, &msg->o, sizeof(msg->o) + nbytes);
    }
    return EOK;
}

static int pipe_pathconf(ioserv_context_t *ctp, io_pathconf_t *msg)
{
    int value;

    switch (msg->i.name) {
    case _PC_CHOWN_RESTRICTED:
        value = 1;
        break;
    case _PC_NO_TRUNC:
        value = 1;
        break;
    case _PC_SYNC_IO:
        value = 1;
        break;
    case PIPE_PC_LINK_DIR:
        value = 0;
        break;
    case _PC_SYMLOOP_MAX:
        value = SYMLOOP_MAX;
        break;
    case _PC_ASYNC_IO:
        value = _POSIX_ASYNC_IO;
        break;
    case _PC_PRIO_IO:
        value = _POSIX_PRIO_IO;
        break;
    default:
        return IOSERV_DEFAULT;
    }
    _IO_SET_PATHCONF_VALUE(ctp, value);
    return EOK;
}

static void pipe_fdinfo(struct pipe_ocb *ocb,
                        pipe_attr_t *attr,
                        struct _fdinfo *info)
{
    info->mode = attr->mode;
    info->ioflag = (uint32_t) ocb->ioflag;
    info->offset = (uint64_t) ocb->offset;
    info->size = (uint64_t) attr->nbytes;
    info->flags = 0;
    info->sflag = 0;
    info->count = attr->count;
    info->rcount = attr->rcount;
    info->wcount = attr->wcount;
    info->rlocks = 0;
    info->wlocks = 0;
    memset(info->zero, 0, sizeof(info->zero));
}

static int pipe_open_default(ioserv_context_t *ctp,
                             io_open_t *msg,
                             pipe_t *pipe,
                             const ioserv_ops_t *funcs)
{
    int error;

    pipe_attr_lock(&pipe->attr);
    error = pipe_open_check(ctp, msg, &pipe->attr, NULL);
    if (error == EOK)
        error = pipe_ocb_attach(ctp, msg, &pipe->attr, funcs);
    pipe_attr_unlock(&pipe->attr);
    return error;
}

static int pipe_openfd(ioserv_context_t *ctp,
                       io_openfd_t *msg,
                       struct pipe_ocb *ocb,
                       int perms)
{
    io_open_t open_msg;
    int error;

    memset(&open_msg, 0, sizeof(open_msg));
    open_msg.connect.ioflag =
        (msg->i.ioflag | (unsigned) perms) &
        (_IO_FLAG_MASK | O_SETFLAG | O_TRUNC | O_NONBLOCK);
    open_msg.connect.sflag = 0;
    open_msg.connect.access =
        (uint32_t) ((ocb->ioflag | perms) & _IO_FLAG_MASK);
    if ((open_msg.connect.ioflag & open_msg.connect.access) !=
        (open_msg.connect.ioflag & _IO_FLAG_MASK))
        return EACCES;
    error = pipe_open_check(ctp, &open_msg, ocb->attr, NULL);
    if (error != EOK)
        return error;
    msg->i.ioflag = open_msg.connect.ioflag;
    return pipe_ocb_attach(ctp, &open_msg, ocb->attr,
                           ioserv_binding_funcs(ctp, &msg->i.info));
}

static void pipe_open_abort(ioserv_context_t *ctp)
{
    struct pipe_ocb *ocb;

    ocb = ioserv_ocb(ctp, &ctp->info);
    if (ocb) {
        (void) ioserv_unbind(&ctp->info);
        (void) pipe_ocb_detach(ocb);
        free(ocb);
    }
}

/* lookup_pipe() - find a pipe/fifo by unique dev/ino pair
 *
 * @head: list head (Pipes or Fifos) @dev: device number @ino: inode number
 * @destroy: ATTR_DESTROY flag to match, or 0
 *
 * Returns the locked pipe, or NULL.
 */
static pipe_t *lookup_pipe(pipe_t *head, dev_t dev, ino_t ino, int destroy)
{
    pipe_t *p;

    for (p = head->link; p; p = p->link) {
        if (p->devno == dev && p->attr.inode == ino) {
            pipe_attr_lock(&p->attr);
            if ((p->attr.flags & ATTR_DESTROY) == destroy)
                break;
            pipe_attr_unlock(&p->attr);
        }
    }
    return p;
}

/* update_pipe() - flush dirty timestamps on a pipe/fifo
 *
 * @p: pipe to update @force: if nonzero, refresh pending timestamps
 */
static void update_pipe(pipe_t *p, bool force)
{
    if (force)
        pipe_time_update(&p->attr);
}

/* create_pipe() - allocate a new pipe/fifo and add it to the list
 *
 * @head: list head (Pipes or Fifos) @name: pathname (NULL for unnamed pipes,
 * strdup'd for FIFOs) @mode: permission bits @owner: client info for ownership
 * (may be NULL)
 *
 * Returns the new pipe (locked), or NULL on failure.
 */
static pipe_t *create_pipe(pipe_t *head,
                           char *name,
                           mode_t mode,
                           struct _client_info *owner)
{
    pipe_t *p;

    if ((p = malloc(sizeof(pipe_t)))) {
        if (!(p->name = name) || (p->name = strdup(name))) {
            if ((p->buffer = malloc(BufferSize))) {
                pipe_attr_init(&p->attr, S_IFIFO | (mode & 0777), &head->attr,
                               owner);
                p->devno = head->attr.mount->dev;
                p->attr.inode = (uintptr_t) p;
                pipe_notify_init(p->notify);
                p->fd = -1;
                p->waiting[PIPE_NOTIFY_INPUT] = NULL;
                p->waiting[PIPE_NOTIFY_OUTPUT] = NULL;
                p->waiting[PIPE_NOTIFY_OBAND] = NULL;
                p->rd = p->wr = 0;
                pipe_attr_lock(&p->attr);
                LOCK(head);
                p->link = head->link;
                head->link = p;
                ++head->attr.nbytes;
                UNLOCK(head);
                return p;
            }
            free(p->name);
        }
        free(p);
    }
    return NULL;
}

/* destroy_pipe() - release a pipe/fifo and remove it from the list
 *
 * @head: list head (locked by caller) @p: pipe to destroy @error: status to
 * return
 *
 * Returns error.
 */
static int destroy_pipe(pipe_t *head, pipe_t *p, int error)
{
    pipe_t **pp;

    LOCK(head);
    for (pp = &head->link; *pp != p; pp = &(*pp)->link) {
        if (!*pp) {
            UNLOCK(head);
            return error;
        }
    }
    *pp = p->link;
    --head->attr.nbytes;
    UNLOCK(head);
    if (error == EOK)
        update_pipe(p, true);
    if (p->fd != -1)
        close(p->fd);
    pipe_notify_remove(NULL, p->notify);
    pipe_attr_unlock(&p->attr);
    pthread_mutex_destroy(&p->attr.mutex);
    free(p->buffer);
    free(p->name);
    free(p);
    return error;
}

/* acquire_pipe() - find or create a FIFO by name
 *
 * @head: list head (Fifos) @name: pathname of the FIFO @pipe: output — the
 * found/created pipe (locked)
 *
 * Returns EOK on success, or an errno value.
 */
static int acquire_pipe(pipe_t *head, char *name, pipe_t **pipe)
{
    pipe_t *p;
    int error;

    LOCK(head);
    if ((p = lookup_pipe(head, head->attr.mount->dev, (ino_t) (uintptr_t) name,
                         0))) {
        *pipe = p;
        error = EOK;
    } else if ((p = create_pipe(head, name, head->attr.mode, NULL))) {
        *pipe = p;
        error = EOK;
    } else {
        error = ENOMEM;
    }
    UNLOCK(head);
    return error;
}

/* release_pipe() - release a pipe/fifo, coordinating with concurrent lookups
 *
 * @p: pipe to release @error: status code
 */
static void release_pipe(pipe_t *p, int error)
{
    pipe_t *head, *find;
    dev_t dev;
    ino_t ino;

    if (!S_ISFIFO(p->attr.mode) || p->attr.count != 0) {
        pipe_attr_unlock(&p->attr);
    } else {
        head = (p->name) ? &Fifos : &Pipes;
        p->attr.flags |= ATTR_DESTROY;
        if (TRYLOCK(head) != EBUSY) {
            destroy_pipe(head, p, error);
        } else {
            dev = p->devno;
            ino = p->attr.inode;
            pipe_attr_unlock(&p->attr);
            LOCK(head);
            find = lookup_pipe(head, dev, ino, ATTR_DESTROY);
            if (find) {
                if (find != p || p->attr.count != 0)
                    pipe_attr_unlock(&find->attr);
                else
                    destroy_pipe(head, p, error);
            }
        }
        UNLOCK(head);
    }
}

/* read_from_pipe() - transfer data from the pipe buffer to the client
 *
 * @pipe: pipe to read from @rcvid: client's receive ID @nbytes: maximum bytes
 * to transfer @offset: byte offset into the client's reply buffer
 *
 * Returns bytes transferred, or negative errno.
 */
static ssize_t read_from_pipe(pipe_t *pipe,
                              int rcvid,
                              size_t nbytes,
                              size_t offset)
{
    iov_t iov[2];
    size_t avail, contig;
    int nparts;

    avail = min((size_t) pipe->attr.nbytes, nbytes);
    if ((contig = BufferSize - pipe->rd) >= avail) {
        nparts = 1;
        SETIOV(&iov[0], &pipe->buffer[pipe->rd], avail);
    } else {
        nparts = 2;
        SETIOV(&iov[0], &pipe->buffer[pipe->rd], contig);
        SETIOV(&iov[1], &pipe->buffer[0], avail - contig);
    }
    if (offset == 0 && avail == nbytes) {
        if (msg_replyv(rcvid, (int) nbytes, iov, nparts) == -1)
            return -errno;
    } else {
        if (msg_writev(rcvid, iov, nparts, (int) offset) == -1)
            return -errno;
    }
    pipe->rd = (pipe->rd + avail) % BufferSize;
    pipe->attr.nbytes -= (off_t) avail;
    pipe->attr.flags |= PIPE_ATTR_ATIME;
    return (ssize_t) avail;
}

/* write_to_pipe() - transfer data from the client into the pipe buffer
 *
 * @pipe: pipe to write to @rcvid: client's receive ID @nbytes: maximum bytes to
 * transfer @offset: byte offset into the client's message @ctp: I/O service
 * context (may be NULL for deferred writes)
 *
 * Returns bytes transferred, or negative errno.
 */
static ssize_t write_to_pipe(pipe_t *pipe,
                             int rcvid,
                             size_t nbytes,
                             size_t offset,
                             ioserv_context_t *ctp)
{
    iov_t iov[2];
    size_t room, contig;
    int nparts;

    room = min(BufferSize - (size_t) pipe->attr.nbytes, nbytes);
    if ((contig = BufferSize - pipe->wr) >= room) {
        nparts = 1;
        SETIOV(&iov[0], &pipe->buffer[pipe->wr], room);
    } else {
        nparts = 2;
        SETIOV(&iov[0], &pipe->buffer[pipe->wr], contig);
        SETIOV(&iov[1], &pipe->buffer[0], room - contig);
    }
    if (ctp && (size_t) ctp->size >= offset + room && nparts == 1) {
        memcpy(GETIOVBASE(&iov[0]), (char *) ctp->msg + offset,
               GETIOVLEN(&iov[0]));
    } else {
        if (msg_readv(rcvid, iov, nparts, (int) offset) == -1)
            return -errno;
    }
    pipe->wr = (pipe->wr + room) % BufferSize;
    pipe->attr.nbytes += (off_t) room;
    pipe->attr.flags |= PIPE_ATTR_MTIME | PIPE_ATTR_CTIME;
    return (ssize_t) room;
}

/* check_readers() - unblock any readers that can now be satisfied
 *
 * @pipe: pipe to check @trans: nonzero if this is a transition (data just
 * arrived)
 */
static void check_readers(pipe_t *pipe, int trans)
{
    blocked_t *b, **bb;
    size_t avail;
    ssize_t chunk;

    bb = &pipe->waiting[PIPE_NOTIFY_INPUT];
    while ((avail = (size_t) pipe->attr.nbytes) != 0 && (b = *bb)) {
        if ((chunk = read_from_pipe(pipe, b->rcvid, b->size, 0)) < 0)
            msg_error(b->rcvid, (int) -chunk);
        else if ((size_t) chunk != b->size)
            msg_reply(b->rcvid, (int) chunk, NULL, 0);
        *bb = b->link;
        free(b);
    }
    if (avail != 0 && pipe_notify_input_check(pipe->notify, (int) avail, trans))
        pipe_notify_trigger(pipe->notify, (int) avail, PIPE_NOTIFY_INPUT);
}

/* check_writers() - unblock any writers that can now make progress
 *
 * @pipe: pipe to check
 */
static void check_writers(pipe_t *pipe)
{
    blocked_t *b, **bb;
    size_t room;
    ssize_t chunk;

    bb = &pipe->waiting[PIPE_NOTIFY_OUTPUT];
    while ((room = BufferSize - (size_t) pipe->attr.nbytes) != 0 && (b = *bb)) {
        if (b->size > AtomicSize || b->nbytes <= room) {
            chunk = write_to_pipe(pipe, b->rcvid, b->nbytes, b->offset, NULL);
            if (chunk < 0 && b->size != b->nbytes)
                chunk = (ssize_t) (b->nbytes = (b->size -= b->nbytes));
            if (chunk < 0 || !(b->nbytes -= (size_t) chunk)) {
                if (chunk < 0)
                    msg_error(b->rcvid, (int) -chunk);
                else
                    msg_reply(b->rcvid, (int) b->size, NULL, 0);
                *bb = b->link;
                free(b);
            } else {
                b->offset += (size_t) chunk;
                bb = &b->link;
            }
        } else {
            bb = &b->link;
        }
    }
    if (room != 0 && pipe_notify_output_check(pipe->notify, (int) room))
        pipe_notify_trigger(pipe->notify, (int) room, PIPE_NOTIFY_OUTPUT);
}

/* block_client() - add a blocking entry for a client
 *
 * @blocked: pointer to list head @ctp: I/O service context @offset: byte offset
 * into the message @size: total transfer size requested @nbytes: bytes
 * remaining @tail: nonzero to append at end of list
 *
 * Returns the new entry, or NULL on ENOMEM.
 */
static blocked_t *block_client(blocked_t **blocked,
                               ioserv_context_t *ctp,
                               size_t offset,
                               size_t size,
                               size_t nbytes,
                               int tail)
{
    blocked_t *b;

    if ((b = malloc(sizeof(blocked_t)))) {
        b->rcvid = ctp->rcvid;
        b->scoid = ctp->info.scoid;
        b->coid = ctp->info.coid;
        b->offset = offset;
        b->size = size;
        b->nbytes = nbytes;
        if (tail) {
            while (*blocked)
                blocked = &(*blocked)->link;
        }
        b->link = *blocked;
        *blocked = b;
    }
    return b;
}

/* unblock_clients() - unblock clients from a pipe's waiting list
 *
 * @blocked: pointer to list head @ctp: I/O service context (NULL = unblock all)
 * @match: if nonzero, match by rcvid; else match by scoid/coid @error: status
 * to deliver (EOK, -EINTR, EPIPE, etc.)
 */
static void unblock_clients(blocked_t **blocked,
                            ioserv_context_t *ctp,
                            int match,
                            int error)
{
    blocked_t *b;

    while ((b = *blocked)) {
        if ((!ctp) || (match && b->rcvid == ctp->rcvid) ||
            (!match && b->scoid == ctp->info.scoid &&
             b->coid == ctp->info.coid)) {
            if (error == EOK) {
                msg_reply(b->rcvid, 0, NULL, 0);
            } else if (error == -EINTR) {
                if (b->size != b->nbytes)
                    msg_reply(b->rcvid, (int) (b->size - b->nbytes), NULL, 0);
                else
                    msg_error(b->rcvid, EINTR);
            } else {
                if (error == EPIPE)
                    msg_deliver_event(b->rcvid, &Event);
                msg_error(b->rcvid, error);
            }
            *blocked = b->link;
            free(b);
        } else {
            blocked = &b->link;
        }
    }
}

/* open_semantics() - handle pipe/fifo open blocking requirements
 *
 * @pipe: the pipe being opened @ctp: I/O service context @ioflag: open flags
 *
 * Returns EOK, IOSERV_NOREPLY, or an errno.
 */
static int open_semantics(pipe_t *pipe, ioserv_context_t *ctp, int ioflag)
{
    int error;

    if (!(ioflag & (_IO_FLAG_RD | _IO_FLAG_WR)))
        error = EOK;
    else if (pipe->attr.rcount != 0 && pipe->attr.wcount != 0)
        unblock_clients(&pipe->waiting[PIPE_NOTIFY_OBAND], NULL, -1,
                        error = EOK);
    else if (ioflag & O_NONBLOCK)
        error = EOK;
    else if (block_client(&pipe->waiting[PIPE_NOTIFY_OBAND], ctp, 0, 0, 0, 0) ==
             NULL)
        error = ENOMEM;
    else
        error = IOSERV_NOREPLY;
    return error;
}

/* pipe_service_open() - open handler for unnamed pipes and /dev/pipe
 *
 * @ctp: I/O service context @msg: open message @handle: pipe handle @extra:
 * extra data (unused)
 */
static int pipe_service_open(ioserv_context_t *ctp,
                             io_open_t *msg,
                             IOSERV_HANDLE_T *handle,
                             void *extra)
{
    pipe_t *pipe;
    struct _client_info info;
    int error, ioflag;

    ioflag = (int) (msg->connect.ioflag & (_IO_FLAG_RD | _IO_FLAG_WR));
    if (msg->connect.file_type == _FTYPE_ANY) {
        error =
            (ioflag != 0) ? EACCES : pipe_open_default(ctp, msg, handle, NULL);
    } else if (msg->connect.file_type == _FTYPE_PIPE) {
        if (pipe_client_info(ctp, (int) msg->connect.ioflag, &info) != EOK)
            error = ESRCH;
        else if (ioflag != _IO_FLAG_RD)
            error = EINVAL;
        else if ((pipe = create_pipe(&Pipes, NULL, Pipes.attr.mode, &info)) ==
                 NULL)
            error = ENOMEM;
        else if ((error = pipe_open_check(ctp, msg, &pipe->attr, &info)) != EOK)
            release_pipe(pipe, error);
        else if ((error = pipe_ocb_attach(ctp, msg, &pipe->attr, NULL)) != EOK)
            release_pipe(pipe, error);
        else
            pipe_attr_unlock(&pipe->attr);
    } else {
        error = ENOSYS;
    }
    return error;
}

/* pipe_service_unlink() - unlink handler (pipes are unnamed, so reject) */
static int pipe_service_unlink(ioserv_context_t *ctp,
                               io_unlink_t *msg,
                               IOSERV_HANDLE_T *handle,
                               void *extra)
{
    return EACCES;
}

/* pipe_service_read() - read handler for pipes and FIFOs
 *
 * @ctp: I/O service context @msg: read message @ocb: open control block
 */
static int pipe_service_read(ioserv_context_t *ctp,
                             io_read_t *msg,
                             IOSERV_OCB_T *ocb)
{
    pipe_t *pipe;
    blocked_t *b;
    size_t nbytes, count;
    int error, nonblock;
    ssize_t chunk;

    pipe = OCB2PIPE(ocb);
    if ((error = pipe_read_verify(msg, ocb, &nonblock)) != EOK)
        return error;
    if ((msg->i.xtype & _IO_XTYPE_MASK) == _IO_XTYPE_OFFSET)
        return ESPIPE;
    else if ((msg->i.xtype & _IO_XTYPE_MASK) != _IO_XTYPE_NONE)
        return ENOSYS;
    if (!(nbytes = msg->i.nbytes))
        return EOK;
    count = 0;
    chunk = 0;
    while (pipe->attr.nbytes != 0 && count < nbytes) {
        chunk = read_from_pipe(pipe, ctp->rcvid, nbytes - count, count);
        if (chunk > 0)
            count += (size_t) chunk;
        else if (!count)
            return (int) -chunk;
        else
            break;
        check_writers(pipe);
    }
    if (!count) {
        _IO_SET_READ_NBYTES(ctp, 0);
        if (!pipe->attr.wcount)
            error = EOK;
        else if (nonblock)
            error = EAGAIN;
        else if (!(b = block_client(&pipe->waiting[PIPE_NOTIFY_INPUT], ctp, 0,
                                    nbytes, nbytes, !0)))
            error = ENOMEM;
        else
            error = IOSERV_NOREPLY;
    } else if ((size_t) chunk == nbytes) {
        error = IOSERV_NOREPLY;
    } else {
        _IO_SET_READ_NBYTES(ctp, count);
        error = EOK;
    }
    return error;
}

/* pipe_service_write() - write handler for pipes and FIFOs
 *
 * @ctp: I/O service context @msg: write message @ocb: open control block
 */
static int pipe_service_write(ioserv_context_t *ctp,
                              io_write_t *msg,
                              IOSERV_OCB_T *ocb)
{
    pipe_t *pipe;
    blocked_t *b;
    size_t nbytes, count, offset;
    int error, nonblock, trans;
    ssize_t chunk;

    pipe = OCB2PIPE(ocb);
    if ((error = pipe_write_verify(msg, ocb, &nonblock)) != EOK)
        return error;
    if ((msg->i.xtype & _IO_XTYPE_MASK) == _IO_XTYPE_OFFSET)
        return ESPIPE;
    else if ((msg->i.xtype & _IO_XTYPE_MASK) != _IO_XTYPE_NONE)
        return ENOSYS;
    if (!pipe->attr.rcount)
        return msg_deliver_event(ctp->rcvid, &Event), EPIPE;
    if (!(nbytes = (size_t) msg->i.nbytes))
        return EOK;
    count = 0;
    chunk = 0;
    offset = (size_t) ctp->offset + sizeof(io_write_t);
    trans = !pipe->attr.nbytes;
    if (nbytes > AtomicSize) {
        while (nbytes > count && (size_t) pipe->attr.nbytes != BufferSize) {
            chunk = write_to_pipe(pipe, ctp->rcvid, nbytes - count,
                                  offset + count, ctp);
            if (chunk > 0)
                count += (size_t) chunk;
            else if (!count)
                return (int) -chunk;
            else
                break;
            check_readers(pipe, trans);
            trans = 0;
        }
    } else if (BufferSize - (size_t) pipe->attr.nbytes >= nbytes) {
        chunk = write_to_pipe(pipe, ctp->rcvid, nbytes, offset, ctp);
        if (chunk > 0)
            count += (size_t) chunk;
        else
            return (int) -chunk;
        check_readers(pipe, trans);
    }
    if (!count && nonblock) {
        error = EAGAIN;
    } else if (count != nbytes && chunk >= 0 && !nonblock) {
        b = block_client(&pipe->waiting[PIPE_NOTIFY_OUTPUT], ctp,
                         offset + count, nbytes, nbytes - count, !0);
        if (!b)
            error = ENOMEM;
        else
            error = IOSERV_NOREPLY;
    } else {
        _IO_SET_WRITE_NBYTES(ctp, count);
        error = EOK;
    }
    return error;
}

/* pipe_service_closeocb() - last-close handler for pipes and FIFOs
 *
 * @ctp: I/O service context @reserved: unused @ocb: open control block
 */
static int pipe_service_closeocb(ioserv_context_t *ctp,
                                 void *reserved,
                                 IOSERV_OCB_T *ocb)
{
    pipe_t *pipe;
    unsigned flags;

    pipe = OCB2PIPE(ocb);
    pipe_attr_lock(&pipe->attr);
    flags = (unsigned) pipe_ocb_detach(ocb);
    free(ocb);
    if (S_ISFIFO(pipe->attr.mode) && flags != 0) {
        if (flags & PIPE_OCB_LAST_READER) {
            unblock_clients(&pipe->waiting[PIPE_NOTIFY_OUTPUT], NULL, -1,
                            EPIPE);
            pipe_notify_trigger(pipe->notify, INT_MAX, PIPE_NOTIFY_OUTPUT);
        }
        if (flags & PIPE_OCB_LAST_WRITER) {
            unblock_clients(&pipe->waiting[PIPE_NOTIFY_INPUT], NULL, -1, EOK);
            pipe_notify_trigger(pipe->notify, INT_MAX, PIPE_NOTIFY_INPUT);
        }
        if (flags & PIPE_OCB_LAST_INUSE) {
            pipe->attr.nbytes = 0;
            pipe->rd = pipe->wr = 0;
        }
    }
    update_pipe(pipe, true);
    release_pipe(pipe, EOK);
    return EOK;
}

/* pipe_service_stat() - fstat handler for pipes and FIFOs
 *
 * @ctp: I/O service context @msg: stat message @ocb: open control block
 */
static int pipe_service_stat(ioserv_context_t *ctp,
                             io_stat_t *msg,
                             IOSERV_OCB_T *ocb)
{
    pipe_t *pipe;
    int error;

    pipe = OCB2PIPE(ocb);
    update_pipe(pipe, true);
    if ((error = pipe_stat(ctp, &pipe->attr, &msg->o)) == EOK) {
        if (S_ISFIFO(pipe->attr.mode)) {
            msg->o.st_dev = (msg->o.st_dev & (dev_t) ~RTMUX_NODE_MASK) |
                            (pipe->devno & RTMUX_NODE_MASK);
            msg->o.st_blksize = (blksize_t) AtomicSize;
            msg->o.st_nblocks = (int32_t) ((BufferSize - 1) / AtomicSize + 1);
        }
        error = IOSERV_PTR(ctp, &msg->o, sizeof(msg->o));
    }
    return error;
}

/* pipe_service_notify() - select/poll handler for pipes and FIFOs
 *
 * @ctp: I/O service context @msg: notify message @ocb: open control block
 */
static int pipe_service_notify(ioserv_context_t *ctp,
                               io_notify_t *msg,
                               IOSERV_OCB_T *ocb)
{
    pipe_t *pipe;
    int error, trigger;

    pipe = OCB2PIPE(ocb);
    if (!S_ISFIFO(pipe->attr.mode)) {
        error = EBADF;
    } else {
        int hup = 0;

        trigger = 0;
        if ((int) pipe->attr.nbytes >= NotifyCounts[PIPE_NOTIFY_INPUT] ||
            !pipe->attr.wcount)
            trigger |= _NOTIFY_COND_INPUT;
        if ((int) (BufferSize - (size_t) pipe->attr.nbytes) >=
                NotifyCounts[PIPE_NOTIFY_OUTPUT] ||
            !pipe->attr.rcount)
            trigger |= _NOTIFY_COND_OUTPUT;

        /* Hangup: the read end whose last writer has gone reports POLLHUP; the
         * write end whose last reader has gone reports POLLERR (a further write
         * would raise EPIPE). Both are output-only poll conditions.
         */
        if ((ocb->ioflag & _IO_FLAG_RD) && !pipe->attr.wcount)
            hup |= POLLHUP;
        if ((ocb->ioflag & _IO_FLAG_WR) && !pipe->attr.rcount)
            hup |= POLLERR;
        error = pipe_notify(ctp, msg, pipe->notify, trigger, hup, NotifyCounts);
    }
    return error;
}

/* pipe_service_devctl() - devctl handler (FIONREAD only)
 *
 * @ctp: I/O service context @msg: devctl message @ocb: open control block
 */
static int pipe_service_devctl(ioserv_context_t *ctp,
                               io_devctl_t *msg,
                               IOSERV_OCB_T *ocb)
{
    pipe_t *pipe;
    uint32_t *fionread;
    int error;

    pipe = OCB2PIPE(ocb);
    if (S_ISFIFO(pipe->attr.mode) &&
        msg->i.devctl_cmd == DEVCTL_TTY_GET_INPUT_COUNT) {
        fionread = (uint32_t *) _DEVCTL_DATA(msg->o);
        msg->o.ret_val = EOK;
        *fionread = !(ocb->ioflag & _IO_FLAG_RD) ? 0
                    : (ctp->info.flags & RTMUX_MI_ENDIAN_DIFF)
                        ? ENDIAN_RET32((uint32_t) pipe->attr.nbytes)
                        : (uint32_t) pipe->attr.nbytes;
        error = IOSERV_PTR(ctp, &msg->o, sizeof(msg->o) + sizeof(*fionread));
    } else {
        error = pipe_devctl(ctp, msg, ocb);
    }
    return error;
}

/* pipe_service_unblock() - unblock handler (by rcvid)
 *
 * @ctp: I/O service context @msg: notification message @ocb: open control block
 */
static int pipe_service_unblock(ioserv_context_t *ctp,
                                io_notification_t *msg,
                                IOSERV_OCB_T *ocb)
{
    pipe_t *pipe;
    struct _msg_info info;

    pipe = OCB2PIPE(ocb);
    if (msg_info(ctp->rcvid, &info) != -1 &&
        (info.flags & RTMUX_MI_UNBLOCK_REQ)) {
        pipe_attr_lock(&pipe->attr);
        unblock_clients(&pipe->waiting[PIPE_NOTIFY_INPUT], ctp, !0, EINTR);
        unblock_clients(&pipe->waiting[PIPE_NOTIFY_OUTPUT], ctp, !0, -EINTR);
        unblock_clients(&pipe->waiting[PIPE_NOTIFY_OBAND], ctp, !0, EINTR);
        pipe_attr_unlock(&pipe->attr);
    }
    return IOSERV_NOREPLY;
}

/* pipe_service_pathconf() - pathconf handler (PC_PIPE_BUF)
 *
 * @ctp: I/O service context @msg: pathconf message @ocb: open control block
 */
static int pipe_service_pathconf(ioserv_context_t *ctp,
                                 io_pathconf_t *msg,
                                 IOSERV_OCB_T *ocb)
{
    pipe_t *pipe;
    int error;

    pipe = OCB2PIPE(ocb);
    if (S_ISFIFO(pipe->attr.mode) && msg->i.name == _PC_PIPE_BUF) {
        _IO_SET_PATHCONF_VALUE(ctp, AtomicSize);
        error = EOK;
    } else {
        error = pipe_pathconf(ctp, msg);
    }
    return error;
}

/* pipe_service_lseek() - lseek handler (seeking on pipes is illegal) */
static int pipe_service_lseek(ioserv_context_t *ctp,
                              io_lseek_t *msg,
                              IOSERV_OCB_T *ocb)
{
    return ESPIPE;
}

/* xtrapermsopenfd() - helper for openfd with extra permissions */
static int xtrapermsopenfd(ioserv_context_t *ctp,
                           io_openfd_t *msg,
                           IOSERV_OCB_T *ocb,
                           int perms)
{
    int save, error;

    save = ocb->ioflag;
    ocb->ioflag |= perms;
    error = pipe_openfd(ctp, msg, ocb, perms);
    ocb->ioflag = save;
    return error;
}

/* fakeopen() - construct a fake open message for internal pipe creation */
static io_open_t *fakeopen(io_open_t *msg)
{
    memset(msg, 0, sizeof(io_open_t));
    msg->connect.type = _IO_CONNECT;
    msg->connect.subtype = _IO_CONNECT_OPEN;
    msg->connect.file_type = _FTYPE_PIPE;
    msg->connect.ioflag = _IO_FLAG_RD;
    msg->connect.access = _IO_FLAG_RD | _IO_FLAG_WR;
    msg->connect.sflag = 0;
    msg->connect.path_len = 1;
    return msg;
}

/* pipe_service_openfd() - openfd handler (coordination with libc pipe())
 *
 * @ctp: I/O service context @msg: openfd message @ocb: open control block
 */
static int pipe_service_openfd(ioserv_context_t *ctp,
                               io_openfd_t *msg,
                               IOSERV_OCB_T *ocb)
{
    pipe_t *pipe;
    io_open_t omsg;
    int error, ioflag;

    pipe = OCB2PIPE(ocb);
    ioflag = msg->i.ioflag & (_IO_FLAG_RD | _IO_FLAG_WR);
    if (msg->i.xtype == _IO_OPENFD_NONE) {
        if (S_ISDIR(pipe->attr.mode) || pipe->name)
            error = ENOSYS;
        else if (ioflag != _IO_FLAG_RD)
            error = EOPNOTSUPP;
        else
            error = pipe_service_open(ctp, fakeopen(&omsg), &Pipes, NULL);
    } else if (msg->i.xtype == _IO_OPENFD_PIPE) {
        if (pipe->attr.wcount != 0)
            error = EBUSY;
        else if (ioflag != _IO_FLAG_WR)
            error = EOPNOTSUPP;
        else
            error = xtrapermsopenfd(ctp, msg, ocb, ioflag);
    } else {
        error = ENOSYS;
    }
    return error;
}

/* pipe_service_fdinfo() - fdinfo handler (provide name for the pipe)
 *
 * @ctp: I/O service context @msg: fdinfo message @ocb: open control block
 */
static int pipe_service_fdinfo(ioserv_context_t *ctp,
                               io_fdinfo_t *msg,
                               IOSERV_OCB_T *ocb)
{
    pipe_t *pipe;
    char *name;
    size_t ulen, xlen, len;

    pipe = OCB2PIPE(ocb);
    ulen = (size_t) msg->i.path_len;
    name = (char *) (&msg->o + 1);
    pipe_fdinfo(ocb, &pipe->attr, &msg->o.info);

    /* Bytes available for the name after msg->o. Compute against the actual
     * message extent and reject a client whose buffer cannot even hold msg->o;
     * a naive `msg_max_size - offset - sizeof` in size_t underflows on a short
     * client buffer and, cast to a printf precision, writes the whole name past
     * the end. snprintf then reserves the NUL, so no off-by-one either.
     */
    size_t base = (size_t) ctp->offset + sizeof(msg->o);
    if ((size_t) ctp->msg_max_size <= base)
        return EMSGSIZE;
    xlen = (size_t) ctp->msg_max_size - base;

    if (S_ISNAM(pipe->attr.mode))
        snprintf(name, xlen, "%s", Pipes.name);
    else if (!pipe->name)
        snprintf(name, xlen, "(pipe)");
    else
        snprintf(name, xlen, "/%s", pipe->name);
    len = strlen(name) +
          1; /* actual bytes written, <= xlen; never over-reports */
    _IO_SET_FDINFO_LEN(ctp, len);
    return IOSERV_PTR(ctp, &msg->o, sizeof(msg->o) + min(ulen, len));
}

/* pipe_service_closedup() - close-dup handler (unblock by scoid/coid)
 *
 * @ctp: I/O service context @msg: close message @ocb: open control block
 */
static int pipe_service_closedup(ioserv_context_t *ctp,
                                 io_close_t *msg,
                                 IOSERV_OCB_T *ocb)
{
    pipe_t *pipe;

    pipe = OCB2PIPE(ocb);
    pipe_attr_lock(&pipe->attr);
    unblock_clients(&pipe->waiting[PIPE_NOTIFY_INPUT], ctp, 0, EBADF);
    unblock_clients(&pipe->waiting[PIPE_NOTIFY_OUTPUT], ctp, 0, EBADF);
    unblock_clients(&pipe->waiting[PIPE_NOTIFY_OBAND], ctp, 0, EBADF);
    pipe_notify_remove(ctp, pipe->notify);
    pipe_attr_unlock(&pipe->attr);
    return EOK;
}

/* pipe_service_lockocb() - OCB lock handler */
static int pipe_service_lockocb(ioserv_context_t *ctp,
                                void *reserved,
                                IOSERV_OCB_T *ocb)
{
    return pipe_attr_lock(ocb->attr);
}

/* pipe_service_unlockocb() - OCB unlock handler */
static int pipe_service_unlockocb(ioserv_context_t *ctp,
                                  void *reserved,
                                  IOSERV_OCB_T *ocb)
{
    update_pipe(OCB2PIPE(ocb), false);
    return pipe_attr_unlock(ocb->attr);
}

/* fifo_service_open() - open handler for named FIFOs
 *
 * @ctp: I/O service context @msg: open message @handle: fifo handle @extra:
 * extra data (unused)
 */
static int fifo_service_open(ioserv_context_t *ctp,
                             io_open_t *msg,
                             IOSERV_HANDLE_T *handle,
                             void *extra)
{
    pipe_t *pipe;
    int error;

    if (msg->connect.file_type != _FTYPE_PIPE) {
        error = ENOSYS;
    } else if ((error = acquire_pipe(&Fifos, msg->connect.path, &pipe)) ==
               EOK) {
        if ((error = pipe_open_check(ctp, msg, &pipe->attr, NULL)) != EOK)
            release_pipe(pipe, error);
        else if ((int) (msg->connect.ioflag & (_IO_FLAG_RD | _IO_FLAG_WR)) ==
                     _IO_FLAG_WR &&
                 !pipe->attr.rcount && ((int) msg->connect.ioflag & O_NONBLOCK))
            release_pipe(pipe, error = ENXIO);
        else if ((error = pipe_ocb_attach(ctp, msg, &pipe->attr, NULL)) != EOK)
            release_pipe(pipe, error);
        else if ((error = open_semantics(pipe, ctp,
                                         (int) msg->connect.ioflag)) != EOK &&
                 error != IOSERV_NOREPLY)
            pipe_open_abort(ctp), release_pipe(pipe, error);
        else
            pipe_attr_unlock(&pipe->attr);
    }
    return error;
}

/* parsesize() - parse a size string with optional K/M suffix
 *
 * @str: input string (e.g. "8K", "1M", "4096")
 *
 * Returns the parsed size, or 0 on error.
 */
static size_t parsesize(const char *str)
{
    char *cp;
    long number;
    char suffix;

    number = strtol(str, &cp, 0);
    if (number <= 0)
        return 0;
    suffix = (char) toupper((unsigned char) *cp);
    if (suffix == 'B')
        number *= 1, ++cp;
    else if (suffix == 'K')
        number <<= 10, ++cp;
    else if (suffix == 'M')
        number <<= 20, ++cp;
    return (*cp == '\0') ? (size_t) number : 0;
}

/* fatal() - print an error message and exit
 *
 * @errmsg: printf-style format string
 */
static void fatal(const char *errmsg, ...)
{
    va_list args;

    fprintf(stderr, "%s: ", program_name);
    va_start(args, errmsg);
    vfprintf(stderr, errmsg, args);
    va_end(args);
    fprintf(stderr, "\n");
    exit(EXIT_FAILURE);
}

int main(int argc, char *argv[])
{
    int opt;
    int daemonize = 1;

    if (argc > 0 && argv[0])
        program_name = argv[0];

    BufferSize = PIPE_SIZE;
    AtomicSize = PIPE_ATOMIC;
    SelWrite1 = !0;
    while ((opt = getopt(argc, argv, ":1a:s:d")) != -1 || optind < argc) {
        switch (opt) {
        case '1':
            SelWrite1 = 0;
            break;
        case 'a':
            if (!(AtomicSize = parsesize(optarg)))
                fatal("invalid atomic write size");
            break;
        case 's':
            if (!(BufferSize = parsesize(optarg)))
                fatal("invalid pipe buffer size");
            break;
        case ':':
            fatal("missing argument for '-%c'", optopt);
            break;
        case 'd':
            daemonize = 0;
            break;
        case '?':
            fatal("unknown option '-%c'", optopt);
            break;
        case -1:
            fatal("unknown operand '%s'", argv[optind]);
            break;
        }
    }
    BufferSize = max(BufferSize, AtomicSize);

    memset(&PipeConnectFuncs, 0, sizeof(PipeConnectFuncs));
    PipeConnectFuncs.nfuncs = IOSERV_CONNECT_NFUNCS;
    memset(&PipeIoFuncs, 0, sizeof(PipeIoFuncs));
    PipeIoFuncs.nfuncs = IOSERV_IO_NFUNCS;
    PipeConnectFuncs.open = pipe_service_open;
    PipeConnectFuncs.unlink = pipe_service_unlink;
    PipeIoFuncs.read = pipe_service_read;
    PipeIoFuncs.write = pipe_service_write;
    PipeIoFuncs.close_ocb = pipe_service_closeocb;
    PipeIoFuncs.stat = pipe_service_stat;
    PipeIoFuncs.notify = pipe_service_notify;
    PipeIoFuncs.devctl = pipe_service_devctl;
    PipeIoFuncs.unblock = pipe_service_unblock;
    PipeIoFuncs.pathconf = pipe_service_pathconf;
    PipeIoFuncs.lseek = pipe_service_lseek;
    PipeIoFuncs.openfd = pipe_service_openfd;
    PipeIoFuncs.fdinfo = pipe_service_fdinfo;
    PipeIoFuncs.close_dup = pipe_service_closedup;
    PipeIoFuncs.lock_ocb = pipe_service_lockocb;
    PipeIoFuncs.unlock_ocb = pipe_service_unlockocb;

    memset(&FifoConnectFuncs, 0, sizeof(FifoConnectFuncs));
    FifoConnectFuncs.nfuncs = IOSERV_CONNECT_NFUNCS;
    FifoConnectFuncs.open = fifo_service_open;

    if (!(Dispatch = dispatch_create()))
        fatal("unable to allocate I/O service context - %s", strerror(errno));
    memset(&IoServiceAttr, 0, sizeof(IoServiceAttr));
    IoServiceAttr.nparts_max = 1;
    IoServiceAttr.msg_max_size = sizeof(struct _io_connect) + _POSIX_PATH_MAX;
    SIGEV_SIGNAL_THREAD_INIT(&Event, SIGPIPE, 0, SI_USER);
    NotifyCounts[PIPE_NOTIFY_INPUT] = 1;
    NotifyCounts[PIPE_NOTIFY_OBAND] = 1;
    NotifyCounts[PIPE_NOTIFY_OUTPUT] = SelWrite1 ? 1 : (int) AtomicSize;

    memset(&Mount, 0, sizeof(Mount));
    Mount.conf = PIPE_PC_CHOWN_RESTRICTED | PIPE_PC_NO_TRUNC | PIPE_PC_SYNC_IO;
    Mount.blocksize = (int32_t) AtomicSize;
    memset(&Pipes, 0, sizeof(Pipes));
    pipe_attr_init(
        &Pipes.attr,
        S_IFNAM | S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH,
        NULL, NULL);
    Pipes.attr.mount = &Mount;
    Pipes.name = PIPE_NAME;
    {
        int rid =
            ioserv_attach(Dispatch, &IoServiceAttr, Pipes.name, _FTYPE_PIPE, 0,
                          &PipeConnectFuncs, &PipeIoFuncs, &Pipes);
        if (rid == -1)
            fatal("unable to register as pipe handler - %s", strerror(errno));
        Pipes.attr.rdev = (dev_t) rid;
    }
    ioserv_devino((int) Pipes.attr.rdev, &Mount.dev, &Pipes.attr.inode);
    memset(&Fifos, 0, sizeof(Fifos));
    pipe_attr_init(&Fifos.attr, S_IFDIR | 0777, NULL, NULL);
    Fifos.attr.mount = &Mount;
    Fifos.name = NULL;
    {
        int rid = ioserv_attach(
            Dispatch, &IoServiceAttr, Fifos.name, _FTYPE_PIPE,
            IOSERV_FLAG_DIR | IOSERV_FLAG_AFTER | IOSERV_FLAG_FTYPEONLY,
            &FifoConnectFuncs, &PipeIoFuncs, &Fifos);
        if (rid == -1)
            fatal("unable to register as fifo handler - %s", strerror(errno));
        Fifos.attr.rdev = (dev_t) rid;
    }

    memset(&PoolAttr, 0, sizeof(PoolAttr));
    pthread_attr_init(PoolAttr.attr = &thread_attr);
    pthread_attr_setdetachstate(&thread_attr, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&thread_attr, PIPE_STACKSIZE);
    PoolAttr.lo_water = 2;
    PoolAttr.hi_water = 4;
    PoolAttr.maximum = 12;
    PoolAttr.increment = 1;
    PoolAttr.context_alloc = ioserv_context_alloc;
    PoolAttr.context_free = ioserv_context_free;
    PoolAttr.block_func = ioserv_block;
    PoolAttr.unblock_func = ioserv_unblock;
    PoolAttr.handler_func = ioserv_handler;
    PoolAttr.handle = Dispatch;
    if (!(Pool = thread_pool_create(&PoolAttr, POOL_FLAG_EXIT_SELF)))
        fatal("unable to allocate thread pool - %s", strerror(errno));

    if (daemonize)
        rtmux_detach(EXIT_SUCCESS);

    /* POOL_FLAG_EXIT_SELF: main thread exits, pool threads take over. */
    thread_pool_start(Pool);

    /* NOTREACHED */
    return EXIT_SUCCESS;
}
