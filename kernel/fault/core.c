/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Fault handling routines.
 *
 * RTMux has no signals — a fault either notifies an attached debugger or kills
 * the faulting thread directly. These are user faults only: a fault taken in
 * the kernel never comes here. strap_handler_c (arch/riscv/trap-handler.c)
 * reports it into the per-hart crash buffer and joins crash_rendezvous()
 * (kernel/printk/kprintf.c), which prints every hart's report and halts; a
 * failed kernel check (crash(), CRASHCHECK) takes the same rendezvous through
 * crash_halt(). The debug-inject crash ops exercise both ends of it.
 */

#include <core/globals.h>


/* deliver_fault — handle a CPU fault on a user thread.
 *
 * Without signals, a fault that reaches this point (i.e. no debugger caught it)
 * is fatal: we mark the thread for destruction and record the fault info in the
 * process siginfo (used by waitpid exit status).
 */
void rdecl deliver_fault(thread_t *thp, siginfo_t *info)
{
    process_t *prp = thp->process;

    /* Any process can fault on purpose, as often as it can respawn. */
    KPRINTF_RATELIMITED(
        "user fault: pid=%d tid=%d signo=%d code=%d fltno=%d addr=%p ip=%p\n",
        prp ? prp->pid : -1, thp->tid, info->si_signo, info->si_code,
        info->si_fltno, info->si_addr, info->si_fltip);

    /* thp->process is cleared only when a dead thread's soul is recycled
     * (thread_recycle_reset), so a thread with no process is already destroyed:
     * there is nothing left to kill, and marking it KILLSELF would only move
     * the NULL dereference into thread_destroy. Every caller today reads
     * thp->process first, so this is defensive, but the code below locks
     * prp->vec_slock and walks prp->threads and must never see prp == NULL.
     */
    if (!prp)
        return;

    prp->fault_siginfo = *info;

    /* Kill all threads in the process. thread_destroyall marks every thread
     * with DETACHED | KILLSELF. We must also force-unblock non-active threads
     * (e.g. system thread blocked in msg_receive_notification) so their
     * KILLSELF gets processed.
     */
    kspinlock_lock(&prp->vec_slock);
    thread_destroyall(thp);
    kspinlock_unlock(&prp->vec_slock);

    {
        int tid;
        thread_t *t;

        /* vec_slock guard: a concurrent vector_add grows prp->threads — it
         * _scalloc's a new buffer, swaps the pointer and _sfree's the old one,
         * all under prp->vec_slock — so an unlocked VECP here could read the
         * freed/garbage buffer and fault, double-faulting out of fault
         * delivery.
         *
         * Read each slot (and nentries) UNDER vec_slock so we always see the
         * current buffer. We must NOT hold vec_slock across thread_destroy —
         * thread_destroy takes it itself to vector_rem (lifecycle.c). "t" stays
         * valid after the lock drops because a reaper frees a thread only after
         * an RCU-sched grace period, and this loop does not block before
         * thread_destroy.
         */
        for (tid = 0;; ++tid) {
            int got;
            kspinlock_lock(&prp->vec_slock);
            if (tid >= prp->threads.nentries) {
                kspinlock_unlock(&prp->vec_slock);
                break;
            }

            /* VECP2 so an unjoined zombie, whose slot is flagged invalid, is
             * reaped too; thread_destroyall detached it.
             */
            got = ((t = VECP2(t, &prp->threads, tid)) && t != thp);
            kspinlock_unlock(&prp->vec_slock);
            if (got)
                thread_destroy(t);
        }
    }
}


/* usr_fault — a thread faulted in user space.
 *
 * code_signo = signo | (si_code << 8) | (fault_num << 16)
 *
 * If a debugger is attached, it gets first crack. Otherwise the fault is fatal:
 * record info and kill the thread.
 */
void rdecl usr_fault(int code_signo, thread_t *thp, uintptr_t addr)
{
    process_t *prp;
    siginfo_t info;

    /* usr_fault is called from trap_handler with thp == act
     * (single-writer-by-CPU); the caller already holds in_kcall[cpu]++ and a
     * function-level preempt fence. The thp->flags update is atomic_clr for
     * cross-CPU safety against force_ready KILLSELF set / exec KCALL_ERR_SET
     * set on the same flags word.
     */
    if (thp->flags & RTMUX_FLG_THR_V86) {
        atomic_clr((volatile unsigned *) &thp->flags, RTMUX_FLG_THR_V86);
        kcall_err(thp, EFAULT);
        return;
    }

    prp = thp->process;

    memset(&info, 0, sizeof info);
    info.si_signo = SIGCODE_SIGNO(code_signo);
    info.si_code = SIGCODE_CODE(code_signo);
    info.si_fltno = SIGCODE_FAULT(code_signo);
    info.si_bdslot = (code_signo & SIGCODE_BDSLOT) ? 1 : 0;
    info.si_addr = (void *) addr;
    info.si_fltip = (void *) KIP(thp);

    /* No si_status: it shares storage with si_addr, so setting it made every
     * fault report the signal number as its address. The wait status is derived
     * from si_signo (proc_wait_check).
     */

    /* Faults should always be faults */
    if (info.si_code <= SI_USER) {
        crash();
    }

    /* Check if being debugged */
    if (prp && prp->debugger) {
        /* If debugger stops the process, don't deliver the fault */
        if (debug_thread_fault(thp, &info)) {
            return;
        }
    }

    deliver_fault(thp, &info);
}
