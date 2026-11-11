/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <core/globals.h>


/* Bind (or query) the calling thread's active address space (kernel extension
 * body).
 *
 * With pid >= 0 the calling thread's as_prp is set to the target process; with
 * pid < 0 the current as_prp is returned instead. Foreign address-space
 * bindings are retained through a process pin owned by act->as_prp. Parameters:
 *   data : pointer to a pid_t (target pid, or negative to query)
 */
void kext_process_bind(void *data)
{
    process_t *prp;
    pid_t pid = *(pid_t *) data;
    thread_t *act = get_active_thread(get_cpunum());
    int status;

    /* Verify the target process and thread exists. */
    if (pid <= 0) {
        prp = NULL;
    } else if (!(prp = lookup_pid_rcu(pid))) {
        rcu_read_unlock(&process_rcu);
        kcall_err(act, ESRCH);
        return;
    } else if (prp->flags & RTMUX_FLG_PROC_TERMING) {
        rcu_read_unlock(&process_rcu);
        kcall_err(act, ESRCH);
        return;
    }

#ifndef NDEBUG
    if (prp) {
        /* Thread must not already be bound to a foreign as. A thread is
         * "unbound" when its current as is its own native process: ROOTTASK_PID
         * threads have as_prp == act->process (their own roottask process_t).
         */
        if (act->as_prp && act->as_prp != act->process)
            crash();
        if (prp->pid != pid)
            crash();
    }
    if (act != get_active_thread(get_cpunum())) {
        crash();
    }

    {
        if (act->process->pid != ROOTTASK_PID) {
            crash();
        }
    }
#endif

    /* If pid >= 0, we're setting it, otherwise querying */
    if (pid >= 0) {
        status = thread_set_as_prp(act, prp);
        if (prp)
            rcu_read_unlock(&process_rcu);
        if (status != EOK) {
            kcall_err(act, status);
            return;
        }
    } else {
        prp = act->as_prp;
    }

    SETKSTATUS(act, prp ? prp->pid : 0);
}

/* Return the calling thread's process_t pointer (kernel extension body).
 *
 * Serves roottask's startup-time self-discovery. U-mode roottask has no other
 * way to obtain a valid "process_t *" to itself: kernel globals
 * (kernel_threads_prp etc.) are unmapped from its as, and PIDs are just
 * integers, not pointers. The kernel extension runs in the calling thread's
 * context, so act->process is the right answer; the pointer is returned to the
 * caller via SETKSTATUS. Parameters:
 *   unused : ignored
 */
void kext_self_prp(void *unused)
{
    (void) unused;
    thread_t *act = get_active_thread(get_cpunum());
    SETKSTATUS(act, act->process);
}

/* Return the calling process's address_t pointer (kernel extension body).
 *
 * Companion to kext_self_prp. Roottask calls this once at startup to populate
 * root_proc(ROOTTASK_PID)->adp with the same value the kernel recorded in its
 * own process_t.memory at boot (set by kernel/init/roottask.c before roottask
 * runs). Ordinary children cache adp in root_process_t via the process_create
 * kcall_args adp_out, but ROOTTASK_PID predates that path; hence this kernel
 * extension. Parameters:
 *   unused : ignored
 */
void kext_self_adp(void *unused)
{
    (void) unused;
    thread_t *act = get_active_thread(get_cpunum());
    SETKSTATUS(act, act->process->memory);
}
