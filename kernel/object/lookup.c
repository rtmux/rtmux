/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <core/globals.h>



/* Look up a process by pid.
 *
 * Validates the generation encoded in the pid against the slot's process so a
 * stale pid does not match a recycled slot. Parameters:
 *   pid : process id to resolve
 * Return value: the process_t, or NULL if no live process matches.
 */
process_t *rdecl lookup_pid(pid_t pid)
{
    process_t *prp;
    unsigned id;

    if ((id = PINDEX(pid)) < process_vector.nentries &&
        VECP(prp, &process_vector, id) && prp->pid == pid) {
        return prp;
    }

    return NULL;
}


/* lookup_pid_rcu — variant for BKL-less callers.
 *
 * Opens an RCU-sched read section, performs the lookup, and leaves the section
 * open. The caller MUST close the RCU read section after it has finished
 * accessing the returned process_t pointer (or pair the unlock even when NULL
 * is returned, since the section is opened either way).
 *
 * The section guarantees that the returned prp pointer remains valid (its
 * memory is not recycled by a concurrent kext_process_destroy) until
 * rcu_read_unlock returns. Inside the section the caller may dereference prp's
 * fields freely; outside the section, the pointer must NOT be retained.
 *
 * Constraints inside the section:
 *   - no block() / no sleep
 *   - keep it short — writers wait for the longest section
 */
process_t *rdecl lookup_pid_rcu(pid_t pid)
{
    process_t *prp;
    unsigned id;

    rcu_sched_read_lock();
    if ((id = PINDEX(pid)) < process_vector.nentries &&
        VECP(prp, &process_vector, id) && prp->pid == pid) {
        return prp; /* caller closes the RCU read section */
    }
    return NULL; /* caller still closes the RCU read section */
}


/* Look up a connection by coid in the active thread's process.
 *
 * Selects the fd-connection or (for RTMUX_SIDE_CHANNEL coids) the channel-
 * connection vector, then resolves the slot. Parameters:
 *   coid : connection id (optionally OR'd with RTMUX_SIDE_CHANNEL)
 * Return value: the connect_t, or NULL if the slot is empty or tagged.
 */
connect_t *rdecl lookup_connect(uint64_t coid)
{
    process_t *prp = get_active_thread(get_cpunum())->process;
    vector_t *vec;
    connect_t *cop = NULL;
    void *ptr;

    if (rtmux_cap_generation((rtmux_cap_t) coid) != 0) {
        void *object = NULL;
        uint64_t context = coid;

        if (rtmux_cap_lookup_pin_context(
                &prp->endpoint_caps, (rtmux_cap_t) coid,
                RTMUX_CAP_ENDPOINT_SEND, &object, &context) != EOK)
            return NULL;
        vec = (context & RTMUX_SIDE_CHANNEL) ? &prp->chancons : &prp->fdcons;
        kspinlock_lock(&prp->vec_slock);
        cop = vector_lookup2(vec, (unsigned) context & ~RTMUX_SIDE_CHANNEL);
        if (cop != object)
            cop = NULL;
        kspinlock_unlock(&prp->vec_slock);
        (void) rtmux_cap_unpin(&prp->endpoint_caps, (rtmux_cap_t) coid);
        return cop;
    }

    vec = &prp->fdcons;
    if (coid & RTMUX_SIDE_CHANNEL) {
        coid &= ~RTMUX_SIDE_CHANNEL;
        vec = &prp->chancons;
    }
    if (!vec)
        return NULL;

    /* Pin vec_slock for the brief read to avoid observing a half-grown
     * vec->vector (see the paired comment in fastmsg.c:inline_lookup_connect).
     */
    kspinlock_lock(&prp->vec_slock);
    if ((unsigned) coid < vec->nentries &&
        VECAND(ptr = VEC(vec, (unsigned) coid), 1) == 0) {
        cop = VECAND(ptr, ~3);
    }
    kspinlock_unlock(&prp->vec_slock);

    return cop;
}


/* This routine looks up a connection and then selects a thread from one
 *
 * RCU contract: lookup_rcvid OPENS connobj_rcu + tthread_rcu read sections on
 * entry and leaves them open on return. Sections are opened unconditionally —
 * even the NULL-return paths leave them open; release is required there too.
 *
 * Idempotency: a per-CPU nesting counter tracks how many rcvid-style lookup
 * windows are open on this CPU. lookup_rcvid_release() decrements the counter
 * and only closes the compiler-barrier RCU section on the outermost release, so
 * nested trap/interrupt cleanup cannot prematurely end the outer lookup window.
 *
 * Why two sections? cop (a connect_t) lives in chancons → connobj_rcu. thp (a
 * thread_t) lives in cop->process->threads → tthread_rcu. Holding both prevents
 * either object from being freed under the caller's deref.
 */
volatile uint8_t rcvid_rcu_nesting[PROCESSORS_MAX];

void rdecl lookup_rcvid_open(void)
{
    unsigned cpu = get_cpunum();
    uint8_t old;

    old = __atomic_load_n(&rcvid_rcu_nesting[cpu], __ATOMIC_RELAXED);
    for (;;) {
        if (old == UINT8_MAX)
            crash();
        if (__atomic_compare_exchange_n(&rcvid_rcu_nesting[cpu], &old,
                                        (uint8_t) (old + 1), 0,
                                        __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
            break;
    }
    rcu_sched_read_lock();
    rcu_sched_read_lock();
}

connect_t *rdecl lookup_rcvid(kcall_args_t *kap, int rcvid, thread_t **thpp)
{
    connect_t *cop;
    thread_t *thp, *act = get_active_thread(get_cpunum());
    unsigned tid;

    lookup_rcvid_open();

    if (!(cop = vector_lookup(&act->process->chancons, MCINDEX(rcvid))) ||
        cop->type != TYPE_CONNECTION || !cop->process) {
        kcall_err(act, ESRCH);
        return NULL;
    }

    tid = MTINDEX(rcvid);
    if (cop->flags & COF_NETCON) {
        if (!cop->un.remote.cred) {
            kcall_err(act, EBADF);
            return NULL;
        }
        if (act->process == remote_ipc.prp &&
            (cop->scoid != MCINDEX(rcvid) ||
             cop->channel->process != remote_ipc.prp)) {
            thp = cop->channel
                      ? vector_lookup(&cop->channel->process->threads, tid)
                      : NULL;
        } else {
            thp = vector_lookup(&vthread_vector, tid);
            if (kap) {
                *thpp = thp;
                return remote_ipc_send(kap, tid, cop, thp);
            }
        }
    } else {
        thp = vector_lookup(&cop->process->threads, tid);
    }

    if (!thp) {
        if (kap && (KTYPE(act) == __KSYS_MSG_DELIVER_EVENT ||
                    KTYPE(act) == __KSYS_MSG_VERIFY_EVENT)) {
            /* If the event is to be delivered to a process and the initial
             * thread if the rcvid doesn't exist, use the valid thread.
             */
            switch (SIGEV_GET_TYPE(kap->msg_deliver_event.event)) {
            case SIGEV_SIGNAL_THREAD:
            case SIGEV_UNBLOCK:
            case SIGEV_INTR:
                break;

            default:
                thp = cop->process->valid_thp;
            }
        }

        if (!thp) {
            kcall_err(act, ESRCH);
            return NULL;
        }
    }

    *thpp = thp;
    return cop;
}


/* lookup_rcvid_release — close the RCU sections opened by lookup_rcvid.
 *
 * Idempotent: gated on the per-CPU nesting counter set by lookup_rcvid_open().
 * Safe to call from a trap entry/return safety net even when no lookup_rcvid
 * was performed (the flag will be 0).
 *
 * Order matches the LIFO of the RCU-sched read locks in lookup_rcvid_open().
 */
void rdecl lookup_rcvid_release(void)
{
    unsigned cpu = get_cpunum();
    uint8_t old;

    old = __atomic_load_n(&rcvid_rcu_nesting[cpu], __ATOMIC_RELAXED);
    for (;;) {
        if (old == 0)
            return;
        if (__atomic_compare_exchange_n(&rcvid_rcu_nesting[cpu], &old,
                                        (uint8_t) (old - 1), 0,
                                        __ATOMIC_RELEASE, __ATOMIC_RELAXED))
            break;
    }
    if (old == 1) {
        rcu_sched_read_unlock();
        rcu_sched_read_unlock();
    }
}


/* Look up the resource-limits record for a user. Parameters:
 *   uid : user id to match
 * Return value: the matching limits_t, or NULL if none.
 */
limits_t *rdecl lookup_limits(uid_t uid)
{
    limits_t *lip;

    for (lip = limits_list; lip; lip = lip->next) {
        if (lip->uid == uid) {
            break;
        }
    }

    return lip;
}
