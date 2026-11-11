/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "externs.h"
#include <sys/mman.h>
#include <sys/storage.h>
#include "root-process.h"

struct proc_mux_lock {
    struct proc_mux_lock *next;
    unsigned requestors;
    pthread_mutex_t mux;
};

static struct proc_mux_lock *mux_free_list;
static pthread_mutex_t mux_mutex = PTHREAD_MUTEX_INITIALIZER;
static struct proc_mux_lock fallback_lock = {
    .requestors = ~0U,
    .mux =
        {
            .owner = RTMUX_SYNC_INITIALIZER,
        },
};

/* Acquire a recursive proc-mux lock, allocating one if *lpp is NULL.
 *
 * Pulls a lock structure from the free list, or allocates a fresh recursive
 * mutex, falling back to a shared singleton when allocation fails. Must be
 * called with mux_mutex held; it is released here. Parameters:
 *   lpp : address of the lock pointer to acquire (set on allocation)
 * Return value: EOK once the lock is held.
 */
static int rdecl acquire_lock(struct proc_mux_lock **lpp)
{
    struct proc_mux_lock *lp;
    int rc;

    lp = *lpp;
    if (!lp) {
        lp = mux_free_list;
        if (lp) {
            mux_free_list = lp->next;
        } else {
            if (fallback_lock.requestors == ~0U) {
                pthread_mutexattr_t attr;
                int err;
                (void) pthread_mutexattr_init(&attr);
                attr.flags |= PTHREAD_RECURSIVE_ENABLE;
                err = pthread_mutex_init(&fallback_lock.mux, &attr);
                CRASHCHECK(err != EOK);
                (void) pthread_mutexattr_destroy(&attr);
                fallback_lock.requestors = 0;
            }
            lp = _smalloc(sizeof(*lp));
            if (!lp) {
                /* devolve to a single lock if we can't allocate a new lock
                 * structure
                 */
                lp = &fallback_lock;
                goto fallback;
            }
            lp->requestors = 0;
            /* Match fallback_lock's recursive semantics: roottask has reentrant
             * paths that take the same prp's mux twice on one thread (e.g.
             * query_object → handler → root_proc_lock of the same pid). Without
             * the recursive flag the mutex is non-recursive + error-check, and
             * the second lock returns EDEADLK and crash()es.
             */
            {
                pthread_mutexattr_t attr;
                (void) pthread_mutexattr_init(&attr);
                attr.flags |= PTHREAD_RECURSIVE_ENABLE;
                (void) pthread_mutex_init(&lp->mux, &attr);
                (void) pthread_mutexattr_destroy(&attr);
            }
        }
    }
fallback:
    *lpp = lp;
    lp->requestors++;
    pthread_mutex_unlock(&mux_mutex);
#ifdef NDEBUG
    pthread_mutex_lock(&lp->mux);
#else

    /* In the non-NDEBUG build, on any pthread_mutex_lock error log lp / &mux /
     * owner / count / rc so a failure is fully diagnosable instead of a blind
     * crash().
     */
    rc = pthread_mutex_lock(&lp->mux);
    if (rc != EOK) {
        root_log(
            "proc_mux_lock: pthread_mutex_lock(lp=%p mux=%p owner=%#lx "
            "count=%ld) rc=%d\n",
            (void *) lp, (void *) &lp->mux, (unsigned long) lp->mux.owner,
            (long) lp->mux.count, rc);
        crash();
    }
#endif
    return EOK;
}

/* Release a proc-mux lock previously taken with acquire_lock.
 *
 * Drops the mutex and, when the last requestor leaves, returns the lock
 * structure to the free list (the shared fallback lock is never recycled).
 * Parameters:
 *   lpp : address of the lock pointer to release (cleared when freed)
 */
static void rdecl release_lock(struct proc_mux_lock **lpp)
{
    struct proc_mux_lock *lp;

    pthread_mutex_lock(&mux_mutex);
    lp = *lpp;
    pthread_mutex_unlock(&mux_mutex);
    if (!lp)
        crash();

#ifdef NDEBUG
    pthread_mutex_unlock(&lp->mux);
#else
    if (pthread_mutex_unlock(&lp->mux) != EOK) {
        crash();
    }
#endif
    pthread_mutex_lock(&mux_mutex);
    lp->requestors--;
    if (lp->requestors == 0 && lp != &fallback_lock) {
        *lpp = NULL;
        lp->next = mux_free_list;
        mux_free_list = lp;
    }
    pthread_mutex_unlock(&mux_mutex);
}


/* Look up a process by pid without locking it.
 *
 * Intended for callers that already know the process cannot terminate
 * underneath them; the query_object is closed immediately. Parameters:
 *   pid : the process id to look up
 * Return value: the process_t pointer, or NULL if no such process.
 */
process_t *root_proc_lookup(pid_t pid)
{
    process_t *prp;

    prp = query_object(_QUERY_PROCESS, (unsigned) pid, _QUERY_PROCESS_VECTOR, 0,
                       0, 0, 0);
    if (prp) {
        /* We can say the query is done early because the code calling this
         * function knows that the returned process is in a state where it can't
         * terminate on us.
         */
        query_object_done(pid);
    }
    return prp;
}

/* Look up a process by pid and lock its roottask-side state.
 *
 * Retries until the slot stably resolves to the requested pid, so the returned
 * process cannot be released while the lock is held. Parameters:
 *   pid : the process id to lock
 * Return value: the process_t pointer with the lock held, or NULL if the
 *               process is gone.
 */
process_t *root_proc_lock(pid_t pid)
{
    process_t *prp;
    root_process_t *tmp = root_proc(pid);
    int r;

    while (1) {
        pthread_mutex_lock(&mux_mutex);
        prp = query_object(_QUERY_PROCESS, (unsigned) pid,
                           _QUERY_PROCESS_VECTOR, 0, 0, 0, 0);
        if (!prp) {
            pthread_mutex_unlock(&mux_mutex);
            return NULL;
        }

        /* Lock roottask-side state via root_proc(pid)->lock; the kernel-half
         * process_t cannot be dereferenced from U-mode roottask (heap is
         * PTE_U=0).
         */
        r = acquire_lock(&tmp->lock);

        /* Can say the query is done here because we've acquired the process
         * lock and that will keep process teardown from releasing the process_t
         * structure (or acquire_lock failed and we don't need the prp).
         */
        query_object_done(pid);

        if (r != EOK) {
#ifndef NDEBUG
            crash();
            /* NOTREACHED */
#endif
            return NULL;
        }

        /* Check that the slot still belongs to the pid after taking the lock.
         *
         * The slot-tag check ("tmp->pid == pid") substitutes for the old
         * "prp->pid == pid" deref: root_process_init/fini keep the tag in
         * lockstep with the kernel's pid-slot ownership.
         *
         * A mismatch means roottask has released this process (its slot was
         * finished, or not yet claimed) while the kernel still lists the pid.
         * That window lasts until the kernel finishes destroying it, and the
         * thread that closes it may be the very one this loop would starve on a
         * single hart, so report the process gone instead of retrying. Callers
         * already treat NULL as "gone or in a death transition"
         * (root_proc_lock_parent sleeps and retries).
         */
        if (tmp->pid == pid) {
            break;
        }
        root_log(
            "root_proc_lock: pid %d: kernel lists it but its roottask slot "
            "holds pid %d\n",
            pid, (int) tmp->pid);
        release_lock(&tmp->lock);
        return NULL;
    }
    return prp;
}

/* Release the lock taken by root_proc_lock for the given pid. Parameters:
 *   pid : the process id to unlock
 */
void root_proc_unlock(pid_t pid)
{
    release_lock(&root_proc(pid)->lock);
}

/* Lock a child's parent process, waiting out any parent death transition.
 * Parameters:
 *   child_pid      : the child whose parent to lock
 *   parent_pid_out : out — the parent's pid (may be NULL)
 * Return value: the locked parent process_t.
 */
process_t *root_proc_lock_parent(pid_t child_pid, pid_t *parent_pid_out)
{
    process_t *parent;
    pid_t ppid;
    uint64_t sleepl;
    unsigned count;

    /* Parent could be in a death transition */
    count = 0;
    for (;;) {
        ppid = root_proc(child_pid)->parent_pid;
        parent = root_proc_lock(ppid);
        if (parent)
            break;
        if (count > 100) {
            /* Sleep for one tick to give the transition more time to complete
             */
            sleepl = 1;
            __timer_timeout(CLOCK_MONOTONIC, RTMUX_TIMEOUT_NANOSLEEP, NULL,
                            &sleepl, NULL);
        } else if (count > 50) {
            /* Give other guys a chance to run */
            (void) __sched_yield();
        }
        ++count;
        CRASHCHECK(count == 0);
    }
    if (parent_pid_out)
        *parent_pid_out = ppid;
    return parent;
}

/* Acquire an arbitrary proc-mux lock. Parameters:
 *   mp : address of the lock pointer to acquire
 * Return value: EOK once held.
 */
int rdecl proc_mux_lock(struct proc_mux_lock **mp)
{
    pthread_mutex_lock(&mux_mutex);
    return acquire_lock(mp);
}

/* Release a proc-mux lock acquired with proc_mux_lock. Parameters:
 *   mp : address of the lock pointer to release
 * Return value: 0.
 */
int rdecl proc_mux_unlock(struct proc_mux_lock **mp)
{
    release_lock(mp);
    return 0;
}

/* Test whether a proc-mux lock is currently held by a given owner. Parameters:
 *   mp    : address of the lock pointer to test
 *   owner : the owner id to test for; 0 means the calling thread
 * Return value: nonzero if held by owner, 0 otherwise.
 */
int rdecl proc_mux_haslock(struct proc_mux_lock **mp, unsigned owner)
{
    int hasit;

    hasit = 0;
    pthread_mutex_lock(&mux_mutex);
    if (*mp) {
        if (owner == 0)
            owner = __tls()->__owner;
        if (((*mp)->mux.owner & ~RTMUX_SYNC_WAITING) == owner) {
            hasit = 1;
        }
    }
    pthread_mutex_unlock(&mux_mutex);
    return hasit;
}


static pthread_mutex_t object_allocator_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Allocate an object from a roottask soul under the allocator mutex.
 * Parameters:
 *   soulp : the soul descriptor to allocate from
 * Return value: the allocated object, or NULL on failure.
 */
void *proc_object_alloc(soul_t *soulp)
{
    void *ptr;

    pthread_mutex_lock(&object_allocator_mutex);
    ptr = object_alloc(soulp);
    pthread_mutex_unlock(&object_allocator_mutex);

    return ptr;
}


/* Free an object back to its roottask soul under the allocator mutex.
 * Parameters:
 *   soulp : the soul descriptor the object belongs to
 *   ptr   : the object to free
 */
void proc_object_free(soul_t *soulp, void *ptr)
{
    pthread_mutex_lock(&object_allocator_mutex);
    object_free(soulp, ptr);
    pthread_mutex_unlock(&object_allocator_mutex);
}

/* Unlock a process (if pid is nonzero) and propagate an error code.
 *
 * Convenience for handler error paths that hold a root_proc_lock. Parameters:
 *   ret : the value to return
 *   pid : the process to unlock, or 0 for none
 * Return value: ret.
 */
int proc_error(int ret, pid_t pid)
{
    if (pid != 0)
        root_proc_unlock(pid);

    return ret;
}

/* proc_isaccess — root-or-same-uid check on the message sender vs. a target
 * process. The prp argument is opaque (the kernel-half process_t can't be
 * dereferenced from U-mode roottask; heap is PTE_U=0), and every caller passes
 * NULL, so the predicate collapses to "is the caller root?".
 *
 * If a future caller wants to compare the sender's euid against a specific
 * target pid, it should fetch the target's cred via
 * process_query_info(target_pid, &qinfo) and compare info.cred.euid with
 * qinfo.cred.euid here. Parameters:
 *   prp     : target process (opaque; currently always NULL)
 *   rcvinfo : message info identifying the sender
 * Return value: nonzero if the sender is root, 0 otherwise.
 */
int proc_isaccess(process_t *prp, struct _msg_info *rcvinfo)
{
    struct _client_info info;

    (void) prp;
    if (connect_client_info(rcvinfo->scoid, &info, 0) == -1)
        return 0;
    return info.cred.euid == 0;
}

/* Reply to an I/O-service request according to a status code.
 *
 * Sends nothing for IOSERV_NOREPLY, msg_error for a positive errno, or
 * msg_replyv for a negative part-count. Parameters:
 *   ctp    : the message context to reply on
 *   status : IOSERV_NOREPLY, a positive errno, or a negative reply
 *            part-count
 * Return value: 0.
 */
int proc_status(ioserv_context_t *ctp, int status)
{
#ifdef CONFIG_TRACE_ROOTTASK_MSGS
    if (status == (int) IOSERV_NOREPLY) {
        root_log("RT<  rcvid=%-6d NOREPLY\n", ctp->rcvid);
    } else if (status > 0) {
        root_log("RT<  rcvid=%-6d errno=%d\n", ctp->rcvid, status);
    } else {
        root_log("RT<  rcvid=%-6d status=%d nparts=%d\n", ctp->rcvid,
                 ctp->status, -status);
    }
#endif

    if (status == (int) IOSERV_NOREPLY) {
        /* nothing to do */
    } else if (status > 0) {
        msg_error(ctp->rcvid, status);
    } else {
        msg_replyv(ctp->rcvid, ctp->status, ctp->iov + 0, -status);
    }

    return 0;
}
