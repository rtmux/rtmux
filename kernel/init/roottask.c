/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* roottask.c — Post-boot first-server initialization
 *
 * License: Apache 2.0
 *
 * initramfs_load_modules() has already recorded the first server ELF. The
 * launcher builds a process and thread for it, marks it for U-mode entry, and
 * lets the scheduler take it from there. The boot CPU returns to idle().
 */

#include <core/globals.h>
#include <module/initramfs-elf.h>
#include <stdbool.h>
#include <stdatomic.h>
#include "as.h" /* full address_t + cpu_mm_as */
#include <sys/storage.h>
#include <sys/rtmux-core.h> /* ROOTTASK_PID */
#include <paging.h>
#include <cpu-inline.h>

static const char *initramfs_basename(const char *name)
{
    const char *base = name;

    for (const char *p = name; *p; p++)
        if (*p == '/')
            base = p + 1;
    return base;
}

/* Hand off to the U-mode first server recorded in initramfs.
 *
 * Sequence:
 *
 *   1. Find /boot/roottask in initramfs_modules[] by name.
 *   2. Build a minimal process_t to carry the U-mode as — its
 *      "memory" (address_t) is wired to the pgdir that
 *      initramfs_elf_load_sv39 already populated.  This process_t is
 *      separate from kernel_threads_prp (which still owns the idle
 *      threads) but takes the ROOTTASK_PID slot in process_vector so
 *      lookup_pid(1) finds the first server from the moment its first
 *      message goes through.
 *   3. Allocate a thread_t under the new process_t, give it a kernel
 *      stack, set reg.pc / REGSP from the loaded ELF, flip SPP=0
 *      via cpu_thread_enter_umode().
 *   4. ready() the thread; the boot CPU returns to idle().  When
 *      the scheduler picks the new thread on any CPU, the standard
 *      kernel_exit / sret path lands at the server's _start in U-mode.
 *
 * No POSIX argv/auxv stack frame for now — the server crt0 is tolerant of an
 * unset argc (the first thing it does is read the boot command line via
 * syspage), and the goal is the absolute simplest first-boot attempt.
 */
static void ksys_start_roottask_umode(void)
{
    static atomic_bool roottask_launch_started;
    struct initramfs_module *server_m = NULL;

    if (atomic_exchange_explicit(&roottask_launch_started, true,
                                 memory_order_acq_rel))
        return;

    /* Roottask is now a STATIC ET_EXEC linked at fixed VA 0xC0000000 (matches
     * RTMUX_ROOTTASK_UADDR_BASE / SV39 L2[3]). All libc symbols live inside
     * roottask's own image. Other user processes continue to use the libc.so +
     * ldso path; only roottask switches to static.
     *
     * Locate roottask's initramfs metadata, then load the static ET_EXEC image.
     */
    for (int i = 0; i < initramfs_nmodules; i++) {
        const char *bn = initramfs_basename(initramfs_modules[i].name);
        if (initramfs_modules[i].type == RTMUX_TYPE_EXEC &&
            !strcmp(bn, "roottask")) {
            server_m = &initramfs_modules[i];
            break;
        }
    }
    if (!server_m)
        crash_halt("No roottask EXEC in initramfs\n");

    {
        uintptr_t entry = initramfs_elf_load_roottask(
            (const void *) server_m->load_base, server_m->load_size,
            server_m->name, server_m);
        if (entry == 0)
            crash_halt("initramfs_elf_load_roottask failed\n");
        server_m->entry = entry;
    }
    if (!server_m->pgdir || server_m->entry == 0)
        crash_halt(
            "first-server post-load state invalid "
            "(pgdir=%p entry=%lx)\n",
            server_m->pgdir, (unsigned long) server_m->entry);

    /* (2) build the process_t + address_t carrying the U-mode pgdir */
    process_t *prp;
    if (!(prp = object_alloc(NULL, &process_souls)))
        crash_halt("roottask launch: process_souls alloc failed\n");

    /* Per-process vector lock — must be initialized before any vec_slock-aware
     * kcall (e.g. ksys_connect_attach) sees this prp. Mirror of the init in
     * kext_process_create / kernel/init/objects.c.
     */
    kspinlock_init(&prp->vec_slock, LOCK_CLASS_PROCVEC, "process_t.vec_slock");
    prp->pin_count = 0;
    if (rtmux_cap_table_init(&prp->endpoint_caps, prp->endpoint_cap_slots,
                             RTMUX_PROCESS_ENDPOINT_CAP_SLOTS) != EOK)
        crash_halt("roottask launch: endpoint cap table init failed\n");
    address_t *adp = _scalloc(sizeof(address_t));
    if (!adp)
        crash_halt("roottask launch: address_t alloc failed\n");

    adp->cpu.pgdir = server_m->pgdir;
    adp->cpu.ptroot_paddr = PTR_TO_PHYS(server_m->pgdir);
    adp->cpu.asid = 0;
    prp->memory = adp;

    /* Cache the roottask prp as the "safe as" target. Required by set_safe_as()
     * — kernel_threads_prp has no memory, so the IPI_TLB_SAFE handler needs
     * roottask's real pgdir to actually detach a remote hart from a dying user
     * as.
     */
    roottask_prp = prp;
    prp->cred = kernel_threads_prp->cred; /* share kernel cred */
    /* Single-threaded early boot: roottask is being installed and is not yet
     * running, so no process create/destroy (and thus no cred_set) can race
     * this share-count bump on another hart. The cred_slock discipline that
     * guards the ++/-- in cred_set therefore does not apply here; this is an
     * init-time bump, not an unlocked mutation of a live, shared credential.
     */
    prp->cred->links++;
    prp->seq = 1;
    prp->boundary_addr = VM_KERN_SPACE_BOUNDARY;
    prp->flags = RTMUX_FLG_PROC_LOADING | RTMUX_FLG_PROC_NOZOMBIE;

    /* The monolithic roottask still hosts coresrv/namesrv code; this grant
     * shrinks to per-server capabilities once those become separate
     * executables.
     */
    prp->priv_caps = RTMUX_PRIV_CAP_ALL;
    prp->default_dpp = kernel_threads_prp->default_dpp;

    /* rlimits: copy from kernel_threads_prp (RLIM_INFINITY for everything
     * except NOFILE which is kopt.max_fds). Without this, ksys_thread_create
     * sees rlimit_vals_soft[RLIMIT_NTHR] = 0 and rejects every pthread_create
     * with EINVAL — roottask's thread_pool_start fails on the first pool
     * thread, prints "FATAL: thread_pool_start failed", and crash()es.
     */
    for (int i = 0; i < RLIM_NLIMITS; i++) {
        prp->rlimit_vals_soft[i] = kernel_threads_prp->rlimit_vals_soft[i];
        prp->rlimit_vals_hard[i] = kernel_threads_prp->rlimit_vals_hard[i];
    }
    prp->max_cpu_time = kernel_threads_prp->max_cpu_time;
    snap_time(&prp->start_time, 1);

    /* Install roottask at process_vector[ROOTTASK_PID].
     *
     * Why ROOTTASK_PID:
     *   - SYNC_OWNER(thp) = (pid<<16)|(tid+1) is stored in mutex->owner
     *     on a successful kernel-side lock; libc's user-side unlock
     *     compares it against TLS->__owner = (ROOTTASK_PID<<16)|1.
     *     Mismatched pids would leave mutexes permanently locked.
     *   - connect_attach(0, ROOTTASK_PID, ...) walks process_vector to
     *     find the target process_t; it must resolve to roottask.
     *   - getpid()/lookup_pid() must agree across the boundary.
     *
     * Eviction of kernel_threads_prp:
     *   Slot 1 was taken at boot by the kernel-thread pseudo-process
     *   (objects.c).  vector_rem removes it first, then vector_add installs
     *   roottask.  The pseudo-process struct stays alive in memory (idle
     *   threads reference it directly via thp->process); this only stops
     *   publishing it through process_vector.  If anything does
     *   lookup_pid(idle_thp->process->pid) and expects to get the
     *   pseudo-process back it will now see roottask — but nothing
     *   in the boot path does that.
     *
     * cred refresh:
     *   process_vector membership pre-supposes a real credentials
     *   block, not the shared kernel one.  The code already points at
     *   kernel_threads_prp->cred above; that's still valid.
     */
    prp->pid = ROOTTASK_PID;
    /* pgrp/session state lives in root_process_t; roottask owns that policy. */
    {
        void *evicted = vector_rem(&process_vector, ROOTTASK_PID);
        if (evicted != kernel_threads_prp)
            crash_halt(
                "ksys_start_roottask_umode: process_vector[%d] was %p, "
                "expected kernel_threads_prp (%p)\n",
                ROOTTASK_PID, evicted, (void *) kernel_threads_prp);
        if (vector_add(&process_vector, prp, ROOTTASK_PID) != ROOTTASK_PID)
            crash_halt(
                "ksys_start_roottask_umode: cannot install roottask "
                "at process_vector[%d]\n",
                ROOTTASK_PID);
    }

    /* (3) allocate the first roottask thread_t */
    thread_t *thp;
    if (!(thp = object_alloc(NULL, &thread_souls)))
        crash_halt("roottask launch: thread_souls alloc failed\n");
    memset(thp, 0, sizeof(*thp));

    if (vector_add(&prp->threads, thp, 0) != 0)
        crash_halt("roottask launch: cannot install thread at tid 0\n");

    thp->process = prp;
    thp->as_prp = prp;
    thp->state = STATE_DEAD; /* ready() does the transition */
    thp->policy = SCHED_RR;
    thp->dpp = thp->orig_dpp = prp->default_dpp;
    thp->priority = thp->real_priority = 10; /* roottask base prio */
    /* runmask semantics: a *cleared* bit means "may run on this CPU". 0 = any
     * CPU welcome. (Idle threads use ~(1<<cpu) — pinned to their own CPU only.)
     * A non-zero mask here would prevent the scheduler from ever picking the
     * thread.
     */
    thp->runmask = thp->default_runmask = 0;
    thp->type = TYPE_THREAD;

    /* Flags: NOMULTISIG inhibits signal multi-delivery quirks.
     * RTMUX_FLG_THR_WAAA is deliberately NOT set here even though that's the
     * conventional first-thread bit. WAAA triggers specialret() ->
     * thread_specret() at the next kernel_exit, and thread_specret tries to
     * allocate a kernel stack for the thread via proc_hooks -> mmap(MAP_STACK)
     * (roottask normally services this through proc_stack_code). At this point
     * roottask has never run, so proc_hooks->proc_stack_code is 0 and the
     * in-kernel roottask_stack_alloc fallback wires through the kernel mm_mmap.
     * Worse, thread_specret unready()s the thread to STATE_STACK, which flips
     * actives[cpu] back to the boot/idle thread — and __kernel_restore then
     * srets to idle()'s entry, restarting start_kernel_threads →
     * ksys_start_roottask → ksys_start_roottask_umode in an infinite loop.
     *
     * The entire register context is already populated (PC/SP/sstatus via
     * cpu_thread_enter_umode below), so there is nothing for thread_specret to
     * do; the cleanest answer is simply not to arm WAAA in the first place.
     * ALLOCED_STACK is set as defence- in-depth in case any future code path
     * checks "did this thread's stack come from proc service".
     */
    thp->flags = RTMUX_FLG_THR_NOMULTISIG | RTMUX_FLG_THR_ALLOCED_STACK;
    thp->last_chid = -1;
    thp->start_time = prp->start_time;
    thp->tid = 0;

    /* Kernel-side stack for trap save (sscratch points at &thp->reg which lives
     * inside the thread_t; the trap handler also needs a usable kernel SP for
     * the brief window before __kcall_dispatch switches to its per-CPU stack).
     */
    void *kstack = _scalloc(KSYS_STACK_NBYTES);
    if (!kstack)
        crash_halt("roottask launch: kernel-stack alloc failed\n");
    cpu_thread_init(NULL, thp, 0);
    SETKSP(thp, STACK_INIT((uintptr_t) kstack, KSYS_STACK_NBYTES));

    /* User entry + stack come straight from the ELF load. */
    thp->reg.pc = (uintptr_t) server_m->entry;
    SETREGSP(&thp->reg, server_m->user_sp);

    /* TLS pointer (== user-mode tp). initramfs_elf_load_sv39's static path
     * placed a 104-byte struct _thread_local_storage at the top of the user
     * stack and prefilled __pid = ROOTTASK_PID, __tid = 1, __errptr →
     * &__errval, __owner. Without this, roottask's crt0 faults the first time
     * it touches __tls()->__pid (in __rtmux_syschan_init -> getpid()): tp = 0,
     * NULL+0x20 load page-faults at U-mode sepc=0x3b7b0 stval=0x20.
     *
     * cpu_thread_enter_umode() copies un.lcl.tls into reg.gpr[TP], and
     * kernel_exit re-applies that on every dispatch (U-mode branch), so setting
     * it here is enough.
     */
    thp->un.lcl.tls = (struct _thread_local_storage *) server_m->tls_uva;

    /* Flip SPP=0 + scrub callee-saved gprs the loader populated. */
    cpu_thread_enter_umode(thp);

    /* Clear PROC_LOADING now that the first thread is fully constructed.
     * thread_create()'s gate at thread/lifecycle.c:282 keeps a process marked
     * PROC_LOADING in S-mode for new threads — appropriate for the loader
     * thread of a still-loading process, fatal for roottask whose pool threads
     * must run U-mode (roottask text is in the heap region with PTE_U=1, S-mode
     * fetch from PTE_U=1 is illegal regardless of SUM). The canonical mirror is
     * process_startup which clears LOADING after calling cpu_thread_enter_umode
     * on the first thread of a normal user process; the equivalent is done
     * here.
     */
    prp->flags &= ~RTMUX_FLG_PROC_LOADING;

    /* Pass user_sp in a0 so roottask's crt0 can build argc/argv from it.
     * cpu_thread_enter_umode zeros every GPR except PC/SP/TP, but the RTMux
     * crt0 (lib/libc/crt0.S) starts with "mv sp, a0; ld a0, 0(sp)" — it picks
     * up its stack from a0, *not* from the hardware sp. Without this, sp is
     * overwritten with 0 on the first instruction after the entry point, the
     * next ld faults at VA 0, producing an instruction/load page fault on the
     * very first U-mode step.
     *
     * The page at user_sp..user_sp+stack pages is already zero-filled by
     * initramfs_elf_load_sv39, so argc=0, argv[0]=NULL, envp[0]=NULL — exactly
     * what the in-kernel-launched roottask needs.
     */
    thp->reg.gpr[RISCV_REG_A0] = (uintptr_t) server_m->user_sp;

    /* Open the SMP dispatch gate. Today this is done by roottask via
     * __KSYS_PRIV_SMP_BRINGUP, but for the U-mode launch path roottask can't
     * get there — the gate is closed, so the scheduler refuses to pick its
     * thread (chicken-and-egg). Open it kernel-side for the boot CPU before
     * queueing the first U-mode thread. Roottask's own SMP bring-up will still
     * run later for AP wake-up.
     */
    {
        extern int smp_dispatch_enabled;
        smp_dispatch_enabled = 1;
    }

    /* Pool-thread stack cache is now populated by roottask itself in
     * message_start(): it as_mmap's user-VA stack pages and registers each via
     * __KSYS_PRIV_REGISTER_POOL_STACK. Doing it from the kernel via _scalloc
     * was the old shortcut and required the entire kernel heap to be PTE_U=1,
     * which leaked every kernel object to every U-mode process.
     * roottask_stack_cache now starts NULL; thread_specret blocks the first
     * roottask thread here only if the pool cache is empty AND its size doesn't
     * match — which never happens once message_start has run.
     */

    /* Wire vm_ops->as to the kernel-resident as_switch before scheduling the
     * first U-mode thread. kernel_exit calls vm_ops->as() on every context
     * switch; without this, the first dispatch jumps through a NULL slot and
     * faults at sepc=0.
     */
    {
        extern void as_switch(process_t *, process_t **);
        vm_ops->as = as_switch;
    }

    /* (4) into the run queue. ready() is a function pointer; with the default
     * scheduler it's ready_default() — adds to per-prio dispatch queue and
     * wakes an idle CPU if one's available.
     */
    ready(thp);
}


/* Launch roottask after boot.
 *
 * Returns to the caller (idle()) once the roottask thread has been queued.
 */
void ksys_start_roottask(void)
{
    ksys_start_roottask_umode();

    /* Whether or not roottask was launched, the boot thread returns to idle().
     * In the U-mode path the scheduler will eventually pick the roottask thread
     * on this or another CPU.
     */
}
