/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <core/globals.h>
#include <core/call-table.h>

ksys_call_t *ksys_call_table[] = {MK_KSYSTABLE(ksys)};

const int ksys_call_entry_num = NUM_ELTS(ksys_call_table);

int kdecl ksys_bad(thread_t *act, struct kcall_args_null *kap)
{
    (void) act;
    (void) kap;
    return ENOSYS;
}

/* Read a cpupage / syspage value (SysCpupageGet kcall, slot 7).
 *
 * Pure read of static globals (privateptr, cpupageptr[]) and per-CPU cpupage
 * entries. No shared mutable kernel state; the preempt fence around the
 * SETKSTATUS write is sufficient. Parameters:
 *   act : the calling (active) thread
 *   kap : kcall args; kap->index selects the value
 * Return value: ENOERROR on success (value in KSTATUS); EINVAL on bad index.
 */
int kdecl ksys_sys_cpupage_get(thread_t *act,
                               struct kcall_args_sys_cpupage_get *kap)
{
    intptr_t status;

    if (kap->index == CPUPAGE_ADDR) {
        status = (intptr_t) privateptr->user_cpupageptr;
    } else if (kap->index == CPUPAGE_SYSPAGE) {
        status = (intptr_t) privateptr->user_syspageptr;
    } else if (kap->index >= 0 && kap->index < CPUPAGE_MAX) {
        intptr_t *cpupage = (intptr_t *) cpupageptr[get_cpunum()];

        status = cpupage[kap->index];
    } else {
        return EINVAL;
    }
    SETKSTATUS(act, status);
    return ENOERROR;
}

/* Set the process-local-storage pointer (SysCpupageSet kcall, slot 8).
 *
 * Mutates act->process->pls and clears pcpu_array[get_cpunum()].pc_active_prp
 * so the next kernel_exit reloads PLS. Parameters:
 *   act : the calling (active) thread
 *   kap : kcall args; kap->index must be CPUPAGE_PLS, kap->value the PLS
 * Return value: EOK on success; EINVAL if kap->index is not CPUPAGE_PLS.
 *
 * Per-process scalar fields:
 *   - act->process->pls — single-pointer atomic store; multiple
 *     threads in the same process racing to set their own PLS is
 *     last-writer-wins by design (POSIX TLS semantics).
 *   - act->process->debugger — read-only here; lifetime managed
 *     by the debug subsystem.
 *
 * Per-CPU:
 *   - pcpu_array[cpu].pc_active_prp — single-writer-by-CPU.
 *
 * No cross-CPU contention; the in-kernel-call preemption fence suffices.
 */
int kdecl ksys_sys_cpupage_set(thread_t *act,
                               struct kcall_args_sys_cpupage_set *kap)
{
    process_t *prp;

    if (kap->index != CPUPAGE_PLS) {
        return EINVAL;
    }

    act->process->pls = (void *) kap->value;

    /* Force a reload of PLS */
    prp = pcpu_array[get_cpunum()].pc_active_prp;
    if (prp->debugger && debug_detach_brkpts) {
        debug_detach_brkpts(prp->debugger);
    }

    /* RUSH3: If we instead assign "kernel_threads_prp", we can probably remove
     * RUSH3: NULL pointer tests in the entry.S files when they check RUSH3: to
     * see if they have to remove breakpoints from the old RUSH3: process in the
     * ksys_exit code.
     */
    pcpu_array[get_cpunum()].pc_active_prp = 0;

    return EOK;
}
