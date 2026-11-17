/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Deferred priority settle of a server whose client departed.
 *
 * force_ready ends the REPLY loan of a client that leaves by timeout, signal or
 * channel destroy (thread_client_departed), but it runs from timer and event
 * delivery under caller locks, so it may not take the PI lock or walk the wait
 * chain the server is blocked in. It queues the server on the running hart's
 * list instead; kernel_exit settles that list in thread context, where both are
 * allowed, before the hart resumes a thread or idles.
 *
 * Memory ordering: the loan-end store precedes the list lock's release, and the
 * settle pops the server under the same lock, so its recomputation reads that
 * store (thread_prio_update fences first as well). A departure that finds the
 * server already queued adds nothing: the settle has not popped it yet, so it
 * too reads the store. One that finds it popped queues it again.
 *
 * Lifetime: only a server destroyed before a queued settle ran could be hurt,
 * and thread_destroy takes it off the list (thread_prio_defer_cancel) once the
 * RCU drain has ended every departure that could still queue it. A settle that
 * popped it earlier holds a pin from inside the list lock, which the destroy
 * waits out.
 */

#include <core/globals.h>

/* One lock guards every hart's list and each queued thread's link fields: a
 * server's departures come from any hart, and a lock per hart would let two of
 * them queue the same server on two lists. Departures that queue are rare.
 */
static kspinlock_t prio_defer_slock;
static thread_t *prio_defer_head[CONFIG_PROCESSORS_MAX];
_Atomic unsigned prio_defer_pending[CONFIG_PROCESSORS_MAX];

void prio_defer_init(void)
{
    kspinlock_init(&prio_defer_slock, LOCK_CLASS_PRIO_DEFER,
                   "prio_defer_slock");
}

void rdecl thread_prio_defer_settle(thread_t *srv)
{
    unsigned cpu = get_cpunum();
    unsigned long flags;

    if (TYPE_MASK(srv->type) != TYPE_THREAD)
        return;
    flags = kspinlock_lock_irqsave(&prio_defer_slock);
    if (!srv->prio_defer_cpu) {
        srv->prio_defer_cpu = (uint8_t) (cpu + 1);
        srv->prio_defer_next = prio_defer_head[cpu];
        prio_defer_head[cpu] = srv;
        __atomic_store_n(&prio_defer_pending[cpu], 1, __ATOMIC_RELAXED);
    }
    kspinlock_unlock_irqrestore(&prio_defer_slock, flags);
}

void rdecl thread_prio_run_deferred(void)
{
    unsigned cpu = get_cpunum();

    for (;;) {
        thread_t *srv;
        unsigned long flags = kspinlock_lock_irqsave(&prio_defer_slock);
        bool pinned = false;

        srv = prio_defer_head[cpu];
        if (srv) {
            prio_defer_head[cpu] = srv->prio_defer_next;
            srv->prio_defer_next = NULL;
            srv->prio_defer_cpu = 0;
            pinned = tthread_pin(srv) == EOK;
        }
        if (!prio_defer_head[cpu])
            __atomic_store_n(&prio_defer_pending[cpu], 0, __ATOMIC_RELAXED);
        kspinlock_unlock_irqrestore(&prio_defer_slock, flags);
        if (!srv)
            return;
        if (pinned) {
            inject_note_prio_deferred_run();
            thread_prio_update(srv);
            tthread_unpin(srv);
        }
    }
}

void rdecl thread_prio_defer_cancel(thread_t *thp)
{
    unsigned long flags = kspinlock_lock_irqsave(&prio_defer_slock);
    unsigned cpu = thp->prio_defer_cpu;
    thread_t **pp;

    /* No unlocked early return: a settle pops the server (clearing the link)
     * and pins it inside this lock, and a cancel that read the cleared link
     * without the lock could reach the pin wait before the pin was taken.
     */
    if (cpu) {
        for (pp = &prio_defer_head[cpu - 1]; *pp;
             pp = &(*pp)->prio_defer_next) {
            if (*pp == thp) {
                *pp = thp->prio_defer_next;
                break;
            }
        }
        thp->prio_defer_next = NULL;
        thp->prio_defer_cpu = 0;
    }
    kspinlock_unlock_irqrestore(&prio_defer_slock, flags);
}
