/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Locking discipline:
 *   Every kernel extension below is dispatched via syscall_priv, which acquires
 *   the kernel lock at entry.  The non-preemption markers in this file
 *   document where a fine-grained slock must be taken before mutating
 *   debugger / target-thread / target-process state once the kernel runs
 *   without the coarse lock.
 *
 *   Slocks needed (none of these exist yet):
 *     debug_slock          — covers prp->debugger pointer install/clear
 *                            (kext_debug_attach / kext_debug_detach).
 *                            Class candidate near LOCK_CLASS_PROCESS.
 *     dep->lock            — per-debug_t lock for dep->flags / dep->tid
 *                            (the RTMUX_DEBUG_* flag/tid mutations).
 *     thp->lock            — per-thread lock for thp->flags / thp->reg /
 *                            RTMUX_FLG_THR_FROZEN; also needed for the
 *                            cpu_greg_load() write into a target thread
 *                            that is not the calling thread.
 *     prp->vec_slock       — already exists; needed to cover the
 *                            vector_lookup of the target thread_t inside
 *                            the kernel extension (today it's open-coded above
 * the switch). process_rcu — already exists; needed around lookup_pid
 *                            (via lookup_pid_rcu) for prp lifetime.
 *
 *   Lock order (when the slocks land):
 *     process_rcu  →  prp->vec_slock  →  debug_slock  →  dep->lock
 *                                                     →  thp->lock
 *                                                     →  chp->lock
 */

#include <core/globals.h>
#include <private/perfregs.h>

struct kcall_args_debug_process {
    pid_t pid;
    int tid;
    union rtmux_debug_data *data;
    enum rtmux_debug_request request;
};

/* Service a debugger request against a process or thread (kernel extension
 * body).
 *
 * Looks up the target process (and, where applicable, thread) and dispatches on
 * kap->request to read or write process/thread info, register sets, debug
 * flags, breakpoints, and run/stop/freeze state. Reports EOK or an errno back
 * to the calling thread. Parameters:
 *   args : pointer to a struct kcall_args_debug_process (pid, tid, request,
 * data)
 */
void kext_debug_process(void *args)
{
    process_t *prp;
    thread_t *thp, *act = get_active_thread(get_cpunum());
    debug_t *dep;
    struct kcall_args_debug_process *kap = args;
    int status;
    int stopped;
    int prp_pinned = 0;
    int thp_pinned = 0;
    unsigned tid;

    if (!(prp = lookup_pid_rcu(kap->pid))) {
        rcu_read_unlock(&process_rcu);
        kcall_err(act, ESRCH);
        return;
    }
    status = process_pin(prp);
    rcu_read_unlock(&process_rcu);
    if (status != EOK) {
        kcall_err(act, status);
        return;
    }
    prp_pinned = 1;

    dep = prp->debugger;

    if (kap->request == RTMUX_DEBUG_PROCESS_INFO ||
        (prp->flags & RTMUX_FLG_PROC_TERMING)) {
        tid = 0;
    } else if (dep && kap->tid == 0) {
        tid = (unsigned) (dep->tid + 1);
    } else {
        tid = (unsigned) kap->tid;
    }

    thp = 0;
    if (tid > 0) {
        kspinlock_lock(&prp->vec_slock);
        thp = vector_search(&prp->threads, tid - 1,
                            (kap->request == RTMUX_DEBUG_THREAD_INFO ||
                             kap->request == RTMUX_DEBUG_STOP)
                                ? (unsigned *) &tid
                                : 0);
        if (thp)
            status = tthread_pin(thp);
        kspinlock_unlock(&prp->vec_slock);
        if (!thp) {
            status = ESRCH;
            goto out;
        }
        if (status != EOK)
            goto out;
        thp_pinned = 1;
    }

    stopped = 0;
    if (thp && ((thp->flags & RTMUX_FLG_THR_TO_BE_STOPPED) ||
                (thp->state != STATE_RUNNING && thp->state != STATE_READY))) {
        stopped = 1;
    }

    status = EINVAL;
    switch (kap->request) {
    case RTMUX_DEBUG_PROCESS_INFO: /* pid:na:debug_process_t */
        status = debug_process(prp, &kap->data->process);
        break;

    case RTMUX_DEBUG_THREAD_INFO: /* pid:tid:debug_thread_t */
        status = debug_thread(prp, thp, &kap->data->thread);
        break;

    case RTMUX_DEBUG_GET_GREG: /* pid:tid:debug_greg_t */
        if (thp) {
            memcpy(&kap->data->greg, &thp->reg, sizeof thp->reg);
            status = EOK;
        }
        break;

    case RTMUX_DEBUG_SET_GREG: /* pid:tid:debug_greg_t */
        if (stopped) {
            /* Preempt-disable marker — needs thp->lock on the stopped target
             * thread; cpu_greg_load writes the full saved-register block.
             */
            cpu_greg_load(thp, (CPU_REGISTERS *) &kap->data->greg);
            status = EOK;
        }
        break;

    case RTMUX_DEBUG_GET_FPREG: /* pid:tid:debug_fpreg_t */
        if (thp) {
            FPU_REGISTERS *fpudata = FPUDATA_PTR(thp->fpudata);
            unsigned cpu = FPUDATA_CPU(thp->fpudata);

            status = ENXIO;
            if (fpudata) {
                if (FPUDATA_INUSE(thp->fpudata) && cpu != get_cpunum()) {
                    /* In use on another CPU; send ipi, restart kernel call */
                    SENDIPI(cpu, IPI_CONTEXT_SAVE);
                    if (thp_pinned)
                        tthread_unpin(thp);
                    if (prp_pinned)
                        process_unpin(prp);
                    KCALL_RESTART(act);
                    return;
                }
                if (pcpu_array[thp->runcpu].pc_active_fpu == thp) {
                    if (get_cpunum() == thp->runcpu) {
                        cpu_force_fpu_save(thp);
                        pcpu_array[get_cpunum()].pc_active_fpu = NULL;
                    }
                    /* else: the area was saved at the thread's last trap */
                }
                memcpy(&kap->data->fpreg, fpudata, sizeof *fpudata);
                status = EOK;
            } else if (thp->un.lcl.tls && thp->un.lcl.tls->__fpuemu_data) {
                /* @@@ NEED TO FIND PROPER SIZE OF EMULATOR DATA */
                memcpy(&kap->data->fpreg, thp->un.lcl.tls->__fpuemu_data,
                       sizeof(*fpudata) + 256);
                status = EOK;
            }
        }
        break;

    case RTMUX_DEBUG_SET_FPREG: /* pid:tid:debug_fpreg_t */
        if (thp && stopped) {
            FPU_REGISTERS *fpudata = FPUDATA_PTR(thp->fpudata);
            unsigned cpu = FPUDATA_CPU(thp->fpudata);

            status = ENXIO;
            if (thp->fpudata) {
                if (FPUDATA_INUSE(thp->fpudata) && cpu != get_cpunum()) {
                    /* In use on another CPU; send ipi, restart kernel call */
                    SENDIPI(cpu, IPI_CONTEXT_SAVE);
                    if (thp_pinned)
                        tthread_unpin(thp);
                    if (prp_pinned)
                        process_unpin(prp);
                    KCALL_RESTART(act);
                    return;
                }
                if (pcpu_array[thp->runcpu].pc_active_fpu == thp) {
                    if (get_cpunum() == thp->runcpu) {
                        cpu_force_fpu_save(thp);
                        pcpu_array[get_cpunum()].pc_active_fpu = NULL;
                    } else {
                        /* Drop the remote hart's claim so the thread reloads
                         * the written state on its next FP use.
                         */
                        struct thread_entry *me = thp;

                        __atomic_compare_exchange_n(
                            &pcpu_array[thp->runcpu].pc_active_fpu, &me, NULL,
                            0, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED);
                    }
                }
                memcpy(fpudata, &kap->data->fpreg, sizeof *fpudata);
                status = EOK;
            } else if (thp->un.lcl.tls && thp->un.lcl.tls->__fpuemu_data) {
                /* @@@ NEED TO FIND PROPER SIZE OF EMULATOR DATA */
                memcpy(thp->un.lcl.tls->__fpuemu_data, &kap->data->fpreg,
                       sizeof(*fpudata) + 256);
                status = EOK;
            }
        }
        break;

    case RTMUX_DEBUG_STOP: /* pid:na:na */
        if (dep) {
            status = debug_stop(prp);
        }
        break;

    case RTMUX_DEBUG_RUN: /* pid:tid:debug_run_t */
        if (dep && stopped) {
            status = debug_run(prp, &kap->data->run);
        }
        break;

    case RTMUX_DEBUG_CURTHREAD: /* pid:tid:NULL */
        if (dep) {
            /* Preempt-disable marker — needs dep->lock for the dep->tid
             * read+write (returns previous tid, then installs the new one).
             */
            SETKSTATUS(act, dep->tid + 1);
            if (thp) {
                dep->tid = (int) thp->tid;
            }
            if (thp_pinned)
                tthread_unpin(thp);
            if (prp_pinned)
                process_unpin(prp);
            return;
        }
        break;

    case RTMUX_DEBUG_FREEZE: /* pid:tid:NULL */
        if (!thp) {
            status = EINVAL;
            break;
        }
        if (stopped) {
            /* Preempt-disable marker — needs thp->lock for the thp->flags RMW;
             * FROZEN bit is read by the dispatcher.
             */
            atomic_or(&thp->flags, RTMUX_FLG_THR_FROZEN);
        }
        break;

    case RTMUX_DEBUG_THAW: /* pid:tid:NULL */
        if (!thp) {
            status = EINVAL;
            break;
        }
        if (stopped) {
            /* Preempt-disable marker — needs thp->lock for the thp->flags RMW.
             */
            atomic_clr(&thp->flags, RTMUX_FLG_THR_FROZEN);
        }
        break;

    case RTMUX_DEBUG_BREAK: /* pid:na:debug_break_t */
        if (dep && stopped) {
            status = debug_break(prp, &kap->data->brk);
        }
        break;

    case RTMUX_DEBUG_GET_BREAKLIST: /* pid:na:debug_breaklist_t */
        status = debug_break_list(prp, &kap->data->brklist);
        break;

    case RTMUX_DEBUG_SET_FLAG: /* pid:na:uint32_t */
        if (dep && !(kap->data->flags & ~(uint32_t) _DEBUG_FLAG_MASK)) {
            /* Preempt-disable marker — needs dep->lock for the dep->flags RMW.
             */
            dep->flags |= kap->data->flags;
        }
        break;

    case RTMUX_DEBUG_CLEAR_FLAG: /* pid:na:uint32_t */
        if (dep && !(kap->data->flags & ~(uint32_t) _DEBUG_FLAG_MASK)) {
            /* Preempt-disable marker — needs dep->lock for the dep->flags RMW.
             */
            dep->flags &= ~kap->data->flags;
        }
        break;

    case RTMUX_DEBUG_GET_ALTREG: /* pid:tid:debug_altreg_t */
        if (thp) {
            status = cpu_debug_get_altregs(thp, &kap->data->altreg);
        }
        break;

    case RTMUX_DEBUG_SET_ALTREG: /* pid:tid:debug_altreg_t */
        if (thp) {
            status = cpu_debug_set_altregs(thp, &kap->data->altreg);
        }
        break;

    case RTMUX_DEBUG_GET_PERFREG:
        if (thp) {
            status = cpu_debug_get_perfregs(thp, &kap->data->perfreg);
        }
        break;

    case RTMUX_DEBUG_SET_PERFREG:
        if (thp && !stopped)
            status = EINVAL;
        else {
            if ((kap->data->flags & ~PERFREGS_ENABLED_FLAG) ==
                cpu_perfreg_id()) {
                status = cpu_debug_set_perfregs(thp, &kap->data->perfreg);
            } else
                status = ENODEV;
        }
        break;
    }

out:
    if (thp_pinned)
        tthread_unpin(thp);
    if (prp_pinned)
        process_unpin(prp);
    if (status != EOK) {
        kcall_err(act, status);
    } else {
        /* Preempt-disable marker — SETKSTATUS writes act->status on the calling
         * thread; pure local write, but the BKL spans the entire dispatch
         * entry/exit so this stays as a marker. In a BKL-less world this is
         * lock-free (act IS this CPU; no cross-CPU writer of act->status).
         */
        SETKSTATUS(act, 0);
    }
}

/* debug_process_call wrapper: servers/roottask/proc/kext-wrappers.c */

struct kcall_args_debug_attach {
    pid_t pid;
    unsigned flags;
};

/* Attach a debugger to a process (kernel extension body).
 *
 * Looks up the target process, rejects ROOTTASK_PID and already-debugged
 * processes, then allocates a debug_t and installs it on prp->debugger with the
 * requested flags. Fails with ESRCH / EINVAL / EBUSY / ENOMEM as appropriate.
 * Parameters:
 *   args : pointer to a struct kcall_args_debug_attach (pid + flags)
 */
void kext_debug_attach(void *args)
{
    struct kcall_args_debug_attach *kap = args;
    process_t *prp;
    thread_t *act = get_active_thread(get_cpunum());
    debug_t *dep;

    if (!(prp = lookup_pid_rcu(kap->pid))) {
        rcu_read_unlock(&process_rcu);
        kcall_err(act, ESRCH);
        return;
    }
    if (process_pin(prp) != EOK) {
        rcu_read_unlock(&process_rcu);
        kcall_err(act, ESRCH);
        return;
    }
    rcu_read_unlock(&process_rcu);

    if (prp->pid == ROOTTASK_PID) {
        process_unpin(prp);
        kcall_err(act, EINVAL);
        return;
    }

    if (prp->debugger) {
        process_unpin(prp);
        kcall_err(act, EBUSY);
        return;
    }

    /* Preempt-disable marker — installs prp->debugger atomically w.r.t. the
     * EBUSY check above. In a BKL-less world this needs debug_slock (or
     * equivalently a per-prp ->dbg_lock) held across the existence-check +
     * object_alloc + pointer install, otherwise two concurrent attach calls
     * race and either both succeed (leak) or one EBUSYs spuriously. dep->flags
     * / dep->process are written before the pointer is observable, so dep->lock
     * isn't yet needed here.
     */
    if (!(dep = prp->debugger = object_alloc(prp, &debug_souls))) {
        process_unpin(prp);
        kcall_err(act, ENOMEM);
        return;
    }
    dep->process = prp;
    dep->flags = kap->flags;
    process_unpin(prp);
    SETKSTATUS(act, 0);
}

/* debug_attach wrapper: servers/roottask/proc/kext-wrappers.c */

struct kcall_args_debug_detach {
    pid_t pid;
};

/* Detach a debugger from a process (kernel extension body).
 *
 * Clears prp->debugger and, if the process was debug-stopped, resolves the
 * stop: pulsing termination if the process is terminating, continuing its
 * threads under run-lock-on-close, or destroying it under kill-lock-on-close.
 * Frees the debug_t last. Parameters:
 *   args : pointer to a struct kcall_args_debug_detach (pid)
 */
void kext_debug_detach(void *args)
{
    struct kcall_args_debug_detach *kap = args;
    process_t *prp;
    debug_t *dep;
    thread_t *act = get_active_thread(get_cpunum());

    if (!(prp = lookup_pid_rcu(kap->pid))) {
        rcu_read_unlock(&process_rcu);
        kcall_err(act, ESRCH);
        return;
    }
    if (process_pin(prp) != EOK) {
        rcu_read_unlock(&process_rcu);
        kcall_err(act, ESRCH);
        return;
    }
    rcu_read_unlock(&process_rcu);

    dep = prp->debugger;

    /* Preempt-disable marker — symmetric with debug_attach. In a BKL-less
     * world:
     *   - debug_slock around prp->debugger snapshot + clear, so
     *     a concurrent attach can't observe ->!debugger and
     *     race in a new dep before we're done tearing down.
     *   - cont_threads(prp, 0) walks prp->threads (needs
     *     prp->vec_slock) and ready()s each (sched_slock).
     *   - process_destroy is heavyweight and currently assumes
     *     full BKL; out of scope for this marker pass.
     *   - object_free of dep is safe once ->debugger is cleared,
     *     but a reader holding a stale dep pointer needs RCU or
     *     a refcount; today the BKL is the de-facto reader lock.
     */
    prp->debugger = 0;
    if (prp->flags & RTMUX_FLG_PROC_DEBUG_STOPPED) {
        prp->flags &= ~RTMUX_FLG_PROC_DEBUG_STOPPED;
        if (prp->flags & RTMUX_FLG_PROC_TERMING) {
            if (proc_hooks->proc_threads_destroyed) {
                struct sigevent ev;

                (*proc_hooks->proc_threads_destroyed)(prp, &ev);
                sigevent_proc(&ev);
            }
        } else if (!dep || (dep->flags & _DEBUG_FLAG_RLC)) {
            cont_threads(prp, 0);
        } else if (dep->flags & _DEBUG_FLAG_KLC) {
            process_destroy(prp, ROOTTASK_PID);
        }
    }
    if (dep) {
        object_free(prp, &debug_souls, dep);
    }
    process_unpin(prp);
    SETKSTATUS(act, 0);
}

/* debug_detach wrapper: servers/roottask/proc/kext-wrappers.c */

struct kcall_args_tctl_args {
    process_t *prp;
    int tid;
    struct kcall_args_thread_ctl tctl;
};

/* Apply a thread-control operation to a target thread (kernel extension body).
 *
 * Verifies the target thread exists within kap->prp, then runs
 * kcall_op_thread_ctl on it. On a real error it records the errno; on success
 * (ENOERROR or EOK) it sets the calling thread's status appropriately.
 * Parameters:
 *   data : pointer to a struct kcall_args_tctl_args (prp, tid, tctl)
 */
void kext_thread_ctl(void *data)
{
    thread_t *thp, *act = get_active_thread(get_cpunum());
    struct kcall_args_tctl_args *kap = (void *) data;
    int err;

    /* Verify the specified thread exists. */
    if ((thp = vector_lookup2(&kap->prp->threads, (unsigned) (kap->tid - 1))) ==
        NULL) {
        kcall_err(act, ESRCH);
        return;
    }

    err = kcall_op_thread_ctl(act, thp, &kap->tctl);
    if (err != ENOERROR) {
        if (err != EOK) {
            kcall_err(act, err);
        } else {
            /* Non-preemption marker: kcall_op_thread_ctl already mutated the
             * target thp under the BKL above; this in-kernel-call fence pairs
             * the SETKSTATUS write into act with that mutation. In a BKL-less
             * world the lock coverage moves *into* kcall_op_thread_ctl (needs
             * thp->lock for the target thread mutation); this site becomes
             * lock-free (act IS this CPU).
             */
            SETKSTATUS(act, 0);
        }
    }
}

/* kext_thread_ctl_call wrapper: servers/roottask/proc/kext-wrappers.c */
