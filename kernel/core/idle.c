/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <core/globals.h>
#include <cpu-inline.h> /* r_satp() — diagnostic */


/* Start the kernel threads and hand off to roottask.
 *
 * Normalizes per-CPU clock-cycle offsets, starts the clock, transiently binds
 * to ROOTTASK_PID so the I/O service framework accepts _IO_MMAP messages from
 * roottask, then launches roottask. Runs as the boot CPU's idle thread; if
 * ksys_start_roottask returns, control falls through to the WFI loop in idle().
 * Parameters:
 *   dummy : unused
 * Return value: always NULL.
 */
static void *start_kernel_threads(void *dummy)
{
#if CONFIG_PROCESSORS_MAX > 1
    int i;
    uint64_t min_cycle = ULLONG_MAX;

    {
        extern unsigned hart_ids[];
        for (i = 0; i < num_processors; i++) {
            unsigned h = hart_ids[i];
            if (pcpu_array[h].pc_clockcycles_offset < min_cycle)
                min_cycle = pcpu_array[h].pc_clockcycles_offset;
        }
        for (i = 0; i < num_processors; i++) {
            pcpu_array[hart_ids[i]].pc_clockcycles_offset -= min_cycle;
        }
    }
#endif
    clock_start(RTMUX_TICKSIZE_DEFAULT);

    /* Bind to ROOTTASK_PID so the I/O service framework recognizes _IO_MMAP
     * messages from roottask (pid 1). Then unbind.
     *
     * process_bind itself lives in roottask, not in the kernel. From kernel
     * context we hit the body directly: it's a kernel-internal function
     * reachable via the slot-2 dispatch table, but here we're already inside
     * the kernel, so call straight through.
     */
    {
        extern void kext_process_bind(void *data);
        pid_t pid = ROOTTASK_PID;
        kext_process_bind(&pid);
        pid = 0;
        kext_process_bind(&pid);
    }

    ksys_start_roottask();

    /* In the real system, start_kernel_threads runs in its own thread and
     * destroys itself here. In our single-thread bring-up, we must NOT destroy
     * the only thread. If ksys_start_roottask returns (e.g. roottask_main's
     * message_start failed), fall through to the WFI loop in idle().
     */
    return NULL;
}

/* kernel_thread_exit_fallback — default __exitfunc for threads whose TLS was
 * not set up with a user-space exit hook (typically __my_thread_exit from
 * libc).
 *
 * Idle threads are the canonical case: objects.c creates them with a zeroed
 * TLS, never calls pthread_create on their behalf, and consequently __exitfunc
 * stays NULL. Any child thread that later inherits __exitfunc from valid_thp
 * (see thread_specret in lifecycle.c) would inherit NULL too, and when such a
 * child's top-level entry function returns it dereferences ra=0, causing a
 * silent pc=0 fetch fault.
 *
 * Pointing the idle thread's __exitfunc at this routine turns that silent crash
 * into a diagnostic one: any thread reaching this path is a bug (entry function
 * shouldn't return without going through __my_thread_exit), so we log who got
 * here and halt.
 */
void __attribute__((noreturn)) kernel_thread_exit_fallback(void *arg)
{
    unsigned cpu = get_cpunum();
    thread_t *act = get_active_thread(cpu);

    kprintf(
        "FATAL: thread exited without __exitfunc set "
        "(arg=%p cpu=%u tid=%d pid=%d)\n",
        arg, cpu, act ? (int) act->tid : -1,
        (act && act->process) ? (int) act->process->pid : -1);
    crash();

    /* crash_halt is not annotated noreturn; loop forever to satisfy the
     * compiler's noreturn analysis.
     */
    for (;;)
        __asm__ volatile("wfi" ::: "memory");
}

struct idle_stack {
    uint64_t stack[(IDLE_STACK_NBYTES + sizeof(struct _thread_local_storage)) /
                   sizeof(uint64_t)];
};

/* idle_unlink_ready — unlink one READY thread from its dispatch queue, clearing
 * the priority bit when the level empties. Caller holds the dpp lock.
 *
 * Returns 1 if the thread was unlinked, 0 if it was no longer at the expected
 * queue position (lost a race).
 */
static int idle_unlink_ready(kdispatch_t *dpp, thread_t *thp)
{
    LINK3_HDR *queue = &DISPATCH_LST(dpp, thp->priority);

    if (!thp->link3_link.prev) {
        if (queue->head != &thp->link3_link)
            return 0;
        thp->link3_link.prev = &queue->head;
    }

    LINK3_REM(*queue, &thp->link3_link, thread_t);
    if (!DISPATCH_THP(dpp, thp->priority))
        DISPATCH_CLR(dpp, thp);
    return 1;
}

/* idle_pick_thread — find a READY thread on the dispatch queue for the given
 * CPU. Called from cpu_idle() in the full-scheduler path.
 * Returns NULL if nothing available.
 *
 * Locking: each per-CPU dispatch instance carries its own kspinlock, which
 * guards the kdispatch_t bitmap/lists and the thp->state field, matching
 * ready_default / block / unready / block_and_ready_default in dispatch.c. The
 * picked thread is marked STATE_RUNNING and runcpu=cpu under the lock so that
 * concurrent ready_default/resched_default on other CPUs cannot re-enqueue or
 * migrate it after we've pulled it from the queue.
 *
 * The lock window ends inside this function — the dispatch lock is a leaf
 * spinlock and cannot straddle a context switch. The per-CPU bookkeeping
 * (set_active_thread, actives_prp) stays in cpu_idle() after this returns;
 * those slots are single-writer (this CPU only) and need no lock. Preserve
 * the caller's IRQ mask across every unlock: enabling a timer after selecting
 * a thread but before cpu_idle installs it can overwrite the active slot and
 * strand a RUNNING thread outside every run queue.
 */
thread_t *idle_pick_thread(unsigned cpu)
{
    kdispatch_t *dpp;
    thread_t *thp;
    unsigned long irqstate;
    extern unsigned hart_ids[];

    /* Boot-once gates — set before APs come up, never cleared. */
    if (!smp_dispatch_enabled || !kernel_threads_prp)
        return NULL;

    /* Each per-CPU dispatch has its own kspinlock. Phase 1: lock LOCAL_DPP,
     * scan this CPU's queue. Phase 2: drop LOCAL_DPP, then for each remote hart
     * lock that
     *          hart's dpp, scan/steal, unlock.  Holding only one
     *          dpp lock at a time avoids ABBA across harts (lock
     *          allows multiple same-class locks but does not enforce
     *          order — so we don't risk overlap by construction).
     * Phase 3: orphan-scan fallback under LOCAL_DPP.
     */
    irqstate = kspinlock_lock_irqsave(&LOCAL_DPP->lock);
    dpp = pcpu_array[cpu].pc_dispatch;
    for (int pri = NUM_PRI - 1; pri >= 0; pri--) {
        if (!DISPATCH_ISSET(dpp, pri))
            continue;
        for (thp = DISPATCH_THP(dpp, pri); thp;
             thp = link3_to_thread(thp->link3_link.next)) {
            if (thp->state == STATE_READY &&
                (thp->runmask & (1u << cpu)) == 0) {
                if (!idle_unlink_ready(dpp, thp))
                    continue;
                thp->state = STATE_RUNNING;
                thp->runcpu = cpu;
                kspinlock_unlock_irqrestore(&LOCAL_DPP->lock, irqstate);
                return thp;
            }
        }
    }
    kspinlock_unlock_irqrestore(&LOCAL_DPP->lock, irqstate);

    /* Phase 2: try to steal from remote harts (each lock taken independently,
     * no nested holds).
     */
    for (int outer = 0; outer < (int) num_processors; outer++) {
        unsigned src_hart = hart_ids[outer];
        if (src_hart == cpu)
            continue;
        dpp = pcpu_array[src_hart].pc_dispatch;
        if (!dpp)
            continue;
        irqstate = kspinlock_lock_irqsave(&dpp->lock);
        for (int pri = NUM_PRI - 1; pri >= 0; pri--) {
            if (!DISPATCH_ISSET(dpp, pri))
                continue;
            for (thp = DISPATCH_THP(dpp, pri); thp;
                 thp = link3_to_thread(thp->link3_link.next)) {
                if (thp->state == STATE_READY &&
                    (thp->runmask & (1u << cpu)) == 0) {
                    if (!idle_unlink_ready(dpp, thp))
                        continue;
                    /* Migrate ownership to this CPU's instance. */
                    thp->dpp = pcpu_array[cpu].pc_dispatch;
                    thp->state = STATE_RUNNING;
                    thp->runcpu = cpu;
                    kspinlock_unlock_irqrestore(&dpp->lock, irqstate);
                    return thp;
                }
            }
        }
        kspinlock_unlock_irqrestore(&dpp->lock, irqstate);
    }

    /* Phase 3: orphan-scan fallback. */
    irqstate = kspinlock_lock_irqsave(&LOCAL_DPP->lock);
    for (int t = 0; t < kernel_threads_prp->threads.nentries; t++) {
        thp = vector_lookup(&kernel_threads_prp->threads, (unsigned) t);
        if (thp && thp->state == STATE_READY &&
            (thp->runmask & (1u << cpu)) == 0) {
            thp->dpp = pcpu_array[cpu].pc_dispatch;
            thp->state = STATE_RUNNING;
            thp->runcpu = cpu;
            kspinlock_unlock_irqrestore(&LOCAL_DPP->lock, irqstate);
            return thp;
        }
    }
    kspinlock_unlock_irqrestore(&LOCAL_DPP->lock, irqstate);
    return NULL;
}

/* Boot CPU idle-thread entry point.
 *
 * Marks this CPU alive, starts the kernel threads (and roottask), then enters
 * cpu_idle() to dispatch ready threads / WFI when idle.
 */
void idle(void)
{
    unsigned cpu;

    cpu = get_cpunum();
    pcpu_array[cpu].pc_alive = true;

    start_kernel_threads(NULL);

    /* idle() runs as the boot CPU's idle-thread entry function in S-mode. We're
     * already past the boot stamp/balance dance: setup.c bumped in_kcall,
     * ksys_start() tail-called kernel_exit() which decremented it to 0 before
     * __kernel_restore dispatched us. By the time idle()'s body runs, in_kcall
     * == 0 — same starting point as APs entering cpu_idle directly.
     *
     * message_start uses POOL_FLAG_RESERVE and returns; the boot thread enters
     * cpu_idle to dispatch pool threads.
     */
    cpu_idle(cpu);
}
