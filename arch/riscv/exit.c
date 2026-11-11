/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* RISC-V kernel_exit — exit kernel and restore active thread's context.
 *
 * Called from:
 *   - ksys_start (assembly) on boot to begin the idle thread
 *   - cpu_idle() (via idle_pick_thread) to dispatch a thread
 *   - __kcall_dispatch() when a kernel call causes a reschedule
 *   - Fault handlers (xfer/core.c, thread/lifecycle.c) resuming a thread
 *     after setting up its register context
 *
 * SMP protocol:
 *   kernel_exit is always entered with the kernel held (INKERNEL_NOW set
 *   for this CPU) — either from __kcall/syscall_priv (which acquired it),
 *   or from cpu_idle (which acquired it in idle_pick_thread).
 *
 *   The kernel is released (via release_kernel) just before restoring the
 *   thread's register context.  This keeps the kernel held for the entire
 * entry-handler-exit sequence.
 */

#include <core/globals.h>
#include <stdatomic.h>
#include "sbi.h" /* sbi_ecall_console_putc — kprintf-bypass diagnostic */
#include <cpu-inline.h>
#include <sys/storage.h>
#include <sbi.h>

/* Called from __kernel_restore (entry.S) when a0 is not a kernel pointer */
void __kernel_restore_bad_ptr(CPU_REGISTERS *regs)
{
    kprintf("FATAL: __kernel_restore bad ptr! a0=%p (expected kernel address)\n",
            (void *) regs);
    kprintf("  ra=%lx sp=%lx\n", (unsigned long) __builtin_return_address(0),
            (unsigned long) __builtin_frame_address(0));
    crash();
}

/* Called from __kernel_restore (entry.S) when saved PC is NULL */
void __kernel_restore_null_pc(CPU_REGISTERS *regs)
{
    unsigned cpu = get_cpunum();
    thread_t *thp = get_active_thread(cpu);
    kprintf("FATAL: __kernel_restore PC=0! cpu=%u thp=%p\n", cpu, (void *) thp);
    if (thp) {
        kprintf("  tid=%u pid=%d state=%u flags=0x%x syscall=%d\n", thp->tid,
                thp->process ? (int) thp->process->pid : -1, thp->state,
                thp->flags, thp->syscall);
        kprintf("  reg.ra=%lx reg.sp=%lx reg.pc=%lx reg.sstatus=%lx\n",
                (unsigned long) regs->gpr[RISCV_REG_RA],
                (unsigned long) regs->gpr[RISCV_REG_SP],
                (unsigned long) regs->pc, (unsigned long) regs->sstatus);
        kprintf("  reg.a0=%lx reg.a7=%lx\n",
                (unsigned long) regs->gpr[RISCV_REG_A0],
                (unsigned long) regs->gpr[RISCV_REG_A7]);
    }
    crash();
}

/* Called from __kernel_restore (entry.S) when the saved sstatus has bits in the
 * kernel-pointer range (39-62) set. A valid RV64 sstatus has UXL at bits 32-33
 * and optionally SD at bit 63 — the intermediate bits 34-62 are RES0. When
 * sstatus carries a stray kernel-virtual address (e.g. 0xffffffc08020b79c),
 * bits 39+ are set, which is impossible for a real sstatus. Crash here with the
 * corrupted value so the post-mortem points at the dispatch site (and at
 * whatever code overwrote sstatus) rather than at the misbehavior the bogus
 * mode bits would have caused.
 *
 * Parameters:
 *   regs : the register frame about to be restored (carries the
 *          corrupted sstatus)
 */
void __kernel_restore_bad_sstatus(CPU_REGISTERS *regs)
{
    unsigned cpu = get_cpunum();
    thread_t *thp = get_active_thread(cpu);

    kprintf("FATAL: __kernel_restore corrupted sstatus 0x%lx! cpu=%u thp=%p\n",
            (unsigned long) regs->sstatus, cpu, (void *) thp);
    if (thp) {
        kprintf("  tid=%u pid=%d state=%u flags=0x%x syscall=%d\n", thp->tid,
                thp->process ? (int) thp->process->pid : -1, thp->state,
                thp->flags, thp->syscall);
        kprintf("  reg.ra=%lx reg.sp=%lx reg.tp=%lx reg.pc=%lx\n",
                (unsigned long) regs->gpr[RISCV_REG_RA],
                (unsigned long) regs->gpr[RISCV_REG_SP],
                (unsigned long) regs->gpr[RISCV_REG_TP],
                (unsigned long) regs->pc);
    }
    crash();
}

/* Called from __kernel_restore (entry.S) when SPP=1 in saved sstatus (returning
 * to S-mode) but the saved sp is outside the kernel direct-map RAM VA range or
 * the saved tp is zero.
 *
 * This is the "S-mode dispatch with broken context" corruption class (e.g. sp=0
 * and tp=0 in the saved trap frame, with the dispatched thread proceeding to
 * fetch from a bogus VA). Catching the bad context BEFORE sret puts the
 * post-mortem on the dispatch site rather than on whichever kernel routine the
 * broken thread happens to land on.
 *
 * Parameters:
 *   regs : the register frame about to be restored
 */
void __kernel_restore_bad_smode_ctx(CPU_REGISTERS *regs)
{
    unsigned cpu = get_cpunum();
    thread_t *thp = get_active_thread(cpu);
    unsigned long sp = (unsigned long) regs->gpr[RISCV_REG_SP];
    unsigned long tp = (unsigned long) regs->gpr[RISCV_REG_TP];

    kprintf(
        "FATAL: __kernel_restore S-mode return with broken ctx! "
        "cpu=%u thp=%p sp=%lx tp=%lx\n",
        cpu, (void *) thp, sp, tp);
    if (thp) {
        kprintf("  tid=%u pid=%d state=%u flags=0x%x syscall=%d\n", thp->tid,
                thp->process ? (int) thp->process->pid : -1, thp->state,
                thp->flags, thp->syscall);
        kprintf("  reg.ra=%lx reg.gp=%lx reg.pc=%lx reg.sstatus=%lx\n",
                (unsigned long) regs->gpr[RISCV_REG_RA],
                (unsigned long) regs->gpr[RISCV_REG_GP],
                (unsigned long) regs->pc, (unsigned long) regs->sstatus);
        kprintf("  reg.s0=%lx reg.s1=%lx reg.a0=%lx reg.a7=%lx\n",
                (unsigned long) regs->gpr[RISCV_REG_FP],
                (unsigned long) regs->gpr[RISCV_REG_S1],
                (unsigned long) regs->gpr[RISCV_REG_A0],
                (unsigned long) regs->gpr[RISCV_REG_A7]);
        kprintf("  trap_aux.kernel_sp=%lx kernel_tp=%lx\n",
                (unsigned long) thp->trap_aux.kernel_sp,
                (unsigned long) thp->trap_aux.kernel_tp);
    }
    crash();
}

/* Called from __kernel_restore (entry.S) when SPP=0 in saved sstatus (returning
 * to U-mode) but the saved PC is a kernel-virtual address.
 *
 * This is the "kernel-PC on U-mode return" corruption class — sret would
 * dispatch a U-mode thread executing kernel addresses, and the resulting fault
 * would be misclassified as a U-mode page fault with the trapping PC sitting in
 * the kernel half of the address space. Crash here, BEFORE sret, so the
 * post-mortem points at the dispatched thread and the corrupted reg.pc — not at
 * whatever kernel routine the U-mode thread happens to land on.
 *
 * Parameters:
 *   regs : the register frame about to be restored
 */
void __kernel_restore_kernel_pc_on_u_return(CPU_REGISTERS *regs)
{
    unsigned cpu = get_cpunum();
    thread_t *thp = get_active_thread(cpu);
    kprintf(
        "FATAL: __kernel_restore U-mode return with kernel PC! "
        "cpu=%u thp=%p\n",
        cpu, (void *) thp);
    if (thp) {
        kprintf("  tid=%u pid=%d state=%u flags=0x%x syscall=%d\n", thp->tid,
                thp->process ? (int) thp->process->pid : -1, thp->state,
                thp->flags, thp->syscall);
        kprintf("  reg.ra=%lx reg.sp=%lx reg.tp=%lx\n",
                (unsigned long) regs->gpr[RISCV_REG_RA],
                (unsigned long) regs->gpr[RISCV_REG_SP],
                (unsigned long) regs->gpr[RISCV_REG_TP]);
        kprintf("  reg.pc=%lx reg.sstatus=%lx (SPP bit %d)\n",
                (unsigned long) regs->pc, (unsigned long) regs->sstatus,
                (int) ((regs->sstatus & SSTATUS_SPP) ? 1 : 0));
        kprintf("  reg.a0=%lx reg.a1=%lx reg.a2=%lx reg.a7=%lx\n",
                (unsigned long) regs->gpr[RISCV_REG_A0],
                (unsigned long) regs->gpr[RISCV_REG_A1],
                (unsigned long) regs->gpr[RISCV_REG_A2],
                (unsigned long) regs->gpr[RISCV_REG_A7]);
        kprintf("  trap_aux.kernel_sp=%lx kernel_tp=%lx\n",
                (unsigned long) thp->trap_aux.kernel_sp,
                (unsigned long) thp->trap_aux.kernel_tp);
    }
    crash();
}

/* num_processors is set in syspage_init(). */
#if CONFIG_PROCESSORS_MAX > 1
unsigned num_processors;
#endif

/* __kernel_restore — assembly function in entry.S Restores all GPRs from
 * CPU_REGISTERS and srets to the thread. Never returns.
 */
extern void __attribute__((noreturn)) __kernel_restore(CPU_REGISTERS *regs);

/* kernel_exit — exit the kernel and resume the active thread.
 *
 * This CPU is doing kernel work on entry (in_kcall incremented).
 *
 * Sequence:
 *   1. Drain deferred timer-pending work
 *   2. Handle address space switch
 *   3. Handle process switch (PLS update)
 *   4. Handle special return processing (specret / WAAA)
 *   5. Handle async thread flags (timeslice/resched)
 *   6. Set TLS pointers and trap_aux fields
 *   7. Decrement in_kcall
 *   8. Restore registers via __kernel_restore (never returns)
 */
void __attribute__((noreturn)) kernel_exit(void)
{
    thread_t *thp;
    unsigned aflags;
    unsigned loop_count = 0;

    for (;;) {
        unsigned cpu = get_cpunum();
        thp = get_active_thread(cpu);
        if (!thp) {
            /* No runnable thread (block() cleared actives[cpu]). Re-enter
             * cpu_idle, which will pick a thread from the dispatch queue. Reset
             * the stack to the per-CPU kernel stack top to prevent stack growth
             * from repeated kernel_exit → cpu_idle transitions. Set tp =
             * &pcpu_array[cpu] before tail-calling cpu_idle so get_cpunum()
             * reads pc_hart at offset PC_HART(tp).
             */
            __asm__ volatile("mv tp, %0" ::"r"(&pcpu_array[cpu]));
            in_kcall_dec();
            __asm__ volatile(
                "slli t0, %0, 3\n"
                "la   t1, ksys_stack\n"
                "add  t1, t1, t0\n"
                "ld   sp, 0(t1)\n" /* sp = ksys_stack[cpu] */
                "mv   a0, %0\n"
                "tail cpu_idle\n" ::"r"((unsigned long) cpu)
                : "memory");
            __builtin_unreachable();
        }

        /* pc_curthread may be a thread that a concurrent thread_destroy on
         * another hart tore down while it was still THIS CPU's current thread.
         * Once process is NULL, restoring it cannot be valid; once object_free
         * recycles the soul, state is zeroed to STATE_DEAD and reg.pc may be 0.
         * Abandon it and loop: the !thp branch above tail-calls cpu_idle, which
         * picks the next runnable thread. Do not key on reg.pc==0 — a
         * freshly-created WAAA thread has pc==0 until specialret() below sets
         * it up. Single-writer preserved: this CPU only ever writes its own
         * pc_curthread.
         */
        if (thp->state == STATE_DEAD || !thp->process) {
            rcu_sched_quiescent();
            set_active_thread(cpu, NULL);
            continue;
        }

        if (++loop_count > 100) {
            loop_count = 0;
        }

        /* If no thread is active (block() found nothing runnable), decrement
         * the in-kernel counter and return to the cpu_idle loop.
         */
        if (!thp) {
            in_kcall_dec();
            cpu_idle(cpu);
            __builtin_unreachable();
        }

        /* The "this CPU is doing kernel work" signal is in_kcall[], which the
         * trap-entry handler maintains; there is no separate in-kernel flag
         * word to update here.
         */
        interrupt_enable();

        /* Sigevents deliver inline from their producer, so there is no
         * pending-event queue to drain here. mark_running()'s active changes
         * are picked up at the top of the loop on the next iteration (continue
         * path below).
         */

        /* Drain deferred timer-pending work. timer_expiry in trap context
         * cannot safely call timer_pending() (which expects INKERNEL_NOW); it
         * sets timer_ops_delayed instead. Here we're in kernel extension
         * context with INKERNEL_NOW set, so the call is safe. timer_pending()
         * clears timer_ops_delayed at its top.
         *
         * CAS-claim ensures at most one hart enters timer_pending()
         * concurrently — without it, every idle hart races into the
         * clock_slock-protected queue walk and livelocks on the
         * interrupt_disable/release sequence inside timer_pending.
         */
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

        /* Consume this CPU's pending-preempt-prio sentinel, set when an inline
         * sigevent (a timer fire or a device IRQ during the trap we are
         * exiting) made a higher-priority thread runnable. If it outranks the
         * thread we were about to resume, reschedule so it actually dispatches:
         * without this a busy SCHED_FIFO thread, which carries no timeslice or
         * resched async flag of its own, resumes and strands the readied
         * higher-priority thread on the run queue until the next tick, so an
         * interrupt-driven wake fails to preempt it. Read and clear with
         * interrupts off so a nested IRQ cannot raise the sentinel between the
         * two and lose it to the clear; resched() then runs with interrupts
         * enabled, the same context the timeslice path below uses.
         */
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

        /* Roottask runs at low VA in its own pgdir; switching satp away from
         * that pgdir would lose roottask text entirely and the next U-mode
         * fetch would fault. So for U-mode returns of ROOTTASK threads, force
         * as_prp back to act's own process so satp stays at roottask. Kexts
         * that need to operate on a bound as must do the temporary satp switch
         * themselves (xfer paths already do this). process_bind on a
         * U-mode-roottask thread becomes a no-op for the satp dimension; the
         * as_prp field still carries the bind for vm_ops->FOO(prp, ...) callers
         * that read it as a target hint.
         */
        if (thp->process && thp->process->pid == ROOTTASK_PID &&
            !(thp->reg.sstatus & SSTATUS_SPP)) {
            (void) thread_set_as_prp(thp, thp->process);
        }

        /* Address space switch */
        if (thp->as_prp && pcpu_array[get_cpunum()].pc_as_prp != thp->as_prp) {
            vm_ops->as(thp->as_prp, &pcpu_array[get_cpunum()].pc_as_prp);
        }
        /* Autonomous dead-as drop.
         *
         * When we resume a thread with NO as of its own (the idle or a kernel
         * thread) the code above leaves SATP pointing at whatever process ran
         * here last. If that process is now being torn down, we'd keep its
         * dying pgdir loaded, forcing the shutdown path to chase us off it with
         * a cross-hart IPI. Instead, drop to the long-lived roottask as HERE,
         * in our own context, on the way out of every trap. The timer interrupt
         * guarantees this path runs on every hart regardless of IPI delivery,
         * so a dying as is relinquished within one tick and the teardown's "is
         * anyone still on the dying as?" check clears on its own. roottask's
         * pgdir is never freed during normal operation; the kernel is mapped
         * high-half in every as, so execution continues seamlessly.
         */
        else if (!thp->as_prp && roottask_prp && roottask_prp->memory) {
            process_t *cur = pcpu_array[get_cpunum()].pc_as_prp;
            if (cur && cur != roottask_prp &&
                (cur->flags & RTMUX_FLG_PROC_TERMING)) {
                vm_ops->as(roottask_prp, &pcpu_array[get_cpunum()].pc_as_prp);
            }
        }

        /* Process switch — update actives_prp and PLS.
         *
         * thp->process can be NULL if thread_destroy ran on this thread
         * mid-fault-delivery (the thread is on its way out but was still the
         * active for this CPU when we re-entered kernel_exit). Skip the update
         * in that case — actives_prp will be refreshed on the next dispatch
         * pick. Without this guard the deref of thp->process->pls below
         * NULL-deref's at offset 0x58.
         */
        if (thp->process &&
            pcpu_array[get_cpunum()].pc_active_prp != thp->process) {
            pcpu_array[get_cpunum()].pc_active_prp = thp->process;
            if (cpupageptr[get_cpunum()])
                cpupageptr[get_cpunum()]->pls = thp->process->pls;
        }

        /* Special return processing (signal delivery, WAAA, etc.) */
        if (thp->flags & RTMUX_FLG_THR_SPECRET_MASK) {
            specialret(thp);
            continue;
        }

        interrupt_disable();

        /* Atomically read and clear async flags */
        aflags = atomic_exchange_explicit(&thp->async_flags, 0,
                                          memory_order_acquire);
        if (aflags) {
            interrupt_enable();
            if (aflags & RTMUX_ATF_CANCEL) {
                /* A cancel posted by another hart. Turn it into the owner-side
                 * special return that redirects to the cancel stub. */
                atomic_or(&thp->flags, RTMUX_FLG_THR_CANCELSELF);
                /* Re-post the other bits we consumed (TIMESLICE/SMP_RESCHED
                 * ride the same IPI) so the next pass still reschedules. */
                aflags &= ~(unsigned) RTMUX_ATF_CANCEL;
                if (aflags)
                    atomic_fetch_or_explicit(&thp->async_flags, aflags,
                                             memory_order_release);
                continue;
            }
            if (aflags & (RTMUX_ATF_TIMESLICE | RTMUX_ATF_SMP_RESCHED)) {
                thread_t *old_act = thp;
                resched();
                /* Only loop if actives[] actually changed — otherwise the timer
                 * tick just re-arms the same flag endlessly
                 */
                if (get_active_thread(cpu) != old_act)
                    continue;
            }
            /* Other async flags (not timeslice): always recheck */
            if (aflags &
                ~(unsigned) (RTMUX_ATF_TIMESLICE | RTMUX_ATF_SMP_RESCHED))
                continue;
        }

        break;
    }

    /* FP ownership. User FP registers are loaded lazily: only a thread whose
     * context is still live in this hart's registers (it was the last to load
     * them here, and no other hart has loaded it since) may resume with FP
     * enabled. Any other thread resumes with FS=Off, so its first FP
     * instruction traps and fpu_first_use loads (or zeroes) its context. This
     * is what keeps one thread's f0-f31 and fcsr from leaking into another.
     */
    if (!(thp->reg.sstatus & SSTATUS_SPP)) {
        unsigned cpu = get_cpunum();

        if (!(thp->fpudata && pcpu_array[cpu].pc_active_fpu == thp &&
              FPUDATA_CPU(thp->fpudata) == cpu))
            thp->reg.sstatus &= ~(uint64_t) SSTATUS_FS;
    }

    /* Set TLS on cpupage for user-visible access */
    if (cpupageptr[get_cpunum()])
        cpupageptr[get_cpunum()]->tls = thp->un.lcl.tls;

    /* Set tp in the saved register context. __kernel_restore will load this
     * into the hardware tp on sret.
     *
     *   S-mode thread: thp->reg.gpr[TP] = &pcpu_array[cpu].
     *     __kernel_restore loads tp from here, so the thread runs in
     *     kernel mode with tp pointing at the current hart's pcpu
     *     — get_cpunum() and get_pcpu() work directly.
     *
     *   U-mode thread: thp->reg.gpr[TP] = thp->un.lcl.tls.
     *     __kernel_restore loads tp from here on sret to U-mode; the
     *     user's TLS pointer is restored.  The kernel's pcpu pointer
     *     for the next U->S trap comes from trap_aux.kernel_tp
     *     (stamped below).
     */
    if (!(thp->reg.sstatus & SSTATUS_SPP)) {
        thp->reg.gpr[RISCV_REG_TP] = (uint64_t) thp->un.lcl.tls;
    } else {
        unsigned cpu = get_cpunum();
        thp->reg.gpr[RISCV_REG_TP] = (uint64_t) &pcpu_array[cpu];
    }

    /* Populate trap_aux fields for the next U-mode trap entry. When this thread
     * traps back from U-mode, sscratch holds &thp->reg, and trap.S reads
     * kernel_sp/kernel_tp at fixed offsets past the register save area.
     * kernel_tp is the current hart's pcpu pointer, regardless of whether the
     * thread is U-mode or S-mode.
     */
    {
        unsigned cpu = get_cpunum();
        chk_kernel_sp((uintptr_t) ksys_stack[cpu],
                      "ksys_exit:trap_aux.kernel_sp");
        thp->trap_aux.kernel_sp = (uint64_t) ksys_stack[cpu];
        thp->trap_aux.kernel_tp = (uint64_t) &pcpu_array[cpu];
    }

    if (!(thp->reg.sstatus & SSTATUS_SPP))
        rcu_sched_quiescent();

    /* in_kcall_dec() closes out this CPU's "doing kernel work" stamp before we
     * sret to the dispatched thread, so the timer-trap routing sees the thread
     * as user-side from this point on.
     */
    in_kcall_dec();

    /* Check: S-mode thread must not have user-space tp */
    if ((thp->reg.sstatus & SSTATUS_SPP) &&
        thp->reg.gpr[RISCV_REG_TP] < 0xffffffc000000000UL) {
        sbi_ecall_console_puts(
            "FATAL ksys_exit: S-mode thread with user tp\r\n");
        crash();
    }

    /* Restore all registers and sret to the thread — never returns.
     * __kernel_restore sets sscratch = &thp->reg if returning to U-mode.
     */
    __kernel_restore(&thp->reg);
}
