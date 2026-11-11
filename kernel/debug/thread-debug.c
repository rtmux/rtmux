/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <core/globals.h>

/* Fill a debug_process_t snapshot for a process.
 *
 * Populates the structure used by the debugger / procfs from kernel-side
 * process_t state (times, ids, credentials, resource counts). Fields that now
 * live in root_process_t (umask, pgrp, sid, signal masks) are reported as zero
 * here — roottask supplies the real values when it serves the procfs read.
 * Parameters:
 *   prp : process to snapshot
 *   dpp : destination structure
 * Return value: EOK.
 */
int rdecl debug_process(process_t *prp, debug_process_t *dpp)
{
    credential_t *crp;
    uint64_t runtime;
    uint64_t systime;

    do {
        runtime = prp->running_time;
    } while (runtime != prp->running_time);
    do {
        systime = prp->system_time;
    } while (systime != prp->system_time);
    dpp->utime = runtime - systime;
    dpp->stime = systime;
    dpp->cutime = prp->kids_running_time - prp->kids_system_time;
    dpp->cstime = prp->kids_system_time;
    dpp->start_time = prp->start_time;
    dpp->pid = prp->pid;
    dpp->parent = prp->parent ? prp->parent->pid : 0;
    dpp->flags = prp->flags;

    /* umask lives in root_process_t; report 0 here — roottask fills the real
     * value when it serves the procfs read.
     */
    dpp->umask = 0;
    dpp->child = prp->child ? prp->child->pid : 0;
    dpp->sibling = prp->sibling ? prp->sibling->pid : 0;

    /* pgrp / sid live in root_process_t (POSIX policy is not kernel business);
     * roottask supplies them via procfs.
     */
    dpp->pgrp = 0;
    dpp->sid = 0;
    dpp->base_address = prp->base_addr;
    dpp->initial_stack = prp->initial_sp;
    if ((crp = prp->cred)) {
        dpp->uid = crp->info.ruid;
        dpp->gid = crp->info.rgid;
        dpp->euid = crp->info.euid;
        dpp->egid = crp->info.egid;
        dpp->suid = crp->info.suid;
        dpp->sgid = crp->info.sgid;
    } else {
        dpp->uid = dpp->gid = dpp->euid = dpp->egid = dpp->suid = dpp->sgid = 0;
    }
    memset(&dpp->sig_ignore, 0, sizeof(dpp->sig_ignore));
    memset(&dpp->sig_queue, 0, sizeof(dpp->sig_queue));
    memset(&dpp->sig_pending, 0, sizeof(dpp->sig_pending));
    dpp->num_chancons = prp->chancons.nentries - prp->chancons.nfree;
    dpp->num_fdcons = prp->fdcons.nentries - prp->fdcons.nfree;
    dpp->num_threads = prp->threads.nentries - prp->threads.nfree;
    dpp->num_timers = prp->timers.nentries - prp->timers.nfree;
    dpp->priority = prp->process_priority;
    dpp->pls = (uintptr_t) prp->pls;
    debug_moduleinfo(prp, NULL, &dpp->extsched);
    memset(dpp->reserved2, 0x00, sizeof dpp->reserved2);
    dpp->sigstub = 0; /* signals removed */
    dpp->canstub = (uintptr_t) prp->canstub;
    memset(dpp->reserved, 0x00, sizeof dpp->reserved);
    return EOK;
}



/* Fill a debug_thread_t snapshot for a thread.
 *
 * Populates the debugger / procfs view of a thread: stop/fault state, register
 * IP/SP, scheduling parameters, and per-state blocked-on detail (connection,
 * channel, sync object, join target, etc.). Signal-related fields are zeroed
 * (signals are gone from the kernel); siginfo now lives in root_process_t.
 * Parameters:
 *   prp : owning process
 *   thp : thread to snapshot (NULL marks the snapshot IPINVAL)
 *   dtp : destination structure
 * Return value: EOK, or ESRCH if thp is NULL.
 */
int rdecl debug_thread(process_t *prp, thread_t *thp, debug_thread_t *dtp)
{
    debug_t *dep;
    uint64_t runtime;

    dtp->flags = 0;
    if (thp) {
        if ((thp->flags & RTMUX_FLG_THR_TO_BE_STOPPED) ||
            thp->state == STATE_DEAD || thp->state == STATE_STOPPED) {
            dtp->flags |= _DEBUG_FLAG_STOPPED;
        }
    } else {
        memset(dtp, 0x00, sizeof *dtp);
        dtp->flags |= _DEBUG_FLAG_IPINVAL;
    }

    /* siginfo lives in root_process_t; leave the field zeroed here — roottask
     * fills it in via procfs when the rich payload is needed.
     */
    memset(&dtp->info, 0, sizeof dtp->info);
    if ((dep = prp->debugger)) {
        dtp->why = (uint16_t) dep->why;
        dtp->what = (uint16_t) dep->what;
        dtp->flags |= dep->flags;
        if (thp && dep->tid == thp->tid) {
            dtp->flags |= _DEBUG_FLAG_CURTID;
        }
    } else if (prp->flags & RTMUX_FLG_PROC_TERMING) {
        dtp->why = (uint16_t) _DEBUG_WHY_TERMINATED;
        dtp->what = 0;
    } else if ((prp->flags & RTMUX_FLG_PROC_COREDUMP) &&
               prp->valid_thp == thp) {
        dtp->why = (uint16_t) _DEBUG_WHY_SIGNALLED;
        dtp->what = 0;
    } else {
        dtp->why = _DEBUG_WHY_REQUESTED;
        dtp->what = 0;
    }
    if (prp->pid == ROOTTASK_PID) {
        dtp->flags |= _DEBUG_FLAG_ISSYS;
    }

    dtp->pid = prp->pid;
    if (!thp) {
        return ESRCH;
    }

    do {
        runtime = thp->running_time;
    } while (runtime != thp->running_time);
    dtp->sutime = runtime;
    dtp->start_time = thp->start_time;
    dtp->tid = (int) (thp->tid + 1); /* 1-based POSIX tid */
    dtp->ip = (dtp->flags & _DEBUG_FLAG_IPINVAL) ? 0 : KIP(thp);
    dtp->sp = KSP(thp);
    dtp->stkbase = (uintptr_t) thp->un.lcl.stackaddr;
    dtp->stksize = thp->un.lcl.stacksize;
    dtp->tls = (uintptr_t) thp->un.lcl.tls;
    dtp->tid_flags = thp->flags & RTMUX_FLG_THR_PUBLIC_MASK;
    dtp->priority = thp->priority;
    dtp->real_priority = thp->real_priority;
    dtp->policy = thp->policy;
    dtp->syscall = (int16_t) _TRACE_GETSYSCALL(thp->syscall);
    switch (dtp->state = thp->state) {
    case STATE_SEND:
    case STATE_REPLY: {
        connect_t *cop = (connect_t *) thp->blocked_on;

        if (cop->flags & COF_NETCON) {
            channel_t *chp;

            dtp->blocked.connect.coid = (int32_t) cop->un.remote.coid;
            dtp->blocked.connect.nd = RTMUX_LOCAL_NODE;
            if ((chp = cop->channel)) {
                dtp->blocked.connect.pid = chp->process->pid;
                dtp->blocked.connect.chid = chp->chid;
            } else {
                dtp->blocked.connect.pid = -1;
                dtp->blocked.connect.chid = -1;
            }
        } else {
            dtp->blocked.connect.coid = (int32_t) thp->args.ms.coid;
            dtp->blocked.connect.nd = cop->un.lcl.nd;
            dtp->blocked.connect.pid = cop->un.lcl.pid;
            dtp->blocked.connect.chid = (int32_t) cop->un.lcl.chid;
        }
        dtp->blocked.connect.scoid = cop->scoid | RTMUX_SIDE_CHANNEL;
        break;
    }
    case STATE_WAITTHREAD:
    case STATE_JOIN:
        dtp->blocked.join.tid = (int) (((thread_t *) thp->blocked_on)->tid + 1);
        break;
    case STATE_SEM:
        dtp->blocked.sync.sync = 0;
        dtp->blocked.sync.id =
            thp->blocked_on ? (int32_t) ((ksync_t *) thp->blocked_on)->addr : 0;
        break;
    case STATE_RECEIVE:
        dtp->blocked.channel.chid = ((channel_t *) thp->blocked_on)->chid;
        break;
    case STATE_WAITPAGE:
        waitpage_status_get(thp, dtp);
        break;
    case STATE_MUTEX:
    case STATE_CONDVAR:
        dtp->blocked.sync.sync = (uintptr_t) thp->args.mu.mutex;
        dtp->blocked.sync.id =
            thp->blocked_on ? (int32_t) ((ksync_t *) thp->blocked_on)->addr : 0;
        break;
    case STATE_STACK:
        dtp->blocked.stack.size =
            (thp->flags & RTMUX_FLG_THR_WAAA) ? thp->un.lcl.stacksize : 0;
        break;
    default:
        break;
    }
    dtp->last_cpu = (uint16_t) thp->runcpu;
    dtp->timeout = thp->timeout_flags;
    dtp->last_chid = thp->last_chid;
    memset(&dtp->sig_blocked, 0, sizeof(dtp->sig_blocked));

    dtp->nsec_since_block = get_nsec_since_block(thp);

    memset(&dtp->sig_pending, 0, sizeof(dtp->sig_pending));
    debug_moduleinfo(prp, thp, &dtp->extsched);
    memset(dtp->reserved2, 0x00, sizeof dtp->reserved2);
    return EOK;
}


/* Compute how long a thread has been blocked. Parameters:
 *   thp : thread to query
 * Return value: nanoseconds since the thread last blocked, or 0 if it is
 *               currently READY or RUNNING.
 */
uint64_t get_nsec_since_block(thread_t *thp)
{
    if (thp->state == STATE_READY || thp->state == STATE_RUNNING)
        return 0ULL;

    uint64_t snapshot;
    snap_time(&snapshot, 0);
    return snapshot - thp->timestamp_last_block;
}


static int debug_code;

/* Notification the proc service to report a debug event. Parameters:
 *   pid      : process the event concerns (passed as the notification value)
 *   priority : notification priority, or -1 to use the active thread's real
 * priority
 */
static void debug_event(pid_t pid, int priority)
{
    struct sigevent event;

    /* Notification the proc service */
    event.sigev_notify = SIGEV_NOTIFICATION;
    event.sigev_coid = ROOTTASK_COID;
    event.sigev_value.sival_int = pid;
    event.sigev_priority =
        (short) (priority == -1 ? get_active_thread(get_cpunum())->real_priority
                                : priority);
    event.sigev_code = (short) debug_code;
    sigevent_proc(&event);
}

/* Stop a process under debugger control.
 *
 * Marks the process debug-stopped, halts all its threads, records the stop
 * reason in the debugger state and notifies the proc service.
 *
 * Debugger ops mutate prp->flags and the debug_t state and iterate prp->threads
 * without a dedicated lock; concurrency is bounded by the
 * one-debugger-per-target convention, and the in-kernel-call preemption fence
 * is the per-CPU fence around the multi-step mutation. Parameters:
 *   prp : process to stop
 * Return value: EOK, or EINVAL if no debugger is attached.
 */
int rdecl debug_stop(process_t *prp)
{
    debug_t *dep;

    if (!(dep = prp->debugger)) {
        return EINVAL;
    }
    if (!(prp->flags & RTMUX_FLG_PROC_DEBUG_STOPPED)) {
        prp->flags |= RTMUX_FLG_PROC_DEBUG_STOPPED;
        stop_threads(prp, 0, 0);
        dep->flags |= _DEBUG_FLAG_ISTOP;
        dep->why = _DEBUG_WHY_REQUESTED;
        dep->what = 0;
        (void) vector_search(&prp->threads, 0, (unsigned *) &dep->tid);
        debug_event(prp->pid, -1);
    }
    return EOK;
}

/* Resume a debug-stopped process or single-step a thread.
 *
 * Applies the debug-run request: optional current-tid change, fault clearing or
 * re-delivery, IP override, single-step setup, and breakpoint skip handling,
 * then either continues all threads or arms a single step. Parameters:
 *   prp : process to run
 *   drp : debug-run request
 * Return value: EOK on success, or an errno (EBADF/EBUSY/ENOTSUP/ENXIO/...).
 */
int rdecl debug_run(process_t *prp, debug_run_t *drp)
{
    debug_t *dep;
    thread_t *thp;
    int tid;
    unsigned drp_flags;

    if (!(dep = prp->debugger)) {
        return EBADF;
    }

    if (!(prp->flags & RTMUX_FLG_PROC_DEBUG_STOPPED)) {
        return EBUSY;
    }

    drp_flags = drp->flags;
    /* Check for unsupported debug modes.... */
    if (drp_flags &
        ~(unsigned) (_DEBUG_RUN_VADDR | _DEBUG_RUN_FAULT | _DEBUG_RUN_CURTID |
                     _DEBUG_RUN_STEP | _DEBUG_RUN_CLRSIG | _DEBUG_RUN_STEP_ALL |
                     _DEBUG_RUN_CLRFLT)) {
        return ENOTSUP;
    }
    /* Find thread pointer (If requested change the current tid) */
    if ((tid = dep->tid) && (drp_flags & _DEBUG_RUN_CURTID)) {
        tid = drp->tid - 1;
    }
    if (!(thp = vector_lookup(&prp->threads, (unsigned) tid))) {
        return ENXIO;
    }


    dep->tid = tid;

    if ((drp_flags & _DEBUG_RUN_CLRSIG) && dep->why == _DEBUG_WHY_SIGNALLED) {
        /* Signals removed — just clear the debugger state */
        dep->why = _DEBUG_WHY_REQUESTED;
    }

    if (dep->why == _DEBUG_WHY_FAULTED) {
        if (drp_flags & _DEBUG_RUN_CLRFLT) {
            dep->why = _DEBUG_WHY_REQUESTED;
        } else {
            /* The saved fault siginfo lives in root_process_t and is not
             * reachable from this path, so deliver a synthetic SIGSEGV instead.
             * This matches the post-fault state; the richer siginfo only
             * mattered for waitpid detail, which is roottask's job to assemble.
             */
            siginfo_t info;
            memset(&info, 0, sizeof info);
            info.si_signo = SIGSEGV;
            info.si_code = 0; /* SI_KERNEL not defined in RTMux */
            deliver_fault(thp, &info);
        }
    }

    if (drp_flags & _DEBUG_RUN_VADDR) {
        SETKIP(thp, drp->ip);
    }

    if (drp_flags & _DEBUG_RUN_FAULT) {
        dep->faults = drp->fault;
    }

    dep->flags &= ~(unsigned) (_DEBUG_FLAG_ISTOP | _DEBUG_FLAG_TRACE_EXEC |
                               _DEBUG_FLAG_TRACE_RD | _DEBUG_FLAG_TRACE_WR |
                               _DEBUG_FLAG_TRACE_MODIFY | _DEBUG_FLAG_SSTEP);

    ITF_CLR(thp, RTMUX_ITF_SSTEP_SUSPEND);
    atomic_clr(&thp->flags, RTMUX_FLG_THR_SSTEP);

    dep->skip_brk = 0;
    if (!(thp->flags & RTMUX_FLG_THR_THREADS_HOLD)) {
        breakpoint_t *d;

        for (d = dep->brk; d; d = d->next) {
            if ((d->brk.type & _DEBUG_BREAK_EXEC) && KIP(thp) == d->brk.addr) {
                dep->skip_brk = d;
                if (!(drp_flags & _DEBUG_RUN_STEP)) {
                    prp->flags &= ~RTMUX_FLG_PROC_DEBUG_STOPPED;
                }
                drp_flags = (drp_flags & ~(unsigned) _DEBUG_RUN_STEP_ALL) |
                            _DEBUG_RUN_STEP;
                break;
            }
        }
    }

    if (drp_flags & (_DEBUG_RUN_STEP | _DEBUG_RUN_STEP_ALL)) {
        int status;

        if ((status = cpu_debug_sstep(dep, thp))) {
            if (status > 0) {
                return status;
            }
            /* negative status, can't support RUN_STEP_ALL */
            drp_flags =
                (drp_flags & ~(unsigned) _DEBUG_RUN_STEP_ALL) | _DEBUG_RUN_STEP;
        }
        atomic_or(&thp->flags, RTMUX_FLG_THR_SSTEP);
    }
    if (drp_flags & _DEBUG_RUN_STEP) {
        if (thp->flags & RTMUX_FLG_THR_THREADS_HOLD) {
            return EBUSY;
        }
        if (!(prp->flags & RTMUX_FLG_PROC_STOPPED)) {
            atomic_clr(&thp->flags, RTMUX_FLG_THR_TO_BE_STOPPED);
            if (thp->state == STATE_STOPPED) {
                ready(thp);
            }
        }
    } else {
        prp->flags &= ~RTMUX_FLG_PROC_DEBUG_STOPPED;
        cont_threads(prp, 0);
    }
    return EOK;
}

/* Add, modify, or remove a breakpoint.
 *
 * Locates the matching breakpoint in the debugger's sorted list and, depending
 * on the request size, removes it, updates it, or allocates a new one,
 * programming the CPU breakpoint hardware accordingly. Parameters:
 *   prp : process being debugged
 *   dbp : breakpoint descriptor (size < 0 removes)
 * Return value: EOK on success, or an errno (EBADF/EINVAL/ENOMEM/...).
 */
int rdecl debug_break(process_t *prp, debug_break_t *dbp)
{
    debug_t *dep;
    breakpoint_t *d, **pd;
    int status;

    if (!(dep = prp->debugger)) {
        return EBADF;
    }
    /* Search for sorted matching breakpt */
    for (pd = &dep->brk; (d = *pd); pd = &d->next) {
        if ((dbp->type & _DEBUG_BREAK_MASK) ==
            (d->brk.type & _DEBUG_BREAK_MASK)) {
            if (dbp->addr == d->brk.addr) {
                break;
            }
            if (dbp->addr > d->brk.addr) {
                d = 0;
                break;
            }
        } else if ((dbp->type & _DEBUG_BREAK_MASK) >
                   (d->brk.type & _DEBUG_BREAK_MASK)) {
            d = 0;
            break;
        }
    }

    /* Remove existing if requested */
    if (dbp->size < 0) {
        if (dbp->size != -1) {
            return EINVAL;
        }
        if (d) {
            (void) cpu_debug_brkpt(dep, d);
            *pd = d->next;
            object_free(prp, &breakpt_souls, d);
            return EOK;
        }
        return EINVAL;
    }

    if (d) {
        d->brk.size = -1;
        (void) cpu_debug_brkpt(dep, d);
        d->brk = *dbp;
    } else {
        /* Allocate new if requested */
        if (!(d = object_alloc(prp, &breakpt_souls))) {
            return ENOMEM;
        }
        d->brk = *dbp;
        d->next = *pd;
        *pd = d;
    }
    if ((status = cpu_debug_brkpt(dep, d))) {
        *pd = d->next;
        object_free(prp, &breakpt_souls, d);
    }
    return status;
}

/* Copy out a range of the process's breakpoints.
 *
 * Walks the sorted breakpoint list and copies up to dbpl->count entries
 * starting at dbpl->offset, reporting the number copied and the total.
 * Parameters:
 *   prp  : process being debugged
 *   dbpl : in/out list descriptor (offset, count; updated count + total_count)
 * Return value: EOK, or EBADF if no debugger is attached.
 */
int rdecl debug_break_list(process_t *prp, debug_breaklist_t *dbpl)
{
    debug_t *dep;
    breakpoint_t *d;
    int add_count, total_count;

    if (!(dep = prp->debugger)) {
        return EBADF;
    }

    add_count = 0;
    total_count = 0;

    /* Search for sorted matching breakpt */
    for (d = dep->brk; d; d = d->next) {
        /* the intention is to add support for a breakpoint ossfet into the list
         */
        if (add_count < dbpl->count && total_count >= dbpl->offset) {
            memcpy(&dbpl->breakpoints[add_count], &d->brk, sizeof(d->brk));
            add_count++;

            /* Only go through as many items as we need to */
            if (add_count == dbpl->count && dbpl->offset != 0) {
                break;
            }
        }
        total_count++;
    }

    dbpl->count = add_count;
    dbpl->total_count = total_count;

    return EOK;
}

/* Hand a thread fault to an attached debugger.
 *
 * Lets the CPU debug layer inspect the fault, handles breakpoint-skip
 * single-stepping, and — if the debugger is interested in this fault number —
 * stops the process, records the fault state and notifies the proc service.
 * Parameters:
 *   thp  : faulting thread
 *   info : fault siginfo
 * Return value: 1 if the debugger consumed the fault, 0 to let it proceed
 *               to normal (fatal) delivery.
 */
static int rdecl thread_fault(thread_t *thp, siginfo_t *info)
{
    debug_t *dep;
    process_t *prp;
    unsigned flags;
    unsigned skip;
    int ret;

    prp = thp->process;
    if (!(dep = prp->debugger)) {
        return 0;
    }

    /* check the status of the fault first up */
    flags = 0;
    ret = cpu_debug_fault(dep, thp, info, &flags);

    /* Insert skip breakpoint, and make all threads runnable if nessessary */
    skip = 0;
    if (dep->skip_brk) {
        debug_detach_brkpts(dep);
        dep->skip_brk = 0;
        debug_attach_brkpts(dep);
        if (!(prp->flags & RTMUX_FLG_PROC_DEBUG_STOPPED)) {
            skip = _DEBUG_FLAG_SSTEP;
            cont_threads(prp, 0);
        }
    }

    if (ret) {
        return 1;
    }

    if (flags & skip) {
        return 1;
    }

    if (info->si_code <= SI_USER) {
        crash();
    }

    atomic_clr(&thp->flags, RTMUX_FLG_THR_SSTEP);

    /* Is there any interest in the fault? */
    if (!BITSET_TST(dep->faults, info->si_fltno)) {
        return 0;
    }

    _TRACE_SYS_EMIT_ADDRESS(thp, KIP(thp));

    prp->flags |= RTMUX_FLG_PROC_DEBUG_STOPPED;
    stop_threads(prp, 0, 0);

    /* Save away fault state */
    dep->flags |= _DEBUG_FLAG_ISTOP |
                  (flags & (_DEBUG_FLAG_SSTEP | _DEBUG_FLAG_TRACE_EXEC |
                            _DEBUG_FLAG_TRACE_RD | _DEBUG_FLAG_TRACE_WR |
                            _DEBUG_FLAG_TRACE_MODIFY));
    dep->why = _DEBUG_WHY_FAULTED;

    /* siginfo lives in root_process_t, not kernel-side prp, so only the fault
     * number is recorded for later DEBUG_RUN — sufficient for the current RTMux
     * kdebug.
     */
    dep->what = info->si_fltno;
    dep->tid = (int) thp->tid;

    /* Don't enqueue a signal for the fault. */
    debug_event(prp->pid, thp->priority);
    return 1;
}

/* kext_debug_install — slot __KSYS_PRIV_DEBUG_INSTALL.
 *
 * Wires up the debug-callback function pointers. Argument carries the
 * notification code (passed via void* to fit the kernel extension signature).
 */
void kext_debug_install(void *data)
{
    debug_code = (int) (intptr_t) data;
    debug_thread_fault = thread_fault;
    debug_attach_brkpts = cpu_debug_attach_brkpts;
    debug_detach_brkpts = cpu_debug_detach_brkpts;
}
