/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "externs.h"
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <unistd.h>
#include <devctl/common.h>
#include <sys/procfs.h>
#include "path-node.h"
#include "path-object.h"
#include "path.h"
#include "mm-internal.h"
#include "root-process.h"

/* Interface break - we really shouldn't be referencing mm fields here, but it's
 * silly to go through the extra hassle.
 */
#define OBJECT_SIZE(o) ((o)->mem.mm.size)

static int mem_handle;
static dev_t mem_devno;

struct mem_ocb {
    uint64_t pos;
    int ioflag;
    int tflags; /* for typed memory objects */
    path_object_t *object;
};

static int mode_check_access(mode_t mode,
                             uid_t uid,
                             gid_t gid,
                             mode_t check,
                             const struct _client_info *info)
{
    mode_t allowed;

    if (info->cred.euid == 0)
        return EOK;
    if ((check & S_ISUID) && info->cred.euid == uid)
        return EOK;
    if (check & S_ISGID) {
        if (info->cred.egid == gid)
            return EOK;
        for (uint32_t i = 0; i < info->cred.ngroups; i++)
            if (info->cred.grouplist[i] == gid)
                return EOK;
    }

    check &= S_IRWXU;
    if (check == 0)
        return EPERM;

    if (info->cred.euid == uid) {
        allowed = mode;
    } else {
        int in_group = info->cred.egid == gid;

        for (uint32_t i = 0; !in_group && i < info->cred.ngroups; i++)
            in_group = info->cred.grouplist[i] == gid;
        allowed = in_group ? mode << 3 : mode << 6;
    }

    return (allowed & check) == check ? EOK : EACCES;
}

typedef struct {
    io_write_t write;
    struct _xtype_offset offset;
} io_pwrite_t;

typedef struct {
    io_read_t read;
    struct _xtype_offset offset;
} io_pread_t;


/* Release-on-close for advisory file locks; defined with the lock helpers
 * below, forward-declared here for mem_close_ocb.
 */
static void mem_flock_release(path_object_t *obj, pid_t pid);
static void mem_flock_wake(path_object_t *obj);
static void mem_flock_purge(int scoid, int coid);

static int mem_read_write(ioserv_context_t *ctp,
                          ioserv_msg_t *msg,
                          struct mem_ocb *ocb)
{
    void *addr;
    size_t nbytes, size;
    process_t *proc;
    path_object_t *obp;
    uint64_t *pos;
    uint64_t offset;
    size_t skip;
    int client_locked = 0;

    /* IO_READ and IO_WRITE messages exactly overlay, as does XTYPE_OFFSET! */

    if (!ocb) {
        nbytes = 0;
        goto ok;
    }
    if (!(ocb->ioflag &
          ((msg->type == _IO_READ) ? _IO_FLAG_RD : _IO_FLAG_WR))) {
        return EBADF;
    }
    nbytes = msg->read.i.nbytes;

    obp = ocb->object;
    if ((msg->type == _IO_WRITE) && (ocb->ioflag & O_APPEND) && obp) {
        ocb->pos = (uint64_t) OBJECT_SIZE(obp);
    }

    switch (msg->read.i.xtype & _IO_XTYPE_MASK) {
    case _IO_XTYPE_NONE:
        pos = &ocb->pos;
        skip = sizeof(msg->read.i);
        break;
    case _IO_XTYPE_OFFSET:
        offset = (uint64_t) ((io_pread_t *) msg)->offset.offset;
        pos = &offset;
        skip = sizeof(io_pread_t);
        break;
    default: /* this really shouldn't happen, but just in case */
        return ENOSYS;
    }

    proc = roottask_prp;
    size = obp ? (size_t) OBJECT_SIZE(obp) : UINT_MAX;
    if (!obp) {
        if (proc_rlock_adp(ctp->info.pid) != EOK) {
            return ESRCH;
        }
        client_locked = 1;
        if (root_proc(ctp->info.pid)->phys_contained) {
            proc_unlock_adp(ctp->info.pid);
            return EPERM;
        }
    }
    if (*pos > size - nbytes || *pos + nbytes > size) {
        if (obp && msg->type == _IO_WRITE) {
            int status;

            status = root_vm_ops.resize(obp, *pos + nbytes);
            if (status != EOK) {
                if (client_locked)
                    proc_unlock_adp(ctp->info.pid);
                return status;
            }
            size = (size_t) OBJECT_SIZE(obp);
        }
        if (*pos >= size) {
            nbytes = 0;
        } else {
            nbytes = size - *pos;
        }
    }

    if (nbytes) {
        int status, err;
        int flags;

        if (proc_thread_pool_reserve() != 0) {
            if (client_locked)
                proc_unlock_adp(ctp->info.pid);
            return EAGAIN;
        }
        process_bind(ROOTTASK_PID);

        proc_wlock_adp(ROOTTASK_PID);
        flags = MAP_SHARED;
        if (obp) {
            proc_mux_lock(&obp->mem.mm.mux);
        } else {
            flags |= MAP_PHYS;
        }
        status = root_vm_ops.mmap(ROOTTASK_PID, proc, 0, nbytes,
                                  PROT_READ | PROT_WRITE, flags, obp, *pos, 0,
                                  nbytes, NOFD, &addr, &size);
        if (obp)
            proc_mux_unlock(&obp->mem.mm.mux);
        proc_unlock_adp(ROOTTASK_PID);
        if (status != EOK) {
            process_bind(0);
            proc_thread_pool_reserve_done();
            if (client_locked)
                proc_unlock_adp(ctp->info.pid);
            return status;
        }

        SETIOV(ctp->iov + 0, addr, nbytes);

        if (msg->type == _IO_READ) {
            status = ioserv_msgwritev(ctp, ctp->iov + 0, 1, 0);
        } else {
            status = ioserv_msgreadv(ctp, ctp->iov + 0, 1, (int) skip);
        }
        err = errno;

        proc_wlock_adp(ROOTTASK_PID);
        (void) root_vm_ops.munmap(ROOTTASK_PID, proc, (uintptr_t) addr, size,
                                  0);
        proc_unlock_adp(ROOTTASK_PID);

        if (status == -1) {
            process_bind(0);
            proc_thread_pool_reserve_done();
            if (client_locked)
                proc_unlock_adp(ctp->info.pid);
            return err;
        }

        *pos += nbytes;
        if (obp && msg->type == _IO_WRITE) {
            obp->mem.pm.mtime = time(0);
        }
        process_bind(0);
        proc_thread_pool_reserve_done();
    }
    if (client_locked)
        proc_unlock_adp(ctp->info.pid);

ok:
    _IO_SET_READ_NBYTES(ctp, nbytes);
    return EOK;
}


static int mem_read(ioserv_context_t *ctp, io_read_t *msg, void *vocb)
{
    return mem_read_write(ctp, (ioserv_msg_t *) msg, (struct mem_ocb *) vocb);
}


static int mem_write(ioserv_context_t *ctp, io_write_t *msg, void *vocb)
{
    return mem_read_write(ctp, (ioserv_msg_t *) msg, (struct mem_ocb *) vocb);
}


static int mem_close_ocb(ioserv_context_t *ctp, void *reserved, void *vocb)
{
    struct mem_ocb *ocb = vocb;

    if (ocb) {
        path_object_t *obp;

        if ((obp = ocb->object)) {
            /* Drop any advisory locks the closing process held on the file.
             * POSIX releases a process's locks on ANY close of a descriptor to
             * the file (and roottask closes a dead process's descriptors, so
             * this also covers exit); mem_close_dup handles the intermediate
             * closes of duplicated descriptors.
             */
            mem_flock_purge(ctp->info.scoid, ctp->info.coid);
            mem_flock_release(obp, ctp->info.pid);
            mem_flock_wake(obp);

            /* RUSH3: Should get rid of vm_tymem_close() and RUSH3: do
             * everything it does in tymem_close()
             */
            if (obp->hdr.type == OBJECT_MEM_TYPED) {
                vm_tymem_close(obp);
            }
            path_object_done(obp);
        }
        _sfree(ocb, sizeof *ocb);
    }
    return EOK;
}


static int mem_lseek(ioserv_context_t *ctp, io_lseek_t *msg, void *vocb)
{
    struct mem_ocb *ocb = vocb;
    uint64_t pos;
    uint64_t size;

    if (!ocb) {
        return EOK;
    }
    pos = (uint64_t) msg->i.offset;
    switch (msg->i.whence) {
    case SEEK_SET:
        break;
    case SEEK_CUR:
        pos = ocb->pos + pos;
        break;
    case SEEK_END:
        size = ocb->object ? (uint64_t) OBJECT_SIZE(ocb->object) : UINT_MAX;
        pos = size + pos;
        break;
    default:
        return EINVAL;
    }
    ocb->pos = pos;
    if (msg->i.combine_len & _IO_COMBINE_FLAG) {
        return EOK;
    }
    msg->o = ocb->pos;
    return IOSERV_PTR(ctp, &msg->o, sizeof msg->o);
}


static int mem_stat(ioserv_context_t *ctp, io_stat_t *msg, void *vocb)
{
    struct mem_ocb *ocb = vocb;
    path_object_t *obp = ocb ? ocb->object : 0;
    size_t size = ocb ? (obp ? (size_t) OBJECT_SIZE(obp) : UINT_MAX) : 0;

    memset(&msg->o, 0x00, sizeof msg->o);
    msg->o.st_size = (off_t) size;
    msg->o.st_nblocks = (int32_t) ((size + (__PAGESIZE - 1)) / __PAGESIZE);
    msg->o.st_blocksize = msg->o.st_blksize = __PAGESIZE;
    msg->o.st_ctime = msg->o.st_mtime = msg->o.st_atime =
        (obp ? obp->mem.pm.mtime : time(0));
    msg->o.st_ino = obp ? (uintptr_t) INODE_XOR(obp) : 1;
    if (ocb && obp) {
        msg->o.st_uid = obp->mem.pm.uid;
        msg->o.st_gid = obp->mem.pm.gid;
        msg->o.st_mode =
            (obp->mem.pm.mode != 0) ? obp->mem.pm.mode : S_IFNAM | 0777;
        msg->o.st_nlink = obp->hdr.refs;
        msg->o.st_dev = path_devno;
        msg->o.st_rdev = S_ISNAM(msg->o.st_mode) ? _S_INSHD : 0;
    } else {
        /* The /dev/mem device is set to rw root only, the /dev/shmem device is
         * set to rwx for all, since it is un-enforced
         */
        msg->o.st_mode = ocb ? S_IFREG | 0600 : S_IFDIR | 0777;
        msg->o.st_nlink = ocb ? 1 : 2;
        msg->o.st_dev = mem_devno;
        msg->o.st_rdev = 0;
    }
    msg->o.st_dev |= (ctp->info.srcnd << RTMUX_NODE_BITS);
    return IOSERV_PTR(ctp, &msg->o, sizeof msg->o);
}


static int mem_devctl(ioserv_context_t *ctp, io_devctl_t *msg, void *vocb)
{
    struct mem_ocb *ocb = vocb;
    path_object_t *obp;
    union {
        int oflag;
        int mountflag;
        devctl_mem_object memobj;
        devctl_mem_object_info memobj_info;
    } *data = (void *) (msg + 1);
    unsigned nbytes = 0;

    switch ((unsigned) msg->i.devctl_cmd) {
    case DEVCTL_GET_FLAGS:
        data->oflag =
            (ocb) ? (ocb->ioflag & ~O_ACCMODE) | ((ocb->ioflag - 1) & O_ACCMODE)
                  : O_RDWR;
        nbytes = sizeof data->oflag;
        break;

    case DEVCTL_SET_FLAGS:
        if (ocb) {
            ocb->ioflag =
                (ocb->ioflag & ~O_SETFLAG) | (data->oflag & O_SETFLAG);
        }
        break;

    case DEVCTL_GET_MOUNT_FLAGS:
        data->mountflag = 0;
        nbytes = sizeof data->mountflag;
        break;

    case DEVCTL_MEM_OBJECT_INFO:
        if (!ocb)
            return IOSERV_DEFAULT;

        memset(&data->memobj_info, 0, sizeof data->memobj_info);
        data->memobj_info.version = 1;
        data->memobj_info.cache_policy = MEMOBJ_INFO_CACHE_DEFAULT;

        obp = ocb->object;
        if (!obp) {
            data->memobj_info.type = MEMOBJ_INFO_TYPE_DEV_MEM;
            data->memobj_info.flags = MEMOBJ_INFO_FLAG_FILE_BACKED |
                                      MEMOBJ_INFO_FLAG_PHYSICAL |
                                      MEMOBJ_INFO_FLAG_PRIVILEGED;
            data->memobj_info.cache_policy = MEMOBJ_INFO_CACHE_DEVICE_NOCACHE;
            data->memobj_info.size = (uint64_t) last_paddr + 1;
        } else {
            data->memobj_info.type = (obp->hdr.type == OBJECT_MEM_TYPED)
                                         ? MEMOBJ_INFO_TYPE_TYPED
                                         : MEMOBJ_INFO_TYPE_SHARED;
            data->memobj_info.flags = MEMOBJ_INFO_FLAG_FILE_BACKED;
            data->memobj_info.size = (uint64_t) OBJECT_SIZE(obp);
            data->memobj_info.tflags = (uint32_t) ocb->tflags;
            if (obp->mem.mm.flags & SHMCTL_PHYS)
                data->memobj_info.flags |= MEMOBJ_INFO_FLAG_PHYSICAL;
            if (obp->mem.mm.flags & SHMCTL_PRIV)
                data->memobj_info.flags |= MEMOBJ_INFO_FLAG_PRIVILEGED;
            if (obp->hdr.type == OBJECT_MEM_TYPED) {
                struct pa_restrict *rp;
                uint64_t extent = 0;

                /* A typed pool has no fixed mm.size (allocation is on demand,
                 * so OBJECT_SIZE is 0). Report its total extent — the sum of
                 * the restriction ranges that define the pool — so
                 * posix_typed_mem_get_info returns a real maximum allocatable
                 * length. Pure list walk (the restriction list is stable for an
                 * open fd); the free-space variant in vm-info needs the memobj
                 * lock this devctl path does not hold.
                 */
                for (rp = obp->mem.mm.restriction; rp; rp = rp->next)
                    extent += (uint64_t) (rp->end - rp->start) + 1;
                data->memobj_info.size = extent;

                data->memobj_info.flags |= MEMOBJ_INFO_FLAG_TYPED;
                if (ocb->tflags & POSIX_TYPED_MEM_ALLOCATE)
                    data->memobj_info.flags |= MEMOBJ_INFO_FLAG_ALLOCATE;
                if (ocb->tflags & POSIX_TYPED_MEM_ALLOCATE_CONTIG)
                    data->memobj_info.flags |= MEMOBJ_INFO_FLAG_ALLOCATE_CONTIG;
                if (ocb->tflags & POSIX_TYPED_MEM_MAP_ALLOCATABLE)
                    data->memobj_info.flags |= MEMOBJ_INFO_FLAG_MAP_ALLOCATABLE;
                if (obp->mem.mm.flags & MM_MEM_RDB)
                    data->memobj_info.flags |= MEMOBJ_INFO_FLAG_RESOURCEDB;
            }
        }
        nbytes = sizeof data->memobj_info;
        break;

    default:
        if (!ocb)
            return IOSERV_DEFAULT;
        obp = ocb->object;
        if (!obp)
            return IOSERV_DEFAULT;
        return object_devctl(ctp, msg, obp);
    }

    if (nbytes) {
        msg->o.ret_val = 0;
        return IOSERV_PTR(ctp, &msg->o, sizeof msg->o + nbytes);
    }
    return EOK;
}


static int mem_utime(ioserv_context_t *ctp, io_utime_t *msg, void *vocb)
{
    struct mem_ocb *ocb = vocb;
    path_object_t *obp;

    if (ocb && (obp = ocb->object)) {
        if (msg->i.cur_flag) {
            obp->mem.pm.mtime = time(0);
        } else {
            obp->mem.pm.mtime = msg->i.times.modtime;
        }
    }
    return EOK;
}


static int mem_space(ioserv_context_t *ctp, io_space_t *msg, void *vocb)
{
    struct mem_ocb *ocb = vocb;
    uint64_t start, end;
    path_object_t *obj;
    int status;

    if (!ocb || !(obj = ocb->object)) {
        return IOSERV_DEFAULT;
    }
    start = (uint64_t) msg->i.start;
    switch (msg->i.whence) {
    case SEEK_SET:
        break;
    case SEEK_CUR:
        start = ocb->pos + start;
        break;
    case SEEK_END:
        start = (uint64_t) OBJECT_SIZE(ocb->object) + start;
        break;
    default:
        return EINVAL;
    }

    /* This is an ENORMOUS HUGE BIG UGLY HACK! We take in 64 bit offsets but all
     * of the memory objects and memory functions use unsigned int values for
     * their counters and sized so that if we just pass the 64 bit values
     * directly then bad things tend to happen. This is a stop-gap measure until
     * we fix that.
     */
    {
        uint64_t lend;
        lend = msg->i.len ? start + msg->i.len : (uint64_t) OBJECT_SIZE(obj);
        if (lend > UINT_MAX) {
            return ENOMEM;
        }
    }

    end = msg->i.len ? start + msg->i.len : (uint64_t) OBJECT_SIZE(obj);

    switch (msg->i.subtype) {
    case F_ALLOCSP:
        if (end > (uint64_t) OBJECT_SIZE(obj)) {
            status = vm_resize(obj, end);
            if (status != EOK) {
                return status;
            }
        }
        break;
    case F_FREESP:
        if (end < (uint64_t) OBJECT_SIZE(obj) || !(ocb->ioflag & _IO_FLAG_WR)) {
            return EINVAL;
        }
        if (start < (uint64_t) OBJECT_SIZE(obj)) {
            status = vm_resize(obj, start);
            if (status != EOK) {
                return status;
            }
        }
        break;
    default:
        return EINVAL;
    }

    msg->o = (uint64_t) OBJECT_SIZE(obj);
    return IOSERV_PTR(ctp, &msg->o, sizeof msg->o);
}


static int mem_change_attr(ioserv_context_t *ctp, ioserv_msg_t *msg, void *vocb)
{
    struct mem_ocb *ocb = vocb;
    path_object_t *obp;
    struct _client_info info;
    mode_t mode;
    int ret;

    if (!ocb || !(obp = ocb->object) ||
        (!S_ISNAM(obp->mem.pm.mode) && !S_ISREG(obp->mem.pm.mode))) {
        return IOSERV_DEFAULT;
    }
    ret = connect_client_info(ctp->info.scoid, &info, NGROUPS_MAX);
    if (ret != EOK)
        return ret;

    switch (msg->type) {
    case _IO_CHOWN:
        if (msg->chown.i.uid == -1)
            msg->chown.i.uid = obp->mem.pm.uid;
        if (msg->chown.i.gid == -1)
            msg->chown.i.gid = obp->mem.pm.gid;
        ret = mode_check_access(obp->mem.pm.mode, obp->mem.pm.uid,
                                obp->mem.pm.gid, 0, &info);
        if (ret != EOK && (obp->mem.pm.uid != msg->chown.i.uid ||
                           info.cred.euid != obp->mem.pm.uid ||
                           mode_check_access(0, 0, msg->chown.i.gid, S_ISGID,
                                             &info) != EOK)) {
            return ret;
        }
        if (info.cred.euid != 0 && (obp->mem.pm.mode & (S_ISUID | S_ISGID)) &&
            !S_ISDIR(obp->mem.pm.mode) &&
            (obp->mem.pm.mode & (S_IXUSR | S_IXGRP | S_IXOTH))) {
            obp->mem.pm.mode &= ~(mode_t) (S_ISUID | S_ISGID);
        }
        obp->mem.pm.gid = msg->chown.i.gid;
        obp->mem.pm.uid = msg->chown.i.uid;
        ret = EOK;
        break;
    case _IO_CHMOD:
        ret = mode_check_access(obp->mem.pm.mode, obp->mem.pm.uid,
                                obp->mem.pm.gid, S_ISUID, &info);
        if (ret != EOK)
            return ret;
        mode = (obp->mem.pm.mode &
                ~(mode_t) (0777 | S_ISUID | S_ISGID | S_ISVTX)) |
               (msg->chmod.i.mode & (0777 | S_ISUID | S_ISGID | S_ISVTX));
        if ((mode & S_ISVTX) && !S_ISDIR(mode) &&
            mode_check_access(obp->mem.pm.mode, obp->mem.pm.uid,
                              obp->mem.pm.gid, 0, &info) != EOK) {
            mode &= ~(mode_t) S_ISVTX;
        }
        if ((mode & S_ISGID) && !S_ISDIR(mode) &&
            (mode & (S_IXUSR | S_IXGRP | S_IXOTH)) &&
            mode_check_access(obp->mem.pm.mode, obp->mem.pm.uid,
                              obp->mem.pm.gid, S_ISGID, &info) != EOK) {
            mode &= ~(mode_t) S_ISGID;
        }
        obp->mem.pm.mode = mode;
        ret = EOK;
        break;
    default:
        ret = IOSERV_DEFAULT;
        break;
    }

    return ret;
}


static int mem_chown(ioserv_context_t *ctp, io_chown_t *msg, void *vocb)
{
    return mem_change_attr(ctp, (ioserv_msg_t *) msg, vocb);
}


static int mem_chmod(ioserv_context_t *ctp, io_chmod_t *msg, void *vocb)
{
    return mem_change_attr(ctp, (ioserv_msg_t *) msg, vocb);
}


static int mem_sync(ioserv_context_t *ctp, io_sync_t *msg, void *vocb)
{
    (void) ctp;
    (void) msg;
    (void) vocb;

    /* A RAM-backed memory object has no slower durable tier to flush to, so
     * fsync()/fdatasync() are trivially complete.
     */
    return EOK;
}


/* POSIX advisory file locks (fcntl F_GETLK/F_SETLK/F_SETLKW).
 *
 * Byte-range locks keyed by (object, pid): path_object_clone() refcounts one
 * path_object_t per file, so every open of a file shares the same object
 * pointer, and locks are per-process (classic fcntl, not per-fd). Each record
 * covers a half-open range [start, end) (end == MEM_FLOCK_END_MAX means "to the
 * largest possible offset", i.e. l_len == 0). A shared read lock (F_RDLCK)
 * coexists with other read locks; a write lock (F_WRLCK) is exclusive; a lock
 * conflicts only with a DIFFERENT pid's incompatible, OVERLAPPING lock. A
 * process's own overlapping ranges are replaced/split on set and unlock. The
 * list is not coalesced (behavior is identical either way). F_SETLKW
 * blocking-on-conflict is not supported (ENOSYS). roottask serves messages on a
 * thread pool, so a single mutex serializes the (rare) lock operations.
 */
#define MEM_FLOCK_END_MAX ((off_t) INT64_MAX)

struct mem_flock {
    struct mem_flock *next;
    path_object_t *obj;
    pid_t pid;
    short type; /* F_RDLCK or F_WRLCK */
    off_t start;
    off_t end; /* half-open; MEM_FLOCK_END_MAX == to the largest offset */
};

static struct mem_flock *mem_flocks;
static pthread_mutex_t mem_flock_mux = PTHREAD_MUTEX_INITIALIZER;

/* Resolve a flock's whence/start/len into an absolute half-open range [*ps,
 * *pe).
 *
 * Returns EOK, or EINVAL for a negative start or a wrapped range.
 */
static int mem_flock_range(const struct mem_ocb *ocb,
                           path_object_t *obp,
                           const flock_t *fl,
                           off_t *ps,
                           off_t *pe)
{
    off_t base, start, end;

    switch (fl->l_whence) {
    case SEEK_SET:
        base = 0;
        break;
    case SEEK_CUR:
        if (ocb->pos > (uint64_t) MEM_FLOCK_END_MAX)
            return EOVERFLOW;
        base = (off_t) ocb->pos;
        break;
    case SEEK_END:
        if ((size_t) OBJECT_SIZE(obp) > (size_t) MEM_FLOCK_END_MAX)
            return EOVERFLOW;
        base = (off_t) OBJECT_SIZE(obp);
        break;
    default:
        return EINVAL;
    }

    /* base >= 0. Check each addition against off_t bounds BEFORE computing it,
     * so no signed overflow (undefined behavior) can slip past.
     */
    if (fl->l_start >= 0 && base > MEM_FLOCK_END_MAX - fl->l_start)
        return EOVERFLOW;
    start = base + fl->l_start;
    if (start < 0)
        return EINVAL; /* range starts before offset 0 */
    if (fl->l_len == 0) {
        end = MEM_FLOCK_END_MAX;
    } else if (fl->l_len > 0) {
        if (start > MEM_FLOCK_END_MAX - fl->l_len)
            return EOVERFLOW;
        end = start + fl->l_len;
    } else {
        /* A negative l_len locks the bytes preceding l_start. start >= 0 so the
         * addition of a negative l_len cannot underflow off_t.
         */
        end = start;
        start = start + fl->l_len;
        if (start < 0)
            return EINVAL;
    }
    *ps = start;
    *pe = end;
    return EOK;
}

/* Caller holds mem_flock_mux.
 *
 * Return a lock on `obj` held by a DIFFERENT pid that is incompatible with
 * taking `type` over [s, e), or NULL if none.
 */
static struct mem_flock *mem_flock_conflict(path_object_t *obj,
                                            pid_t pid,
                                            short type,
                                            off_t s,
                                            off_t e)
{
    struct mem_flock *fl;

    for (fl = mem_flocks; fl; fl = fl->next) {
        if (fl->obj != obj || fl->pid == pid)
            continue;
        if (type != F_WRLCK && fl->type != F_WRLCK)
            continue; /* two read locks never conflict */
        if (s < fl->end && fl->start < e)
            return fl;
    }
    return NULL;
}

/* Caller holds mem_flock_mux. Drop every lock on `obj` held by `pid`
 * (release-on-close).
 */
static void mem_flock_drop(path_object_t *obj, pid_t pid)
{
    struct mem_flock **pp = &mem_flocks;

    while (*pp) {
        struct mem_flock *fl = *pp;

        if (fl->obj == obj && fl->pid == pid) {
            *pp = fl->next;
            _sfree(fl, sizeof *fl);
        } else {
            pp = &fl->next;
        }
    }
}

/* Caller holds mem_flock_mux. Remove `pid`'s coverage of [s, e) on `obj`,
 * splitting any lock that extends past the range so the outside fragments
 * survive.
 *
 * Returns EOK, or ENOMEM if a fragment allocation failed (rare; the list stays
 * consistent, just missing that fragment).
 */
static int mem_flock_clear(path_object_t *obj, pid_t pid, off_t s, off_t e)
{
    struct mem_flock **pp = &mem_flocks;

    while (*pp) {
        struct mem_flock *fl = *pp;
        struct mem_flock *lo = NULL, *hi = NULL;
        off_t ls, le;
        short lt;

        if (fl->obj != obj || fl->pid != pid || s >= fl->end ||
            fl->start >= e) {
            pp = &fl->next;
            continue;
        }
        ls = fl->start;
        le = fl->end;
        lt = fl->type;

        /* Allocate the surviving outside fragments BEFORE unlinking the
         * original, so an allocation failure leaves the lock intact rather than
         * silently unlocking bytes the owner still believes it holds.
         */
        if (ls < s && !(lo = _scalloc(sizeof *lo)))
            return ENOMEM;
        if (le > e && !(hi = _scalloc(sizeof *hi))) {
            if (lo)
                _sfree(lo, sizeof *lo);
            return ENOMEM;
        }
        *pp = fl->next;
        _sfree(fl, sizeof *fl);
        if (lo) {
            lo->obj = obj;
            lo->pid = pid;
            lo->type = lt;
            lo->start = ls;
            lo->end = s;
            lo->next = *pp;
            *pp = lo;
            pp = &lo->next;
        }
        if (hi) {
            hi->obj = obj;
            hi->pid = pid;
            hi->type = lt;
            hi->start = e;
            hi->end = le;
            hi->next = *pp;
            *pp = hi;
            pp = &hi->next;
        }
    }
    return EOK;
}

/* Caller holds mem_flock_mux. Fill a freshly allocated record and push it onto
 * the lock list. The caller must allocate before any mem_flock_clear() so a
 * failure can't drop an existing lock; this helper only links the record in.
 */
static void mem_flock_link(struct mem_flock *nl,
                           path_object_t *obj,
                           pid_t pid,
                           short type,
                           off_t s,
                           off_t e)
{
    nl->obj = obj;
    nl->pid = pid;
    nl->type = type;
    nl->start = s;
    nl->end = e;
    nl->next = mem_flocks;
    mem_flocks = nl;
}

/* Drop `pid`'s locks on `obj` under the lock (used by the close path). */
static void mem_flock_release(path_object_t *obj, pid_t pid)
{
    pthread_mutex_lock(&mem_flock_mux);
    mem_flock_drop(obj, pid);
    pthread_mutex_unlock(&mem_flock_mux);
}

/* An F_SETLKW client blocked on a conflict. The handler returns IOSERV_NOREPLY
 * (freeing the pool thread) and the client stays blocked in fcntl() until
 * mem_flock_wake() grants the lock and msg_reply()s the saved rcvid, the
 * .unblock path msg_error()s it (killed/interrupted), or a close purges it.
 */
struct mem_flock_waiter {
    struct mem_flock_waiter *next;
    int rcvid;
    int scoid;
    int coid;
    path_object_t *obj;
    pid_t pid;
    short type;
    off_t start;
    off_t end;
};

static struct mem_flock_waiter *mem_flock_waiters;

/* Caller holds mem_flock_mux. Record an F_SETLKW waiter; 1 on success, 0 on
 * ENOMEM.
 */
static int mem_flock_enqueue(ioserv_context_t *ctp,
                             path_object_t *obj,
                             pid_t pid,
                             short type,
                             off_t s,
                             off_t e)
{
    struct mem_flock_waiter *w = _scalloc(sizeof *w);

    if (!w)
        return 0;
    w->rcvid = ctp->rcvid;
    w->scoid = ctp->info.scoid;
    w->coid = ctp->info.coid;
    w->obj = obj;
    w->pid = pid;
    w->type = type;
    w->start = s;
    w->end = e;
    w->next = mem_flock_waiters;
    mem_flock_waiters = w;
    return 1;
}

/* After a lock on `obj` is released, grant every waiter whose range no longer
 * conflicts and wake it. Replies are sent after the mutex is dropped, so no IPC
 * runs under the lock. Granting one waiter can re-block a later one, so each is
 * re-checked in turn.
 */
static void mem_flock_wake(path_object_t *obj)
{
    struct mem_flock_waiter *granted = NULL, *failed = NULL;
    struct mem_flock_waiter **pp = &mem_flock_waiters;

    pthread_mutex_lock(&mem_flock_mux);
    while (*pp) {
        struct mem_flock_waiter *w = *pp;
        struct mem_flock *nl;

        if (w->obj != obj ||
            mem_flock_conflict(obj, w->pid, w->type, w->start, w->end)) {
            pp = &w->next;
            continue;
        }

        /* Grantable: unlink, then allocate the record before clearing this
         * pid's overlap so a failure can't drop an existing lock. On OOM the
         * range is already free, so leaving the waiter queued could hang it
         * forever (nothing else will wake it); fail it with ENOMEM instead.
         */
        *pp = w->next;
        if (!(nl = _scalloc(sizeof *nl)) ||
            mem_flock_clear(obj, w->pid, w->start, w->end) != EOK) {
            if (nl)
                _sfree(nl, sizeof *nl);
            w->next = failed;
            failed = w;
            continue;
        }
        mem_flock_link(nl, obj, w->pid, w->type, w->start, w->end);
        w->next = granted;
        granted = w;
    }
    pthread_mutex_unlock(&mem_flock_mux);

    while (granted) {
        struct mem_flock_waiter *w = granted;

        granted = w->next;
        msg_reply(w->rcvid, 0, NULL, 0);
        _sfree(w, sizeof *w);
    }
    while (failed) {
        struct mem_flock_waiter *w = failed;

        failed = w->next;
        msg_error(w->rcvid, ENOMEM);
        _sfree(w, sizeof *w);
    }
}

/* Remove and error out every waiter on the exact connection (scoid+coid) being
 * closed, so closing one descriptor cannot disturb a blocked F_SETLKW issued
 * through a different descriptor that shares the server connection.
 */
static void mem_flock_purge(int scoid, int coid)
{
    struct mem_flock_waiter *done = NULL;
    struct mem_flock_waiter **pp = &mem_flock_waiters;

    pthread_mutex_lock(&mem_flock_mux);
    while (*pp) {
        struct mem_flock_waiter *w = *pp;

        if (w->scoid == scoid && w->coid == coid) {
            *pp = w->next;
            w->next = done;
            done = w;
        } else {
            pp = &w->next;
        }
    }
    pthread_mutex_unlock(&mem_flock_mux);

    while (done) {
        struct mem_flock_waiter *w = done;

        done = w->next;
        msg_error(w->rcvid, EBADF);
        _sfree(w, sizeof *w);
    }
}

/* .unblock handler: a blocked F_SETLKW client was interrupted (killed or a
 * signal). Drop its waiter and fail the fcntl with EINTR.
 */
static int mem_unblock(ioserv_context_t *ctp,
                       io_notification_t *msg,
                       void *vocb)
{
    struct _msg_info info;
    struct mem_flock_waiter **pp = &mem_flock_waiters;
    int found = 0;

    (void) msg;
    (void) vocb;
    if (msg_info(ctp->rcvid, &info) == -1 ||
        !(info.flags & RTMUX_MI_UNBLOCK_REQ))
        return IOSERV_NOREPLY;

    pthread_mutex_lock(&mem_flock_mux);
    while (*pp) {
        struct mem_flock_waiter *w = *pp;

        if (w->rcvid == ctp->rcvid) {
            *pp = w->next;
            _sfree(w, sizeof *w);
            found = 1;
        } else {
            pp = &w->next;
        }
    }
    pthread_mutex_unlock(&mem_flock_mux);
    if (found)
        msg_error(ctp->rcvid, EINTR);
    return IOSERV_NOREPLY;
}

static int mem_lock(ioserv_context_t *ctp, io_lock_t *msg, void *vocb)
{
    struct mem_ocb *ocb = vocb;
    path_object_t *obp;
    flock_t fl;
    iov_t iov;
    pid_t pid;
    off_t s, e;
    int cmd, rc = EOK, wake = 0;

    if (!ocb || !(obp = ocb->object))
        return EINVAL;

    /* Owner is the calling process, so a lock through a dup'd/inherited
     * descriptor is owned by whoever issues it, not the opener.
     */
    pid = ctp->info.pid;
    cmd = (int) msg->i.subtype;

    /* The flock argument follows the _io_lock header in the message. */
    SETIOV(&iov, &fl, sizeof fl);
    if (ioserv_msgreadv(ctp, &iov, 1, (int) sizeof(struct _io_lock)) == -1)
        return errno;

    if (fl.l_type != F_RDLCK && fl.l_type != F_WRLCK && fl.l_type != F_UNLCK)
        return EINVAL;

    /* POSIX: a read lock needs the descriptor open for reading, a write lock
     * for writing.
     */
    if ((fl.l_type == F_RDLCK && !(ocb->ioflag & _IO_FLAG_RD)) ||
        (fl.l_type == F_WRLCK && !(ocb->ioflag & _IO_FLAG_WR)))
        return EBADF;
    if ((rc = mem_flock_range(ocb, obp, &fl, &s, &e)) != EOK)
        return rc;

    pthread_mutex_lock(&mem_flock_mux);
    if (cmd == F_GETLK) {
        struct mem_flock *c =
            (fl.l_type == F_UNLCK)
                ? NULL
                : mem_flock_conflict(obp, pid, fl.l_type, s, e);
        if (c) {
            fl.l_type = c->type;
            fl.l_pid = c->pid;
            fl.l_whence = SEEK_SET;
            fl.l_start = c->start;
            fl.l_len = (c->end == MEM_FLOCK_END_MAX) ? 0 : (c->end - c->start);
        } else {
            fl.l_type = F_UNLCK;
        }
        pthread_mutex_unlock(&mem_flock_mux);
        SETIOV(&iov, &fl, sizeof fl);
        if (ioserv_msgwritev(ctp, &iov, 1,
                             (int) sizeof(struct _io_lock_reply)) == -1)
            return errno;
        return EOK;
    }
    if (cmd != F_SETLK && cmd != F_SETLKW) {
        rc = EINVAL;
    } else if (fl.l_type == F_UNLCK) {
        rc = mem_flock_clear(obp, pid, s, e);
        wake = (rc == EOK);
    } else if (mem_flock_conflict(obp, pid, fl.l_type, s, e)) {
        /* F_SETLK reports the conflict; F_SETLKW blocks until it clears: the
         * caller is enqueued and the handler returns IOSERV_NOREPLY, leaving
         * fcntl() blocked until mem_flock_wake() grants and replies.
         */
        if (cmd == F_SETLK)
            rc = EAGAIN;
        else
            rc = mem_flock_enqueue(ctp, obp, pid, fl.l_type, s, e)
                     ? IOSERV_NOREPLY
                     : ENOMEM;
    } else {
        /* Replace this pid's overlapping ranges, then record the new lock.
         * Allocate the record first so a failure can't drop existing locks.
         */
        struct mem_flock *nl = _scalloc(sizeof *nl);

        if (!nl) {
            rc = ENOMEM;
        } else if ((rc = mem_flock_clear(obp, pid, s, e)) != EOK) {
            _sfree(nl, sizeof *nl);
        } else {
            mem_flock_link(nl, obp, pid, fl.l_type, s, e);
            /* A downgrade (WRLCK -> RDLCK) can free a waiting reader. */
            wake = 1;
        }
    }
    pthread_mutex_unlock(&mem_flock_mux);
    if (wake)
        mem_flock_wake(obp);
    return rc;
}

/* Closing one of several duplicated/inherited descriptors: drop the closing
 * process's advisory locks on the file now, rather than waiting for the final
 * descriptor to close (mem_close_ocb). Idempotent — dropping already-dropped
 * locks is a no-op.
 */
static int mem_close_dup(ioserv_context_t *ctp, io_close_t *msg, void *vocb)
{
    struct mem_ocb *ocb = vocb;

    (void) msg;
    if (ocb && ocb->object) {
        mem_flock_purge(ctp->info.scoid, ctp->info.coid);
        mem_flock_release(ocb->object, ctp->info.pid);
        mem_flock_wake(ocb->object);
    }
    return EOK;
}

static int mem_dir_fdinfo(io_fdinfo_t *msg, const char *dir)
{
    char *path = (char *) (&msg->o + 1);
    size_t cap = (size_t) msg->i.path_len;
    size_t len = strlen(dir) + 1;

    memset(&msg->o, 0, sizeof msg->o);
    msg->o.info.mode = S_IFDIR | 0777;
    if (cap > 0)
        memcpy(path, dir, cap < len ? cap : len);
    return (int) len;
}

static int mem_fdinfo(ioserv_context_t *ctp, io_fdinfo_t *msg, void *vocb)
{
    int len;

    (void) ctp;
    if (vocb)
        return ENOSYS;
    len = mem_dir_fdinfo(msg, "/dev/shmem");
    _IO_SET_FDINFO_LEN(ctp, len);
    return IOSERV_PTR(
        ctp, &msg->o,
        sizeof msg->o + (unsigned) ((size_t) msg->i.path_len < (size_t) len
                                        ? (size_t) msg->i.path_len
                                        : (size_t) len));
}

static int tymem_fdinfo(ioserv_context_t *ctp, io_fdinfo_t *msg, void *vocb)
{
    int len;

    (void) ctp;
    if (vocb)
        return ENOSYS;
    len = mem_dir_fdinfo(msg, "/dev/tymem");
    _IO_SET_FDINFO_LEN(ctp, len);
    return IOSERV_PTR(
        ctp, &msg->o,
        sizeof msg->o + (unsigned) ((size_t) msg->i.path_len < (size_t) len
                                        ? (size_t) msg->i.path_len
                                        : (size_t) len));
}

static const ioserv_ops_t mem_io_funcs = {
    .nfuncs = IOSERV_IO_NFUNCS,
    .read = mem_read,
    .write = mem_write,
    .close_ocb = mem_close_ocb,
    .close_dup = mem_close_dup,
    .unblock = mem_unblock,
    .stat = mem_stat,
    .devctl = mem_devctl,
    .lock = mem_lock,
    .lseek = mem_lseek,
    .chmod = mem_chmod,
    .chown = mem_chown,
    .utime = mem_utime,
    .space = mem_space,
    .fdinfo = mem_fdinfo,
    .sync = mem_sync,
};


int devmem_check_perm(ioserv_context_t *ctp,
                      mode_t mode,
                      uid_t uid,
                      gid_t gid,
                      mode_t check)
{
    struct _client_info info;
    int ret;

    if ((ret = connect_client_info(ctp->info.scoid, &info, NGROUPS_MAX)) != EOK)
        return ret;

    return mode_check_access(mode, uid, gid, check, &info);
}


int mem_open(ioserv_context_t *ctp, io_open_t *msg, void *handle, void *extra)
{
    path_object_t *object = handle;
    struct mem_ocb *ocb;

    if (object) {
        /* Only set the ownership the first time around  */
        if ((msg->connect.ioflag & O_CREAT) && !object->mem.pm.mode) {
            struct _client_info info;
            connect_client_info(ctp->info.scoid, &info, 0);
            object->mem.pm.uid = info.cred.euid;
            object->mem.pm.gid = info.cred.egid;
            object->mem.pm.mode =
                ((msg->connect.file_type == _FTYPE_SHMEM) ? S_IFNAM : S_IFREG) |
                (msg->connect.mode & ~(unsigned) S_IFMT);
            /* Only check perms on things which we have set permission on ... */
        } else if (object->mem.pm.mode != 0) {
            mode_t check = 0;
            int ret;
            check |= (msg->connect.ioflag & _IO_FLAG_RD) ? S_IRUSR : 0;
            check |= (msg->connect.ioflag & _IO_FLAG_WR) ? S_IWUSR : 0;
            ret = 0;
            if (check != 0) {
                ret = devmem_check_perm(ctp, object->mem.pm.mode,
                                        object->mem.pm.uid, object->mem.pm.gid,
                                        check);
            }
            if (ret != 0) {
                return ret;
            }
        }

        if ((msg->connect.ioflag & O_TRUNC) && OBJECT_SIZE(object)) {
            int status;

            status = root_vm_ops.resize(object, 0);
            if (status != 0) {
                return status;
            }
        }
    } else if ((msg->connect.ioflag & (_IO_FLAG_RD | _IO_FLAG_WR)) &&
               !proc_isaccess(0, &ctp->info)) {
        return EPERM;
    }
    if (!(ocb = _scalloc(sizeof *ocb))) {
        return ENOMEM;
    }

    ocb->ioflag = (int) msg->connect.ioflag;
    ocb->object = object ? path_object_clone(object) : NULL;

    ctp->id = mem_handle;
    if (ioserv_open_bind(ctp, ocb, &mem_io_funcs) == -1) {
        if (object) {
            path_object_done(object);
        }
        return errno;
    }

    return EOK;
}


static const ioserv_connect_ops_t mem_connect_funcs = {
    .nfuncs = IOSERV_CONNECT_NFUNCS,
    .open = mem_open,
};


static int shmem_open(ioserv_context_t *ctp,
                      io_open_t *msg,
                      void *handle,
                      void *extra)
{
    if (msg->connect.path_len + sizeof(*msg) >= ctp->msg_max_size) {
        return ENAMETOOLONG;
    }

    if ((msg->connect.eflag & _IO_CONNECT_EFLAG_DIR) ||
        msg->connect.path[0] == '\0') {
        if (msg->connect.ioflag & _IO_FLAG_WR) {
            return EISDIR;
        }
        if (ioserv_open_bind(ctp, 0, 0) == -1) {
            return errno;
        }
        return EOK;
    }

    _IO_SET_CONNECT_RET(ctp, _IO_CONNECT_RET_FTYPE);
    msg->ftype_reply.status = EOK;
    msg->ftype_reply.file_type =
        (msg->connect.file_type == _FTYPE_SHMEM) ? _FTYPE_SHMEM : _FTYPE_FILE;
    return (IOSERV_PTR(ctp, msg, sizeof(struct _io_connect_ftype_reply)));
}


static const ioserv_connect_ops_t shmem_connect_funcs = {
    .nfuncs = IOSERV_CONNECT_NFUNCS,
    .open = shmem_open,
};


/* RUSH3: Instead of this, use "mem_io_funcs" and error check in the RUSH3:
 * functions for OBJECT_MEM_TYPED?
 */
static const ioserv_ops_t tymem_io_funcs = {
    .nfuncs = IOSERV_IO_NFUNCS,
    .close_ocb = mem_close_ocb,
    .stat = mem_stat,
    .devctl = mem_devctl,
    .chmod = mem_chmod,
    .chown = mem_chown,
    .utime = mem_utime,
    .fdinfo = tymem_fdinfo,
    .sync = mem_sync,
};


int tymem_open(ioserv_context_t *ctp, io_open_t *msg, void *handle, void *extra)
{
    struct mem_ocb *ocb;
    unsigned tflags;
    unsigned count;

    if (msg->connect.path_len + sizeof(*msg) >= ctp->msg_max_size) {
        return ENAMETOOLONG;
    }

    if ((msg->connect.eflag & _IO_CONNECT_EFLAG_DIR) ||
        msg->connect.path[0] == '\0') {
        if (msg->connect.ioflag & _IO_FLAG_WR) {
            return EISDIR;
        }
        /* RUSH3: Simply fakes "/dev/tymem" as empty copy of "/dev/shmem". */
        if (ioserv_open_bind(ctp, 0, &tymem_io_funcs) == -1) {
            return errno;
        }
        return EOK;
    }
    if (msg->connect.extra_type != _IO_CONNECT_EXTRA_TYMEM) {
        return ENOTSUP;
    }
    if (msg->connect.extra_len != sizeof(uint32_t)) {
        return ENOTSUP;
    }
    if (msg->connect.ioflag & ~(unsigned) (O_ACCMODE | O_CREAT | O_NOCTTY)) {
        return EINVAL;
    }

#define ALL_TMEM                                                  \
    (POSIX_TYPED_MEM_ALLOCATE | POSIX_TYPED_MEM_ALLOCATE_CONTIG | \
     POSIX_TYPED_MEM_MAP_ALLOCATABLE)
    tflags = *(uint32_t *) extra;
    count = 0;
    if (tflags & POSIX_TYPED_MEM_ALLOCATE)
        ++count;
    if (tflags & POSIX_TYPED_MEM_ALLOCATE_CONTIG)
        ++count;
    if (tflags & POSIX_TYPED_MEM_MAP_ALLOCATABLE)
        ++count;
    if ((count > 1) || (tflags & ~(unsigned) ALL_TMEM)) {
        return EINVAL;
    }
    if (!(tflags &
          (POSIX_TYPED_MEM_ALLOCATE | POSIX_TYPED_MEM_ALLOCATE_CONTIG))) {
        if (!proc_isaccess(0, &ctp->info)) {
            /* Only root processes are allowed to specify direct offsets */
            return EPERM;
        }
    }

    ocb = _scalloc(sizeof *ocb);
    if (!ocb) {
        errno = ENOMEM;
        goto fail1;
    }

    ocb->tflags = (int) tflags;
    ocb->ioflag = (int) msg->connect.ioflag;

    errno = vm_tymem_open(msg->connect.path, ocb->tflags, &ocb->object,
                          ctp->info.pid);
    if (errno != EOK)
        goto fail2;

    /* RUSH1: need permission checking on path */

    ctp->id = mem_handle;
    if (ioserv_open_bind(ctp, ocb, &tymem_io_funcs) == -1)
        goto fail3;

    path_object_clone(ocb->object);
    return EOK;

fail3:
    vm_tymem_close(ocb->object);

fail2:
    _sfree(ocb, sizeof(*ocb));

fail1:
    return errno;
}


static const ioserv_connect_ops_t tymem_connect_funcs = {
    .nfuncs = IOSERV_CONNECT_NFUNCS,
    .open = tymem_open,
};


void devmem_init(void)
{
    mem_handle = ioserv_attach(dpp, NULL, "/dev/mem", 0, 0, &mem_connect_funcs,
                               &mem_io_funcs, 0);
    ioserv_attach(dpp, NULL, "/dev/shmem", 0, IOSERV_FLAG_DIR,
                  &shmem_connect_funcs, &mem_io_funcs, 0);

    /* RUSH3: Decide if "/dev/tymem" should be a fully usable directory RUSH3:
     * structure
     */
    ioserv_attach(dpp, NULL, "/dev/tymem", _FTYPE_TYMEM, IOSERV_FLAG_DIR,
                  &tymem_connect_funcs, &tymem_io_funcs, 0);
    hwres_proc_devno("dev", &mem_devno, -1, 0);
}


int devmem_check(const ioserv_ops_t *funcs,
                 mem_map_t *msg,
                 void *handle,
                 path_object_t **pobp)
{
    struct mem_ocb *ocb = handle;

    if (funcs == &tymem_io_funcs) {
        /* Typed memory */
        if (msg) {
            unsigned flags = 0;

            if (ocb->tflags & POSIX_TYPED_MEM_ALLOCATE) {
                flags |= IMAP_TYMEM_ALLOCATE;
            }
            if (ocb->tflags & POSIX_TYPED_MEM_ALLOCATE_CONTIG) {
                flags |= IMAP_TYMEM_ALLOCATE_CONTIG;
            }
            if (ocb->tflags & POSIX_TYPED_MEM_MAP_ALLOCATABLE) {
                flags |= IMAP_TYMEM_MAP_ALLOCATABLE;
            }
            msg->i.flags |= flags;
        }
    } else if (funcs != &mem_io_funcs) {
        /* not "/dev/mem" or "/dev/shmem/..." */
        return -1;
    }
    *pobp = ocb->object;

    if (msg) {
        if ((ocb->ioflag & _IO_FLAG_RD) == 0) {
            return EACCES;
        }
        if ((msg->i.flags & MAP_TYPE) == MAP_SHARED) {
            if ((ocb->ioflag & _IO_FLAG_WR) == 0) {
                if (msg->i.prot & PROT_WRITE) {
                    /* RUSH3: Is this test needed anymore - as_mmap() should
                     * catch it now
                     */
                    return EACCES;
                }
                /* Indicate that we can't allow PROT_WRITE later... */
                msg->i.flags |= IMAP_OCB_RDONLY;
            }
            if (!*pobp) {
                msg->i.flags |= MAP_PHYS;
            }
        }
    }

    return EOK;
}
