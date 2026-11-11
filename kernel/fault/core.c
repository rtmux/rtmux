/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* Fault handling routines.
 *
 * RTMux has no signals — a fault either notifies an attached debugger or kills
 * the faulting thread directly.
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

    kprintf(
        "user fault: pid=%d tid=%d signo=%d code=%d fltno=%d addr=%p ip=%p\n",
        prp ? prp->pid : -1, thp->tid, info->si_signo, info->si_code,
        info->si_fltno, info->si_addr, info->si_fltip);

    if (prp)
        prp->fault_siginfo = *info;

    /* Kill all threads in the process. thread_destroyall marks every thread
     * with DETACHED | KILLSELF. We must also force-unblock non-active threads
     * (e.g. system thread blocked in msg_receive_notification) so their
     * KILLSELF gets processed.
     */
    thread_destroyall(thp);

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
         * current buffer. We must NOT hold vec_slock across force_ready /
         * thread_destroy — thread_destroy takes it itself to vector_rem
         * (lifecycle.c). The thread_t soul "t" is pool memory and outlives the
         * vector realloc, so acting on it after dropping the lock is safe.
         */
        for (tid = 0;; ++tid) {
            int got;
            kspinlock_lock(&prp->vec_slock);
            if (tid >= prp->threads.nentries) {
                kspinlock_unlock(&prp->vec_slock);
                break;
            }
            got = (VECP(t, &prp->threads, tid) && t != thp);
            kspinlock_unlock(&prp->vec_slock);
            if (got) {
                force_ready(t, _FORCE_KILL_SELF);
                thread_destroy(t);
            }
        }
    }
}


/* kdebug_enter — enter the kernel debugger (if attached).
 *
 * Invokes the registered kdebug fault entry callback; if none can handle a
 * fatal/process fault, falls through to shutdown(). Parameters:
 *   prp     : faulting process (NULL is resolved to kernel_threads_prp)
 *   sigcode : fault/signal code
 *   reg     : register context at the fault
 * Return value: the (possibly modified) sigcode the caller should act on.
 */
unsigned kdebug_enter(process_t *prp, unsigned sigcode, CPU_REGISTERS *reg)
{
    struct kdebug_callback *funcs;
    unsigned (*func)(struct kdebug_entry *entry, unsigned sigcode, void *reg);

    if ((funcs = privateptr->debug_call) && (func = funcs->fault_entry)) {
        if (!prp && pcpu_array[get_cpunum()].pc_alive) {
            prp = kernel_threads_prp;
        }
        sigcode = func(prp ? &prp->kdebug : 0, sigcode, reg);
        return sigcode;
    }
    if (sigcode & (SIGCODE_FATAL | SIGCODE_PROC)) {
        /* when we *have* to have some sort of kernel debugger */
        shutdown(sigcode, reg);
        return 0;
    }
    /* when we can ignore the fault */
    return sigcode;
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

    info.si_signo = SIGCODE_SIGNO(code_signo);
    info.si_code = SIGCODE_CODE(code_signo);
    info.si_fltno = SIGCODE_FAULT(code_signo);
    info.si_bdslot = (code_signo & SIGCODE_BDSLOT) ? 1 : 0;
    info.si_addr = (void *) addr;
    info.si_fltip = (void *) KIP(thp);
    info.si_status = info.si_signo;

    /* Faults should always be faults */
    if (info.si_code <= SI_USER) {
        crash();
    }

    /* Check if being debugged */
    if (prp->debugger) {
        /* If debugger stops the process, don't deliver the fault */
        if (debug_thread_fault(thp, &info)) {
            return;
        }
    }

    deliver_fault(thp, &info);
}
