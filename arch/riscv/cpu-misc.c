/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* RISC-V CPU-specific kernel functions. */

#include <core/globals.h>
#include <sys/storage.h>
#include <sbi.h>
#include <cpu-inline.h>
#include <stdatomic.h>

static void arch_timer_program_next(struct qtime_entry *qtp, int idle);

/* CPU specific initialization of syspage pointers & stuff. Nothing
 * RISC-V-specific needed for the standalone kernel test.
 */
void cpu_syspage_init(void)
{
    /* NYI: RISC-V-specific syspage init */
}

/* cpu_thread_init -- initialize a new thread's saved register context.
 *
 * Zeroes the entire register struct, then sets tp (for TLS access) and sstatus
 * (S-mode with SPP=1 for kernel threads; cleared to SPP=0 later by
 * cpu_thread_enter_umode() for user-mode threads).
 *
 * \note Must be called BEFORE SETKIP/SETKSP, since the memset
 *       zeroes the whole reg struct including pc and sp.
 */
void cpu_thread_init(thread_t *act, thread_t *thp, int align)
{
    (void) act;
    (void) align;

    /* Zero all saved registers so __kernel_restore doesn't load garbage. The
     * kernel allocator (object_alloc) does not zero thread structs.
     */
    memset(&thp->reg, 0, sizeof(thp->reg));

    /* Set tp register in saved context so __kernel_restore puts the right TLS
     */
    thp->reg.gpr[RISCV_REG_TP] = (uintptr_t) thp->un.lcl.tls;

    /* Default to S-mode (SPP=1) — kernel/roottask threads.
     * cpu_thread_enter_umode() clears SPP for user-mode threads.
     */
    thp->reg.sstatus = SSTATUS_SPP | SSTATUS_SPIE | SSTATUS_SUM;
}

/* CPU specific stuff for putting a thread into execution for the first time. */
void cpu_thread_waaa(thread_t *thp)
{
    /* Update tp in saved register context to match (re)initialized TLS */
    thp->reg.gpr[RISCV_REG_TP] = (uintptr_t) thp->un.lcl.tls;

    /* Pass the thread argument in a0 (C calling convention) */
    thp->reg.gpr[RISCV_REG_A0] = (uintptr_t) thp->un.lcl.tls->__arg;

    /* Set ra so that returning from the start function lands in __exitfunc
     * (typically __my_thread_exit → pthread_exit)
     */
    if (thp->un.lcl.tls->__exitfunc)
        thp->reg.gpr[RISCV_REG_RA] = (uintptr_t) thp->un.lcl.tls->__exitfunc;
}

/* CPU specific stuff for allowing a thread to do privileged operations. */
void cpu_thread_priv(thread_t *thp)
{
    /* NYI: In RISC-V, privilege is mode-based, not flag-based */
    (void) thp;
}

/* CPU specific stuff for destroying a thread. */
void cpu_thread_destroy(thread_t *thp)
{
    /* RISC-V has no per-thread perf counter save area yet. */
    (void) thp;
}

/* CPU specific stuff for allowing/disallowing alignment faults on a thread. */
void cpu_thread_align_fault(thread_t *thp)
{
    /* NYI: RISC-V does not have a per-thread alignment check flag. */
    (void) thp;
}

/* cpu_signal_save/restore removed — signal subsystem eliminated */

/* CPU specific stuff for saving registers needed for interrupt delivery. */
void cpu_intr_attach(interrupt_t *itp, thread_t *thp)
{
    /* Nothing needed for RISC-V */
    (void) itp;
    (void) thp;
}

/* Copy register set, ensuring no security holes. For RISC-V, this is a simple
 * memcpy since there are no privileged bits in the general register set.
 */
void cpu_greg_load(thread_t *thp, CPU_REGISTERS *regs)
{
    thp->reg = *regs;
}

/* CPU specific code to cause a thread to invoke a user function. Similar to
 * MIPS: just set the instruction pointer.
 */
void cpu_invoke_func(thread_t *thp, uintptr_t addr)
{
    SETKIP(thp, addr);
}

/* CPU specific stuff for process startup. */
/* cpu_thread_enter_umode — reset a thread's register context for user-mode
 * execution.
 *
 * Called from process_startup when a thread that previously ran the kernel ELF
 * loader (in S-mode) is being repurposed as the initial user-mode thread of a
 * process. Its saved GPRs still contain kernel addresses from the loader's
 * stack frame; we zero them to prevent kernel address leaks, then restore PC
 * and SP which process_startup has already set.
 *
 * Sets sstatus so that __kernel_restore → sret enters U-mode (SPP=0).
 *
 * \note In the future, a process may start with multiple threads
 *       (OS/2-style). Each thread entering user mode must go through
 *       this function independently.
 */
void cpu_thread_enter_umode(thread_t *thp)
{
    /* Save the values process_startup already set */
    uintptr_t saved_pc = thp->reg.pc;
    uintptr_t saved_sp = REGSP(&thp->reg);

    /* Zero all GPRs — the loader left kernel addresses in s0-s11 */
    memset(&thp->reg.gpr, 0, sizeof(thp->reg.gpr));

    /* Restore PC and SP */
    thp->reg.pc = saved_pc;
    SETREGSP(&thp->reg, saved_sp);

    /* Set tp for the user thread's TLS */
    thp->reg.gpr[RISCV_REG_TP] = (uintptr_t) thp->un.lcl.tls;

    /* U-mode: SPP=0, SPIE=1 (enable interrupts after sret), SUM=1, FS=Off. FP
     * instructions trap until the thread's first one loads its context (see
     * fpu_first_use in trap-handler.c), so a thread that never uses FP never
     * owns the FP registers.
     */
    thp->reg.sstatus = SSTATUS_SPIE | SSTATUS_SUM | SSTATUS_FS_OFF;

    /* A new image must not see the old one's f0-f31 or fcsr (exec reuses the
     * thread): zero its save area and drop its claim on any hart's registers,
     * so the first FP instruction loads zeros.
     */
    if (thp->fpudata) {
        fpu_drop_claims(thp);
        memset(FPUDATA_PTR(thp->fpudata), 0, sizeof(FPU_REGISTERS));
    }
}

/* cpu_console_getc — read one character from the kernel console.
 *
 * Return value: the character read, or a negative SBI error if none is
 * available.
 */
int cpu_console_getc(void)
{
    return sbi_ecall_console_getc();
}

/* idle_run_thread — jump directly into a thread's start function.
 *
 * Loads the thread's saved stack pointer, return address, first argument, and
 * entry point and transfers control with no return path. Used to launch the
 * very first thread from the idle context.
 *
 * Parameters:
 *   thp : thread to begin executing
 */
void __attribute__((noreturn)) idle_run_thread(thread_t *thp)
{
    uintptr_t sp = KSP(thp);
    uintptr_t ip = KIP(thp);
    uintptr_t ra = CPU_REG_RA(&thp->reg);
    uintptr_t arg = CPU_REG_ARG(&thp->reg, 0);

    __asm__ volatile(
        "mv sp, %0\n"
        "mv ra, %1\n"
        "mv a0, %2\n"
        "jr %3\n" ::"r"(sp),
        "r"(ra), "r"(arg), "r"(ip)
        : "memory");
    __builtin_unreachable();
}

/* CPU specific stuff for cleaning up before rebooting. Uses SBI SRST (System
 * Reset) extension for cold reboot.
 */
void cpu_reboot(void)
{
    sbi_system_reset(SBI_SRST_RESET_TYPE_COLD_REBOOT,
                     SBI_SRST_RESET_REASON_NONE);
    /* If SRST fails, fall back to the SBI v0.1 shutdown call. */
    SBI_ECALL_0(SBI_EXT_0_1_SHUTDOWN);
}

/* CPU specific stuff for powering off the machine. Uses SBI SRST shutdown —
 * QEMU will exit its process.
 */
void cpu_shutdown(void)
{
    sbi_system_reset(SBI_SRST_RESET_TYPE_SHUTDOWN, SBI_SRST_RESET_REASON_NONE);
    /* If SRST fails, fall back to the SBI v0.1 shutdown call. */
    SBI_ECALL_0(SBI_EXT_0_1_SHUTDOWN);
}

/* cpu_start_others — start secondary CPUs via SBI HSM. start = address of
 * _smpstart; ~0UL means shutdown (no-op).
 */
void cpu_start_others(uintptr_t start)
{
    if (start == ~0UL)
        return;

    /* SBI needs a physical address; convert if we're running at high VA */
    if (CPU_VADDR_IN_RANGE(start))
        start = CPU_V2P(start);

    extern unsigned hart_ids[];
    for (unsigned i = 0; i < num_processors; i++) {
        unsigned hart = hart_ids[i];
        if (hart != get_cpunum()) {
            long ret = sbi_hart_start(hart, start, hart);
            if (ret == SBI_SUCCESS)
                kprintf("cpu_start_others: hart %u started\n", hart);
            else if (ret != SBI_ERR_ALREADY_AVAILABLE)
                kprintf("cpu_start_others: hart %u failed: %ld\n", hart, ret);
        }
    }

#ifdef CONFIG_MONITORING_KTHREAD
    if (monitor_hart_id != ~0U) {
        extern void _monitor_start(void);
        uintptr_t mstart = (uintptr_t) _monitor_start;
        if (CPU_VADDR_IN_RANGE(mstart))
            mstart = CPU_V2P(mstart);
        long ret = sbi_hart_start(monitor_hart_id, mstart, monitor_hart_id);
        if (ret == SBI_SUCCESS)
            kprintf("cpu_start_others: monitor hart %u started\n",
                    monitor_hart_id);
        else if (ret != SBI_ERR_ALREADY_AVAILABLE)
            kprintf("cpu_start_others: monitor hart %u failed: %ld\n",
                    monitor_hart_id, ret);
    }
#endif
}

/* CPU specific code to force FPU save: write this hart's live FP registers
 * into the thread's save area. The caller has checked that thp's context is the
 * one loaded on this hart.
 */
void cpu_force_fpu_save(thread_t *thp)
{
    if (thp->fpudata)
        cpu_fpu_save(FPUDATA_PTR(thp->fpudata));
}

/* CPU specific mutex adjustment. */
void cpu_mutex_adjust(thread_t *thp)
{
    /* Nothing needed for RISC-V */
    (void) thp;
}

/* next_instruction: return address of the next instruction after current PC.
 * Used for single-stepping. Stub for now.
 */
uintptr_t next_instruction(CPU_REGISTERS *ctx)
{
    /* NYI: needs to decode instruction length (2 for compressed, 4 for normal)
     */
    return ctx->pc + 4;
}

/* RISC-V cause-to-signal mapping table. Maps RISC-V exception causes to signal
 * numbers.
 */
RISCV_CAUSE2SIGMAP_CONST unsigned long __riscv_cause2sig_map[] = {
    0, /* placeholder — NYI */
};


/* Functions below were moved from the old call stub file (arch-specific code
 * that belongs in the per-CPU source file).
 */

/* clock_cycles — read the RISC-V cycle counter. */
uint64_t clock_cycles(void)
{
    uint64_t cycles;
    __asm__ volatile("rdcycle %0" : "=r"(cycles));
    return cycles;
}

/* cpu_clock_ticks — read the RISC-V time counter.
 *
 * The time CSR is the counter qtimeptr->cycles_per_sec describes, so a span
 * measured with this converts to nanoseconds exactly. Use it, not
 * clock_cycles(), wherever a duration leaves the kernel as nanoseconds: the
 * cycle CSR is a separate counter whose rate is not cycles_per_sec, and under
 * QEMU it is host-derived rather than guest-timer-derived. The cost is
 * resolution: on QEMU virt the time base is 10 MHz, so one tick is 100 ns.
 */
uint64_t cpu_clock_ticks(void)
{
    uint64_t ticks;
    __asm__ volatile("rdtime %0" : "=r"(ticks));
    return ticks;
}

/* halt — halt the processor (used by crash/panic paths). */
void halt(void)
{
    for (;;) {
        __asm__ volatile("wfi");
    }
}

/* send_ipi — send an inter-processor interrupt.
 *
 * OR the command bits into the target hart's pending-IPI word and call
 * sbi_send_ipi unconditionally. An "if old==0, fire SBI" optimization would
 * race the receiver's ipi_handler: if a sender's atomic_or runs between the
 * receiver clearing sip.SSIP and TAS-clearing ipicmds, the sender could see old
 * != 0 and skip the SBI call, leaving bits pending in ipicmds with no hardware
 * notification. The unconditional ECALL costs only tens of cycles and avoids
 * that lost-wakeup.
 *
 * Parameters:
 *   cpu : hart ID of the target CPU (ipicmds[] is hart-indexed and
 *         sbi_send_ipi takes a hart mask)
 *   cmd : IPI command bits to OR into the target's pending word
 */
void rdecl send_ipi(unsigned cpu, int cmd)
{
    if (num_processors <= 1)
        return;

    if (cmd != 0) {
        atomic_fetch_or_explicit(&pcpu_array[cpu].pc_ipicmds, (unsigned) cmd,
                                 memory_order_release);
    }
    sbi_send_ipi(1UL << cpu, 0);
}

/* cpu_icache_sync_all — make freshly-written instructions visible to
 * instruction fetch on EVERY hart.
 *
 * RISC-V keeps no coherence between data stores and the I-cache: code written
 * via ordinary stores (e.g. a loader reading a program's text into
 * freshly-allocated pages) is invisible to a hart's fetch unit until that hart
 * executes fence.i. When a page is recycled, a hart's I-cache can still hold
 * the previous tenant's lines for the same physical address, so a
 * freshly-loaded program dispatched to that hart fetches stale instructions and
 * jumps into garbage.
 *
 * The loading hart fences locally; the others are fenced via the SBI
 * REMOTE_FENCE_I broadcast so the program is safe to run wherever the scheduler
 * later dispatches it. (On a single-hart system the local fence is sufficient
 * and the broadcast is skipped.)
 */
void cpu_icache_sync_all(void)
{
    extern unsigned hart_ids[];
    unsigned self = get_cpunum();
    unsigned long mask = 0;
    int i;

    cpu_icache_sync(); /* local fence.i */

    if (num_processors <= 1)
        return;

    for (i = 0; i < num_processors; ++i) {
        unsigned h = hart_ids[i];
        if (h != self && pcpu_array[h].pc_alive)
            mask |= (1UL << h);
    }
    if (mask != 0)
        sbi_remote_fence_i(mask, 0);
}

/* smp_flush_tlb — flush TLB across all CPUs. */
void rdecl smp_flush_tlb(void)
{
    __asm__ volatile("sfence.vma" ::: "memory");
}

/* cpu_idle — per-CPU idle loop (all CPUs are treated equally). Timer interrupts
 * are enabled so clock_handler runs on all CPUs.
 */
void cpu_idle(unsigned cpu)
{
    interrupt_disable();
    /* tp is invariant per hart (set once at bootstrap, never reassigned in
     * S-mode), so thread migration cannot leave it stale. ksys_exit's
     * idle-fallback path stamps tp = &pcpu_array[cpu] before tail-calling here.
     */

    /* Discard every translation cached during _smpstart bring-up before we
     * publish pc_alive. cpu_tlb_flush_complete() skips not-yet-alive harts in
     * its cross-hart shootdown; this sfence is the other half of that contract
     * — once a hart is alive its TLB holds nothing stale, so a concurrent
     * flusher that skipped it (saw pc_alive == false) left no stale entry
     * behind. _smpstart already sfences right after writing satp; this covers
     * anything walked between that point and here.
     */
    sfence_vma();
    pcpu_array[cpu].pc_alive = true;

    /* Set PLIC threshold to 0 so this hart can receive external interrupts.
     * Must be done per-hart.
     */
    {
        extern void plic_inithart(void);
        plic_inithart();
    }

    /* Enable timer + IPI in sie so wfi wakes on either. Keep global interrupts
     * DISABLED (sstatus.SIE=0) except inside wfi — enabling interrupts while
     * spinning in acquire_kernel() or while setting up actives[] causes trap
     * handler re-entry on the same kernel stack, corrupting the stack frame.
     */
    w_sie(r_sie() | SIE_SSIE | SIE_SEIE | (kopt.disable_timer ? 0 : SIE_STIE));

    for (;;) {
        /* Idle state convention: while we're in this loop (no real thread has
         * the CPU), the idle thread for this CPU is conceptually RUNNING. The
         * state field is flipped to READY just before we dispatch a real thread
         * below, so /proc readers reflect the real picture.
         */
        if (pcpu_array[cpu].pc_idle_thp)
            pcpu_array[cpu].pc_idle_thp->state = STATE_RUNNING;

        /* If a thread was already mark_running()'d for us before we entered the
         * loop (the U-mode roottask bring-up path does this from kernel
         * context, with no trap-return to consume actives[]), dispatch it
         * directly — idle_pick_thread won't find it because it's not on the
         * dispatch queue.
         */
        {
            thread_t *pre = get_active_thread(cpu);
            if (pre && pre != pcpu_array[cpu].pc_idle_thp &&
                pre->state == STATE_RUNNING) {
                if (pcpu_array[cpu].pc_idle_thp)
                    pcpu_array[cpu].pc_idle_thp->state = STATE_READY;
                pcpu_array[cpu].pc_active_prp = pre->process;
                pre->runcpu = cpu;
                if (!kopt.disable_timer)
                    arch_timer_reload(qtimeptr);
                in_kcall_inc();
                in_kcall_phase(IN_KCALL_PHASE_DISPATCH);
                kernel_exit();
                /* not reached — kernel_exit decrements in_kcall. */
            }
        }

        thread_t *thp = idle_pick_thread(cpu);
        if (thp) {
            if (!thp->process) {
                kprintf(
                    "FATAL: cpu_idle cpu=%u picked thread %p tid=%d "
                    "process=NULL\n",
                    cpu, (void *) thp, thp->tid);
                crash();
            }
            /* Idle is about to be displaced by a real thread. Flip its state to
             * READY so /proc readers show it in the runqueue-visible ready
             * bucket.
             */
            if (pcpu_array[cpu].pc_idle_thp)
                pcpu_array[cpu].pc_idle_thp->state = STATE_READY;

            /* thp->state and thp->runcpu were set under sched_slock inside
             * idle_pick_thread() — don't re-write here. The per-CPU slots below
             * are single-writer (this CPU only).
             */
            sched_account_restart(); /* idle time is billed to nobody */
            set_active_thread(cpu, thp);
            pcpu_array[cpu].pc_active_prp = thp->process;

            if (!kopt.disable_timer)
                arch_timer_reload(qtimeptr);

            in_kcall_inc();
            in_kcall_phase(IN_KCALL_PHASE_DISPATCH);
            kernel_exit();
            /* kernel_exit never returns — the thread resumes from its saved
             * context and eventually re-enters cpu_idle when it blocks and no
             * other thread is runnable. kernel_exit decrements in_kcall before
             * __kernel_restore.
             */
        } else {
            /* Nothing to dispatch: this hart is going idle. Point its active
             * slot at the (permanent, never-freed) idle thread so pc_curthread
             * does not lag, still naming the last real thread we ran. A stale
             * pc_curthread on an idle hart is the freed-active UAF: when that
             * last thread is later destroyed, the destroyer's guards see this
             * hart "still running" it and either defer forever or free it out
             * from under our next trap. The idle thread is never freed, so the
             * free-site check (in thread_destroy) can never match it.
             */
            if (pcpu_array[cpu].pc_idle_thp)
                set_active_thread(cpu, pcpu_array[cpu].pc_idle_thp);

            /* Deferred-timer-work drain — same as kernel_exit. timer_pending
             * needs INKERNEL_NOW set, so bump in_kcall to satisfy that.
             * Sigevents from IRQs deliver inline from their producer, so no
             * IRQ-side drain is needed here.
             *
             * CAS-claim (timer_drain_busy): only one hart at a time enters
             * timer_pending; others see the busy flag and skip, avoiding a
             * multi-hart livelock on the timer queue.
             */
            int expected = 0;
            if (timer_ops_delayed &&
                atomic_compare_exchange_strong_explicit(
                    &timer_drain_busy, &expected, 1, memory_order_acq_rel,
                    memory_order_acquire)) {
                in_kcall_inc();
                in_kcall_phase(IN_KCALL_PHASE_TIMER);
                interrupt_enable();
                timer_pending();
                interrupt_disable();
                in_kcall_dec();
                atomic_store_explicit(&timer_drain_busy, 0,
                                      memory_order_release);
                continue;
            }

            /* Clear the per-CPU pending-preempt-prio before we wfi. An IRQ that
             * fires during the wfi delivers its sigevent inline (no queue), so
             * the bit is restored fresh by the trap if a wakeup happens.
             */
            pcpu_array[cpu].pc_pending_preempt_prio = 0;

            /* Drop to the long-lived roottask (pid 1) address space before
             * sleeping. Otherwise an idle hart retains the last user thread's
             * as, and when that process later exits the shutdown path must
             * chase us off the dying pgdir via a cross-hart IPI. Releasing the
             * as here, in our own context, lets the shutdown path's "still on
             * the dying as?" test never match an idle hart. roottask's pgdir is
             * never freed during normal operation, and the kernel is mapped
             * high-half in every as, so this loop keeps running. Guarded so we
             * only pay the satp write + sfence on the user->idle transition,
             * not every wfi. (Skipped before roottask exists, or before its
             * address space is ready.)
             */
            if (roottask_prp && roottask_prp->memory &&
                pcpu_array[cpu].pc_as_prp != roottask_prp) {
                vm_ops->as(roottask_prp, &pcpu_array[cpu].pc_as_prp);
            }

            /* Arm a periodic tick before sleeping. The idle STIMER path in
             * trap-handler.c does not re-arm the timer, so without this an idle
             * hart's timer would fire only once (from its last dispatch) and
             * the hart would sleep in wfi with no heartbeat, rousable only by
             * an IPI. Re-arming on every wfi-entry guarantees an idle hart
             * wakes within one timer_load and runs the trap-entry IPI pre-drain
             * — dropping a dying as, picking up a freshly-readied thread —
             * without depending on the IPI itself to wake the hart.
             */
            if (!kopt.disable_timer)
                arch_timer_program_next(qtimeptr, 1);

            /* Final re-pick before sleeping — closes an idle lost-wakeup race.
             * Between idle_pick_thread() at the top of the loop and here, the
             * idle prologue (timer drain, as drop, timer arm) runs with
             * interrupts disabled. Another hart can ready a thread onto THIS
             * hart's dispatch and SENDIPI(cpu, IPI_RESCHED) during that window;
             * the IPI is then delivered by the interrupt_enable() below and its
             * pending bit cleared, so a plain wfi would sleep with a runnable
             * thread stranded on our queue until the next coarse timer tick
             * (or, under a long tick, far longer). Re-pick with interrupts
             * still disabled and dispatch instead of sleeping.
             * idle_pick_thread() honours runmask, so this cannot spin on an
             * unpickable thread.
             */
            {
                thread_t *late = idle_pick_thread(cpu);
                if (late) {
                    if (!late->process)
                        crash();
                    if (pcpu_array[cpu].pc_idle_thp)
                        pcpu_array[cpu].pc_idle_thp->state = STATE_READY;
                    sched_account_restart();
                    set_active_thread(cpu, late);
                    pcpu_array[cpu].pc_active_prp = late->process;
                    if (!kopt.disable_timer)
                        arch_timer_reload(qtimeptr);
                    in_kcall_inc();
                    in_kcall_phase(IN_KCALL_PHASE_DISPATCH);
                    kernel_exit(); /* not reached */
                }
            }

            /* No thread, no pending events: sleep until interrupt. wfi runs
             * with interrupts still masked (SSTATUS.SIE=0): RISC-V resumes wfi
             * on any locally enabled pending interrupt regardless of the global
             * enable, so a reschedule IPI that lands between the final re-pick
             * above and here stays pending and wakes wfi immediately, instead
             * of being taken by a handler that returns straight into wfi and
             * then sleeps on a stranded thread until the
             * IDLE_MAX_SLEEP_NS-capped STIMER backstop fires. After waking,
             * open a window so the pending interrupt is actually serviced
             * (readying threads, PLIC claim/complete), then loop to re-pick.
             */
            rcu_sched_quiescent();
            __asm__ volatile("wfi" ::: "memory");
            interrupt_enable();
            interrupt_disable();
        }
    }
}

/* VA offset for switching secondary CPUs from PA to high VA. Stored as a
 * variable because loading a 64-bit constant in RISC-V assembly requires many
 * instructions; a single ld is simpler.
 */
const uint64_t kern_va_offset __attribute__((used)) = KERNEL_VIRT_BASE;

/* _smpstart — assembly entry point for all CPUs except the boot CPU. Called by
 * SBI HSM with a0=hartid, a1=opaque (also hartid).
 *
 * Sets up MMU, switches to high VA (same as the boot CPU), then calls
 * cpu_idle() at the high VA. All CPUs are equal — there is no AP/BP
 * distinction.
 */
void __attribute__((naked)) _smpstart(void)
{
    __asm__ volatile(
        /* a0 = hartid from SBI. sscratch = 0 (no U-mode context yet). tp will
         * be set to &pcpu_array[hartid] after MMU is enabled (see below).
         */
        "csrw sscratch, zero\n"

        /* Set SSTATUS_SUM so S-mode code on this AP can touch PTE_U pages. Boot
         * CPU does this in init_cpu(), but APs never reach that function — they
         * jump directly to cpu_idle. Without this, scause=15 faults on user-VA
         * accesses from kernel handlers invoked on behalf of user threads.
         */
        "li   t0, 0x40000\n" /* SSTATUS_SUM bit */
        "csrs sstatus, t0\n"

        /* Enable MMU — same page tables as the boot CPU. Identity mapping keeps
         * this PA-based code running after satp is written.
         */
        "la   t2, boot_satp_val\n"
        "ld   t2, 0(t2)\n"
        "csrw satp, t2\n"
        "sfence.vma\n"

        /* Load the VA offset (KERNEL_VIRT_BASE) for PA→VA switch. All addresses
         * obtained via "la" below are PA (PC-relative from our PA execution
         * context). We add the offset to get the corresponding high VA.
         */
        "la   t3, kern_va_offset\n"
        "ld   t3, 0(t3)\n" /* t3 = KERNEL_VIRT_BASE */

        /* Set stvec to HIGH VA of trap handler */
        "la   t0, __strap_entry\n"
        "add  t0, t0, t3\n"
        "csrw stvec, t0\n"

        /* Set up per-CPU kernel stack (already at high VA) */
        "la   t0, ksys_stack\n"
        "add  t0, t0, t3\n" /* ksys_stack VA */
        "slli t1, a0, 3\n"  /* t1 = hartid * 8 */
        "add  t0, t0, t1\n"
        "ld   sp, 0(t0)\n" /* sp = ksys_stack[hartid] */

        /* tp = &pcpu_array[hartid]. pcpu_array[hartid].pc_hart was populated by
         * kernel_entry on the boot CPU before this AP was released, so
         * get_cpunum() = "lw PC_HART(tp)" returns the correct hart ID
         * immediately.
         */
        "la   t0, pcpu_array\n"
        "add  t0, t0, t3\n"            /* pcpu_array VA */
        "slli t1, a0, %[pcpu_shift]\n" /* t1 = hartid * sizeof(struct pcpu) */
        "add  tp, t0, t1\n"            /* tp = &pcpu_array[hartid] */

        /* Jump to cpu_idle at its HIGH VA. This is the PA→VA switch point —
         * after this jump, the CPU executes at high VAs just like the boot CPU.
         * a0 still holds hartid.
         */
        "la   t0, cpu_idle\n"
        "add  t0, t0, t3\n" /* VA of cpu_idle */
        "jr   t0\n"         /* jump; a0 = hartid */

        /* Should never reach here */
        "1: wfi\n"
        "   j 1b\n" ::[pcpu_shift] "i"(__builtin_ctz(sizeof(struct pcpu))));
}

/* RISC-V atomic operations (AMO instructions). */

#include <atomic.h>

/* atomic_or — atomically OR bits into *loc (emits amoor.w).
 *
 * For an actual atomic store, use _smp_xchg (32-bit) or _smp_xchg_ul (64-bit)
 * and discard the return value.
 *
 * Parameters:
 *   loc  : word to modify
 *   bits : bits to set
 */
void atomic_or(volatile unsigned *loc, unsigned bits)
{
    __asm__ volatile("amoor.w zero, %1, (%0)" ::"r"(loc), "r"(bits) : "memory");
}

void atomic_clr(volatile unsigned *loc, unsigned bits)
{
    __asm__ volatile("amoand.w zero, %1, (%0)" ::"r"(loc), "r"(~bits)
                     : "memory");
}

void atomic_add(volatile unsigned *loc, unsigned incr)
{
    __asm__ volatile("amoadd.w zero, %1, (%0)" ::"r"(loc), "r"(incr)
                     : "memory");
}

void atomic_sub(volatile unsigned *loc, unsigned decr)
{
    unsigned neg = (unsigned) (-(int) decr);
    __asm__ volatile("amoadd.w zero, %1, (%0)" ::"r"(loc), "r"(neg) : "memory");
}

void atomic_toggle(volatile unsigned *loc, unsigned bits)
{
    __asm__ volatile("amoxor.w zero, %1, (%0)" ::"r"(loc), "r"(bits)
                     : "memory");
}

unsigned atomic_add_value(volatile unsigned *loc, unsigned incr)
{
    unsigned old;
    __asm__ volatile("amoadd.w.aqrl %0, %2, (%1)"
                     : "=r"(old)
                     : "r"(loc), "r"(incr)
                     : "memory");
    return old;
}

unsigned atomic_sub_value(volatile unsigned *loc, unsigned decr)
{
    unsigned neg = (unsigned) (-(int) decr);
    unsigned old;
    __asm__ volatile("amoadd.w.aqrl %0, %2, (%1)"
                     : "=r"(old)
                     : "r"(loc), "r"(neg)
                     : "memory");
    return old;
}

unsigned atomic_or_value(volatile unsigned *loc, unsigned bits)
{
    unsigned old;
    __asm__ volatile("amoor.w.aqrl %0, %2, (%1)"
                     : "=r"(old)
                     : "r"(loc), "r"(bits)
                     : "memory");
    return old;
}

unsigned atomic_clr_value(volatile unsigned *loc, unsigned bits)
{
    unsigned old;
    __asm__ volatile("amoand.w.aqrl %0, %2, (%1)"
                     : "=r"(old)
                     : "r"(loc), "r"(~bits)
                     : "memory");
    return old;
}

unsigned atomic_toggle_value(volatile unsigned *loc, unsigned bits)
{
    unsigned old;
    __asm__ volatile("amoxor.w.aqrl %0, %2, (%1)"
                     : "=r"(old)
                     : "r"(loc), "r"(bits)
                     : "memory");
    return old;
}

/* 64-bit (doubleword) atomic operations using .d AMO instructions. */

void atomic_add_64(volatile unsigned long *loc, unsigned long val)
{
    __asm__ volatile("amoadd.d zero, %1, (%0)" ::"r"(loc), "r"(val) : "memory");
}

unsigned long atomic_add_value_64(volatile unsigned long *loc,
                                  unsigned long val)
{
    unsigned long old;
    __asm__ volatile("amoadd.d.aqrl %0, %2, (%1)"
                     : "=r"(old)
                     : "r"(loc), "r"(val)
                     : "memory");
    return old;
}

void atomic_sub_64(volatile unsigned long *loc, unsigned long val)
{
    unsigned long neg = (unsigned long) (-(long) val);
    __asm__ volatile("amoadd.d zero, %1, (%0)" ::"r"(loc), "r"(neg) : "memory");
}

unsigned long atomic_sub_value_64(volatile unsigned long *loc,
                                  unsigned long val)
{
    unsigned long neg = (unsigned long) (-(long) val);
    unsigned long old;
    __asm__ volatile("amoadd.d.aqrl %0, %2, (%1)"
                     : "=r"(old)
                     : "r"(loc), "r"(neg)
                     : "memory");
    return old;
}

void atomic_or_64(volatile unsigned long *loc, unsigned long bits)
{
    __asm__ volatile("amoor.d zero, %1, (%0)" ::"r"(loc), "r"(bits) : "memory");
}

unsigned long atomic_or_value_64(volatile unsigned long *loc,
                                 unsigned long bits)
{
    unsigned long old;
    __asm__ volatile("amoor.d.aqrl %0, %2, (%1)"
                     : "=r"(old)
                     : "r"(loc), "r"(bits)
                     : "memory");
    return old;
}

void atomic_clr_64(volatile unsigned long *loc, unsigned long bits)
{
    __asm__ volatile("amoand.d zero, %1, (%0)" ::"r"(loc), "r"(~bits)
                     : "memory");
}

unsigned long atomic_clr_value_64(volatile unsigned long *loc,
                                  unsigned long bits)
{
    unsigned long old;
    __asm__ volatile("amoand.d.aqrl %0, %2, (%1)"
                     : "=r"(old)
                     : "r"(loc), "r"(~bits)
                     : "memory");
    return old;
}

void atomic_toggle_64(volatile unsigned long *loc, unsigned long bits)
{
    __asm__ volatile("amoxor.d zero, %1, (%0)" ::"r"(loc), "r"(bits)
                     : "memory");
}

unsigned long atomic_toggle_value_64(volatile unsigned long *loc,
                                     unsigned long bits)
{
    unsigned long old;
    __asm__ volatile("amoxor.d.aqrl %0, %2, (%1)"
                     : "=r"(old)
                     : "r"(loc), "r"(bits)
                     : "memory");
    return old;
}

/* SMP exchange/compare-exchange using RISC-V LR/SC. */
unsigned _smp_xchg(volatile unsigned *loc, unsigned val)
{
    unsigned old;
    __asm__ volatile("amoswap.w.aqrl %0, %1, (%2)"
                     : "=r"(old)
                     : "r"(val), "r"(loc)
                     : "memory");
    return old;
}

unsigned _smp_cmpxchg(volatile unsigned *loc, unsigned old, unsigned new)
{
    unsigned prev, tmp;
    __asm__ volatile(
        "1: lr.w.aq %0, (%2)\n"
        "   bne %0, %3, 2f\n"
        "   sc.w.rl %1, %4, (%2)\n"
        "   bnez %1, 1b\n"
        "2:"
        : "=&r"(prev), "=&r"(tmp)
        : "r"(loc), "r"(old), "r"(new)
        : "memory");
    return prev;
}

unsigned _mux_smp_cmpxchg(volatile unsigned *loc, unsigned old, unsigned new)
{
    return _smp_cmpxchg(loc, old, new);
}

/* 64-bit (unsigned long) variants for LP64. */
unsigned long _smp_xchg_ul(volatile unsigned long *loc, unsigned long val)
{
    unsigned long old;
    __asm__ volatile("amoswap.d.aqrl %0, %1, (%2)"
                     : "=r"(old)
                     : "r"(val), "r"(loc)
                     : "memory");
    return old;
}

unsigned long _mux_smp_cmpxchg_ul(volatile unsigned long *loc,
                                  unsigned long old,
                                  unsigned long new)
{
    unsigned long prev, tmp;
    __asm__ volatile(
        "1: lr.d.aq %0, (%2)\n"
        "   bne %0, %3, 2f\n"
        "   sc.d.rl %1, %4, (%2)\n"
        "   bnez %1, 1b\n"
        "2:"
        : "=&r"(prev), "=&r"(tmp)
        : "r"(loc), "r"(old), "r"(new)
        : "memory");
    return prev;
}

/* arch_timer_value — remaining ticks until next interrupt. Only used by
 * profiling/debug, not critical for basic operation.
 */
unsigned arch_timer_value(struct qtime_entry *qtp)
{
    (void) qtp;
    return 0;
}

static uint64_t timer_base_cycle;
static uint64_t timer_base_nsec;
/* Seqlock guarding the (timer_base_cycle, timer_base_nsec) pair. Even means
 * stable, odd means an update is in progress. arch_timer_now_ns runs on every
 * hart now (snap_time and SNAP_TIME_INLINE read it for CLOCK_MONOTONIC), so a
 * clock_resolution() base update on the boot CPU must not hand a reader on
 * another hart a torn pair (new cycle with old nsec, or vice versa), which
 * would make CLOCK_MONOTONIC jump. Interrupt-disabling only covers the same
 * hart.
 */
static _Atomic uint32_t timer_base_seq;

uint64_t arch_timer_now_ns(struct qtime_entry *qtp)
{
    uint64_t cyc, nsec;
    uint32_t s;

    if (qtp->timer_scale != -9)
        return qtp->nsec;
    do {
        s = atomic_load_explicit(&timer_base_seq, memory_order_acquire);
        cyc = timer_base_cycle;
        nsec = timer_base_nsec;
        atomic_thread_fence(memory_order_acquire);
    } while ((s & 1u) ||
             s != atomic_load_explicit(&timer_base_seq, memory_order_relaxed));
    return nsec + ((r_time() - cyc) * qtp->timer_rate);
}

static uint64_t timer_delta_cycles(struct qtime_entry *qtp, uint64_t delta_ns)
{
    uint64_t rate = qtp->timer_rate ? qtp->timer_rate : 1;
    uint64_t cycles = (delta_ns + rate - 1) / rate;

    return cycles ? cycles : 1;
}

/* Backlog drain cadence: when timer_expiry() leaves due timers behind (per-IRQ
 * fire cap hit), shorten the next interrupt to this instead of a full tick so
 * the burst drains fast. Bounded far above the ~1-cycle livelock window (see
 * the "Do NOT special-case an overdue deadline" note below) so a kernel call
 * interrupted by the re-fire still advances. 100us at any sane HZ.
 */
#define TIMER_BACKLOG_DELTA_NS 100000ULL
extern volatile uint8_t timer_backlog;

/* Maximum idle sleep. An idle hart never programs its next STIMER farther out
 * than this even when the composite deadline is farther (or absent), so a
 * missed cross-hart reschedule IPI is rescued within this bound instead of
 * stranding a thread forever. 50 ms keeps a quiet hart near 20 interrupts per
 * second, far below the nsec_inc heartbeat, while bounding the rare lost-wakeup
 * latency.
 */
#define IDLE_MAX_SLEEP_NS 50000000ULL

static void arch_timer_program_next(struct qtime_entry *qtp, int idle)
{
    uint64_t delta_ns = qtp->nsec_inc;
    uint64_t next;

    /* Idle reprogram: a hart with no runnable work sleeps until the composite
     * deadline (timer wheel, sporadic replenishment, TOD convergence, stall
     * watchdog on the boot CPU; the running RR slice per hart) instead of the
     * fixed nsec_inc heartbeat, but never longer than IDLE_MAX_SLEEP_NS. The
     * cap is a liveness backstop: a hart that slept only to a far composite
     * deadline (or forever) and relied solely on a cross-hart IPI to wake can
     * miss that wakeup under a rare SMP race and strand a freshly readied or
     * migrated thread, since the old periodic tick that used to rescue it is
     * gone. Normal wakeups still arrive immediately by IPI or device IRQ; the
     * cap only bounds the pathological miss, so a quiet hart drops from the
     * nsec_inc rate to at most 1e9 / IDLE_MAX_SLEEP_NS interrupts per second
     * rather than to zero.
     */
    if (idle) {
        uint64_t now = arch_timer_now_ns(qtp);
        uint64_t horizon = now + IDLE_MAX_SLEEP_NS;

        next = clock_next_deadline(qtp, get_cpunum(), NULL, NULL);
        if (next == ~(uint64_t) 0 || next > horizon)
            next = horizon;
        /* An already-overdue deadline arms a bounded short delta rather than a
         * near-zero one, matching the periodic path's anti-livelock reasoning
         * below: the idle hart wakes promptly to service it without spinning.
         */
        delta_ns = next > now ? next - now : TIMER_BACKLOG_DELTA_NS;
        atomic_or_64(&qtp->flags, QTIME_FLAG_TICKLESS);
        sbi_set_timer(r_time() + timer_delta_cycles(qtp, delta_ns));
        return;
    }

    if (get_cpunum() == boot_cpunum) {
        uint64_t now = arch_timer_now_ns(qtp);
        timer_next_deadline(&next);
        /* Trim the next interrupt down to an upcoming deadline, but only when
         * it is still in the future. Do NOT special-case an overdue deadline
         * with a near-zero delta to "fire immediately": clock_handler() rearms
         * the timer at its top, before the in_kernel_context() bailout, so when
         * the boot CPU is mid-kernel-call (timer_expiry deferred) a ~1-cycle
         * delta re-fires before the interrupted call can advance — a livelock.
         * An overdue timer instead falls back to nsec_inc and fires at the next
         * tick that finds the CPU outside the kernel (<= one period late). The
         * trim-down path is self-limiting: as `now` reaches `next` it stops
         * trimming and reverts to nsec_inc.
         */
        if (next != ~(uint64_t) 0 && next > now && next - now < delta_ns)
            delta_ns = next - now;
        /* A backlog from the fire cap: come back in TIMER_BACKLOG_DELTA_NS to
         * drain the rest, rather than waiting a full tick. Bounded, so no
         * re-entry livelock if this fires mid-kernel-call.
         */
        if (timer_backlog && delta_ns > TIMER_BACKLOG_DELTA_NS)
            delta_ns = TIMER_BACKLOG_DELTA_NS;
    }

    /* qtp->flags lives in the shared syspage and is updated by every CPU on
     * every reprogram; use atomic RMW so a sibling CPU's update can't clobber
     * unrelated flag bits.
     */
    atomic_clr_64(&qtp->flags, QTIME_FLAG_TICKLESS);
    sbi_set_timer(r_time() + timer_delta_cycles(qtp, delta_ns));
}

/* arch_timer_reload — re-arm the timer each tick. Called at the top of
 * clock_handler(). Must return 1 so clock_handler() proceeds (returning 0 would
 * bail out immediately).
 */
int arch_timer_reload(struct qtime_entry *qtp)
{
    if (!kopt.disable_timer)
        arch_timer_program_next(qtp, 0);
    return 1; /* must stay 1 or clock_handler bails out */
}

/* arch_timer_load — initial timer setup. Called from clock_resolution() to
 * program the first deadline and enable the timer interrupt.
 */
int arch_timer_load(struct qtime_entry *qtp)
{
    if (!kopt.disable_timer) {
        /* The clock ISR reads timer_base_cycle/timer_base_nsec as a pair in
         * arch_timer_now_ns(). On a resolution change the timer interrupt is
         * already live, so keep it disabled across the base update, the
         * reprogram, and the STIE enable — the same-CPU ISR must not observe a
         * half-updated base or fire against an un-reprogrammed compare.
         */
        int en = intr_get();
        interrupt_disable();
        /* Seqlock write: bump to odd, update the pair, bump to even, so a
         * cross-hart arch_timer_now_ns reader never sees a half-updated base.
         */
        atomic_fetch_add_explicit(&timer_base_seq, 1u, memory_order_release);
        atomic_thread_fence(memory_order_release);
        timer_base_cycle = r_time();
        timer_base_nsec = qtp->nsec;
        atomic_thread_fence(memory_order_release);
        atomic_fetch_add_explicit(&timer_base_seq, 1u, memory_order_release);
        arch_timer_program_next(qtp, 0);
        w_sie(r_sie() | SIE_STIE);
        if (en)
            interrupt_enable();
    }
    return 1;
}

/* mmu_set_domain — MMU domain control (ARM concept, no-op on RISC-V). */
void mmu_set_domain(unsigned domain)
{
    (void) domain;
}

/* cpu_debug stubs — debug/breakpoint support. */
int rdecl cpu_debug_sstep(debug_t *dep, thread_t *thp)
{
    (void) dep;
    (void) thp;
    return 0;
}

int rdecl cpu_debug_brkpt(debug_t *dep, breakpoint_t *bpp)
{
    (void) dep;
    (void) bpp;
    return 0;
}

void rdecl cpu_debug_attach_brkpts(debug_t *dep)
{
    (void) dep;
}

void rdecl cpu_debug_detach_brkpts(debug_t *dep)
{
    (void) dep;
}

int rdecl cpu_debug_fault(debug_t *dep,
                          thread_t *thp,
                          siginfo_t *info,
                          unsigned *pflags)
{
    (void) dep;
    (void) thp;
    (void) info;
    (void) pflags;
    return 0;
}

int rdecl cpu_debug_get_altregs(thread_t *thp, debug_altreg_t *regs)
{
    (void) thp;
    (void) regs;
    return ENOSYS;
}

int rdecl cpu_debug_set_altregs(thread_t *thp, debug_altreg_t *regs)
{
    (void) thp;
    (void) regs;
    return ENOSYS;
}

int rdecl cpu_debug_set_perfregs(thread_t *thp, debug_perfreg_t *regs)
{
    (void) thp;
    (void) regs;
    return ENOSYS;
}

int rdecl cpu_debug_get_perfregs(thread_t *thp, debug_perfreg_t *regs)
{
    (void) thp;
    (void) regs;
    return ENOSYS;
}

unsigned cpu_perfreg_id(void)
{
    return 0;
}

void rdecl cpu_save_perfregs(void *regs)
{
    (void) regs;
}

void rdecl cpu_restore_perfregs(void *regs)
{
    (void) regs;
}

void rdecl cpu_free_perfregs(thread_t *thp)
{
    (void) thp;
}

void rdecl cpu_debug_init_perfregs(void) {}

extern unsigned hart_ids[];
