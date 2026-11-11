/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "as.h"
#include "root-process.h"

/* RUSH3: Should we stick with the one condvar, or have one per as? RUSH3:
 * Having only one reduces the number of sync objects that the RUSH3: kernel has
 * to keep track of, at the cost of possibly more roottask RUSH3: threads waking
 * up when we pthread_cond_broadcast(). On the other RUSH3: hand, we probably
 * don't have too many times where we have multiple RUSH3: threads blocked
 * waiting for multiple as_regions.
 */

/* RUSH3: However, if we _do_ use one condvar per as, we can keep track RUSH3:
 * if there are any threads requesting R/W locks (similar to RUSH3:
 * _PROMOTE_REQ) and use that information in the unlock code to RUSH3: sometimes
 * do a pthread_cond_signal() rather than broadcast RUSH3: (when there are write
 * requests, but no read). That would allow us RUSH3: to avoid waking up a whack
 * of threads for no purpose.
 */

/* Static mutex: PRIO_NONE until kernel turnstiles provide real PI. */
static pthread_mutex_t mm_mux = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t mm_cond = PTHREAD_COND_INITIALIZER;

static void mm_mutex_lock_checked(const char *op,
                                  pid_t pid,
                                  root_process_t *tmp)
{
    int r = pthread_mutex_lock(&mm_mux);

    if (r != EOK) {
        root_err(
            "%s: pthread_mutex_lock(mm_mux) pid=%d r=%d owner=%lx "
            "tls_owner=%lx count=%lx rwlock=%x\n",
            op, (int) pid, r, mm_mux.owner, __tls()->__owner, mm_mux.count,
            tmp ? tmp->rwlock_lock : 0);
        crash();
    }
}

/* R/W lock we use a single 32-bit unsigned for this. the bottom 28 bits are
 * used as reader count top bits are a writer lock and promotion request
 *
 * The lock makes no attempt at fairness, and probably should not be used for
 * heavily contested resources.
 */

#define _RLOCK_MASK 0x0fffffffU
#define _WLOCK_BIT 0x10000000U
#define _PROMOTE_REQ 0x20000000
#define _PENDING 0x80000000U


/* Address-space rwlock primitives.
 *
 * The lock state (rwlock_lock + fault_owner) is pure roottask state and lives
 * in root_process_t; the kernel only inspects the page directory and never
 * observes these locks.
 *
 * Functions take pid_t directly — every caller has the pid in scope from the
 * surrounding root_proc_lock.
 */

/* Acquire a read lock on a process's address space.
 *
 * Waits while a writer holds the lock, then bumps the reader count. Parameters:
 *   pid : process whose address-space rwlock to acquire
 * Return value: 0 on success.
 */
int rdecl proc_rlock_adp(pid_t pid)
{
    root_process_t *tmp = root_proc(pid);
    int r;

    mm_mutex_lock_checked("proc_rlock_adp", pid, tmp);
    /* RUSH3: Should this wait for the _PROMOTE_REQ bit to be off as well? */
    while (tmp->rwlock_lock & _WLOCK_BIT) {
        tmp->rwlock_lock |= _PENDING;
        r = pthread_cond_wait(&mm_cond, &mm_mux);
        CRASHCHECK(r != EOK);
        CRASHCHECK((mm_mux.owner & ~RTMUX_SYNC_WAITING) != __tls()->__owner);
    }

    tmp->rwlock_lock++;
    if (tmp->rwlock_lock & _WLOCK_BIT)
        crash();

    CRASHCHECK((mm_mux.owner & ~RTMUX_SYNC_WAITING) != __tls()->__owner);
    r = pthread_mutex_unlock(&mm_mux);
    CRASHCHECK(r != EOK);
    return 0;
}


/* Acquire a write lock on a process's address space.
 *
 * Waits until the lock is fully unlocked (no readers, no writer), then takes
 * the writer bit. Parameters:
 *   pid : process whose address-space rwlock to acquire
 * Return value: 0 on success.
 */
int rdecl proc_wlock_adp(pid_t pid)
{
    root_process_t *tmp = root_proc(pid);
    int r;

    mm_mutex_lock_checked("proc_wlock_adp", pid, tmp);
    while (tmp->rwlock_lock != 0) {
        tmp->rwlock_lock |= _PENDING;
        r = pthread_cond_wait(&mm_cond, &mm_mux);
        CRASHCHECK(r != EOK);
        CRASHCHECK((mm_mux.owner & ~RTMUX_SYNC_WAITING) != __tls()->__owner);
    }
    tmp->rwlock_lock = _WLOCK_BIT;

    CRASHCHECK((mm_mux.owner & ~RTMUX_SYNC_WAITING) != __tls()->__owner);
    r = pthread_mutex_unlock(&mm_mux);
    CRASHCHECK(r != EOK);
    return 0;
}


/* Promote an already-held read lock to a write lock.
 *
 * Sets the promote-request bit, drops this caller's reader count, and waits for
 * all other readers and any writer to drain before taking the writer bit. Fails
 * if another promotion is already in progress. Parameters:
 *   pid : process whose address-space rwlock to promote
 * Return value: 0 on success, -1 if a promotion is already pending.
 */
int rdecl proc_rlock_promote_adp(pid_t pid)
{
    root_process_t *tmp = root_proc(pid);
    int r;
    int ret;

    mm_mutex_lock_checked("proc_rlock_promote_adp", pid, tmp);

    if (tmp->rwlock_lock & _PROMOTE_REQ) {
        ret = -1;
    } else {
        tmp->rwlock_lock |= _PROMOTE_REQ;
        tmp->rwlock_lock--;
        while ((tmp->rwlock_lock & (_WLOCK_BIT | _RLOCK_MASK)) != 0) {
            tmp->rwlock_lock |= _PENDING;
            r = pthread_cond_wait(&mm_cond, &mm_mux);
            CRASHCHECK(r != EOK);
            CRASHCHECK((mm_mux.owner & ~RTMUX_SYNC_WAITING) !=
                       __tls()->__owner);
        }
        tmp->rwlock_lock = _WLOCK_BIT;
        ret = 0;
    }

    CRASHCHECK((mm_mux.owner & ~RTMUX_SYNC_WAITING) != __tls()->__owner);
    r = pthread_mutex_unlock(&mm_mux);
    CRASHCHECK(r != EOK);
    return ret;
}

/* Release a read or write lock on a process's address space.
 *
 * Drops the writer bit or decrements the reader count as appropriate, clears
 * the fault owner, and broadcasts the condvar if a waiter is pending and the
 * lock is now free. Parameters:
 *   pid : process whose address-space rwlock to release
 * Return value: 0.
 */
int rdecl proc_unlock_adp(pid_t pid)
{
    root_process_t *tmp = root_proc(pid);
    unsigned locked;

    tmp->fault_owner = 0;
    pthread_mutex_lock(&mm_mux);

    locked = tmp->rwlock_lock;
    if (locked & _WLOCK_BIT) {
        locked &= ~_WLOCK_BIT;
    } else {
        unsigned count = (locked & _RLOCK_MASK);

        if (count != 0) {
            locked = (locked & ~_RLOCK_MASK) | (count - 1);
        }
        CRASHCHECK(tmp->fault_owner != 0);
    }
    if ((locked & (_WLOCK_BIT | _RLOCK_MASK | _PENDING)) == _PENDING) {
        locked &= ~_PENDING;
        pthread_cond_broadcast(&mm_cond);
    }
    tmp->rwlock_lock = locked;

    CRASHCHECK((mm_mux.owner & ~RTMUX_SYNC_WAITING) != __tls()->__owner);
    pthread_mutex_unlock(&mm_mux);
    return 0;
}

/* Record the calling thread as the write-lock fault owner.
 *
 * Must be called only while the process's address space is write locked. Stores
 * the caller's owner identity in fault_owner the first time it is set.
 * Parameters:
 *   pid : process whose fault owner to mark
 */
void rdecl proc_lock_owner_mark(pid_t pid)
{
    root_process_t *tmp = root_proc(pid);
    if (!(tmp->rwlock_lock & _WLOCK_BIT)) {
        root_log("proc_lock_owner_mark: pid=%d rwlock=0x%x not WLOCK ra=%p\n",
                 pid, tmp->rwlock_lock, __builtin_return_address(0));
    }
    CRASHCHECK(!(tmp->rwlock_lock & _WLOCK_BIT));
    if (tmp->fault_owner == 0) {
        tmp->fault_owner = __tls()->__owner;
    }
}

/* Lock a memory object, taking a reference for the duration.
 *
 * Clones (ref-counts) the object so it cannot be freed while held, then takes
 * its mutex. Parameters:
 *   obp : memory object to lock
 */
void rdecl memobj_lock(path_object_t *obp)
{
    path_object_clone(obp);
    proc_mux_lock(&obp->mem.mm.mux);
}

/* Conditionally lock a memory object.
 *
 * If the calling thread already holds the object's mutex, returns immediately;
 * otherwise clones the object and takes the mutex like memobj_lock. Parameters:
 *   obp : memory object to lock
 * Return value: 1 if the lock was already held, 0 if newly acquired.
 */
int rdecl memobj_cond_lock(path_object_t *obp)
{
    struct proc_mux_lock **ml = &obp->mem.mm.mux;

    if (proc_mux_haslock(ml, 0))
        return 1;
    path_object_clone(obp);
    proc_mux_lock(ml);
    return 0;
}


/* Unlock a memory object and release the reference taken at lock time.
 *
 * Parameters:
 *   obp : memory object to unlock
 */
void rdecl memobj_unlock(path_object_t *obp)
{
    VERIFY_OBJ_LOCK(obp);
    proc_mux_unlock(&obp->mem.mm.mux);
    path_object_done(obp);
}
