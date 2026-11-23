/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Dispatch-owner tripwire (CONFIG_DEBUG_DISPATCH_OWNER).
 *
 * A thread is current on at most one hart. Two harts that both resume it share
 * its saved registers, and since kernel_exit stamps the thread's trap_aux with
 * the hart's kernel stack and pcpu pointer, the first hart's next trap then
 * runs on the second hart's stack: the failure shows up much later, as a stack
 * or register corruption with no trace of the dispatch that caused it.
 *
 * Every pc_curthread store goes through set_active_thread on the hart that owns
 * the slot. Before the store the hart claims the incoming thread with a
 * compare-and-swap of dbg_owner.oncpu from 0 to 1 + its number; after the store
 * it gives the outgoing one back the same way. A hart that makes a thread
 * current while another still has it fails the claim and crashes on the spot,
 * naming both harts, the thread's wait state, and its last dispatch and last
 * wakeup with their call sites; with CONFIG_DEBUG_INJECT it adds the thread's
 * lifecycle trace.
 *
 * ready_default also reports a thread queued by another hart while it is still
 * current there, rate limited and without stopping: the pickers hold such a
 * thread back until its hart switches away (thread_current_elsewhere), so it is
 * a late dispatch, not a double one, but it means that a waker beat the switch
 * by more than ready() waits for.
 *
 * Only real threads carry the fields (a vthread soul is shorter), and a hart's
 * idle thread is current only on that hart, so neither needs an exemption
 * beyond the type test.
 */

#include <core/globals.h>

#ifdef CONFIG_DEBUG_DISPATCH_OWNER

/* The caller of mark_running on each hart, recorded just before its
 * set_active_thread, so the report names the dispatch path and not mark_running
 * itself. Owner hart only.
 */
static void *run_site[CONFIG_PROCESSORS_MAX];

static inline bool owner_tracked(const thread_t *thp)
{
    return thp && TYPE_MASK(thp->type) == TYPE_THREAD;
}

static void owner_hart(const char *what, unsigned cpu)
{
    const struct pcpu *pc = &pcpu_array[cpu];

    kprintf("  %s cpu %u: curthread %p syscall %d in_kcall %u\n", what, cpu,
            (const void *) pc->pc_curthread, pc->pc_syscall, in_kcall[cpu]);
}

static void __attribute__((noreturn)) owner_fire(thread_t *thp,
                                                 unsigned cpu,
                                                 unsigned other,
                                                 void *ra)
{
    kprintf_crash_mode_on();
    if (other == ~0U)
        kprintf(
            "dispatch-owner: thread %d.%d (%p) taken off a run queue on "
            "cpu %u by %p in state %u: it was queued twice\n",
            thp->process ? (int) thp->process->pid : -1, (int) thp->tid,
            (void *) thp, cpu, ra, (unsigned) thp->state);
    else
        kprintf(
            "dispatch-owner: thread %d.%d (%p) made current on cpu %u by "
            "%p while current on cpu %u\n",
            thp->process ? (int) thp->process->pid : -1, (int) thp->tid,
            (void *) thp, cpu, ra, other);
    kprintf(
        "  state %u flags %#x itf %#x blocked_on %p runcpu %u syscall %d "
        "prio %u\n",
        (unsigned) thp->state, (unsigned) thp->flags,
        (unsigned) thp->internal_flags, thp->blocked_on, (unsigned) thp->runcpu,
        (int) thp->syscall, (unsigned) thp->priority);
    kprintf("  last dispatch: cpu %u by %p at t=%lu\n",
            (unsigned) thp->dbg_owner.run_cpu, thp->dbg_owner.run_ra,
            (unsigned long) thp->dbg_owner.run_t);
    for (unsigned k = 0; k < 2; k++)
        kprintf("  wakeup -%u: cpu %u by %p from state %u at t=%lu\n", k,
                (unsigned) thp->dbg_owner.wake[k].cpu,
                thp->dbg_owner.wake[k].ra,
                (unsigned) thp->dbg_owner.wake[k].state,
                (unsigned long) thp->dbg_owner.wake[k].t);
    owner_hart("this", cpu);
    if (other < CONFIG_PROCESSORS_MAX)
        owner_hart("owner", other);
    kprintf("  now t=%lu\n", (unsigned long) r_time());
    thr_trace_dump(thp);
    crash_halt("dispatch-owner: thread current on two harts");
}

void dispatch_owner_run_site(void *ra)
{
    run_site[get_cpunum()] = ra;
}

/* set_active_thread(cpu, new), on hart cpu, before the store. */
void dispatch_owner_claim(unsigned cpu, thread_t *old, thread_t *new, void *ra)
{
    void *site = run_site[cpu];

    run_site[cpu] = NULL;
    if (old == new || !owner_tracked(new))
        return;
    if (site)
        ra = site;

    uint32_t seen = 0;

    /* A hart that has stored its next thread but not yet given new back is no
     * longer running it, only about to drop the claim: wait for that.
     */
    while (!__atomic_compare_exchange_n(&new->dbg_owner.oncpu, &seen, cpu + 1,
                                        false, __ATOMIC_ACQ_REL,
                                        __ATOMIC_ACQUIRE)) {
        if (seen - 1 >= CONFIG_PROCESSORS_MAX ||
            __atomic_load_n(&pcpu_array[seen - 1].pc_curthread,
                            __ATOMIC_ACQUIRE) == new)
            owner_fire(new, cpu, seen - 1, ra);
        seen = 0;
    }
    new->dbg_owner.run_cpu = (uint8_t) cpu;
    new->dbg_owner.run_ra = ra;
    new->dbg_owner.run_t = r_time();
}

/* set_active_thread(cpu, new), on hart cpu, after the store. */
void dispatch_owner_release(unsigned cpu, thread_t *old, thread_t *new)
{
    uint32_t mine = cpu + 1;

    if (old == new || !owner_tracked(old))
        return;

    /* Fails only for a soul that was freed and reused while still this hart's
     * current thread; its new owner's claim is not ours to drop.
     */
    (void) __atomic_compare_exchange_n(&old->dbg_owner.oncpu, &mine, 0, false,
                                       __ATOMIC_RELEASE, __ATOMIC_RELAXED);
}

/* A picker is taking thp off a run queue. Only a READY thread is queued: any
 * other state means a second wakeup queued it while its first had it run, and
 * this entry is stale.
 */
void dispatch_owner_pick(thread_t *thp, void *ra)
{
    if (owner_tracked(thp) && thp->state != STATE_READY)
        owner_fire(thp, get_cpunum(), ~0U, ra);
}

/* ready_default, about to queue thp. */
void dispatch_owner_ready(thread_t *thp, void *ra)
{
    uint32_t on;

    if (!owner_tracked(thp))
        return;
    thp->dbg_owner.wake[1] = thp->dbg_owner.wake[0];
    thp->dbg_owner.wake[0].cpu = (uint8_t) get_cpunum();
    thp->dbg_owner.wake[0].ra = ra;
    thp->dbg_owner.wake[0].state = thp->state;
    thp->dbg_owner.wake[0].t = r_time();
    on = __atomic_load_n(&thp->dbg_owner.oncpu, __ATOMIC_ACQUIRE);
    if (on && on - 1 != get_cpunum())
        KPRINTF_RATELIMITED(
            "dispatch-owner: thread %d.%d queued on cpu %u by "
            "%p while current on cpu %u (state %u)\n",
            thp->process ? (int) thp->process->pid : -1, (int) thp->tid,
            get_cpunum(), ra, on - 1, (unsigned) thp->state);
}

#endif /* CONFIG_DEBUG_DISPATCH_OWNER */
