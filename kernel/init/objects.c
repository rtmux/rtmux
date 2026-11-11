/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <core/globals.h>
#include <cpu-inline.h> /* chk_kernel_sp() */

/* VM and proc-hook backing storage.
 *
 * The kernel owns this table. All function-pointer slots point at
 * kernel-resident "mm_*" symbols (kernel/mm/), because roottask runs in U-mode
 * (PTE_U=1) and an S-mode fetch from its text is illegal regardless of SUM.
 * Roottask keeps its own LOCAL vm_ops_t struct (servers/roottask/sys/, not
 * visible to the kernel) for its own internal use; that's independent of
 * vm_ops.
 */
static vm_ops_t vm_ops_storage;
static proc_hooks_t proc_hooks_storage;
vm_ops_t *vm_ops = &vm_ops_storage;
proc_hooks_t *proc_hooks = &proc_hooks_storage;

/* Pre-grow a soul's object pool to its configured minimum. Parameters:
 *   sp : the soul whose backing object pool to grow.
 */
static void init_soul(soul_t *sp)
{
    (void) object_grow(sp);
}


/* Build the kernel's core object set at boot.
 *
 * Links all query vectors, pre-allocates the minimum number of each object soul
 * (processes, threads, channels, connects, etc.), creates the hash tables and
 * global spinlocks, allocates the kernel-threads process container, sets up the
 * kernel stacks (with guard pages), initializes the scheduler, and creates one
 * idle thread per CPU.
 */
void init_objects()
{
    process_t *prp;
    thread_t *thp;
    unsigned i;
    unsigned ksys_stack_size;
    uintptr_t ksys_sp;
    kdispatch_t *dpp;

    /* Link all vectors off the query vector. */
    vector_add(&query_vector, &soul_vector, 0);
    vector_add(&query_vector, &process_vector, 0);
    vector_add(&query_vector, &interrupt_vector, 0);
    vector_add(&query_vector, &vthread_vector, 0);
    vector_add(&query_vector, &global_channel_vector, 0);

    /* soul_slock serializes every soul freelist pop/push and its accounting. It
     * must be live before the first init_soul below (object_grow links new
     * souls under it), and it nests SOUL -> MEMPART when growing/shrinking, so
     * it is initialized after heap_init has already set up alloc_slock. Still
     * single-CPU here, so this init is uncontended.
     */
    kspinlock_init(&soul_slock, LOCK_CLASS_SOUL, "soul_slock");

    /* Create all the objects. */
    init_soul(&process_souls);        /* Allocate the min number of processes */
    init_soul(&thread_souls);         /* Allocate the min number of threads */
    init_soul(&channel_souls);        /* Allocate the min number of channels */
    init_soul(&global_channel_souls); /* Allocate global channel souls */
    init_soul(&connect_souls);        /* Allocate the min number of connects */
    init_soul(
        &notification_souls);     /* Allocate the min number of notifications */
    init_soul(&sync_souls);       /* Allocate the min number of syncs */
    init_soul(&syncevent_souls);  /* Allocate the min number of syncevents */
    init_soul(&interrupt_souls);  /* Allocate the min number of interrupts */
    init_soul(&timer_souls);      /* Allocate the min number of timers */
    init_soul(&credential_souls); /* Allocate the min number of credentials */
    init_soul(&ssinfo_souls);     /* Allocate the min number of ss schedinfos */
    init_soul(
        &threadname_souls); /* Allocate the min number of thread name souls */

    /* Create all hash tables */
    sync_hash.table = _scalloc((sync_hash.mask + 1) * sizeof(ksync_t *));
    kspinlock_init(&sync_slock, LOCK_CLASS_SYNC, "sync_slock");

    /* Initialize the chain-keyed sync wait queue table. See kernel/sync/wait.c
     * for the design.
     */
    extern void sync_init(void);
    sync_init();

    /* Each per-CPU dispatch instance carries its own scheduler lock,
     * initialized inside init_scheduler_default(). The send/receive/reply
     * queues are guarded by the per-channel channel_t.lock (chp->lock),
     * initialized at channel-create time in channel.c. global_channel_slock —
     * global-channel directory lock. Plays the same role for
     * global_channel_vector that prp->vec_slock plays for prp->chancons. Same
     * class as PROCVEC, distinct instance (lock_debug allows same-class
     * distinct-instance acquires).
     */
    kspinlock_init(&global_channel_slock, LOCK_CLASS_PROCVEC,
                   "global_channel_slock");

    /* as_slock — serializes ksys_manipulate. Without it, concurrent
     * ksys_manipulate on overlapping as_regions corrupts the PTE state machine
     * (see manipulate.c::do_manipulation).
     */
    kspinlock_init(&as_slock, LOCK_CLASS_MEMPART, "as_slock");
    /* Locks for the BKL-less kernel extensions in kernel/ext/. */
    kspinlock_init(&process_vector_slock, LOCK_CLASS_PROCVEC,
                   "process_vector_slock");
    kspinlock_init(&cred_slock, LOCK_CLASS_PROCESS, "cred_slock");
    kspinlock_init(&limits_slock, LOCK_CLASS_PROCESS, "limits_slock");
    kspinlock_init(&trace_slock, LOCK_CLASS_INTREVENT, "trace_slock");

    /* roottask_stack_cache free-list lock — guards the push/pop/drain race
     * between kext_register_pool_stack, roottask_stack_alloc,
     * roottask_stack_free, roottask_stack_purge.
     */
    kspinlock_init(&roottask_stack_slock, LOCK_CLASS_MEMPART,
                   "roottask_stack_slock");
    kspinlock_init(&pa_slock, LOCK_CLASS_PA, "pa_slock");

    /* alloc_slock is initialized at the top of heap_init() — must be ready
     * before the very first _scalloc, which happens earlier in boot than
     * init_objects().
     */

    /* Allocate the process table mapping vector. Reserves first slot. */
    vector_add(&process_vector, 0, process_souls.min); /* Pregrow vector */

    /* Create a process container for all the kernel threads. */
    prp = object_alloc(NULL, &process_souls);

    /* Per-process vector lock — must be initialized before any of the
     * vector_add calls below. Mirror of the kext_process_create init for the
     * runtime case.
     */
    kspinlock_init(&prp->vec_slock, LOCK_CLASS_PROCVEC, "process_t.vec_slock");
    prp->pin_count = 0;
    if (rtmux_cap_table_init(&prp->endpoint_caps, prp->endpoint_cap_slots,
                             RTMUX_PROCESS_ENDPOINT_CAP_SLOTS) != EOK)
        crash();
    vector_add(&process_vector, prp,
               0); /* Stuff slot 0 so any attempt to use faults */
    kernel_threads_prp = prp;
    prp->pid = vector_add(&process_vector, prp, 1);
    /* "pgrp" lives in root_process_t now; nothing kernel-side owns it. */

    prp->cred = credential_list = object_alloc(NULL, &credential_souls);
    prp->cred->next = NULL;
    prp->cred->links = 1;
    prp->seq = 1;

    /* init the getrlimit/setrlimit stuff */
    for (i = 0; i < RLIM_NLIMITS; i++) {
        prp->rlimit_vals_soft[i] = prp->rlimit_vals_hard[i] = RLIM_INFINITY;
    }

    /* override the max file descriptor entry */
    prp->rlimit_vals_soft[RLIMIT_NOFILE] =
        prp->rlimit_vals_hard[RLIMIT_NOFILE] = kopt.max_fds;

    /* Boot processes are not bounded by an RLIMIT_CPU. */
    prp->max_cpu_time = RLIM_INFINITY;

    prp->start_time = qtimeptr->nsec_tod_adjust;
    if (kopt.limits_max[0]) {
        prp->limits = object_alloc(0, &limits_souls);
        prp->limits->links = 1;
        memcpy(prp->limits->max, kopt.limits_max, sizeof(prp->limits->max));
    }
    prp->flags = RTMUX_FLG_PROC_SLEADER | RTMUX_FLG_PROC_NOCLDSTOP;
    prp->priv_caps = RTMUX_PRIV_CAP_ALL;
    prp->num_active_threads = num_processors;
    prp->boundary_addr = VM_KERN_SPACE_BOUNDARY;
    /* Signal ignore/queue masks removed — signal subsystem eliminated */

    /* Alloc space for all the kernel stacks, with a 4KB guard page below each
     * stack. The guard page PTE is invalidated so any access triggers a page
     * fault (immediate stack overflow detection).
     *
     * Layout per CPU (stack grows down):
     *   [guard 4KB] [usable stack KSYS_STACK_NBYTES]
     */
    ksys_stack_size = (KSYS_STACK_NBYTES + __PAGESIZE) * num_processors;
    run_ker_stack_bot = (uintptr_t) _scalloc(ksys_stack_size);
    run_ker_stack_top = run_ker_stack_bot + ksys_stack_size;
    ksys_sp = STACK_INIT(run_ker_stack_bot, ksys_stack_size);

    /* Initialize scheduler and default dispatch queue */
    dpp = init_scheduler();
    prp->default_dpp = dpp;

    /* Build LEGAL_CPU_BITMASK from hart_ids[] */
    {
        extern unsigned hart_ids[];
        unsigned mask = 0;
        for (unsigned k = 0; k < num_processors; k++)
            mask |= (1U << hart_ids[k]);
        legal_cpu_bitmask = mask;
    }

    /* Create idle threads — one per CPU, indexed by hart ID.
     *
     * hart_ids[] is populated by the FDT parser in startup and contains the
     * actual RISC-V hart IDs. The kernel uses these directly as indices into
     * actives[], ksys_stack[], cpupageptr[], etc. No logical-to-physical CPU
     * mapping — hart IDs are the only CPU identifiers in the system.
     */
    {
        extern unsigned hart_ids[];

        i = 0;
        do {
            unsigned cpu = hart_ids[i];
            uintptr_t stack_top;
            void *dummy __attribute__((unused));

            thp = object_alloc(NULL, &thread_souls);
            memset(thp, 0, sizeof(*thp));
            thp->tid = (uint32_t) vector_add(&prp->threads, thp,
                                             0); /* always succeeds at init */
            thp->state = STATE_RUNNING;
            thp->process = prp;
            thp->runcpu = cpu;
            /* default_runmask stays zero so children inherit the parent. */
            thp->runmask = ~(1U << cpu);
            thp->last_chid = -1;
            thp->policy = SCHED_RR;
            thp->sched_flags = 0;
            thp->dpp = thp->orig_dpp = dpp;
            thp->type = TYPE_THREAD;
            thp->flags = RTMUX_FLG_THR_NOMULTISIG | RTMUX_FLG_THR_WAAA;

            /* Idle priority is 0 — the reserved "only run when nothing else is
             * ready" slot. This is the fixed-priority runqueue floor and the
             * IDLE sentinel used by chk_dispatch below.
             *
             * User __thread_create enforces priority > 0 (lifecycle.c), so no
             * user thread can compete with idle at this priority.
             */
            thp->real_priority = thp->priority = 0;
            thp->start_time = prp->start_time;
            thp->un.lcl.stackaddr = (void *) _smalloc(sizeof(startup_stack));
            thp->un.lcl.stacksize = sizeof(startup_stack);
            stack_top = STACK_INIT((uintptr_t) thp->un.lcl.stackaddr,
                                   thp->un.lcl.stacksize);
            STACK_ALLOC(thp->un.lcl.tls, stack_top, stack_top,
                        sizeof(struct _thread_local_storage));
            memset(thp->un.lcl.tls, 0, sizeof(struct _thread_local_storage));
            thp->un.lcl.tls->__errptr = &thp->un.lcl.tls->__errval;
            thp->un.lcl.tls->__flags = PTHREAD_CANCEL_DISABLE;
            thp->un.lcl.tls->__pid = 1; /* kernel process */
            thp->un.lcl.tls->__tid =
                (int) (thp->tid + 1); /* tid+1 fits in int */
            /* Give idle threads a sentinel __exitfunc. Idle threads themselves
             * never exit, but they're the first threads in the process:
             * prp->valid_thp below is set to the last idle thread, and
             * thread_specret() in lifecycle.c copies __exitfunc from valid_thp
             * into any child whose TLS didn't get an explicit hook. Without
             * this, such children inherit NULL and crash silently at pc=0 when
             * their entry returns.
             */
            {
                extern void kernel_thread_exit_fallback(void *arg);
                thp->un.lcl.tls->__exitfunc = kernel_thread_exit_fallback;
            }
            thp->un.lcl.tls->__owner =
                (unsigned) ((thp->un.lcl.tls->__pid << 16) |
                            thp->un.lcl.tls->__tid) &
                ~RTMUX_SYNC_WAITING;
            thp->un.lcl.tls->__stackaddr = thp->un.lcl.stackaddr;
            STACK_ALLOC(dummy, stack_top, stack_top,
                        STACK_INITIAL_CALL_CONVENTION_USAGE);
            cpu_thread_init(NULL, thp, kopt.align_fault);
            SETKSP(thp, stack_top);
            SETKIP(thp, &idle);

            set_active_thread(cpu, thp);
            pcpu_array[cpu].pc_active_prp = prp;
            idle_tls[cpu] = thp->un.lcl.tls;
            pcpu_array[cpu].pc_idle_thp = thp;

            STACK_ALLOC(dummy, ksys_stack[cpu], ksys_sp,
                        STACK_INITIAL_CALL_CONVENTION_USAGE);
            chk_kernel_sp((uintptr_t) ksys_stack[cpu], "init:ksys_stack[cpu]");
            STACK_ALLOC(dummy, ksys_sp, ksys_sp, KSYS_STACK_NBYTES);
            /* Guard page: skip 4KB and invalidate its PTE */
            ksys_sp -= __PAGESIZE;
            {
                extern void kernel_guard_page(uintptr_t va);
                kernel_guard_page((uintptr_t) ksys_sp);
            }
        } while (++i < num_processors);
    }
    prp->valid_thp = thp;
}

/* init_vm_ops — wire the kernel-resident vm_ops table.
 *
 * After the U-mode-roottask flip the kernel cannot legally call into as_*
 * (roottask text, PTE_U=1, S-mode fetch forbidden). Every vm_ops->FOO call from
 * kernel S-mode therefore lands in a kernel-resident mm_* symbol. roottask
 * keeps its own private "vm_ops" struct for its own internal use; that's not
 * what kernel vm_ops points at.
 */
extern int mm_mmap(pid_t,
                   process_t *,
                   uintptr_t,
                   size_t,
                   int,
                   int,
                   path_object_t *,
                   uint64_t,
                   size_t,
                   size_t,
                   int,
                   void **,
                   size_t *);
extern int mm_munmap(pid_t, process_t *, uintptr_t, size_t, int);
extern path_object_t *mm_vaddr_to_memobj(process_t *, void *, paddr_t *, int);
extern unsigned mm_vaddrinfo(pid_t,
                             process_t *,
                             uintptr_t,
                             paddr_t *,
                             size_t *,
                             unsigned);
extern int mm_mcreate(process_t *);
extern void mm_mdestroy(process_t *);
extern void as_switch(process_t *, process_t **);
extern int mm_map_xfer(process_t *actprp,
                       process_t *prp,
                       iov_t **piov,
                       int *pparts,
                       int *poff,
                       iov_t *niov,
                       int *pnparts,
                       unsigned flags);
extern void proc_threads_destroyed(process_t *prp, struct sigevent *evp);

void init_vm_ops(void)
{
    vm_ops->pagesize = privateptr->pagesize;
    vm_ops->mmap = mm_mmap;
    vm_ops->munmap = mm_munmap;
    vm_ops->vaddr_to_memobj = mm_vaddr_to_memobj;
    vm_ops->vaddrinfo = mm_vaddrinfo;
    vm_ops->mcreate = mm_mcreate;
    vm_ops->mdestroy = mm_mdestroy;
    vm_ops->as = as_switch;
    vm_ops->map_xfer = mm_map_xfer;

    /* roottask text is PTE_U=1 and the kernel cannot fetch from it in S-mode.
     * The single proc hook the kernel-side termination path needs is
     * proc_threads_destroyed; wire it to the kernel-resident substitute in
     * process.c. See lifecycle.c:719-724 for the call site.
     */
    proc_hooks->proc_threads_destroyed = proc_threads_destroyed;
}
