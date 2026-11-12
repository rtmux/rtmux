/* SPDX-License-Identifier: Apache-2.0 */
#include <core/globals.h>
#include <stdatomic.h>
extern void aarch64_idle_enter(unsigned, uintptr_t) __attribute__((noreturn));
extern void aarch64_restore(CPU_REGISTERS *, uintptr_t)
    __attribute__((noreturn));
extern _Atomic int timer_drain_busy;
void __attribute__((noreturn)) kernel_exit(void)
{
    thread_t *thp;
    unsigned aflags;
    unsigned loop_count = 0;

    for (;;) {
        /* Selection and the transition to idle must stay atomic with respect
         * to this CPU's timer. Kernel work below explicitly enables IRQs. */
        interrupt_disable();
        unsigned cpu = get_cpunum();
        thp = get_active_thread(cpu);
        if (!thp) {
            in_kcall_dec();
            aarch64_idle_enter(cpu, ksys_stack[cpu]);
            __builtin_unreachable();
        }

        if (thp->state == STATE_DEAD || !thp->process) {
            rcu_sched_quiescent();
            set_active_thread(cpu, NULL);
            continue;
        }

        if (++loop_count > 100) {
            loop_count = 0;
        }

        if (!thp) {
            in_kcall_dec();
            cpu_idle(cpu);
            __builtin_unreachable();
        }

        interrupt_enable();

        int expected = 0;
        if (timer_ops_delayed &&
            atomic_compare_exchange_strong_explicit(
                &timer_drain_busy, &expected, 1, memory_order_acq_rel,
                memory_order_acquire)) {
            timer_pending();
            atomic_store_explicit(&timer_drain_busy, 0, memory_order_release);
            if (get_active_thread(get_cpunum()) != thp)
                continue;
        }

        interrupt_disable();
        uint8_t pend = pcpu_array[get_cpunum()].pc_pending_preempt_prio;
        pcpu_array[get_cpunum()].pc_pending_preempt_prio = 0;
        interrupt_enable();
        if (pend > thp->priority) {
            thread_t *old_act = thp;
            resched();
            if (get_active_thread(get_cpunum()) != old_act)
                continue;
        }

        if (thp->process && thp->process->pid == ROOTTASK_PID &&
            (thp->reg.pstate & 15) == 0) {
            (void) thread_set_as_prp(thp, thp->process);
        }

        if (thp->as_prp && pcpu_array[get_cpunum()].pc_as_prp != thp->as_prp) {
            vm_ops->as(thp->as_prp, &pcpu_array[get_cpunum()].pc_as_prp);
        }

        else if (!thp->as_prp && roottask_prp && roottask_prp->memory) {
            process_t *cur = pcpu_array[get_cpunum()].pc_as_prp;
            if (cur && cur != roottask_prp &&
                (cur->flags & RTMUX_FLG_PROC_TERMING)) {
                vm_ops->as(roottask_prp, &pcpu_array[get_cpunum()].pc_as_prp);
            }
        }

        if (thp->process &&
            pcpu_array[get_cpunum()].pc_active_prp != thp->process) {
            pcpu_array[get_cpunum()].pc_active_prp = thp->process;
            if (cpupageptr[get_cpunum()])
                cpupageptr[get_cpunum()]->pls = thp->process->pls;
        }

        if (thp->flags & RTMUX_FLG_THR_SPECRET_MASK) {
            specialret(thp);
            continue;
        }

        interrupt_disable();

        aflags = atomic_exchange_explicit(&thp->async_flags, 0,
                                          memory_order_acquire);
        if (aflags) {
            interrupt_enable();
            if (aflags & RTMUX_ATF_CANCEL) {
                atomic_or(&thp->flags, RTMUX_FLG_THR_CANCELSELF);

                aflags &= ~(unsigned) RTMUX_ATF_CANCEL;
                if (aflags)
                    atomic_fetch_or_explicit(&thp->async_flags, aflags,
                                             memory_order_release);
                continue;
            }
            if (aflags & (RTMUX_ATF_TIMESLICE | RTMUX_ATF_SMP_RESCHED)) {
                thread_t *old_act = thp;
                resched();

                if (get_active_thread(cpu) != old_act)
                    continue;
            }

            if (aflags &
                ~(unsigned) (RTMUX_ATF_TIMESLICE | RTMUX_ATF_SMP_RESCHED))
                continue;
        }

        /* resched() above may have restored enabled IRQs. Do not publish
         * in_kcall == 0 until interrupts are masked through ERET. */
        interrupt_disable();
        break;
    }

    thp->reg.tls = (uintptr_t) thp->un.lcl.tls;
    if (cpupageptr[get_cpunum()])
        cpupageptr[get_cpunum()]->tls = thp->un.lcl.tls;
    if ((thp->reg.pstate & 15) == 0)
        rcu_sched_quiescent();
    in_kcall_dec();
    aarch64_restore(&thp->reg, ksys_stack[get_cpunum()]);
}
