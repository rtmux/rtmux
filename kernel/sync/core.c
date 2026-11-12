/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <core/globals.h>


#define HASH_PAGEBITS 12
#define HASH_PAGESIZE (1 << HASH_PAGEBITS)
#define HASH_FUNC(hash, val) (((val) >> HASH_PAGEBITS) & hash.mask)
#define SEC_HASH_BINS (0x40)
#define SEC_HASH(addr) (((addr) >> 3) & (SEC_HASH_BINS - 1))


/* Create a kernel sync object backing a user sync_t.
 *
 * Allocates a ksync_t, maps the user sync's virtual address to a physical
 * address (marking the page as holding a sync object), and registers it in the
 * sync hash keyed by that physical address. Parameters:
 *   bill_prp : process billed via the create hook
 *   sync     : user-space sync_t address
 *   flags    : PTHREAD_PROCESS_* sharing flags
 * Return value: the new ksync_t, or NULL on error (with an errno on the
 *               active thread).
 */
ksync_t *rdecl sync_create(process_t *bill_prp, sync_t *sync, unsigned flags)
{
    ksync_t *syp;
    path_object_t *obp;
    paddr_t addr;
    thread_t *act = get_active_thread(get_cpunum());
    process_t *prp = act->process;

    /* Allocate a sync entry. */
    if (!(syp = object_alloc(prp, &sync_souls))) {
        kcall_err(act, EAGAIN);
        return NULL;
    }

    /* Map the virtual address of the users sync to a physical addr. (flags &
     * PTHREAD_PROCESSSHARED_MASK) may be useful to vm_ops Note that we tell
     * vaddr_to_memobj to mark this page as containing a sync object.
     */
    if ((obp = vm_ops->vaddr_to_memobj(prp, sync, &addr, 1)) ==
        (path_object_t *) -1) {
        kcall_err(act, EINVAL);
        return NULL;
    }
    /* Add the entry to the sync vector. */
    if (synchash_add(obp, addr, syp) == -1) {
        object_free(prp, &sync_souls, syp);
        kcall_err(act, EBUSY);
        return NULL;
    }

    return syp;
}

/* Destroy the kernel sync object backing a user sync_t.
 *
 * Verifies the sync exists, marks the user sync_t RTMUX_SYNC_DESTROYED
 * (rejecting a mutex still owned by another thread), force-readies any waiters,
 * and removes it from the sync hash. Parameters:
 *   prp  : process billed via the destroy hook
 *   sync : user-space sync_t address
 * Return value: EOK, or an errno (EINVAL/EBUSY).
 */
int rdecl sync_destroy(process_t *prp, sync_t *sync)
{
    path_object_t *obp;
    paddr_t addr;
    ksync_t *syp;
    thread_t *thp;
    unsigned long owner;
    thread_t *act = get_active_thread(get_cpunum());

    /* This must run as a kernel extension body inside a kcall. */
    CRASHCHECK(in_kcall[get_cpunum()] == 0);

    /* Map the virtual address of the users sync to a physical addr. */
    if ((obp = vm_ops->vaddr_to_memobj(act->process, sync, &addr, 0)) ==
        (path_object_t *) -1) {
        return EINVAL;
    }
    /* Verify the sync exists. */
    kspinlock_lock(&sync_slock);
    syp = synchash_lookup(obp, addr);
    kspinlock_unlock(&sync_slock);
    if (!syp) {
        return EINVAL;
    }


    /* Make it invalid incase someone tried to use it in the future. The owner
     * must be zero or we are busy. If it is zero atomicly set it to
     * RTMUX_SYNC_DESTROYED (-2) to indicate it is destroyed. If the sync is a
     * mutex owned by the caller, it can be destroyed. If it is not a mutex also
     * destroy the sync.
     */
    if ((owner = _mux_smp_cmpxchg_ul(&sync->owner, 0, RTMUX_SYNC_DESTROYED)) !=
            0 &&
        SYNC_OWNER(act) != (owner & ~(RTMUX_SYNC_WAITING)) &&
        owner <= RTMUX_SYNC_DEAD) {
        return EBUSY;
    }
    sync->owner = RTMUX_SYNC_DESTROYED;

    /* Force ready any threads blocked on the sync. Note: This will only happen
     * for semaphore since they have no concept
     *       of ownership.
     */
    while ((thp = pril_first(&syp->waiting))) {
        if (TYPE_MASK(thp->type) != TYPE_THREAD) {
            break;
        }
        force_ready(thp, EINVAL);
    }

    /* Remove from the sync hash and release object back to sync_souls. */
    synchash_rem(addr, obp, addr, addr, NULL, NULL);

    /* since synchash_rem() does not have any return code ... */
    CRASHCHECK(synchash_lookup(obp, addr));

    return EOK;
}


/* Look up (and optionally auto-create) the kernel sync object for a user
 * sync_t.
 *
 * Probes the user sync's owner/count fields (SMP-safe atomic_or) to fault them
 * in and privatise the page, resolves the physical address, and searches the
 * sync hash. If not found and the user initializer permits, auto-initializes a
 * new sync. Parameters:
 *   sync   : user-space sync_t address
 *   create : the create discriminator (e.g. RTMUX_SYNC_COND), or
 *            RTMUX_SYNC_INITIALIZER to forbid auto-create
 * Return value: the ksync_t, or NULL on error (with an errno set).
 */
ksync_t *rdecl sync_lookup(sync_t *sync, unsigned create)
{
    ksync_t *syp;
    thread_t *act = get_active_thread(get_cpunum());
    paddr_t addr;
    paddr_t mem_addr;
    path_object_t *obp;
    ksync_t **owner;

    if (!sync || ((uintptr_t) sync & (sizeof(unsigned) - 1)) ||
        !WITHIN_BOUNDARY((uintptr_t) sync, (uintptr_t) (sync + 1),
                         act->process->boundary_addr)) {
        kcall_err(act, EFAULT);
        return NULL;
    }

    /* Reference sync so it will fault if not-present or invalid. We do this
     * with atomic_or rather than the usual "WR_PROBE_INT(act, &sync->owner, 1)"
     * because another CPU in an SMP system might be playing around with the
     * memory at the same time and the WR_PROBE_INT accessing is *not* SMP safe.
     * Don't do this to us again! (atomic_or here OR's 0 in — i.e. probes by
     * writing back what's there atomically.)
     *
     * We also have to probe the 'count' field so that it will get privatized
     * before we do the vm_ops->vaddr_to_memobj() call. Since the 'count' field
     * is at the the start of the sync object, its address is used for the
     * vm_ops->vaddr_to_memobj return - if it happens to fall on a different
     * page than the 'owner' field, we could have it's paddr move on us after
     * the vaddr_to_memobj has returned it and we've entered the original value
     * in the hash table.
     */
    if (xfer_atomic_or_user((volatile unsigned *) &sync->count, 0) != 0 ||
        xfer_atomic_or_user((volatile unsigned *) &sync->owner, 0) != 0) {
        kcall_err(act, EFAULT);
        return NULL;
    }

    obp = vm_ops->vaddr_to_memobj(act->process, sync, &mem_addr, 0);
    if (!obp) {
        kcall_err(act, EINVAL);
        return NULL;
    }

    /* The following assignment is for improved code generation. By doing it,
     * the compiler knows that it can cache the 'addr' value in a register
     * (harder for 'mem_addr', since it's address was taken above).
     */
    addr = mem_addr;

    /* Walk the hash chain under sync_slock. Drop the lock before the auto-init
     * path, which calls sync_create -> synchash_add, itself a sync_slock taker;
     * the chain may have changed between the failed lookup and the create
     * attempt, but synchash_add re-checks for a duplicate under the lock so the
     * race is benign.
     */
    kspinlock_lock(&sync_slock);
    owner = (ksync_t **) sync_hash.table[HASH_FUNC(sync_hash, addr)];
    if (owner) {
        owner += SEC_HASH(addr);
        for (;;) {
            syp = *owner;
            if (!syp)
                break;
            if ((syp->addr == addr) && (syp->obj == obp)) {
                kspinlock_unlock(&sync_slock);
                goto found_it;
            }
            owner = &syp->next;
        }
    }
    kspinlock_unlock(&sync_slock);

    /* If sync object not found, then we autoinit a sync. Auto-init is allowed
     * when:
     *   owner == RTMUX_SYNC_INITIALIZER (0xFFFFFFFF, standard initializer), or
     *   owner == create (e.g., RTMUX_SYNC_COND for PTHREAD_COND_INITIALIZER)
     * Reject if create == RTMUX_SYNC_INITIALIZER (caller said "don't
     * auto-create").
     */
    if (create == RTMUX_SYNC_INITIALIZER ||
        (sync->owner != RTMUX_SYNC_INITIALIZER &&
         sync->owner != (unsigned long) create)) {
        kcall_err(act, EINVAL);
        return NULL;
    }

    if ((syp = sync_create(act->process, sync, PTHREAD_PROCESS_PRIVATE)) ==
        NULL) {
        return NULL;
    }

    sync->owner = create;
    if (create == RTMUX_SYNC_COND) {
        sync->count = CLOCK_REALTIME;
    }

    KSYS_PREEMPT(act, NULL);

found_it:
    if (sync->owner == RTMUX_SYNC_INITIALIZER) {
        kcall_err(act, EINVAL);
        return NULL;
    }

    return syp;
}


/* Wake one or all threads blocked on a sync object.
 *
 * Walks syp->waiting under sync_slock, removing and readying waiters; the
 * highest-priority mutex/condvar waiter is pinned to this CPU to win the mutex
 * and avoid an SMP priority inversion. sync_slock is dropped across the
 * WAIT_PENDING spin and across ready() (lock ordering is SCHED < SYNC). Callers
 * MUST NOT hold sync_slock on entry.
 *
 * A tthread_rcu section spans the walk so a concurrent thread_destroy cannot
 * free a snapshotted waiter while sync_slock is dropped. RCU-sched read locks
 * are compiler barriers; the grace period is bounded by peer kcall quiescence,
 * so nested helper sections are harmless. Parameters:
 *   syp : sync object whose waiters to wake
 *   all : non-zero to wake every waiter, zero to wake only the first
 */
void rdecl sync_wakeup(ksync_t *syp, int all)
{
    thread_t *thp;
    bool first;
    unsigned orig_runmask;
    unsigned new_runmask;
    thread_t *act = get_active_thread(get_cpunum());

    /* Open a tthread_rcu section across the walk + ready() dispatch. thp
     * pointers are snapshotted from syp->waiting under sync_slock; the slock is
     * dropped around the WAIT_PENDING spin and around ready(). Without the RCU
     * section, a concurrent thread_destroy(thp) could complete its vector_rem +
     * rcu_sched_synchronize + object_free while we still hold thp, leaving
     * pril_rem to walk zeroed prev/next.
     */
    rcu_read_lock(&tthread_rcu);

    first = true;
    kspinlock_lock(&sync_slock);
    while ((thp = pril_first(&syp->waiting))) {
        if (TYPE_MASK(thp->type) != TYPE_THREAD) {
            thp = pril_next(thp);
            if (!thp || TYPE_MASK(thp->type) != TYPE_THREAD) {
                break;
            }
        }

        /* If thp is mid-transition into the wait state (the waiter has
         * pril_add'd but not yet finished unready), pril_rem-and-ready would
         * race ready_default's "thp == active" check or stamp on the unready in
         * flight. Drop sync_slock so the waiter's unready (which takes
         * sched_slock; we hold sync_slock = high-rank) can complete, spin until
         * the flag clears, then re-walk the list.
         */
        if (thp->internal_flags & RTMUX_ITF_WAIT_PENDING) {
            kspinlock_unlock(&sync_slock);
            while (thp->internal_flags & RTMUX_ITF_WAIT_PENDING)
                __asm__ volatile("" ::: "memory");

            /* Hardware fence: pair with the writer's full memory barrier before
             * its WAIT_PENDING clear. Ensures we see the writer's preceding
             * stores (blocked_on, state) once we observe the flag cleared.
             * Arch-appropriate via cpu_smp_mb.
             */
            cpu_smp_mb();
            kspinlock_lock(&sync_slock);
            continue;
        }
        pril_rem(&syp->waiting, thp);

        /* Clear blocked_on UNDER sync_slock so that a concurrent
         * adjust_priority_default (STATE_CONDVAR/SEM/MUTEX arms) — which
         * re-reads state + blocked_on under the same lock after we drop it —
         * sees blocked_on==NULL and skips the rem+add cycle on an entry we have
         * just removed. Otherwise adjust_priority's pril_rem walks stale
         * prev/next pointers and its pril_add re-attaches the stale pril,
         * surfacing later as the waiter re-entering cv_wait while still pinned
         * to syp->waiting. (Setting state to READY here would close the race
         * too but breaks ready_default's state==READY-at-entry invariant;
         * clearing blocked_on is the narrower fix.)
         *
         * Force_ready's STATE_MUTEX / STATE_CONDVAR / STATE_SEM cases
         * NULL-check blocked_on before deref — see dispatch.c.
         */
        thp->blocked_on = NULL;

        /* This screws up the ETIMEDOUT return code from sync_condvar_wait.
         * Instead, we EOK the status when the thread goes onto the waiting
         * list... SETKSTATUS(thp,EOK);
         */

        orig_runmask = thp->runmask;
        switch (thp->state) {
        case STATE_MUTEX:
        case STATE_CONDVAR:
            atomic_or(&thp->flags, RTMUX_FLG_THR_ACQUIRE_MUTEX);
            if (first) {
                if (thp->priority > act->priority) {
                    /* Make sure that the first (highest priority) blocked
                     * thread gets run on _this_ CPU. That way, it'll have first
                     * crack at the mutex and avoid an SMP priority inversion
                     * problem (PR 8591).
                     */
                    new_runmask = 1u << get_cpunum();
                    if ((thp->runmask & new_runmask) == 0) {
                        thp->runmask = ~new_runmask;
                    }
                }
                first = false;
            }
            break;
        default:
            break;
        }
        kspinlock_unlock(&sync_slock);
        ready(thp);
        thp->runmask = orig_runmask;
        if (!all) {
            rcu_read_unlock(&tthread_rcu);
            return;
        }
        kspinlock_lock(&sync_slock);
    }
    kspinlock_unlock(&sync_slock);
    rcu_read_unlock(&tthread_rcu);
}

/* Caller must hold sync_slock.
 *
 * Returns the matching ksync_t in the sync_hash chain, or NULL. Public callers
 * in this file (synchash_add as a duplicate-check, sync_destroy, the kernel
 * extension vm_ops-callback path) acquire sync_slock around their own use.
 */
ksync_t *rdecl synchash_lookup(path_object_t *obp, paddr_t addr)
{
    ksync_t **owner;
    ksync_t *syp;

    owner = (ksync_t **) sync_hash.table[HASH_FUNC(sync_hash, addr)];
    if (owner) {
        owner += SEC_HASH(addr);
        for (;;) {
            syp = *owner;
            if (!syp)
                break;
            if ((syp->addr == addr) && (syp->obj == obp)) {
                return syp;
            }
            owner = &syp->next;
        }
    }
    return NULL;
}


/* Insert a sync object into the sync hash.
 *
 * sync_slock covers the duplicate-check (synchash_lookup) and the chain
 * mutation as one atomic operation, so concurrent adders for the same addr/obp
 * cannot race. Parameters:
 *   obp  : memory object backing the sync's page
 *   addr : physical address keying the hash
 *   syp  : sync object to insert
 * Return value: 0 on success, -1 on duplicate or allocation failure.
 */
int rdecl synchash_add(path_object_t *obp, paddr_t addr, ksync_t *syp)
{
    paddr_t index, sindex;
    ksync_t **table;
    int rc;

    kspinlock_lock(&sync_slock);

    if (synchash_lookup(obp, addr)) {
        rc = -1;
        goto out;
    }

    syp->obj = obp;
    syp->addr = addr;

    index = HASH_FUNC(sync_hash, addr);
    table = (ksync_t **) (void *) sync_hash.table[index];

    if (!table) {
        table = _scalloc(sizeof(ksync_t *) * SEC_HASH_BINS);
        if (!table) {
            rc = -1;
            goto out;
        }
        sync_hash.table[index] = table;
    }
    sindex = SEC_HASH(addr);
    syp->next = table[sindex];
    table[sindex] = syp;

    rc = 0;
out:
    kspinlock_unlock(&sync_slock);
    return rc;
}


/* Remove sync objects from the sync hash.
 *
 * Removes a single sync (addr1 == addr2) or every sync whose address falls
 * within [addr1, addr2] (a whole-page unmap). When prp is given, handles a
 * mapped-out region that may contain a held mutex: probes the user sync
 * (delivering robust-mutex events, force-readying waiters) before unlinking and
 * freeing the sync object.
 *
 * sync_hash chain access is wrapped in sync_slock. The user-space sync->owner
 * probe (atomic_or) can fault, so sync_slock is dropped across the probe —
 * otherwise IRQs would stay disabled for an arbitrary page-fault handler; the
 * chain mutation always runs under sync_slock. Parameters:
 *   addr  : physical address used to index the hash
 *   obp   : memory object backing the sync's page
 *   addr1 : start of the physical address range to remove
 *   addr2 : end of the range (== addr1 for a single removal)
 *   prp   : process being torn down, or NULL for a plain destroy
 *   vaddr : base user virtual address of the unmapped region (with prp)
 */
void rdecl synchash_rem(paddr_t addr,
                        path_object_t *obp,
                        paddr_t addr1,
                        paddr_t addr2,
                        process_t *prp,
                        void *vaddr)
{
    paddr_t index, sindex, eindex;
    ksync_t *prev, *syp, **table;

    index = HASH_FUNC(sync_hash, addr);
    table = (ksync_t **) (void *) sync_hash.table[index];

    if (!table)
        return;

    /* We have two cases, either remove one or scan a whole page */
    if (addr1 == addr2) {
        sindex = SEC_HASH(addr);
        eindex = sindex + 1;
    } else {
        sindex = 0;
        eindex = SEC_HASH_BINS;
    }

    kspinlock_lock(&sync_slock);

    for (index = sindex; index < eindex; index++) {
        for (prev = (ksync_t *) (void *) &table[index]; (syp = prev->next);
             prev = syp) {
            if (syp->obj == obp && syp->addr >= addr1 && syp->addr <= addr2) {
                process_t *prp1;
                thread_t *thp;
                struct syncevent_entry *syncep;
                struct syncevent_entry *next;
                unsigned event_state;

#define EVENT_DELIVER 0x01
#define EVENT_PROCESSED 0x02

                if (prp) {
                    /* Are we are unmapping a region that has a mutex in it? */
                    sync_t *sync =
                        (sync_t *) ((char *) vaddr + (syp->addr - addr1));

                    /* Drop sync_slock around the user-space probe; a write
                     * fault here must not run with IRQs disabled on this CPU.
                     * The BKL still holds the kernel, so the chain itself can't
                     * be mutated by another CPU while we are dropped.
                     */
                    kspinlock_unlock(&sync_slock);

                    /* An inaccessible owner word here just means the mapping is
                     * already gone, which the walk below handles.
                     */
                    (void) xfer_atomic_or_user(
                        (volatile unsigned *) &sync->owner, 0);
                    event_state = 0;

                    kspinlock_lock(&sync_slock);

                    /* Is the mutex currently locked by a thread in the
                     * unmapping process
                     */
                    if (SYNC_PINDEX(sync->owner) ==
                        SYNC_PINDEX(SYNC_OWNER_BITS(prp->pid, 0))) {
                        event_state = EVENT_DELIVER;

                        thp = pril_first(&syp->waiting);
                        if (thp) {
                            unsigned type = TYPE_MASK(thp->type);

                            if ((type == TYPE_THREAD &&
                                 thp->state == STATE_MUTEX) ||
                                (type == TYPE_SYNCEVENT &&
                                 ((struct syncevent_entry *) thp)->subtype ==
                                     SYNCEVENT_SUBTYPE_PRIORITYCEILING)) {
                                if (thp->args.mu.owner) {
                                    /* mutex_holdlist_rem now takes sync_slock
                                     * internally — drop our hold around it.
                                     */
                                    kspinlock_unlock(&sync_slock);
                                    mutex_holdlist_rem(syp);
                                    kspinlock_lock(&sync_slock);
                                    thp->args.mu.owner = 0;
                                }
                            }
                        }
                        sync->owner = RTMUX_SYNC_DEAD;
                    }
                    /* looking for any event attached to the mutex */
                    for (syncep = pril_first(&syp->waiting); syncep;
                         syncep = next) {
                        next = pril_next(syncep);
                        if (TYPE_MASK(syncep->type) == TYPE_SYNCEVENT &&
                            syncep->subtype == SYNCEVENT_SUBTYPE_EVENT) {
                            prp1 = syncep->un.ev.process;
                            if (prp1 == prp) {
                                /* Guardian state lives in root_process_t
                                 * (roottask side) and is not reachable in the
                                 * sync hot path, so drop the mutex event
                                 * unconditionally. Mutex robustness, if wanted,
                                 * is coordinated by the holder and cleaner via
                                 * roottask messaging (proc_thread_pool /
                                 * sys_chp).
                                 */
                                pril_rem(&syp->waiting, syncep);
                                object_free(prp, &syncevent_souls, syncep);
                                continue;
                            }
                            if (event_state & EVENT_DELIVER) {
                                /* deliver event */
                                event_state |= EVENT_PROCESSED;
                                thp = vector_lookup(&prp1->threads,
                                                    syncep->un.ev.tid);
                                if ((thp) || (thp = prp1->valid_thp)) {
                                    sigevent_exe(&syncep->un.ev.event, thp, 1,
                                                 0);
                                }
                            }
                        }
                    }

                    if (event_state == EVENT_DELIVER) {
                        /* supposed to deliver event, but no event found */
                        sync->owner = RTMUX_SYNC_DESTROYED;
                        while ((thp = pril_first(&syp->waiting))) {
                            if (TYPE_MASK(thp->type) != TYPE_THREAD) {
                                thp = pril_next(thp);
                                if (!((thp) &&
                                      TYPE_MASK(thp->type) == TYPE_THREAD)) {
                                    break;
                                }
                            }

                            /* Kill all threads waiting on the mutex without a
                             * timeout
                             */
                            if ((thp->timeout_flags &
                                 (RTMUX_FLG_TIMEOUT_ACTIVE |
                                  (1 << STATE_MUTEX))) !=
                                (RTMUX_FLG_TIMEOUT_ACTIVE |
                                 (1 << STATE_MUTEX))) {
                                /* force_ready's STATE_MUTEX path takes
                                 * sync_slock via mutex_holdlist_rem — drop our
                                 * hold around it. Deadlock: force ready with
                                 * EOWNERDEAD (signals removed)
                                 */
                                kspinlock_unlock(&sync_slock);
                                force_ready(thp, EOWNERDEAD);
                                kspinlock_lock(&sync_slock);
                            }
                        }
                    }
                } else {
                    /* remove the sync from its holding list */
                    thp = pril_first(&syp->waiting);
                    if (thp) {
                        unsigned type = TYPE_MASK(thp->type);

                        if (((type == TYPE_THREAD &&
                              thp->state == STATE_MUTEX) ||
                             (type == TYPE_SYNCEVENT &&
                              ((struct syncevent_entry *) thp)->subtype ==
                                  SYNCEVENT_SUBTYPE_PRIORITYCEILING)) &&
                            thp->args.mu.owner) {
                            /* mutex_holdlist_rem now takes sync_slock
                             * internally — drop our hold around it.
                             */
                            kspinlock_unlock(&sync_slock);
                            mutex_holdlist_rem(syp);
                            kspinlock_lock(&sync_slock);
                            thp->args.mu.owner = 0;
                        }
                        /* Force ready any threads blocked on the sync */
                        do {
                            if (TYPE_MASK(thp->type) == TYPE_THREAD) {
                                /* force_ready's STATE_MUTEX path takes
                                 * sync_slock via mutex_holdlist_rem — drop our
                                 * hold around it.
                                 */
                                kspinlock_unlock(&sync_slock);
                                force_ready(thp, EINVAL);
                                kspinlock_lock(&sync_slock);
                            } else {
                                syncep = (struct syncevent_entry *) thp;
                                pril_rem(&syp->waiting, syncep);
                                if (syncep->subtype ==
                                    SYNCEVENT_SUBTYPE_EVENT) {
                                    object_free(syncep->un.ev.process,
                                                &syncevent_souls, syncep);
                                } else { /* SYNCEVENT_SUBTYPE_PRIORITYCEILING */
                                    object_free(NULL, &thread_souls, syncep);
                                }
                            }
                            thp = pril_first(&syp->waiting);
                        } while (thp);
                    }

                    prev->next = syp->next;
                    object_free(NULL, &sync_souls, syp);
                    syp = prev;
                    /* Optimize the case of single delete */
                    if (addr1 == addr2) {
                        kspinlock_unlock(&sync_slock);
                        return;
                    }
                }
            }
        }
    }

    kspinlock_unlock(&sync_slock);
}


struct kcall_args_synchash_rem {
    paddr_t addr;
    path_object_t *obp;
    paddr_t start;
    paddr_t end;
    process_t *prp;
    void *vaddr;
};

/* Kext entry that removes sync objects across a (possibly multi-page) address
 * range.
 *
 * Calls synchash_rem for each hash page in the range, yielding to preemption
 * between pages when not invoked with the kernel locked. Parameters:
 *   data : kcall_args_synchash_rem describing the range and target process
 */
void ksys_synchash_rem(void *data)
{
    struct kcall_args_synchash_rem *kap = data;
    unsigned ksys_lock;

    ksys_lock = get_inkernel() & INKERNEL_LOCK;
    if (kap->addr > (kap->end | (HASH_PAGESIZE - 1))) {
        kext_status(NULL, 0);
        return;
    }
    for (;;) {
        synchash_rem(kap->addr, kap->obp, kap->start, kap->end, kap->prp,
                     kap->vaddr);
        if ((kap->addr & ~(paddr_t) (HASH_PAGESIZE - 1)) ==
            (kap->end & ~(paddr_t) (HASH_PAGESIZE - 1)))
            break;
        kap->addr += HASH_PAGESIZE;
        if (ksys_lock == 0) {
            KEXT_PREEMPT(get_active_thread(get_cpunum()));
        }
    }
    kext_status(NULL, 0);
}

/* memobj_destroyed — kernel-internal entry used by mm_munmap. U-mode roottask
 * has its own equivalent in servers/roottask/proc/kext-wrappers.c that goes
 * through syscall_priv(__KSYS_PRIV_SYNCHASH_REM, ...).
 */
void memobj_destroyed(path_object_t *obp,
                      paddr_t start,
                      paddr_t end,
                      process_t *prp,
                      void *vaddr)
{
    struct kcall_args_synchash_rem data;

    data.addr = start;
    data.obp = obp;
    data.start = start;
    data.end = end;
    data.prp = prp;
    data.vaddr = vaddr;
    if (data.addr > (data.end | (HASH_PAGESIZE - 1)))
        return;
    for (;;) {
        ksys_synchash_rem(&data);
        if ((data.addr & ~(paddr_t) (HASH_PAGESIZE - 1)) ==
            (data.end & ~(paddr_t) (HASH_PAGESIZE - 1)))
            break;
        data.addr += HASH_PAGESIZE;
    }
}


/* mutex_holdlist_add / mutex_holdlist_rem walk the ksync_t wait-list
 * (pril_first) and mutate the per-thread mutex_holdlist links. Two flavours:
 *
 *   *_locked  — caller already holds sync_slock; primitive does not take
 *               it.  Used by paths needing a larger sync_slock-protected
 *               critical section that also covers surrounding
 *               pril_first / pril_rem on syp->waiting (force_ready
 *               STATE_MUTEX, adjust_priority_default STATE_MUTEX).
 *
 *   public form — takes sync_slock internally, calls the locked form,
 *                 releases.  Used by every other caller.
 *
 * Public callers MUST NOT hold sync_slock on entry — notably, synchash_rem
 * drops it around these calls.
 */
void mutex_holdlist_add_locked(thread_t *thp, ksync_t *syp)
{
    ksync_t **owner;
    ksync_t *curr;
    thread_t *new;

    new = pril_first(&syp->waiting);
    owner = &thp->mutex_holdlist;
    for (;;) {
        curr = *owner;
        if (!curr)
            break;
        thp = pril_first(&curr->waiting);
        if (thp->priority < new->priority)
            break;
        owner = &thp->args.mu.next;
    }
    new->args.mu.next = curr;
    new->args.mu.prev = owner;
    if (curr) {
        thp->args.mu.prev = &new->args.mu.next;
    }
    *owner = syp;
}

void mutex_holdlist_add(thread_t *thp, ksync_t *syp)
{
    kspinlock_lock(&sync_slock);
    mutex_holdlist_add_locked(thp, syp);
    kspinlock_unlock(&sync_slock);
}


void mutex_holdlist_rem_locked(ksync_t *syp)
{
    thread_t *thp;
    thread_t *next_thp;
    ksync_t *next_syp;

    thp = pril_first(&syp->waiting);
    next_syp = thp->args.mu.next;
    *thp->args.mu.prev = next_syp;
    if (next_syp) {
        next_thp = pril_first(&next_syp->waiting);
        next_thp->args.mu.prev = thp->args.mu.prev;
    }
}

void mutex_holdlist_rem(ksync_t *syp)
{
    kspinlock_lock(&sync_slock);
    mutex_holdlist_rem_locked(syp);
    kspinlock_unlock(&sync_slock);
}
