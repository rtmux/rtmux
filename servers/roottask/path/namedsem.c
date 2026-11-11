/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

struct sem_ocb;
#define IOSERV_OCB_T struct sem_ocb
struct semaphore;
#define IOSERV_HANDLE_T struct semaphore

#include "externs.h"
#include <align.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <mqueue.h>
#include <semaphore.h>
#include <signal.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <sys/conf.h>
#include <devctl/common.h>
#include <devctl/misc.h>
#include <sys/iomsg.h>
#include <sys/ioserv.h>
#include <sys/resource.h>
#include <unistd.h>

#define OCB2SEM(_ocb) ((semaphore_t *) ((_ocb)->attr))

typedef struct blocked {
    int rcvid;
    int scoid;
    int coid;
    unsigned priority;
    struct blocked *link;
} blocked_t;

typedef struct sem_mount {
    dev_t dev;
} sem_mount_t;

typedef struct sem_attr {
    sem_mount_t *mount;
    mode_t mode;
    uid_t uid;
    gid_t gid;
    nlink_t nlink;
    dev_t rdev;
    ino_t inode;
    off_t nbytes;
    unsigned count;
    time_t atime;
    time_t mtime;
    time_t ctime;
} sem_attr_t;

struct sem_ocb {
    sem_attr_t *attr;
    int ioflag;
    off_t offset;
};

typedef struct semaphore {
    sem_attr_t attr;
    unsigned initial;
    char *name;
    blocked_t *blocked;
    struct semaphore *link;
} semaphore_t;

typedef struct {
    sem_attr_t *attr;
    uint32_t *initial;
    uint32_t *ioflag;
    int verification;
} creation_t;

static sem_mount_t Mount;

static int sem_client_info(ioserv_context_t *ctp, struct _client_info *info)
{
    return connect_client_info(ctp->info.scoid, info, NGROUPS_MAX);
}

static int sem_check_access(const sem_attr_t *attr,
                            mode_t check,
                            const struct _client_info *info)
{
    mode_t allowed;

    if (info->cred.euid == 0)
        return EOK;
    if ((check & S_ISUID) && info->cred.euid == attr->uid)
        return EOK;
    if (check & S_ISGID) {
        if (info->cred.egid == attr->gid)
            return EOK;
        for (uint32_t i = 0; i < info->cred.ngroups; i++)
            if (info->cred.grouplist[i] == attr->gid)
                return EOK;
    }

    check &= S_IRWXU;
    if (check == 0)
        return EPERM;

    if (info->cred.euid == attr->uid) {
        allowed = attr->mode;
    } else {
        int in_group = info->cred.egid == attr->gid;

        for (uint32_t i = 0; !in_group && i < info->cred.ngroups; i++)
            in_group = info->cred.grouplist[i] == attr->gid;
        allowed = in_group ? attr->mode << 3 : attr->mode << 6;
    }
    return (allowed & check) == check ? EOK : EACCES;
}

static void sem_attr_init(sem_attr_t *attr,
                          mode_t mode,
                          const struct _client_info *info)
{
    time_t now = time(NULL);

    memset(attr, 0, sizeof(*attr));
    attr->mode = mode;
    attr->uid = info ? info->cred.euid : 0;
    attr->gid = info ? info->cred.egid : 0;
    attr->nlink = 1;
    attr->mount = &Mount;
    attr->ctime = now;
    attr->mtime = now;
    attr->atime = now;
}

static int sem_open_check(io_open_t *msg,
                          sem_attr_t *attr,
                          const struct _client_info *cred)
{
    unsigned ioflag;
    mode_t check = 0;

    if (msg->connect.subtype != _IO_CONNECT_OPEN &&
        msg->connect.subtype != _IO_CONNECT_COMBINE &&
        msg->connect.subtype != _IO_CONNECT_COMBINE_CLOSE)
        return ENOSYS;
    ioflag =
        msg->connect.ioflag & (~(unsigned) _IO_FLAG_MASK | msg->connect.access);
    if ((ioflag & (_IO_FLAG_WR | O_TRUNC)) == O_TRUNC)
        msg->connect.ioflag &= ~(unsigned) O_TRUNC;
    if ((ioflag & _IO_FLAG_WR) && S_ISDIR(attr->mode))
        return EISDIR;
    if (ioflag & _IO_FLAG_RD)
        check |= S_IRUSR;
    if (ioflag & _IO_FLAG_WR)
        check |= S_IWUSR;
    return check ? sem_check_access(attr, check, cred) : EOK;
}

static int sem_ocb_attach(ioserv_context_t *ctp,
                          io_open_t *msg,
                          IOSERV_OCB_T *ocb,
                          sem_attr_t *attr)
{
    ocb->attr = attr;
    ocb->ioflag = (int) (msg->connect.ioflag &
                         (~(unsigned) _IO_FLAG_MASK | msg->connect.access));
    ocb->offset = 0;
    if (ioserv_open_bind(ctp, ocb, NULL) == -1)
        return errno;
    attr->count++;
    return EOK;
}

static int sem_service_open(ioserv_context_t *ctp,
                            io_open_t *msg,
                            IOSERV_HANDLE_T *handle,
                            void *extra);
static int sem_service_unlink(ioserv_context_t *ctp,
                              io_unlink_t *msg,
                              IOSERV_HANDLE_T *handle,
                              void *extra);
static int sem_service_read(ioserv_context_t *ctp,
                            io_read_t *msg,
                            IOSERV_OCB_T *ocb);
static int sem_service_write(ioserv_context_t *ctp,
                             io_write_t *msg,
                             IOSERV_OCB_T *ocb);
static int sem_service_close(ioserv_context_t *ctp,
                             void *reserved,
                             IOSERV_OCB_T *ocb);
static int sem_service_stat(ioserv_context_t *ctp,
                            io_stat_t *msg,
                            IOSERV_OCB_T *ocb);
static int sem_service_devctl(ioserv_context_t *ctp,
                              io_devctl_t *msg,
                              IOSERV_OCB_T *ocb);
static int sem_service_unblock(ioserv_context_t *ctp,
                               io_notification_t *msg,
                               IOSERV_OCB_T *ocb);
static int sem_dir_service_open(ioserv_context_t *ctp,
                                io_open_t *msg,
                                IOSERV_HANDLE_T *handle,
                                void *extra);
static int sem_dir_service_unlink(ioserv_context_t *ctp,
                                  io_unlink_t *msg,
                                  IOSERV_HANDLE_T *handle,
                                  void *extra);
static int sem_dir_service_read(ioserv_context_t *ctp,
                                io_read_t *msg,
                                IOSERV_OCB_T *ocb);
static int sem_dir_service_close(ioserv_context_t *ctp,
                                 void *reserved,
                                 IOSERV_OCB_T *ocb);
static int sem_dir_service_stat(ioserv_context_t *ctp,
                                io_stat_t *msg,
                                IOSERV_OCB_T *ocb);
static IOSERV_OCB_T *sem_ocb_alloc(ioserv_context_t *ctp, sem_attr_t *attr);
static void sem_ocb_free(IOSERV_OCB_T *ocb);

static semaphore_t NamedSemaphores;
static ioserv_connect_ops_t SemConnectFuncs, DirConnectFuncs;
static ioserv_ops_t SemIoFuncs, DirIoFuncs;
static ioserv_attr_t ioserv_attr;
static int DirPathname, MaxSems;
static size_t PrefixLength;
static pthread_mutex_t SemaphoreMutex = PTHREAD_MUTEX_INITIALIZER;

/* Compensate for the sem_open()/sem_unlink() name normalization used by libc.
 */
static char *name_sem(char *name)
{
    if (*name == '\0')
        return NULL;
    if (strncmp(&NamedSemaphores.name[1], name, PrefixLength) ||
        name[PrefixLength] != '/')
        return name;
    return &name[PrefixLength + 1];
}

/* Manipulate a free list of 'blocked_t' items (avoid malloc/free). */
static blocked_t *blocked_item(blocked_t *b)
{
    if (b) {
        b->link = NamedSemaphores.blocked, NamedSemaphores.blocked = b;
    } else if ((b = NamedSemaphores.blocked)) {
        NamedSemaphores.blocked = b->link;
    } else {
        b = _smalloc(sizeof(blocked_t));
    }
    return (b);
}

/* Enqueue a blocked sem_wait() client (FIFO by priority). */
static blocked_t *enqueue_client(semaphore_t *sem, ioserv_context_t *ctp)
{
    blocked_t *blk, **head;

    if ((blk = blocked_item(NULL))) {
        blk->rcvid = ctp->rcvid;
        blk->scoid = ctp->info.scoid;
        blk->coid = ctp->info.coid;
        blk->priority = (unsigned) ctp->info.priority;
        for (head = &sem->blocked; *head && (*head)->priority >= blk->priority;
             head = &(*head)->link)
            ;
        blk->link = *head, *head = blk;
        --sem->attr.nbytes;
    }
    return (blk);
}

/* Dequeue a blocked sem_wait() client (the head of the queue). */
static blocked_t *dequeue_client(semaphore_t *sem, int *rcvid, unsigned *pri)
{
    blocked_t *blk;

    if ((blk = sem->blocked)) {
        *rcvid = blk->rcvid, *pri = blk->priority;
        ++sem->attr.nbytes;
        sem->blocked = blk->link;
        (void) blocked_item(blk);
    }
    return (blk);
}

/* Unblock all/specific clients (semaphore close or interrupt). Remember that
 * hanging off 'NamedSemaphores' is actually a free list of items!
 */
static void unblock_clients(semaphore_t *sem,
                            ioserv_context_t *ctp,
                            int match,
                            int error)
{
    blocked_t **head, *b;

    if (!S_ISDIR(sem->attr.mode)) {
        head = &sem->blocked;
        while ((b = *head)) {
            if ((!ctp) || (match && b->rcvid == ctp->rcvid) ||
                (!match && b->scoid == ctp->info.scoid &&
                 b->coid == ctp->info.coid)) {
                if (error == EOK)
                    msg_reply(b->rcvid, 0, NULL, 0);
                else
                    msg_error(b->rcvid, error);
                ++sem->attr.nbytes;
                *head = b->link;
                (void) blocked_item(b);
            } else {
                head = &b->link;
            }
        }
    }
}

/* Create a new semaphore, and link into the list of created semaphores. */
static semaphore_t *create_sem(semaphore_t *head,
                               const char *name,
                               sem_attr_t *attr,
                               uint32_t *initial)
{
    semaphore_t *sem;

    if (head->attr.nbytes >= MaxSems) {
        errno = ENFILE;
        return (NULL);
    }

    sem = _smalloc(sizeof(semaphore_t));
    if (!sem) {
        errno = ENOMEM;
        return (NULL);
    }

    size_t slen = strlen(name) + 1;
    sem->name = _smalloc(slen);
    if (!sem->name) {
        errno = ENOMEM;
        _sfree(sem, sizeof(semaphore_t));
        return (NULL);
    }

    memcpy(sem->name, name, slen); /* '\0' guaranteed to be copied */
    memcpy(&sem->attr, attr, sizeof(sem_attr_t));
    sem->attr.nbytes = sem->initial = ((initial) ? *initial : head->initial);
    sem->blocked = NULL;
    sem->link = head->link, head->link = sem;
    ++head->attr.nbytes;
    return (sem);
}

/* Lookup the given semaphore in the list of created semaphores (and optional
 * create a new one - atomic within mutex lock).
 */
static semaphore_t *lookup_sem(semaphore_t *head,
                               const char *name,
                               creation_t *create)
{
    semaphore_t *s;

    for (s = head->link; s; s = s->link)
        if (!strcmp(name, s->name))
            return (s);
    if (!create)
        errno = ENOENT;
    else if (create->verification != EOK)
        errno = create->verification;
    else if ((s = create_sem(head, name, create->attr, create->initial)))
        *create->ioflag &= ~(uint32_t) O_EXCL;
    return (s);
}

/* Cleanup the given semaphore (as a result of unlink() or close() handling). */
static void destroy_sem(semaphore_t *head, semaphore_t *sem, bool force)
{
    semaphore_t **sp;

    if (head) {
        for (sp = &head->link; *sp != sem; sp = &(*sp)->link)
            ;
        *sp = sem->link;
        --head->attr.nbytes;
    }
    if ((!sem->attr.nlink && !sem->attr.count) || force) {
        unblock_clients(sem, NULL, -1, EBADF);
        _sfree(sem->name, strlen(sem->name) + 1);
        _sfree(sem, sizeof(semaphore_t));
    }
}

/* Construct a stat structure from the given semaphore. */
static void detail_sem(ioserv_context_t *ctp,
                       semaphore_t *sem,
                       struct stat *st,
                       int namespace)
{
    memset(st, 0, sizeof(struct stat));
    st->st_ino = (uintptr_t) sem;
    st->st_size = (sem->attr.nbytes >= 0) ? sem->attr.nbytes : 0;
    st->st_dev = (ctp->info.srcnd << RTMUX_NODE_BITS) | sem->attr.mount->dev;
    st->st_rdev = namespace ? _S_INSEM : sem->attr.rdev;
    st->st_uid = sem->attr.uid, st->st_gid = sem->attr.gid;
    st->st_ctime = sem->attr.ctime, st->st_mtime = sem->attr.mtime,
    st->st_atime = sem->attr.atime;
    st->st_mode = sem->attr.mode, st->st_nlink = sem->attr.nlink;
}

/* Provide the nth item from the list of active named semaphores (readdir). */
static int get_nth_entry(semaphore_t *head, int index, semaphore_t **nth)
{
    while ((head = head->link) && --index >= 0)
        ;
    return (*nth = head, index < 0);
}

/* Perform readdir() processing - list all the active named semaphores in a
 * single ("/dev/sem") pseduo-directory.
 */
static int list_sem(ioserv_context_t *ctp,
                    semaphore_t *dir,
                    off_t *offset,
                    struct dirent *dp,
                    unsigned dpsize,
                    int xtra,
                    int *nbytes)
{
    semaphore_t *sem;
    struct dirent_extra_stat *dextra;
    size_t nmlen;

    *nbytes = 0;
    for (;;) {
        size_t reclen;

        if (!get_nth_entry(dir, (int) *offset, &sem))
            return (EOK);
        nmlen = strlen(sem->name);
        reclen =
            lib_align_up_size(offsetof(struct dirent, d_name) + nmlen + 1, 8);
        if (dpsize < reclen)
            break;
        memset(dp, 0, sizeof(struct dirent));
        dp->d_ino = (uintptr_t) sem;
        dp->d_offset = *offset;
        dp->d_namelen = (uint16_t) nmlen;
        memcpy(dp->d_name, sem->name, nmlen + 1);
        dextra = (struct dirent_extra_stat *) _DEXTRA_FIRST(dp);
        dp->d_reclen = (uint16_t) ((char *) dextra - (char *) dp);
        reclen = lib_align_up_size(
            dp->d_reclen + sizeof(struct dirent_extra_stat), 8);
        if (xtra && dpsize >= (unsigned) reclen) {
            dextra->d_datalen = sizeof(struct stat);
            dextra->d_type = _DTYPE_LSTAT;
            detail_sem(ctp, sem, &dextra->d_stat, 1);
            dextra = (struct dirent_extra_stat *) _DEXTRA_NEXT(dextra);
            dp->d_reclen = (uint16_t) ((char *) dextra - (char *) dp);
        }
        *nbytes += dp->d_reclen, dpsize -= dp->d_reclen;
        dp = (struct dirent *) ((char *) dp + dp->d_reclen);
        ++*offset;
    }
    return ((*nbytes != 0) ? EOK : EMSGSIZE);
}

/* Handle client/libc "sem_open()". */
static int sem_service_open(ioserv_context_t *ctp,
                            io_open_t *msg,
                            IOSERV_HANDLE_T *handle,
                            void *extra)
{
    semaphore_t *sem;
    char *name;
    creation_t create;
    struct _client_info cred;
    sem_attr_t attr;
    int error;

    if ((msg->connect.extra_type != _IO_CONNECT_EXTRA_NONE &&
         msg->connect.extra_type != _IO_CONNECT_EXTRA_SEM) ||
        msg->connect.file_type != _FTYPE_SEM)
        return (ENOSYS);
    if ((error = sem_client_info(ctp, &cred)) != EOK)
        return (error);
    if (!(name = name_sem(msg->connect.path)))
        return (EINVAL);
    if (msg->connect.ioflag & (unsigned) O_CREAT) {
        create.ioflag = &msg->connect.ioflag;
        msg->connect.mode = S_IFNAM | (msg->connect.mode & ~(unsigned) S_IFMT);
        sem_attr_init(&attr, msg->connect.mode, &cred);
        create.attr = &attr;
        create.verification = sem_check_access(&handle->attr, S_IWUSR, &cred);
        if (msg->connect.extra_type != _IO_CONNECT_EXTRA_SEM)
            create.initial = NULL;
        else if (msg->connect.extra_len != sizeof(uint32_t))
            create.verification = ENOSYS;
        else if (*(create.initial = (uint32_t *) extra) > SEM_VALUE_MAX)
            create.verification = EINVAL;
    } else {
        create.verification = ENOENT;
    }
    pthread_mutex_lock(&SemaphoreMutex);
    if (!(sem = lookup_sem(handle, name, &create)))
        error = errno;
    else if ((msg->connect.ioflag & (unsigned) (O_CREAT | O_EXCL)) ==
             (unsigned) (O_CREAT | O_EXCL)) {
        error = EEXIST;
    } else if ((error = sem_open_check(msg, &sem->attr, &cred)) == EOK) {
        IOSERV_OCB_T *ocb = sem_ocb_alloc(ctp, &sem->attr);

        if (!ocb)
            error = errno;
        else if ((error = sem_ocb_attach(ctp, msg, ocb, &sem->attr)) != EOK)
            sem_ocb_free(ocb);
    }
    pthread_mutex_unlock(&SemaphoreMutex);
    return ((error != EOK || (ctp->info.flags & RTMUX_MI_ENDIAN_DIFF))
                ? error
                : sem_service_stat(ctp, (io_stat_t *) msg,
                                   ioserv_ocb(ctp, &ctp->info)));
}

/* Handle client/libc "sem_unlink()" (alternate implementation). */
static int sem_service_unlink(ioserv_context_t *ctp,
                              io_unlink_t *msg,
                              IOSERV_HANDLE_T *handle,
                              void *extra)
{
    return (sem_dir_service_unlink(ctp, msg, handle, extra));
}

/* Handle client/libc "sem_wait()/sem_trywait()". */
static int sem_service_read(ioserv_context_t *ctp,
                            io_read_t *msg,
                            IOSERV_OCB_T *ocb)
{
    semaphore_t *sem;
    struct _msg_info info;
    int error, nonblock;

    sem = OCB2SEM(ocb);
    if ((ocb->ioflag & _IO_FLAG_RD) == 0)
        return (EBADF);
    nonblock = (ocb->ioflag & O_NONBLOCK) != 0;
    if (msg->i.xtype & _IO_XFLAG_BLOCK)
        nonblock = 0;
    else if (msg->i.xtype & _IO_XFLAG_NONBLOCK)
        nonblock = 1;
    if ((msg->i.xtype & _IO_XTYPE_MASK) != _IO_XTYPE_NONE)
        return (ENOSYS);
    if (msg->i.nbytes != 0)
        return (EMSGSIZE);
    pthread_mutex_lock(&SemaphoreMutex);
    if (sem->attr.nbytes > 0) {
        --sem->attr.nbytes;
        _IO_SET_READ_NBYTES(ctp, 0);
        error = EOK;
    } else if (nonblock) {
        error = EAGAIN;
    } else if (msg_info(ctp->rcvid, &info) != -1 &&
               (info.flags & RTMUX_MI_UNBLOCK_REQ)) {
        /* The client's unblock (a REPLY timeout or signal) was requested while
         * this request was still being dispatched, so a peer worker may have
         * handled its unblock notification before the client was queued: that
         * handler found nothing to unblock and the kernel sends only one.
         * RTMUX_MI_UNBLOCK_REQ is set before the notification is delivered and
         * checked here under SemaphoreMutex, which also serializes
         * sem_service_unblock, so either the unblock handler sees the queued
         * client or this path sees the request. Never block such a client.
         */
        error = EINTR;
    } else if (!enqueue_client(sem, ctp)) {
        error = ENOMEM;
    } else {
        error = IOSERV_NOREPLY;
    }
    pthread_mutex_unlock(&SemaphoreMutex);
    return (error);
}

/* Handle client/libc "sem_post()". */
static int sem_service_write(ioserv_context_t *ctp,
                             io_write_t *msg,
                             IOSERV_OCB_T *ocb)
{
    semaphore_t *sem;
    unsigned pri;
    int error, rcvid;

    sem = OCB2SEM(ocb);
    if ((ocb->ioflag & _IO_FLAG_WR) == 0)
        return (EBADF);
    if ((msg->i.xtype & _IO_XTYPE_MASK) != _IO_XTYPE_NONE)
        return (ENOSYS);
    if (msg->i.nbytes != 0)
        return (EMSGSIZE);
    pthread_mutex_lock(&SemaphoreMutex);
    if (sem->attr.nbytes >= SEM_VALUE_MAX) {
        error = EAGAIN;
    } else if (sem->attr.nbytes >= 0 || !dequeue_client(sem, &rcvid, &pri)) {
        ++sem->attr.nbytes;
        _IO_SET_WRITE_NBYTES(ctp, 0);
        error = EOK;
    } else {
        pthread_mutex_unlock(&SemaphoreMutex);
        if (pri > ctp->info.priority) {
            msg_reply(rcvid, 0, NULL, 0);
            msg_reply(ctp->rcvid, 0, NULL, 0);
        } else {
            msg_reply(ctp->rcvid, 0, NULL, 0);
            msg_reply(rcvid, 0, NULL, 0);
        }
        return (IOSERV_NOREPLY);
    }
    pthread_mutex_unlock(&SemaphoreMutex);
    return (error);
}

/* Handle client/libc "sem_close()". */
static int sem_service_close(ioserv_context_t *ctp,
                             void *reserved,
                             IOSERV_OCB_T *ocb)
{
    semaphore_t *sem;

    sem = OCB2SEM(ocb);
    pthread_mutex_lock(&SemaphoreMutex);
    (void) ctp;
    if (ocb->attr->count > 0)
        ocb->attr->count--;
    sem_ocb_free(ocb);
    unblock_clients(sem, ctp, 0, EBADF);
    destroy_sem(NULL, sem, false);
    pthread_mutex_unlock(&SemaphoreMutex);
    return (EOK);
}

/* Handle client/libc "sem_getvalue()" (alternate implementation). */
static int sem_service_stat(ioserv_context_t *ctp,
                            io_stat_t *msg,
                            IOSERV_OCB_T *ocb)
{
    detail_sem(ctp, OCB2SEM(ocb), &msg->o, 0);
    return (IOSERV_PTR(ctp, &msg->o, sizeof(msg->o)));
}

/* Handle client/libc "sem_getvalue()" (historic implementation). */
static int sem_service_devctl(ioserv_context_t *ctp,
                              io_devctl_t *msg,
                              IOSERV_OCB_T *ocb)
{
    semaphore_t *sem;
    struct mq_attr *value;
    union {
        int oflag;
        int mountflag;
    } *data = (void *) (msg + 1);
    int error;

    sem = OCB2SEM(ocb);
    switch (msg->i.devctl_cmd) {
    case DEVCTL_GET_FLAGS:
        data->oflag =
            (ocb->ioflag & O_SETFLAG) | ((ocb->ioflag - 1) & O_ACCMODE);
        msg->o.ret_val = 0;
        error = IOSERV_PTR(ctp, &msg->o, sizeof(msg->o) + sizeof(data->oflag));
        break;
    case DEVCTL_SET_FLAGS:
        ocb->ioflag = (ocb->ioflag & ~O_SETFLAG) | (data->oflag & O_SETFLAG);
        error = EOK;
        break;
    case DEVCTL_GET_MOUNT_FLAGS:
        data->mountflag = 0;
        msg->o.ret_val = 0;
        error =
            IOSERV_PTR(ctp, &msg->o, sizeof(msg->o) + sizeof(data->mountflag));
        break;
    case DEVCTL_MQ_GET_ATTR:
        memset(value = (struct mq_attr *) _DEVCTL_DATA(msg->i), 0,
               sizeof(struct mq_attr));
        value->mq_curmsgs = (sem->attr.nbytes >= 0) ? sem->attr.nbytes : 0;
        if (ctp->info.flags & RTMUX_MI_ENDIAN_DIFF) {
            ENDIAN_SWAP32(&value->mq_curmsgs);
        }
        error =
            IOSERV_PTR(ctp, &msg->o, sizeof(msg->o) + sizeof(struct mq_attr));
        break;
    default:
        error = IOSERV_DEFAULT;
        break;
    }
    return (error);
}

/* Handle unblocking (client sem_wait()). */
static int sem_service_unblock(ioserv_context_t *ctp,
                               io_notification_t *msg,
                               IOSERV_OCB_T *ocb)
{
    struct _msg_info info;

    if (msg_info(ctp->rcvid, &info) != -1 &&
        (info.flags & RTMUX_MI_UNBLOCK_REQ)) {
        pthread_mutex_lock(&SemaphoreMutex);
        unblock_clients(OCB2SEM(ocb), ctp, 1, EINTR);
        pthread_mutex_unlock(&SemaphoreMutex);
    }
    return (IOSERV_NOREPLY);
}

/* Handle filesystem/pathname "open()". The directory can be opened for readdir
 * but the semaphores cannot be accessed except for stat.
 */
static int sem_dir_service_open(ioserv_context_t *ctp,
                                io_open_t *msg,
                                IOSERV_HANDLE_T *handle,
                                void *extra)
{
    semaphore_t *sem;
    char *name;
    struct _client_info cred;
    int error;

    if (msg->connect.extra_type != _IO_CONNECT_EXTRA_NONE ||
        msg->connect.file_type != _FTYPE_ANY)
        return (ENOSYS);
    if ((error = sem_client_info(ctp, &cred)) != EOK)
        return (error);
    if ((name = name_sem(msg->connect.path)) &&
        (msg->connect.ioflag & (_IO_FLAG_RD | _IO_FLAG_WR)))
        return (EACCES);
    pthread_mutex_lock(&SemaphoreMutex);
    sem = (!name) ? handle : lookup_sem(handle, name, NULL);
    if (!sem)
        error = (msg->connect.ioflag & O_CREAT) ? EACCES : ENOENT;
    else if ((error = sem_open_check(msg, &sem->attr, &cred)) == EOK) {
        IOSERV_OCB_T *ocb = sem_ocb_alloc(ctp, &sem->attr);

        if (!ocb)
            error = errno;
        else if ((error = sem_ocb_attach(ctp, msg, ocb, &sem->attr)) != EOK)
            sem_ocb_free(ocb);
    }
    pthread_mutex_unlock(&SemaphoreMutex);
    return (error);
}

/* Handle filesystem/pathname "unlink()" (and historic sem_unlink()). */
static int sem_dir_service_unlink(ioserv_context_t *ctp,
                                  io_unlink_t *msg,
                                  IOSERV_HANDLE_T *handle,
                                  void *extra)
{
    semaphore_t *sem;
    char *name;
    struct _client_info cred;
    int error;

    if (msg->connect.extra_type != _IO_CONNECT_EXTRA_NONE)
        return (ENOSYS);
    if ((error = sem_client_info(ctp, &cred)) != EOK)
        return (error);
    if (!(name = name_sem(msg->connect.path)))
        return (S_ISDIR(msg->connect.mode) ? EBUSY : ENOENT);
    pthread_mutex_lock(&SemaphoreMutex);
    if (!(sem = lookup_sem(handle, name, NULL)))
        error = ENOENT;
    else if ((error = sem_check_access(&handle->attr, S_IWUSR, &cred)) == EOK) {
        if ((handle->attr.mode & S_ISVTX) && cred.cred.euid != 0 &&
            cred.cred.euid != sem->attr.uid &&
            cred.cred.euid != handle->attr.uid) {
            error = EPERM;
        } else {
            if (sem->attr.nlink > 0)
                sem->attr.nlink--;
            destroy_sem(handle, sem, false);
        }
    }
    pthread_mutex_unlock(&SemaphoreMutex);
    return (error);
}

/* Handle filesystem/pathname "readdir()". */
static int sem_dir_service_read(ioserv_context_t *ctp,
                                io_read_t *msg,
                                IOSERV_OCB_T *ocb)
{
    struct _client_info cred;
    int error, xtra, nbytes;

    if ((ocb->ioflag & _IO_FLAG_RD) == 0)
        return (EBADF);
    if (!S_ISDIR(ocb->attr->mode))
        return (ENOTDIR);
    if ((msg->i.xtype & _IO_XTYPE_MASK) != _IO_XTYPE_NONE)
        return (ENOSYS);
    xtra = (msg->i.xtype & _IO_XFLAG_DIR_EXTRA_HINT) != 0 &&
           sem_client_info(ctp, &cred) == EOK &&
           sem_check_access(ocb->attr, S_IXUSR, &cred) == EOK;
    pthread_mutex_lock(&SemaphoreMutex);
    error = list_sem(
        ctp, OCB2SEM(ocb), &ocb->offset, (struct dirent *) msg,
        min_uint((unsigned) msg->i.nbytes,
                 (unsigned) (ctp->msg_max_size - (unsigned) ctp->offset)),
        xtra, &nbytes);
    pthread_mutex_unlock(&SemaphoreMutex);
    ocb->attr->atime = time(NULL);
    _IO_SET_READ_NBYTES(ctp, nbytes);
    return ((error != EOK) ? error : IOSERV_PTR(ctp, msg, nbytes));
}

/* Handle filesystem/pathname "close()". */
static int sem_dir_service_close(ioserv_context_t *ctp,
                                 void *reserved,
                                 IOSERV_OCB_T *ocb)
{
    return (sem_service_close(ctp, reserved, ocb));
}

/* Handle filesystem/pathname "fstat()". */
static int sem_dir_service_stat(ioserv_context_t *ctp,
                                io_stat_t *msg,
                                IOSERV_OCB_T *ocb)
{
    if (S_ISDIR(ocb->attr->mode)) {
        detail_sem(ctp, OCB2SEM(ocb), &msg->o, 1);
    } else {
        detail_sem(ctp, OCB2SEM(ocb), &msg->o, 1);
    }
    return (IOSERV_PTR(ctp, &msg->o, sizeof(msg->o)));
}

static int sem_dir_service_fdinfo(ioserv_context_t *ctp,
                                  io_fdinfo_t *msg,
                                  IOSERV_OCB_T *ocb)
{
    const char path[] = "/dev/sem";
    char *out = (char *) (&msg->o + 1);
    size_t cap = (size_t) msg->i.path_len;
    size_t len = sizeof path;

    if (!S_ISDIR(ocb->attr->mode))
        return ENOSYS;
    memset(&msg->o, 0, sizeof msg->o);
    msg->o.info.mode = (uint32_t) ocb->attr->mode;
    msg->o.info.offset = (uint64_t) ocb->offset;
    msg->o.info.size = (uint64_t) ocb->attr->nbytes;
    if (cap > 0)
        memcpy(out, path, cap < len ? cap : len);
    _IO_SET_FDINFO_LEN(ctp, len);
    return IOSERV_PTR(ctp, &msg->o,
                      sizeof msg->o + (unsigned) (cap < len ? cap : len));
}

/* Specific OCB allocation/free routines. */
static IOSERV_OCB_T *sem_ocb_alloc(ioserv_context_t *ctp, sem_attr_t *attr)
{
    IOSERV_OCB_T *ocb;

    if ((ocb = _scalloc(sizeof(IOSERV_OCB_T))))
        return (ocb);
    errno = ENFILE;
    return (NULL);
}

static void sem_ocb_free(IOSERV_OCB_T *ocb)
{
    _sfree(ocb, sizeof(IOSERV_OCB_T));
}

/* Initialize the named semaphore subsystem. */
void namedsem_init(void)
{
    memset(&SemConnectFuncs, 0, sizeof(SemConnectFuncs));
    memset(&SemIoFuncs, 0, sizeof(SemIoFuncs));
    SemConnectFuncs.nfuncs = IOSERV_CONNECT_NFUNCS;
    SemIoFuncs.nfuncs = IOSERV_IO_NFUNCS;
    SemConnectFuncs.open = sem_service_open,
    SemConnectFuncs.unlink = sem_service_unlink;
    SemIoFuncs.read = sem_service_read, SemIoFuncs.write = sem_service_write;
    SemIoFuncs.close_ocb = sem_service_close,
    SemIoFuncs.stat = sem_service_stat;
    SemIoFuncs.devctl = sem_service_devctl,
    SemIoFuncs.unblock = sem_service_unblock;
    memset(&DirConnectFuncs, 0, sizeof(DirConnectFuncs));
    memset(&DirIoFuncs, 0, sizeof(DirIoFuncs));
    DirConnectFuncs.nfuncs = IOSERV_CONNECT_NFUNCS;
    DirIoFuncs.nfuncs = IOSERV_IO_NFUNCS;
    DirConnectFuncs.open = sem_dir_service_open,
    DirConnectFuncs.unlink = sem_dir_service_unlink;
    DirIoFuncs.read = sem_dir_service_read,
    DirIoFuncs.close_ocb = sem_dir_service_close,
    DirIoFuncs.stat = sem_dir_service_stat;
    DirIoFuncs.fdinfo = sem_dir_service_fdinfo;
    memset(&ioserv_attr, 0, sizeof(ioserv_attr));
    ioserv_attr.nparts_max = 1,
    ioserv_attr.msg_max_size = sizeof(struct _io_connect) + _POSIX_PATH_MAX;

    memset(&NamedSemaphores, 0, sizeof(NamedSemaphores));
    sem_attr_init(&NamedSemaphores.attr, S_IFDIR | S_ISVTX | 0777, NULL);
    ++NamedSemaphores.attr.nlink;
    NamedSemaphores.initial = 0;
    PrefixLength = strlen(NamedSemaphores.name = "/dev/sem") - 1;
    memset(&Mount, 0, sizeof(Mount));
    NamedSemaphores.attr.rdev =
        (dev_t) ioserv_attach(dpp, &ioserv_attr, NULL, _FTYPE_SEM,
                              IOSERV_FLAG_DIR | IOSERV_FLAG_FTYPEONLY,
                              &SemConnectFuncs, &SemIoFuncs, &NamedSemaphores);
    NamedSemaphores.attr.inode = (uintptr_t) &NamedSemaphores;
    DirPathname = ioserv_attach(dpp, &ioserv_attr, NamedSemaphores.name,
                                _FTYPE_ANY, IOSERV_FLAG_DIR, &DirConnectFuncs,
                                &DirIoFuncs, &NamedSemaphores);
    hwres_proc_devno(_MAJOR_FSYS, &Mount.dev, -1, 0);

    roottask_sysconf_set(0, _CONF_NUM, _SC_SEMAPHORES, _POSIX_VERSION, NULL);

    /* Set _SC_SEM_NSEMS_MAX to -1 (indeterminate value) since the limit applies
     * to both unnamed and named sempahores. There is no limit for unnamed
     * semaphores since they are sync objects.
     */
    MaxSems = (int) max_uint(roottask_max_fds - STDERR_FILENO - 2,
                             (unsigned) _POSIX_SEM_NSEMS_MAX);
    roottask_sysconf_set(0, _CONF_NUM, _SC_SEM_NSEMS_MAX, -1, NULL);
    roottask_sysconf_set(0, _CONF_NUM, _SC_SEM_VALUE_MAX, SEM_VALUE_MAX, NULL);
}
