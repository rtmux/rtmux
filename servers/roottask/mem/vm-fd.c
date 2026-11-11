/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <fcntl.h>
#include <sys/elf.h>
#include "externs.h"
#include <sys/ioserv.h>
#include <time.h>
#include "mm-internal.h"


/* Functions to handle mmap'ing of generic files. */

#define STALE_CLEAN_INTERVAL 1

static vector_t vm_fd_vector;
static int vm_fd_code;
static int timerid;

/* We want to use !obp->hdr.next as an indicator that we're not on the zombie
 * list.
 */
#define ZOMBIE_END ((path_object_t *) 1)

/* the mutex is used to protect condvar, fd_vector, fdmem_open_lock and
 * fdmem_close_lock
 */
static pthread_mutex_t fdmem_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t fdmem_condvar = PTHREAD_COND_INITIALIZER;
static unsigned fdmem_open_lock;
static unsigned fdmem_close_lock;
static path_object_t *fdmem_zombies = ZOMBIE_END;
static unsigned fdmem_zombie_gen;
static unsigned fdmem_cleanup;
static path_object_t *fdmem_cleaner_object;


#define MMAPFD_CLOSE_WAIT 0x80000000
#define MMAPFD_CLOSELOCK_MASK 0x7fffffff

#define MMAPFD_STALES_HIGHWATER 50

#define MMAPFD_CLEANUP_QUEUED 0x00000001
#define MMAPFD_CLEANUP_INPROGRESS 0x00000002

#define MMAPFD_WAKEUP_CLOSER                    \
    if (fdmem_open_lock == 0) {                 \
        pthread_cond_broadcast(&fdmem_condvar); \
    }

#define MMAPFD_WAKEUP_OPENER                    \
    if (fdmem_close_lock == 0) {                \
        pthread_cond_broadcast(&fdmem_condvar); \
    }

/* Timeout in seconds after which mappings get dumped */
#define STICKY_TIMEOUT 300

/* Minimum free mem below which stale mappings always get dumped */
#define MIN_MEM_FREE_SIZE 65536


/* Initialize a file-descriptor-backed memory object.
 *
 * Records the mmap fd, seeds the default restriction list, and inserts the
 * object into the fd vector keyed by the mmap fd. Parameters:
 *   obp   : object to initialize
 *   extra : the mmap fd, passed as a uintptr_t
 * Return value: EOK on success, ENOMEM on vector-add failure.
 */
int fdmem_create(path_object_t *obp, void *extra)
{
    int mmap_fd = (int) (uintptr_t) extra;
    int fd;
    unsigned fd_idx;

    obp->fdmem.fd = mmap_fd;
    /* RUSH2: Need a way to inherit/modify the restrict list. */
    obp->mem.mm.restriction = restrict_user;
    obp->mem.mm.pmem_cache = NULL; /* start at head */
    obp->mem.mm.size = -1;
    fd_idx = (unsigned) mmap_fd;
    fd = vector_add(&vm_fd_vector, obp, fd_idx);
    if (fd != mmap_fd) {
        if (fd != -1)
            crash();
        return ENOMEM;
    }
    return EOK;
}


/* Finalize a file-descriptor-backed memory object.
 *
 * When called from the cleaner for the object currently being reaped, resizes
 * it to zero, removes it from the fd vector, and closes the fd. Otherwise, if
 * the object has no remaining references, stamps its stale time and places it
 * on the zombie list for later cleanup. Parameters:
 *   obp : object to finalize
 * Return value: 1 if the object was fully torn down, 0 if it was
 *               deferred to the zombie list or still in use.
 */
int fdmem_done(path_object_t *obp)
{
    if (obp == fdmem_cleaner_object) {
        atomic_add(&fdmem_close_lock, 1);

        /* RUSH1: We should release the fdmem_mutex over the close, RUSH1: but
         * then we get into race conditions with the open
         */
        if (root_vm_ops.resize(obp, 0) != EOK)
            crash();
        vector_rem(&vm_fd_vector, (unsigned) obp->fdmem.fd);
        close(obp->fdmem.fd);
        free(obp->fdmem.name);
        atomic_sub(&fdmem_close_lock, 1);
        return 1;
    }

    pthread_mutex_lock(&fdmem_mutex);

    /* RUSH3: Should we crash if obp is ever a zombie in this function, RUSH3:
     * rather just if refs is?
     */

    /* We have to check for hdr.refs != 0 as well since object_done() could have
     * checked for hdr.refs == 0 after mem_map_fd() has incremented it but
     * before it did the memobj_lock().
     */

    if ((!obp->mem.mm.refs) && ((obp->hdr.refs & ~0x80000000) == 0)) {
        obp->mem.pm.mtime = time(0);
        if (obp->mem.mm.flags & MM_FDMEM_STICKY) {
            obp->mem.pm.mtime += STICKY_TIMEOUT;
        }
        if (!obp->hdr.next) {
            /* Put it on the zombie list */
            obp->hdr.next = fdmem_zombies;
            fdmem_zombies = obp;
            ++fdmem_zombie_gen;
            (void) vm_fd_compact();
        }
    } else if (obp->hdr.next) {
        /* Somebody was using an object on the zombie list. Not good. */
        crash();
    }

    pthread_mutex_unlock(&fdmem_mutex);

    return 0;
}


/* Copy a file-descriptor-backed object's name into a caller buffer.
 *
 * Parameters:
 *   obp  : object whose name to retrieve
 *   max  : size of the destination buffer
 *   dest : destination buffer
 * Return value: length of the copied name, or 0 if the object is unnamed.
 */
size_t fdmem_name(path_object_t *obp, size_t max, char *dest)
{
    char *name;

    name = obp->fdmem.name;
    /* RUSH1: query the name from the file system? */
    if (!name)
        return 0;

    STRLCPY(dest, name, max);
    return strlen(dest);
}

/* This sets the name of the fdmem object. Called from as_debuginfo. We need to
 * do this here and properly protect access to the name field.
 */
int vm_fd_setname(path_object_t *obp, char *name)
{
    pthread_mutex_lock(&fdmem_mutex);
    memobj_lock(obp);

    free(obp->fdmem.name);
    obp->fdmem.name = strdup(name);

    memobj_unlock(obp);
    pthread_mutex_unlock(&fdmem_mutex);

    return 0;
}

/* Send an _IO_MMAP message to the I/O service backing an fd.
 *
 * Builds the _IO_MMAP request from the caller's mmap parameters and sends it
 * over <fd>, returning the server-side coid the I/O service assigns to the
 * mapping. Parameters:
 *   info  : connection info for the original client fd
 *   fd    : connection to the backing I/O service
 *   ctp   : I/O service context of the requesting client
 *   mmapp : the client's mmap request
 *   prot  : protection bits to request
 * Return value: the mapping coid, or -1 on error (errno set).
 */
static int io_mmap(struct _msg_info *info,
                   int fd,
                   ioserv_context_t *ctp,
                   mem_map_t *mmapp,
                   unsigned prot)
{
    io_mmap_t msg;

    msg.i.type = _IO_MMAP;
    msg.i.combine_len = sizeof msg.i;
    msg.i.offset = (uint64_t) mmapp->i.offset;
    msg.i.prot = prot;
    msg.i.info = *info;
    msg.i.info.nd = ctp->info.nd;
    msg.i.info.pid = ctp->info.pid;
    msg.i.info.coid = mmapp->i.fd;

    root_trace(
        "io_mmap: sending _IO_MMAP fd=%d info.pid=%d info.coid=%d "
        "info.scoid=%d info.chid=%d\n",
        fd, msg.i.info.pid, msg.i.info.coid, msg.i.info.scoid, msg.i.info.chid);

    /* ipc-send: roottask-fsio -> romfs,lkl-fsd,dev-nvme,pipe */
    int ret = msg_sendnc(fd, &msg.i, sizeof msg.i, &msg.o, sizeof msg.o);
    root_trace("io_mmap: msg_sendnc returned %d, errno=%d, reply coid=%d\n",
               ret, errno, msg.o.coid);
    if (ret != EOK) {
        if (ret > 0)
            errno = ret;
        return -1;
    }
    return msg.o.coid;
}


/* Resolve (or create) the fd-backed memory object for an mmap request.
 *
 * Looks up the backing I/O service via connect_server_info, opens a connection,
 * and sends the _IO_MMAP request. On first access for a given mapping fd it
 * creates the OBJECT_MEM_FD object; subsequent accesses reuse the existing one,
 * removing it from the zombie list if present.
 *
 * Returns with the object locked and the as write-locked. Parameters:
 *   ctp     : I/O service context of the requesting client
 *   prp_pid : pid of the requesting process
 *   prp     : requesting process (kernel-side handle)
 *   mmapp   : the client's mmap request
 *   object  : OUT — receives the locked memory object on success
 * Return value: 0 on success, or an errno on failure.
 */
static int mem_map_fd(ioserv_context_t *ctp,
                      pid_t prp_pid,
                      process_t *prp,
                      mem_map_t *mmapp,
                      path_object_t **object)
{
    int fd, error;
    int mmap_fd;
    path_object_t *obp;
    unsigned prot;
    unsigned orig_prot;
    struct _msg_info info;
    int status;

    /* Get information on passed fd */
    if (connect_server_info(ctp->info.pid, (uint64_t) mmapp->i.fd, &info) !=
        mmapp->i.fd) {
        root_trace("mem_map_fd: connect_server_info failed pid=%d fd=%d\n",
                   ctp->info.pid, mmapp->i.fd);
        return EBADF;
    }
    root_trace(
        "mem_map_fd: connect_server_info pid=%d fd=%d -> nd=%d pid=%d chid=%d "
        "scoid=%d coid=%d\n",
        ctp->info.pid, mmapp->i.fd, info.nd, info.pid, info.chid, info.scoid,
        info.coid);

    pthread_mutex_lock(&fdmem_mutex);
    while (fdmem_close_lock) {
        pthread_cond_wait(&fdmem_condvar, &fdmem_mutex);
    }

    atomic_add(&fdmem_open_lock, 1);
    pthread_mutex_unlock(&fdmem_mutex);

    /* make a connection to the channel */
    fd = connect_attach(info.nd, info.pid, info.chid, 0, 0);
    root_trace("mem_map_fd: connect_attach(%d,%d,%d) = %d\n", info.nd, info.pid,
               info.chid, fd);
    if (fd == -1) {
        error = errno;
        atomic_sub(&fdmem_open_lock, 1);
        MMAPFD_WAKEUP_CLOSER return error;
    }

    /* @@@ what we need is a lock per scoid to serialize lookups and closes send
     * the mmap message
     */

    /* RUSH3: This needs to be reworked - the _IO_MMAP message should return
     * RUSH3: whether the client's fd has write perms.
     */

    orig_prot = mmapp->i.prot;
    prot = orig_prot | PROT_WRITE;
    if ((mmapp->i.flags & MAP_TYPE) == MAP_PRIVATE) {
        orig_prot &= ~(unsigned) PROT_WRITE;
    }
    mmap_fd = io_mmap(&info, fd, ctp, mmapp, prot);
    if (mmap_fd == -1) {
        switch (errno) {
        case EACCES:
        case EROFS:
            prot = orig_prot;
            if (!(prot & PROT_WRITE)) {
                /* try again without forcing PROT_WRITE */
                mmap_fd = io_mmap(&info, fd, ctp, mmapp, prot);
            }
            break;
        default:
            break;
        }
    }
    if (!(prot & PROT_WRITE) && ((mmapp->i.flags & MAP_TYPE) == MAP_SHARED)) {
        mmapp->i.flags |= IMAP_OCB_RDONLY;
    }

    if (mmap_fd == -1) {
        error = errno;
        __connect_detach((uint64_t) fd);
        atomic_sub(&fdmem_open_lock, 1);
        MMAPFD_WAKEUP_CLOSER return error;
    }

    if (fd != mmap_fd) {
        /* already accessing this file, use the first fd */
        __connect_detach((uint64_t) fd);
    }

    pthread_mutex_lock(&fdmem_mutex);
    obp = vector_lookup(&vm_fd_vector, (unsigned) mmap_fd);
    if (!obp) {
        /* first time accessing this file. Creator pays cost of creation */
        obp = object_create(OBJECT_MEM_FD, (void *) (uintptr_t) mmap_fd,
                            ctp->info.pid);
        if (!obp) {
            /* not enough memory */
            if (fd != mmap_fd) {
                pthread_mutex_unlock(&fdmem_mutex);
                atomic_sub(&fdmem_open_lock, 1);
                MMAPFD_WAKEUP_CLOSER return ENOMEM;
            }

            while (fdmem_open_lock != 1) {
                pthread_cond_wait(&fdmem_condvar, &fdmem_mutex);
            }
            if ((obp = vector_lookup(&vm_fd_vector, (unsigned) mmap_fd)) ==
                NULL) {
                atomic_sub(&fdmem_open_lock, 1);
                atomic_add(&fdmem_close_lock, 1);
                pthread_mutex_unlock(&fdmem_mutex);
                MMAPFD_WAKEUP_CLOSER close(mmap_fd);
                atomic_sub(&fdmem_close_lock, 1);
                MMAPFD_WAKEUP_OPENER return ENOMEM;
            }
        }
    }

    if (obp->hdr.next) {
        path_object_t **owner;
        path_object_t *chk;

        CRASHCHECK(obp->mem.mm.refs);

        /* It's been scheduled for removal, so we need to take it off the zombie
         * list since we're using it again
         */
        owner = &fdmem_zombies;
        for (;;) {
            chk = *owner;
            CRASHCHECK(!chk);
            if (chk == ZOMBIE_END)
                crash();
            if (chk == obp)
                break;
            owner = &chk->hdr.next;
        }
        *owner = chk->hdr.next;
        ++fdmem_zombie_gen;
        obp->hdr.next = NULL;
    }

    /* We have to first unlock the fdmem mutex to avoid deadlocks. The
     * object_done path locks "adp" then "fdmem", so we must take them in the
     * same order here: drop fdmem, then re-acquire the as write lock below
     * before locking the object.
     */
    path_object_clone(obp);
    pthread_mutex_unlock(&fdmem_mutex);

    /* Have to re-obtain as write lock before locking the object to avoid
     * deadlocks
     */
    status = proc_wlock_adp(prp_pid);
    CRASHCHECK(status == -1);

    memobj_lock(obp);
    path_object_unclone(obp);

    atomic_sub(&fdmem_open_lock, 1);

    /* wake up closers if any */
    MMAPFD_WAKEUP_CLOSER *object = obp;

    return (0);
}


/* Open the fd-backed memory object for a (non-anonymous) mmap request.
 *
 * Releases the as lock, resolves the object via mem_map_fd, then validates the
 * underlying file (stat, directory check, inode/dev match) and refreshes its
 * size and stale flags. Sticky files whose mtime changed are reloaded.
 * Parameters:
 *   ctp     : I/O service context of the requesting client
 *   prp_pid : pid of the requesting process
 *   prp     : requesting process (kernel-side handle)
 *   mmapp   : the client's mmap request
 *   obpp    : OUT — receives the locked memory object on success
 * Return value: 0 on success, or an errno on failure.
 */
int vm_open_fd(ioserv_context_t *ctp,
               pid_t prp_pid,
               process_t *prp,
               mem_map_t *mmapp,
               path_object_t **obpp)
{
    int fd;
    struct stat sbuf;
    int again = 1;
    int ret;
    path_object_t *obp;
    int status;

    if (mmapp->i.flags & MAP_ANON) {
        return EINVAL;
    }

again:
    proc_unlock_adp(prp_pid);
    ret = mem_map_fd(ctp, prp_pid, prp, mmapp, obpp);
    if (ret != EOK) {
        /* If mem_map_fd failed, we still haven't re-locked the as, so do it
         * now.
         */
        status = proc_wlock_adp(prp_pid);
        CRASHCHECK(status == -1);
        return ret;
    }

    obp = *obpp;
    /* Successful lookup means that the object has been locked */

    fd = obp->fdmem.fd;

    if (fstat(fd, &sbuf) || S_ISDIR(sbuf.st_mode)) {
        memobj_unlock(obp);
        return ENOTSUP;
    }

    if ((obp->fdmem.ino && obp->fdmem.ino != sbuf.st_ino) ||
        (obp->fdmem.dev && obp->fdmem.dev != sbuf.st_dev)) {
        /* This code might not be needed. I leave it here in case. */
        memobj_unlock(obp);
        if (again) {
            again = 0;
            goto again;
        }
        return ENOTSUP;
    }
    /* obp->mem.ctime is used here for mtime. */
    if (obp->fdmem.ino && obp->fdmem.ftime &&
        obp->fdmem.ftime != sbuf.st_mtime) {
        size_t osize;

        /* for sticky objects the file is changed, reload the content for all
         * who share it
         */

        /* FUTURE: Find a more efficient way of doing this (e.g.
         * msync(MS_INVALIDATE)).
         */
        osize = (size_t) obp->mem.mm.size;
        (void) root_vm_ops.resize(obp, 0);
        (void) root_vm_ops.resize(obp, osize);
        obp->fdmem.ftime = sbuf.st_mtime;
    } else {
        obp->fdmem.ino = sbuf.st_ino;
        obp->fdmem.dev = sbuf.st_dev;
        obp->fdmem.ftime = sbuf.st_mtime;
    }

    obp->mem.mm.size = sbuf.st_size;
    if (sbuf.st_mode & S_ISVTX)
        obp->mem.mm.flags |= MM_FDMEM_STICKY;

    return 0;
}

/* Schedule a pass to clean up stale mapped-file entries.
 *
 * Queues a cleanup by sending a notification to the cleanup handler unless one
 * is already queued or in progress. Always called from Proc or the termer.
 * Return value: EOK.
 */
int vm_fd_compact(void)
{
    if (!(atomic_or_value(&fdmem_cleanup, MMAPFD_CLEANUP_QUEUED) &
          (MMAPFD_CLEANUP_QUEUED | MMAPFD_CLEANUP_INPROGRESS))) {
        if (msg_send_notification(ROOTTASK_COID, -1, vm_fd_code, 0) != 0) {
            atomic_clr(&fdmem_cleanup, MMAPFD_CLEANUP_QUEUED);
        }
    }

    return EOK;
}


/* Notification handler that reaps stale mapped-file objects.
 *
 * Walks the zombie list and frees objects whose stale time has passed (or all
 * of them when memory is low), serializing against concurrent opens. At most
 * one cleanup runs at a time. Re-arms the stale timer if zombies remain.
 * Parameters:
 *   ctp   : message context (unused)
 *   code  : notification code (unused)
 *   flags : dispatch flags (unused)
 *   handle: dispatch handle (unused)
 * Return value: EOK.
 */
static int vm_fd_cleanup(message_context_t *ctp,
                         int code,
                         unsigned flags,
                         void *handle)
{
    path_object_t **owner;
    path_object_t *obp;
    int force;
    time_t nexttime;
    time_t currtime;
    time_t mtime;
    unsigned gen;

#define TIME_INFINITY (~(time_t) 0)

    nexttime = TIME_INFINITY;

    if (atomic_or_value(&fdmem_cleanup, MMAPFD_CLEANUP_INPROGRESS) &
        MMAPFD_CLEANUP_INPROGRESS) {
        /* Somebody's already in here */
        return EOK;
    }

    if (proc_thread_pool_reserve() == 0) {
        pthread_mutex_lock(&fdmem_mutex);

        /* RUSH3: we used to set force if we had more than
         * MMAPFD_STALES_HIGHWATER RUSH3: to potentially free, but since we
         * don't keep track of the RUSH3: number any more, we can't do that. Can
         * we get away without RUSH3: that?
         */
        force = 0;

        currtime = time(0);
        gen = fdmem_zombie_gen;
        owner = &fdmem_zombies;
        for (;;) {
            obp = *owner;
            CRASHCHECK(!obp);
            if (obp == ZOMBIE_END)
                break;
            if (pa_free_size() < MIN_MEM_FREE_SIZE)
                force = 1;
            if (force) {
                atomic_or(&fdmem_close_lock, MMAPFD_CLOSE_WAIT);
            }
            while (fdmem_open_lock) {
                /* opening operation is still on, need to wait */
                pthread_cond_wait(&fdmem_condvar, &fdmem_mutex);
            }
            atomic_clr(&fdmem_close_lock, MMAPFD_CLOSE_WAIT);
            if (gen != fdmem_zombie_gen) {
                /* List has been changed, restart from begining */
                gen = fdmem_zombie_gen;
                owner = &fdmem_zombies;
                /* RUSH1: Do we want to limit the number of restarts? */
                continue;
            }
            mtime = obp->mem.pm.mtime;
            if (force || (currtime >= mtime)) {
                int r;

                *owner = obp->hdr.next;
                fdmem_cleaner_object = obp;
                r = object_done(obp);
                fdmem_cleaner_object = NULL;
                if (!r) {
                    /* Free failed for some reason, put things back */
                    *owner = obp;
                    nexttime = currtime;
                }
                if (*owner == ZOMBIE_END)
                    break;
            } else if (mtime < nexttime) {
                if (mtime == 0)
                    crash();
                nexttime = mtime;
            }
            owner = &(*owner)->hdr.next;
        }
        pthread_mutex_unlock(&fdmem_mutex);

        proc_thread_pool_reserve_done();
    }

    fdmem_cleanup = 0;

    /* wake up opener if any */
    MMAPFD_WAKEUP_OPENER if (fdmem_zombies != ZOMBIE_END)
    {
        struct _itimer value;

        /* There are fd's pending to be cleaned */
        currtime = time(0);
        if (nexttime == TIME_INFINITY)
            nexttime = currtime;
        if (nexttime <= currtime)
            nexttime = currtime + STALE_CLEAN_INTERVAL;
        value.nsec = (uint64_t) nexttime * 1000000000ULL,
        value.interval_nsec = 0;
        if (__timer_settime((timer_t) timerid, TIMER_ABSTIME, &value, NULL) !=
            0) {
            root_log(
                "\nCannot arm timer (%d). Memory mapped file timed stale "
                "cleaning may not work.",
                errno);
        }
    }

    return EOK;
}


/* Initialize the mapped-file subsystem.
 *
 * Attaches the cleanup notification handler and creates the stale-cleanup
 * timer.
 */
void vm_fd_init(void)
{
    struct sigevent event;

    vm_fd_code = notification_attach(dpp, MSG_FLAG_ALLOC_NOTIFICATION, 0,
                                     vm_fd_cleanup, NULL);
    /* Use sender priority inheritance for stale-cleanup notifications. */
    SIGEV_NOTIFICATION_INIT(&event, ROOTTASK_COID, -1, vm_fd_code, 0);
    /* RUSH3: Use CLOCK_MONOTONIC? */
    if ((timerid = __timer_create(CLOCK_REALTIME, &event)) == -1) {
        root_log(
            "\nCannot create timer (%d). Memory mapped file timed stale "
            "cleaning may not work.",
            errno);
    }
}
