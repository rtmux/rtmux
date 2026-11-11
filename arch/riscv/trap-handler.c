/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * RISC-V S-mode trap handler (C part).
 *
 * strap_handler_c is called from trap.S for all S-mode traps: interrupts
 * (timer, PLIC, IPI), ecalls, and exceptions.
 *
 * The argument is a pointer to a CPU_REGISTERS frame on the kernel stack — same
 * layout as thread_t.reg. For ecalls that reschedule, the frame is copied to
 * act->reg and kernel_exit restores from there. For interrupts and error
 * returns, the assembly return path restores directly from the frame.
 *
 * License: Apache 2.0
 */

#include <core/globals.h>
#include <cpu-inline.h>
#include <sbi.h>
#include <arch/fault.h>
#include "trace-ring.h"
#include <cpu-paging.h> /* PTE_V/R/W/X/U/A/D, PX, PTE2PA */
#include <stdatomic.h>
#ifdef CONFIG_DEBUG_INJECT
#include <private/inject.h> /* inject_spin_begin/end */
#endif

extern pte_t *cpu_pte_lookup(pagetable_t pgdir, uintptr_t vaddr);

/* Recovered-xfer-fault reports to print per boot before going quiet. Assumption
 * class: tuning. Diagnostics only; the recovery itself is unaffected. Revisit
 * trigger: a debugging session that needs the full stream, which should raise
 * this rather than remove the throttle.
 */
#define XFER_FAULT_REPORT_MAX 16

/* Cause names for the most common S-mode exceptions. */
static const char *scause_name(uint64_t cause)
{
    switch (cause) {
    case RISCV_EXC_INSN_MISALIGNED:
        return "instruction address misaligned";
    case RISCV_EXC_INSN_ACCESS_FAULT:
        return "instruction access fault";
    case RISCV_EXC_ILLEGAL_INSN:
        return "illegal instruction";
    case RISCV_EXC_BREAKPOINT:
        return "breakpoint";
    case RISCV_EXC_LOAD_MISALIGNED:
        return "load address misaligned";
    case RISCV_EXC_LOAD_ACCESS_FAULT:
        return "load access fault";
    case RISCV_EXC_STORE_MISALIGNED:
        return "store address misaligned";
    case RISCV_EXC_STORE_ACCESS_FAULT:
        return "store access fault";
    case RISCV_EXC_ECALL_U:
        return "ecall from U-mode";
    case RISCV_EXC_ECALL_S:
        return "ecall from S-mode";
    case RISCV_EXC_INSN_PAGE_FAULT:
        return "instruction page fault";
    case RISCV_EXC_LOAD_PAGE_FAULT:
        return "load page fault";
    case RISCV_EXC_STORE_PAGE_FAULT:
        return "store page fault";
    default:
        return "unknown";
    }
}

/* ===================================================================== *
 * Verbose U-mode fault report — gated on CONFIG_UMODE_VERBOSE_FAULT_REPORT.
 *
 * deliver_fault() always prints a one-line "user fault:" summary; that tells us
 * a process died but not where the corruption came from. When this option is
 * enabled, dump_umode_fault() additionally reports the exact scause, the
 * page-table state of both the fault IP and the fault address, the full
 * register file, and a fault-safe scan of the user stack for candidate return
 * addresses — enough to distinguish a wild *data* pointer (fault IP stable,
 * data addr wanders) from *control-flow* corruption (the fault IP itself
 * wanders). This was how the FU740 I-cache stale-instruction faults were
 * identified.
 *
 * Everything here is read-only and every user-memory access is validated
 * through cpu_pte_lookup() first, so the dump can never itself double- fault.
 * Compiled out entirely when the option is off (default).
 */
#ifdef CONFIG_UMODE_VERBOSE_FAULT_REPORT
#define FAULT_STACK_SCAN_BYTES 4096 /* how far up the stack to sieve   */
#define FAULT_STACK_SCAN_MAX 32     /* cap on candidate ret addrs      */

/* fault_pte_decode — read-only walk of pgdir for a user VA; print the
 * page-table state ("UNMAPPED" or the V/R/W/X/U/G/A/D bits) under the given
 * label.
 *
 * Returns the leaf PTE value (0 if unmapped) so the caller can test PTE_X etc.
 */
static uint64_t fault_pte_decode(pagetable_t pgdir,
                                 uintptr_t va,
                                 const char *what)
{
    pte_t *pte = cpu_pte_lookup(pgdir, va);
    uint64_t pv;

    if (!pte) {
        kprintf("  %-4s %lx  UNMAPPED\n", what, (unsigned long) va);
        return 0;
    }
    pv = (uint64_t) *pte;
    kprintf("  %-4s %lx  pa=%lx [%c%c%c%c%c%c%c%c]\n", what, (unsigned long) va,
            (unsigned long) PTE2PA(pv), (pv & PTE_V) ? 'V' : '-',
            (pv & PTE_R) ? 'R' : '-', (pv & PTE_W) ? 'W' : '-',
            (pv & PTE_X) ? 'X' : '-', (pv & PTE_U) ? 'U' : '-',
            (pv & PTE_G) ? 'G' : '-', (pv & PTE_A) ? 'A' : '-',
            (pv & PTE_D) ? 'D' : '-');
    return pv;
}

/* dump_umode_fault — rich diagnostic for a fatal user-space fault.
 *
 * Prints the exact scause, the PTE state of both the fault IP and the fault
 * address, the full register file, and a fault-safe scan of the user stack for
 * words that point into an executable user page — the realistic backtrace given
 * userland is built -O2 with no frame pointer. Parameters:
 *   act    : the faulting thread (caller guarantees act && act->process)
 *   scause : raw RISC-V exception cause from the trap
 */
static void dump_umode_fault(thread_t *act, uint64_t scause)
{
    CPU_REGISTERS *r = &act->reg;
    process_t *prp = act->process;
    uintptr_t ip = (uintptr_t) r->pc;
    uintptr_t sp = (uintptr_t) r->gpr[RISCV_REG_SP];
    uintptr_t stval = (uintptr_t) r_stval();
    uint64_t satp = r_satp();
    uint64_t pgdir_pa = (satp & ((1UL << 44) - 1)) << 12;
    pagetable_t pgdir = (pagetable_t) (pgdir_pa + KERNEL_VIRT_BASE);
    uintptr_t a;
    int n;

    kprintf("\n=== U-mode fault: pid=%d tid=%d scause=%lu (%s) ===\n",
            prp ? prp->pid : -1, act->tid, (unsigned long) scause,
            scause_name(scause));

    /* Where is the fault IP, and where is the faulting address? */
    fault_pte_decode(pgdir, ip, "ip");
    fault_pte_decode(pgdir, stval, "addr");

    /* Full register file (ra = caller, sp/s0 = frame). */
    kprintf("  ra:%lx sp:%lx gp:%lx tp:%lx\n",
            (unsigned long) r->gpr[RISCV_REG_RA],
            (unsigned long) r->gpr[RISCV_REG_SP],
            (unsigned long) r->gpr[RISCV_REG_GP],
            (unsigned long) r->gpr[RISCV_REG_TP]);
    kprintf("  s0:%lx s1:%lx s2:%lx s3:%lx\n",
            (unsigned long) r->gpr[RISCV_REG_FP],
            (unsigned long) r->gpr[RISCV_REG_S1],
            (unsigned long) r->gpr[RISCV_REG_S2],
            (unsigned long) r->gpr[RISCV_REG_S3]);
    kprintf("  a0:%lx a1:%lx a2:%lx a3:%lx\n",
            (unsigned long) r->gpr[RISCV_REG_A0],
            (unsigned long) r->gpr[RISCV_REG_A1],
            (unsigned long) r->gpr[RISCV_REG_A2],
            (unsigned long) r->gpr[RISCV_REG_A3]);
    kprintf("  a4:%lx a5:%lx a6:%lx a7:%lx\n",
            (unsigned long) r->gpr[RISCV_REG_A4],
            (unsigned long) r->gpr[RISCV_REG_A5],
            (unsigned long) r->gpr[RISCV_REG_A6],
            (unsigned long) r->gpr[RISCV_REG_A7]);

    /* Stack-scan "backtrace": -O2 userland omits the frame pointer, so we can't
     * chain s0. Instead sieve [sp, sp+N) for words that point into an
     * executable user page (candidate return addresses). Each stack word is
     * read only after cpu_pte_lookup() confirms its page is mapped+readable; a
     * garbage sp simply yields no candidates.
     */
    kprintf("  stack-scan from sp (X-mapped words = candidate ret addrs):\n");
    n = 0;
    for (a = sp; a < sp + FAULT_STACK_SCAN_BYTES && n < FAULT_STACK_SCAN_MAX;
         a += 8) {
        pte_t *spte = cpu_pte_lookup(pgdir, a);
        uint64_t w;
        pte_t *wpte;

        if (!spte ||
            (*spte & (PTE_V | PTE_R | PTE_U)) != (PTE_V | PTE_R | PTE_U))
            continue;
        w = *(volatile uint64_t *) a;
        if (w == 0 || w >= KERNEL_VIRT_BASE)
            continue;
        wpte = cpu_pte_lookup(pgdir, (uintptr_t) w);
        if (wpte &&
            (*wpte & (PTE_V | PTE_X | PTE_U)) == (PTE_V | PTE_X | PTE_U)) {
            kprintf("    [sp+%-4lu] %lx\n", (unsigned long) (a - sp),
                    (unsigned long) w);
            n++;
        }
    }
    kprintf("=== end fault dump ===\n");
}
#endif /* CONFIG_UMODE_VERBOSE_FAULT_REPORT */

/* IPI handler — process inter-processor interrupt commands. Called from
 * strap_handler_c on scause=1 (supervisor software interrupt).
 *
 * Ordering: clear SSIP BEFORE reading ipicmds to avoid lost-wakeup race.
 */
static void ipi_handler(void)
{
    unsigned cpu = get_cpunum();

    /* Count IPI-handler invocations per hart; read by the optional monitor
     * kthread to confirm IPIs are being delivered.
     */
    pcpu_array[cpu].pc_ipi_handler_count++;

    /* Clear SSIP before reading ipicmds — prevents lost-wakeup race */
    w_sip(r_sip() & ~SIP_SSIP);

    /* Atomically swap out all pending commands. */
    uint32_t cmds = atomic_exchange_explicit(&pcpu_array[cpu].pc_ipicmds, 0,
                                             memory_order_acquire);
    if (cmds == 0)
        return;

    if (cmds & IPI_TLB_FLUSH) {
        /* Snapshot the request ticket before the sfence so the ack only covers
         * stores from requesters whose ticket we already observed — cumulative
         * ordering then guarantees the sfence flushes them. A later request
         * (req past the snapshot) simply waits for the next pass. Counters are
         * monotonic; the requester uses serial-number comparison so wraparound
         * is harmless.
         */
        unsigned req = atomic_load_explicit(&pcpu_array[cpu].pc_tlb_flush_req,
                                            memory_order_acquire);
        __asm__ volatile("sfence.vma" ::: "memory");
        atomic_store_explicit(&pcpu_array[cpu].pc_tlb_flush_done, req,
                              memory_order_release);
    }

    if ((cmds & IPI_CLOCK_LOAD) && !kopt.disable_timer)
        (void) arch_timer_reload(qtimeptr); /* honor the dynamic deadline */

    if (cmds & IPI_PARKIT)
        for (;;)
            __asm__ volatile("wfi");

    if (cmds & IPI_CRASH) {
        /* A sibling CPU is crashing and IPI'd us to quiesce. Snapshot our
         * last-known state into our per-CPU crash buffer (sepc at IPI entry,
         * current thread) and join the SMP crash rendezvous — never returns.
         * This stops user-thread dispatch on this CPU before any related broken
         * kernel state can trip the dispatch-time fault cascades that otherwise
         * interfere with the crash dump.
         */
        crash_quiesce_self();
        /* NOTREACHED */
    }

    if (cmds & IPI_TLB_SAFE) {
        extern void set_safe_as(unsigned cpu);
        set_safe_as(cpu);
    }

    /* IPI_RESCHED: a peer readied a higher-priority thread onto this hart or
     * otherwise changed scheduling state. An idle hart re-evaluates on its own
     * once the wfi wakeup returns it to the idle loop, but a hart busy in a
     * U-mode thread has no such re-evaluation: the SSW trap would sret straight
     * back to the interrupted lower-priority thread and strand the readied one.
     * Mark the running thread for reschedule so the trap-return dispatch gate
     * routes through kernel_exit. resched() re-checks priority, so marking when
     * nothing outranks the current thread simply resumes it.
     *
     * IPI_TIMESLICE, IPI_CHECK_INTR: the wfi wakeup still suffices.
     * IPI_CONTEXT_SAVE, IPI_INTR_MASK/UNMASK: NYI - log if hit unexpectedly.
     */
#ifdef CONFIG_DEBUG_INJECT
    if (cmds & IPI_DEBUG_SPIN) {
        /* Busy-wait with interrupts off (a trap entered with SIE clear), so
         * this hart looks stalled to the rest of the system. UINT32_MAX hangs
         * for ever. */
        uint32_t ms = pcpu_array[cpu].pc_inject_spin_ms;
        uint64_t per_ms = qtimeptr->cycles_per_sec / 1000;
        uint64_t end = r_time() + (uint64_t) ms * per_ms;

        /* Tell intrevent_flush's debug watchdog this stall is deliberate. */
        inject_spin_begin();
        if (pcpu_array[cpu].pc_inject_hold_console)
            kprintf_inject_hold(ms);
        else
            while (ms == UINT32_MAX || r_time() < end)
                ;
        inject_spin_end();
    }
#endif
    if (cmds & IPI_RESCHED) {
        thread_t *cur = get_active_thread(cpu);
        if (cur)
            atomic_fetch_or_explicit(&cur->async_flags, RTMUX_ATF_SMP_RESCHED,
                                     memory_order_release);
    }
}

/* enter_dispatch_exit — mark this CPU as in-kernel via in_kcall[] and enter
 * kernel_exit, which checks async_flags, calls resched(), and dispatches the
 * next thread. kernel_exit decrements in_kcall before __kernel_restore so the
 * dispatched thread runs with in_kcall == 0. This is the common tail of the
 * S-mode preempt path and both U-mode trap-return dispatch gates; each caller
 * owns its own gating condition and only the dispatch entry is shared here.
 */
static void __attribute__((noreturn)) enter_dispatch_exit(void)
{
    in_kcall_inc();
    in_kcall_phase(IN_KCALL_PHASE_DISPATCH);
    kernel_exit();
}

/* smode_preempt — called when a timer interrupt sets async_flags on the active
 * S-mode thread. The full register context is saved in the trap frame (all 31
 * GPRs).
 */
void __attribute__((noreturn)) smode_preempt(void)
{
    enter_dispatch_exit();
}

int priv_call_phase(unsigned id)
{
    if (id == __KSYS_PRIV_HEARTBEAT)
        return IN_KCALL_PHASE_HEARTBEAT;
    if (id == __KSYS_PRIV_PA_ALLOC_GIVEN || id == __KSYS_PRIV_PA_ALLOC_FULL)
        return IN_KCALL_PHASE_PRIV_PA_ALLOC;
    if (id == __KSYS_PRIV_PA_FREE || id == __KSYS_PRIV_PA_FREE_INFO ||
        id == __KSYS_PRIV_PA_FREE_PADDR || id == __KSYS_PRIV_PA_FREE_LIST)
        return IN_KCALL_PHASE_PRIV_PA_FREE;
    if (id == __KSYS_PRIV_KPAGE_ZERO)
        return IN_KCALL_PHASE_PRIV_PA_ZERO;
    if (id == __KSYS_PRIV_PA_INIT || id == __KSYS_PRIV_PA_START_BACKGROUND ||
        id == __KSYS_PRIV_PA_SYSRAM_OVERLAP || id == __KSYS_PRIV_PA_GET_SIZES)
        return IN_KCALL_PHASE_PRIV_PA_INFO;
    if (id == __KSYS_PRIV_PA_QUANTUM_TO_PADDR ||
        id == __KSYS_PRIV_PA_PADDR_TO_QUANTUM)
        return IN_KCALL_PHASE_PRIV_PA_XLAT;
    if (id == __KSYS_PRIV_PA_PQ_OP)
        return IN_KCALL_PHASE_PRIV_PA_PQ;

#define PRIV_PHASE_SYSTEM IN_KCALL_PHASE_PRIV_SYSTEM
#define PRIV_PHASE_VM IN_KCALL_PHASE_PRIV_VM
#define PRIV_PHASE_PTE IN_KCALL_PHASE_PRIV
#define PRIV_PHASE_IPC IN_KCALL_PHASE_PRIV_IPC
#define PRIV_PHASE_CRED IN_KCALL_PHASE_PRIV
#define PRIV_PHASE_DEBUG IN_KCALL_PHASE_PRIV
#define PRIV_PHASE_PROCESS IN_KCALL_PHASE_PRIV_PROCESS
#define PRIV_PHASE_PA IN_KCALL_PHASE_PRIV_PA
#define PRIV_PHASE_NOTIFICATION IN_KCALL_PHASE_PRIV
#define PRIV_PHASE_THREAD IN_KCALL_PHASE_PRIV_THREAD
#define PRIV_PHASE_TIME IN_KCALL_PHASE_PRIV
#define PRIV_PHASE_TRACE IN_KCALL_PHASE_PRIV
#define PRIV_PHASE_INJECT IN_KCALL_PHASE_PRIV
#define PRIV_PHASE_ROW(name, cap, handler) \
    if (id == __KSYS_PRIV_##name)          \
        return PRIV_PHASE_##cap;
    RTMUX_PRIV_CALL_LIST(PRIV_PHASE_ROW)
#undef PRIV_PHASE_ROW
#undef PRIV_PHASE_INJECT
#undef PRIV_PHASE_TRACE
#undef PRIV_PHASE_TIME
#undef PRIV_PHASE_THREAD
#undef PRIV_PHASE_NOTIFICATION
#undef PRIV_PHASE_PA
#undef PRIV_PHASE_PROCESS
#undef PRIV_PHASE_DEBUG
#undef PRIV_PHASE_CRED
#undef PRIV_PHASE_IPC
#undef PRIV_PHASE_PTE
#undef PRIV_PHASE_VM
#undef PRIV_PHASE_SYSTEM
    return IN_KCALL_PHASE_PRIV;
}

/* strap_handler_c — C entry point for all S-mode traps.
 *
 * Called from trap.S with a pointer to the saved register frame. Dispatches on
 * scause: interrupts (timer, PLIC external, IPI software), ecalls from U-mode
 * or S-mode (kernel-call dispatch), and exceptions (page faults — recovered via
 * the xfer handler or turned into a signal for U-mode faults, otherwise a fatal
 * S-mode crash dump).
 *
 * Parameters:
 *   regs : the saved CPU register frame for this trap
 */
/* User FP state is saved at every trap from U-mode in which the thread dirtied
 * it, not at switch-out: the scheduler links a preempted thread onto a run queue
 * before it marks the next one running, so another hart could dispatch it before
 * a switch-out hook ran. By the time the thread can be seen anywhere else, its
 * registers are already in its save area. The kernel never executes FP
 * instructions, so the registers still hold the thread's values here. The
 * registers stay live (FS=Clean), so resuming on this hart costs nothing.
 */
static inline void fpu_trap_save(CPU_REGISTERS *regs)
{
    if (!(regs->sstatus & SSTATUS_SPP) &&
        (regs->sstatus & SSTATUS_FS) == SSTATUS_FS_DIRTY) {
        thread_t *thp = get_active_thread(get_cpunum());

        if (thp && thp->fpudata) {
            cpu_fpu_save(FPUDATA_PTR(thp->fpudata));
            regs->sstatus =
                (regs->sstatus & ~(uint64_t) SSTATUS_FS) | SSTATUS_FS_CLEAN;
        }
    }
}

/* Is the U-mode instruction at sepc one that needs the FP unit? Matches the
 * RV64D LOAD-FP/STORE-FP/FMADD/FMSUB/FNMSUB/FNMADD/OP-FP major opcodes and CSR
 * accesses to fflags, frm, and fcsr. The ABI has no C extension, so a 16-bit
 * encoding is not an FP instruction here.
 */
static int insn_is_fp(uint32_t insn)
{
    switch (insn & 0x7f) {
    case 0x07: /* FLW/FLD */
    case 0x27: /* FSW/FSD */
    case 0x43:
    case 0x47:
    case 0x4b:
    case 0x4f:
    case 0x53: /* OP-FP */
        return 1;
    case 0x73: { /* SYSTEM: CSR access to fflags(1), frm(2), fcsr(3) */
        uint32_t csr = insn >> 20;

        uint32_t f3 = (insn >> 12) & 7;

        return f3 != 0 && f3 != 4 && csr >= 1 && csr <= 3;
    }
    default:
        return 0;
    }
}

/* First FP instruction since this thread last lost the FP registers: allocate
 * its save area if it has none (zeroed, so a thread reads zeros before its first
 * write), load its context, take ownership of this hart's registers, and let the
 * instruction retry. Returns 0 if the trap is not an FP use or no memory is
 * left, in which case the caller treats it as an illegal instruction.
 */
static int fpu_first_use(thread_t *thp, CPU_REGISTERS *regs)
{
    unsigned cpu = get_cpunum();
    uint32_t insn = (uint32_t) r_stval();
    FPU_REGISTERS *fp;

    /* Only a thread with FP disabled can be taking a first-use trap; with FS
     * already on, an illegal FP encoding would otherwise retry forever.
     */
    if ((regs->sstatus & SSTATUS_FS) != SSTATUS_FS_OFF)
        return 0;
    if (!insn && xfer_memcpy(&insn, (void *) (uintptr_t) regs->pc,
                             sizeof insn) != 0)
        return 0;
    if (!insn_is_fp(insn))
        return 0;

    fp = FPUDATA_PTR(thp->fpudata);
    if (!fp) {
        if (!(fp = object_alloc(thp->process, &fpu_souls))) {
            kprintf("fpu_souls exhausted pid=%d tid=%u\n", thp->process->pid,
                    thp->tid);
            return 0;
        }
        memset(fp, 0, sizeof *fp);
    }
    cpu_fpu_restore(fp);
    pcpu_array[cpu].pc_active_fpu = thp;
    thp->fpudata = (FPU_REGISTERS *) ((uintptr_t) fp | cpu);
    regs->sstatus =
        (regs->sstatus & ~(uint64_t) SSTATUS_FS) | SSTATUS_FS_CLEAN;
    return 1;
}

void strap_handler_c(CPU_REGISTERS *regs)
{
    uint64_t scause = r_scause();

    fpu_trap_save(regs);

    /* Record this trap's PC and cause for the optional monitor kthread. For a
     * timer interrupt taken while the kernel is spinning, the PC is the
     * kernel-mode spin location, which helps identify stuck code. Plain per-CPU
     * stores, lock-free.
     */
    unsigned _mon_cpu = get_cpunum();
    pcpu_array[_mon_cpu].pc_last_kernel_pc = regs->pc;
    pcpu_array[_mon_cpu].pc_last_scause = scause;

    if (scause & (1UL << 63)) {
        unsigned long _cause_early = scause & ~(1UL << 63);

        /* IPIs (S-mode software interrupt) are dispatched here. ipi_handler()
         * is lock-free by design (atomics on pcpu_array[].pc_ipicmds + sip
         * clear), so it runs directly from trap context with SIE off and no
         * kernel lock held.
         */
        if (_cause_early == RISCV_INT_SSW) {
            atomic_add(&inkernel, 1);
            ipi_handler();
            atomic_sub(&inkernel, 1);

            /* On a U-mode return, honor a reschedule that ipi_handler flagged
             * (an IPI_RESCHED that readied a higher-priority thread onto this
             * busy hart). The SSW path does not fall through to the shared
             * dispatch gate below, so without this the sret would resume the
             * interrupted lower-priority thread and strand the readied one.
             * S-mode returns (SPP=1) drain on their own kernel_exit.
             */
            unsigned cpu = get_cpunum();
            thread_t *cur = get_active_thread(cpu);
            if (!(regs->sstatus & SSTATUS_SPP) && cur &&
                (atomic_load_explicit(&cur->async_flags, memory_order_acquire) &
                 (RTMUX_ATF_TIMESLICE | RTMUX_ATF_SMP_RESCHED))) {
                enter_dispatch_exit();
                /* never returns */
            }
            return;
        }

        /* Pre-drain pending IPIs before processing an STIMER/SEXT trap. We
         * arrive with SIE off (RISC-V trap entry clears it), so no SSW
         * hardware-trap can fire to dispatch a pending IPI until we sret.
         * Draining here lets a peer hart that is spinning for our pc_ipicmds
         * bit to clear (e.g. the IPI_TLB_SAFE roundtrip in
         * kext_process_shutdown, where the sender waits for set_safe_as to flip
         * pc_as_prp away from the dying process) observe the ack in finite
         * time. ipi_handler() is lock-free, so this is safe with SIE off.
         */
        if (atomic_load_explicit(&pcpu_array[_mon_cpu].pc_ipicmds,
                                 memory_order_acquire) != 0) {
            atomic_add(&inkernel, 1);
            ipi_handler();
            atomic_sub(&inkernel, 1);
        }

        /* Interrupt — increment nesting depth (byte 0 of inkernel). Matches
         * MIPS ADJUST_INKERNEL(k0,k1,k1,1,1) at interrupt entry. Read by
         * INKERNEL_INTRMASK consumers in ipc/event.c, trace/core.c, ext/page.c,
         * init/shutdown.c to detect interrupt context.
         */
        atomic_add(&inkernel, 1);

        register unsigned long sp_at_entry __asm__("sp");
        unsigned long saved_sp = sp_at_entry;

        unsigned long cause = scause & ~(1UL << 63);

        if (cause == RISCV_INT_STIMER) {
            /* Count every supervisor timer interrupt this hart takes, on both
             * the minimal (in-kernel) and full paths. Each hart writes only its
             * own slot; the boot CPU reads the array for the tickless idle
             * diagnostic, so relaxed atomics keep the cross-hart access
             * race-free. Lets a quiet-window trace prove idle harts stop taking
             * the periodic tick.
             */
            extern _Atomic uint64_t stimer_intr_count[];
            atomic_fetch_add_explicit(&stimer_intr_count[_mon_cpu], 1,
                                      memory_order_relaxed);
#ifdef CONFIG_STALL_DETECTOR
            /* Progress mark for the neighbour watchdog (kernel/debug/
             * hartwatch.c): single writer (this hart), relaxed. The check of
             * this hart's neighbour runs here too, ahead of the in-kernel
             * short-circuit below: a hart spinning inside the kernel on the
             * stalled one (TLB shootdown, intrevent_flush) takes only that
             * minimal path, and would otherwise never get to report it.
             */
            atomic_fetch_add_explicit(&pcpu_array[_mon_cpu].pc_timer_progress,
                                      1, memory_order_relaxed);
            hartwatch_tick();
#endif

            /* Supervisor timer interrupt.
             *
             * If this CPU holds the kernel, just rearm the timer and return —
             * don't call ANY C handler code. Even calling clock_handler (which
             * returns NULL immediately) uses caller-saved registers as scratch,
             * and the C compiler's address computations for BSS globals
             * (actives_fpu, ksys_stack, etc.) are somehow leaking into user
             * thread register save areas.
             *
             * When the kernel is NOT active, call clock_handler normally — it
             * only touches this CPU's thread (per-CPU model, no cross-CPU
             * access).
             *
             * Also short-circuit when this CPU isn't pc_alive yet: during boot,
             * kernel_init() runs in S-mode without having issued a kcall, so
             * in_kernel_context() returns false and the else branch would
             * iterate over a not-yet-built interrupt_level[0].queue. pc_alive
             * is set in idle() before any clock_handler is attached, so before
             * that point there's nothing legitimate to dispatch — just rearm
             * and resume kernel init.
             */
            if (in_kernel_context() || !pcpu_array[_mon_cpu].pc_alive) {
                /* Minimal path: just rearm the timer, honoring the dynamic
                 * deadline cache rather than a flat periodic interval.
                 */
                if (!kopt.disable_timer)
                    (void) arch_timer_reload(qtimeptr);
            } else {
                /* clock_handler runs and returns NULL by contract; its sigevent
                 * delivery happens inline from timer_fire via
                 * sigevent_deliver_inline, so there is no return value to
                 * enqueue here.
                 */
                intr_level_t *ilp = &interrupt_level[0];
                interrupt_t *itp = ilp->queue;
                while (itp) {
                    itp->fire_count++;
                    if (itp->handler) {
                        (void) itp->handler(itp->area, itp->id);
                    }
                    itp = itp->next;
                }
            }
        } else if (cause == RISCV_INT_SEXT) {
            /* Supervisor external interrupt (PLIC) */
            extern int plic_claim(void);
            extern void plic_complete(int irq);
            extern void plic_record_claim(int irq);
            int irq = plic_claim();
            if (irq > 0) {
                /* Convert PLIC IRQ to internal interrupt level using the same
                 * mapping as interrupt_attach/Unmask. The PLIC vector =
                 * PLIC_VECTOR_BASE + irq.
                 */
                int level = get_interrupt_level(NULL, (unsigned) (irq + 32));
                if (level < 0) {
                    plic_complete(irq);
                } else {
                    intr_level_t *ilp = &interrupt_level[level];
                    interrupt_t *itp = ilp->queue;
                    while (itp) {
                        itp->fire_count++;
                        _TRACE_IRQ_CLAIM(ilp, itp);

                        /* Dispatch by itp->kind. Every IST wake and every
                         * __interrupt_attach_event delivery happens inline from
                         * this trap context — there is no deferred-event queue.
                         */
                        if (itp->kind == INTR_KIND_THREAD && itp->thread) {
                            /* Synthetic SIGEV_INTR to the IST. Inlined to avoid
                             * building/copying a struct sigevent for the hot
                             * path.
                             */
                            thread_t *target = itp->thread;

                            /* IRQ pinning invariant: the PLIC source is
                             * SENABLE'd only on the owner hart
                             * (plic_owner_hart[irq]==this hart), AND the IST's
                             * runmask pins it to the same hart. So "target" is
                             * either currently active on THIS hart, or blocked
                             * in STATE_INTR on this hart's dispatch. No
                             * cross-hart scan needed.
                             *
                             * If target is currently running here, or in any
                             * state other than STATE_INTR (e.g. mid- transition
                             * into block), set the sticky PENDING bit — the IST
                             * will observe it at its next interrupt_wait check.
                             * Otherwise the IST is blocked on STATE_INTR
                             * locally; wake it via ready().
                             */
                            thread_t *here = get_active_thread(get_cpunum());

                            /* IST dispatch trace fires from ksys_interrupt_wait
                             * — see docs/irq-contract.md: the measured interval
                             * is trap entry to IST dispatch (the point
                             * interrupt_wait returns to U-mode), not trap entry
                             * to ready().
                             */
                            if (target == here || target->state != STATE_INTR) {
                                atomic_or((volatile unsigned *) &target->flags,
                                          RTMUX_FLG_THR_INTR_PENDING);
                            } else {
                                ready(target);
                            }
                        } else if (itp->kind == INTR_KIND_EVENT) {
                            /* Pre-registered sigevent via
                             * __interrupt_attach_event — deliver inline.
                             * SIGEV_THREAD was rejected at registration so it
                             * cannot reach trap context here.
                             */
                            sigevent_deliver_inline(&itp->event, itp->thread,
                                                    itp->nresv);
                        }

                        /* INTR_KIND_HANDLER is an in-kernel callout, not
                         * reachable from a PLIC IRQ in RTMux. Only
                         * clock_handler (STIMER trap path, see above) and the
                         * trace subsystem (core.c) use kind=HANDLER, and
                         * neither is attached to a PLIC vector. Skipped here
                         * intentionally: no RTMux driver attaches an in-kernel
                         * ISR through the public API. The interrupt_attach()
                         * function-pointer form was never wired into RTMux's
                         * libc or kcall table.
                         */
                        itp = itp->next;
                    }

                    /* Auto-mask at PLIC level (priority=0) until the owner
                     * raises the level back via __interrupt_unmask.
                     */
                    interrupt_mask((unsigned) level, NULL);

                    /* Defer plic_complete to the __interrupt_unmask path so the
                     * PLIC gateway transitions InService→Idle (not Pending),
                     * which would otherwise produce one spurious IRQ per real
                     * one. Always-deferred now that the kernel-handler
                     * short-circuit is gone.
                     */
                    plic_record_claim(irq);
                } /* else (level >= 0) */
            }
        }
        /* RISCV_INT_SSW (IPI) handled in BKL-free early-return above. Note:
         * interrupt handlers (especially PLIC IST wakeup via ready()) can
         * legitimately change actives[cpu]. This is the normal threaded-IRQ
         * wake path: the scheduler decides whether the woken thread should
         * preempt the current one. kernel_exit will handle the dispatch on
         * return from the interrupt.
         *
         * Only check sp — a corrupted sp would indicate a real bug.
         */
        {
            register unsigned long sp_now __asm__("sp");
            if (sp_now != saved_sp) {
                kprintf(
                    "INTR BUG: sp changed! cause=%lu "
                    "sp_entry=%lx sp_now=%lx\n",
                    cause, saved_sp, sp_now);
                crash();
            }
        }

        /* Decrement interrupt nesting depth (matches increment above). Matches
         * MIPS ADJUST_INKERNEL(k0,k1,k1,-1,0) at interrupt exit.
         */
        atomic_sub(&inkernel, 1);

        /* On return to U-mode (SPP=0), route through kernel_exit when dispatch
         * needs to run, so sret does not return straight to the interrupted
         * thread. Two independent signals require it:
         *
         *   1. A higher-priority wakeup delivered inline by the trap bumped
         *      this hart's pc_pending_preempt_prio (sigevent_deliver_inline).
         *   2. The running thread has a pending RTMUX_ATF_TIMESLICE or
         *      RTMUX_ATF_SMP_RESCHED. clock_handler sets RTMUX_ATF_TIMESLICE
         *      when a round-robin slice expires with an equal-or-higher peer
         *      ready, but it only bumps pc_pending_preempt_prio for a thread
         *      that was IN the kernel; a U-mode CPU-bound thread makes no
         *      kcall to drain the flag, so without this check its slice would
         *      never end and an equal-priority peer would starve. kernel_exit
         *      consumes the flag (exit.c) and resched() rotates.
         *
         * If SPP=1 (interrupt from S-mode kernel code) we do not route here;
         * the interrupted kernel extension drains on its own kernel_exit.
         */
        unsigned cpu = get_cpunum();
        thread_t *cur = get_active_thread(cpu);
        if (!(regs->sstatus & SSTATUS_SPP) &&
            (pcpu_array[cpu].pc_pending_preempt_prio != 0 ||
             (cur &&
              (atomic_load_explicit(&cur->async_flags, memory_order_acquire) &
               (RTMUX_ATF_TIMESLICE | RTMUX_ATF_SMP_RESCHED))))) {
            enter_dispatch_exit();
            /* never returns */
        }
        return;
    }

    /* ecall from U-mode (8) or S-mode (9) — dispatch kernel call. S-mode ecalls
     * happen when cpu_force_thread_destroyall() sets a roottask thread's PC to
     * kcallptr (ecall instruction in syspage).
     */
    if (RISCV_EXC_IS_ECALL(scause)) {
        unsigned num = (unsigned) regs->gpr[RISCV_REG_A7];
        int from_smode = (scause == RISCV_EXC_ECALL_S);

        /* Pack arguments from the trap frame (read-only: copies register values
         * into a local kargs before we touch the kernel).
         */
        union kcall_args kargs;
        memset(&kargs, 0, sizeof(kargs));
        uint64_t *kap = (uint64_t *) &kargs;
        kap[0] = regs->gpr[RISCV_REG_A0];
        kap[1] = regs->gpr[RISCV_REG_A1];
        kap[2] = regs->gpr[RISCV_REG_A2];
        kap[3] = regs->gpr[RISCV_REG_A3];
        kap[4] = regs->gpr[RISCV_REG_A4];
        kap[5] = regs->gpr[RISCV_REG_A5];

#ifdef CONFIG_TRACE_SYSCALLS
        if (num != __KSYS_DBGPRINT) {
            static const char *const _knames[] = {
                [0] = "nop",
                [1] = "trace_event",
                [2] = "priv",
                [3] = "dbgprint",
                [7] = "cpupage_get",
                [8] = "cpupage_set",
                [10] = "msg_current",
                [11] = "msg_sendv",
                [12] = "msg_sendvnc",
                [13] = "msg_error",
                [14] = "msg_recv",
                [15] = "msg_replyv",
                [16] = "msg_readv",
                [17] = "msg_writev",
                [18] = "msg_rdwr",
                [19] = "msg_info",
                [20] = "send_notification",
                [21] = "deliver_ev",
                [22] = "msg_keydata",
                [23] = "msg_readiov",
                [24] = "msg_rcvnotification",
                [25] = "msg_verify_ev",
                [35] = "ch_create",
                [36] = "ch_destroy",
                [39] = "con_attach",
                [40] = "con_detach",
                [41] = "con_srvinfo",
                [42] = "con_cliinfo",
                [43] = "con_flags",
                [46] = "thr_create",
                [47] = "thr_destroy",
                [48] = "thr_destroyall",
                [49] = "thr_detach",
                [50] = "thr_join",
                [52] = "thr_ctl",
                [65] = "clock_time",
                [67] = "clock_period",
                [70] = "timer_cre",
                [71] = "timer_del",
                [72] = "timer_set",
                [75] = "timer_timeout",
                [78] = "sync_create",
                [79] = "sync_destroy",
                [80] = "mtx_lock",
                [81] = "mtx_unlock",
                [82] = "cv_wait",
                [83] = "cv_signal",
                [84] = "sem_post",
                [85] = "sem_wait",
                [88] = "sched_get",
                [89] = "sched_set",
                [90] = "sched_yield",
            };
            const char *_nm =
                (num < sizeof(_knames) / sizeof(_knames[0]) && _knames[num])
                    ? _knames[num]
                    : "?";
            kprintf("ecall: %-14s(%3u) a0=%lx a1=%lx a2=%lx\n", _nm, num,
                    regs->gpr[RISCV_REG_A0], regs->gpr[RISCV_REG_A1],
                    regs->gpr[RISCV_REG_A2]);
        }
#endif

        /* in_kcall[cpu]++ marks this CPU as "doing kernel work" so the timer
         * trap routes to its minimal kernel-self path (see in_kernel_context()
         * in arch/riscv/include/runtime.h). Every exit from this branch
         * decrements: kernel_exit decrements before __kernel_restore on the
         * success path; the two error returns below decrement explicitly.
         */
        in_kcall_inc();

        /* Derive "act" directly from "regs" instead of trusting
         * "_actives[cpu]".
         *
         * For U-mode ECALL, "regs" is &act->reg — the trap.S U-mode path saved
         * registers directly into the active thread's register area (sscratch
         * held &thp->reg while U-mode was running). See trap.S head-of-file
         * banner.
         *
         * The "_actives[cpu]" slot is a write-broadcast for *cross- CPU*
         * readers; it is allowed to lag behind the truth on its own CPU between
         * dispatch events (a "block()" no-runnable- thread fallback can clear
         * it shortly before a U-mode thread's ECALL fires). The regs-derived
         * form has no such window: "sscratch" was set to &thp->reg at
         * __kernel_restore time, hardware preserved it unchanged across U-mode
         * execution, and trap.S handed it to us in "regs".
         *
         * For S-mode ECALL, "regs" is a stack frame (NOT &act->reg); fall back
         * to _actives in that case. S-mode kernel extension entry runs in
         * kernel context where _actives is already coherent with the calling
         * thread.
         */
        thread_t *act;
        if (from_smode) {
            act = get_active_thread(get_cpunum());
        } else {
            act = (thread_t *) ((char *) regs -
                                __builtin_offsetof(thread_t, reg));
        }

        /* Do NOT self-heal _actives[cpu] from the regs-derived thread here.
         * Doing so re-publishes an "act" whose "state" may not have been
         * refreshed to RUNNING (e.g. left at STATE_RECEIVE from a partial
         * blocking sequence), which then trips "unready"'s "state ==
         * STATE_RUNNING" invariant when the kcall body calls unready(act, ...).
         * Cross-CPU consumers of "_actives[cpu]" (force_ready IPI targeting,
         * debugger, /proc readers) are diagnostic/heuristic and tolerate brief
         * lag; the kcall body's own correctness only needs the regs-derived
         * "act" used directly above.
         */

        if (!act->process) {
            /* act->process transitioned to NULL during a concurrent
             * thread_destroy on another CPU — the thread is mid-teardown. Don't
             * attempt the kcall, and don't restore this thread: drop it from
             * pc_curthread so kernel_exit's thp==NULL branch tail-calls
             * cpu_idle and picks the next runnable thread. Leaving the dying
             * thread installed as pc_curthread would have kernel_exit
             * re-restore it, and once its soul is object_free'd/zeroed that is
             * the "__kernel_restore PC=0!" FATAL. Abandoning it here (backed by
             * the STATE_DEAD guard in kernel_exit) closes that race.
             */
            kprintf(
                "ecall: cpu %u thread tid=%u has NULL process "
                "(teardown race), redispatching\n",
                get_cpunum(), act->tid);
            set_active_thread(get_cpunum(), NULL);
            kernel_exit();
            /* NOTREACHED */
        }

        if (from_smode) {
            /* S-mode ecall: regs is a stack frame, not &act->reg. Copy to
             * act->reg and fix up sstatus for S-mode return. Must preserve SUM
             * so kcall handlers can touch user memory, and preserve FS so FPU
             * state isn't lost.
             */
            act->reg = *regs;
            act->reg.sstatus = SSTATUS_SPP | SSTATUS_SPIE | SSTATUS_SUM |
                               (regs->sstatus & SSTATUS_FS);
        }

        /* U-mode ecall: regs IS &act->reg — trap.S saved directly into the
         * thread's register area. No copy needed.
         *
         * Advance past the ecall instruction (4 bytes). act->reg is
         * single-writer-by-CPU on this CPU (no other CPU writes a different
         * CPU's saved register frame), so the read-modify-write is safe without
         * any cross-CPU lock. Operate on act->reg (not "regs"): for U-mode they
         * alias, for S-mode the frame copy we just did makes act->reg the
         * definitive store going forward.
         */
        act->reg.pc += 4;

        /* Reject invalid syscall numbers AFTER advancing the PC so the faulting
         * thread resumes past the ecall instead of re-triggering it in an
         * infinite loop.
         */
        if (num >= __KSYS_BAD) {
            kprintf("ecall: invalid syscall %u from sepc=%lx\n", num,
                    (unsigned long) (act->reg.pc - 4));
            act->reg.gpr[RISCV_REG_A0] = (uint64_t) (-1L);
            lookup_rcvid_release();
            in_kcall_dec();
            return;
        }

        act->syscall = (int32_t) num;
        if (num == __KSYS_PRIV) {
            __atomic_store_n(&pcpu_array[get_cpunum()].pc_syscall,
                             priv_call_phase((unsigned) kap[0]),
                             __ATOMIC_RELEASE);
        } else {
            __atomic_store_n(&pcpu_array[get_cpunum()].pc_syscall, (int) num,
                             __ATOMIC_RELEASE);
        }
        SETREGSTATUS(&act->reg, 0);
        REGTYPE(&act->reg) = num;

        /* Clear per-call state that must not leak across syscalls. A persistent
         * RTMUX_FLG_THR_KCALL_ERR_SET causes paths like ksys_sync_condvar_wait
         * to abort without blocking — producing a tight user-space cond_wait
         * loop that never yields to the IST.
         */
        if (act->flags & (RTMUX_FLG_THR_KCALL_ERR_SET | RTMUX_FLG_THR_BUFF_MSG))
            atomic_clr(&act->flags,
                       (RTMUX_FLG_THR_KCALL_ERR_SET | RTMUX_FLG_THR_BUFF_MSG));

        /* Every kcall body runs without a big kernel lock. in_kcall[cpu]++ at
         * the start of this branch already marks "kernel work in progress" for
         * timer-trap routing, and the handler is solely responsible for
         * protecting shared state with fine-grained kspinlocks (sync_slock,
         * sched_slock, channel_slock, alloc_slock, per-channel_t chp->lock,
         * per-connect_t cop->lock, per-process_t vec_slock, etc. — see
         * KerFuncRef for the per-handler audit).
         */

        /* Snapshot the syscall argument registers so KCALL_RESTART can restore
         * them before re-executing the ecall. A0 in particular is overwritten
         * with the return/status value (SETKSTATUS) when a blocking call is
         * replied to; without this restore a restarted syscall would re-issue
         * with a0 = status instead of its original first argument. (Re-entry
         * after a restart re-runs this snapshot over the just-restored values,
         * which is harmless.)
         */
        act->kcall_c_args[0] = kap[0];
        act->kcall_c_args[1] = kap[1];
        act->kcall_c_args[2] = kap[2];
        act->kcall_c_args[3] = kap[3];
        act->kcall_c_args[4] = kap[4];
        act->kcall_c_args[5] = kap[5];

        /* Bracket the handler for the per-kernel-call cost block. The span is
         * kernel-side work only: the handler returns before kernel_exit()
         * performs any context switch, so a blocking call's peer-wait time is
         * excluded by construction. The hooks compile to nothing without
         * CONFIG_INSTRUMENTATION, leaving both locals dead. `act` is
         * snapshotted up front and never dereferenced on the way out; see the
         * hook banner in trace/events.h for why.
         */
        unsigned kcall_tr = _TRACE_KCALL_ARMED(num);
        uint32_t kcall_pid = kcall_tr ? _TRACE_KCALL_PID(act) : 0U;
        uint32_t kcall_tid = kcall_tr ? _TRACE_KCALL_TID(act) : 0U;
        uint64_t kcall_t0 = _TRACE_KCALL_TICKS(kcall_tr);
        int ret = ksys_call_table[num](act, &kargs);
        _TRACE_KCALL_EMIT(kcall_pid, kcall_tid, num, kcall_t0);

        /* Close any lookup_rcvid RCU sections that the kcall body left open.
         * Idempotent — see banner in lookup.c. Fires unconditionally on every
         * kcall return path so that a kcall which forgot an explicit release
         * (or which was never expected to release, e.g. KCALL_RESTART that
         * returns to user before re-entering) can't stall a concurrent writer's
         * rcu_sched_synchronize.
         */
        lookup_rcvid_release();

        if (ret == ENOERROR) {
            kernel_exit();

            /* NOTREACHED — kernel_exit decrements in_kcall before
             * __kernel_restore.
             */
        }

        if (act->state != STATE_RUNNING ||
            get_active_thread(get_cpunum()) != act) {
            kernel_exit();
            /* NOTREACHED — same as above. */
        }

        /* Error return — write negated errno to act->reg (same as regs for
         * U-mode, different for S-mode). For U-mode: regs == &act->reg, so
         * writing regs works. For S-mode: we already copied to act->reg above,
         * but the caller's stack frame also needs the return value.
         */
        in_kcall_dec();
        regs->gpr[RISCV_REG_A0] = (uint64_t) (-(long) ret);
        return;
    }

    /* Page faults (scause 12/13/15): if an xfer fault handler is installed for
     * this CPU, invoke it — this is the recovery path for xfer_memcpy and
     * friends when they touch unmapped user pages.
     *
     * Hardening: only dispatch to the xfer handler if the faulting address
     * (stval) is in user space. A fault on a kernel address while an xfer
     * handler is active indicates a real kernel bug — fall through to the crash
     * dump instead of silently longjmp'ing.
     */
    if (RISCV_EXC_IS_PAGE_FAULT(scause)) {
        unsigned cpu = get_cpunum();
        const struct fault_handlers *fh = pcpu_array[cpu].pc_xfer_handlers;
        if (fh && fh->fault) {
            uint64_t fault_addr = r_stval();
            if (fault_addr < KERNEL_VIRT_BASE) {
                /* User-space fault during xfer — expected, recover.
                 *
                 * Rate-limited because this is now reachable on demand: any
                 * process can hand a kcall a null or unmapped pointer and land
                 * here, and an unthrottled kprintf is both a console flood and
                 * a realtime hazard, since a kernel-side console write costs
                 * hundreds of microseconds (see the kcall-dbgprint row in
                 * docs/rt-budgets.md). Keep the first few per boot for
                 * diagnosis, then count silently. Racy across harts by design;
                 * it is a log throttle, not an accounting record.
                 */
                static unsigned xfer_fault_reports;
                thread_t *act = get_active_thread(cpu);
                unsigned flags = RISCV_EXC_IS_STORE(scause) ? SIGCODE_STORE : 0;

                if (xfer_fault_reports < XFER_FAULT_REPORT_MAX) {
                    xfer_fault_reports++;
                    kprintf(
                        "XFER fault: cpu=%u sepc=%lx stval=%lx scause=%lu "
                        "(%s) flags=%x pid=%d tid=%u%s\n",
                        cpu, (unsigned long) r_sepc(),
                        (unsigned long) fault_addr, scause, scause_name(scause),
                        flags, (act && act->process) ? act->process->pid : -1,
                        act ? act->tid : 0U,
                        xfer_fault_reports == XFER_FAULT_REPORT_MAX
                            ? " (further reports suppressed)"
                            : "");
                }
                fh->fault(act, &act->reg, flags);
                /* fault handler does longjmp — should not return */
            }
            /* Kernel-address fault during xfer — fall through to crash */
            kprintf(
                "XFER BUG: page fault at kernel VA %lx during xfer "
                "(sepc=%lx scause=%lu)\n",
                (unsigned long) fault_addr, (unsigned long) r_sepc(), scause);
        }
    }

    /* U-mode fault: the user program did something illegal. Call usr_fault to
     * record the fault info and mark the thread for destruction, then enter
     * kernel_exit which will destroy it and eventually notify the parent via
     * proc_threads_destroyed.
     */
    if (!(regs->sstatus & SSTATUS_SPP)) {
        thread_t *act = get_active_thread(get_cpunum());
        if (act && act->process) {
            int signo, code;

            switch (scause) {
            case RISCV_EXC_INSN_MISALIGNED:
                signo = SIGBUS;
                code = BUS_ADRALN;
                break;
            case RISCV_EXC_INSN_ACCESS_FAULT:
                signo = SIGSEGV;
                code = SEGV_ACCERR;
                break;
            case RISCV_EXC_ILLEGAL_INSN:
                if (fpu_first_use(act, regs))
                    return;
                signo = SIGILL;
                code = ILL_ILLOPC;
                break;
            case RISCV_EXC_LOAD_MISALIGNED:
            case RISCV_EXC_STORE_MISALIGNED:
                signo = SIGBUS;
                code = BUS_ADRALN;
                break;
            case RISCV_EXC_LOAD_ACCESS_FAULT:
            case RISCV_EXC_STORE_ACCESS_FAULT:
                signo = SIGSEGV;
                code = SEGV_ACCERR;
                break;
            case RISCV_EXC_INSN_PAGE_FAULT:
            case RISCV_EXC_LOAD_PAGE_FAULT:
            case RISCV_EXC_STORE_PAGE_FAULT:
                signo = SIGSEGV;
                code = SEGV_MAPERR;
                break;
            default:
                signo = SIGILL;
                code = ILL_ILLOPC;
                break;
            }

            int code_signo = signo | (code << 8) | (FLTPAGE << 16);

#ifdef CONFIG_UMODE_VERBOSE_FAULT_REPORT
            /* Rich pre-teardown diagnostic (CONFIG_UMODE_VERBOSE_FAULT_REPORT).
             * Runs while the register state and address space are still intact;
             * deliver_fault() prints the one-line summary either way. Compiled
             * out when the option is off (default).
             */
            dump_umode_fault(act, scause);
#endif

            in_kcall_inc();
            in_kcall_phase(IN_KCALL_PHASE_FAULT);
            usr_fault(code_signo, act, r_stval());
            kernel_exit();
            /* NOTREACHED — kernel_exit decrements in_kcall. */
        }
        /* No thread_t for this U-mode fault — fall through to crash. */
    }

    /* S-mode exception — diagnostic + halt */
    uint64_t sepc = r_sepc();
    uint64_t stval = r_stval();

    /* Re-entry guard. If this CPU is already inside a crash dump (an outer trap
     * is mid-diagnostic and got pre-empted by us, or the saved sret target
     * itself faults and we keep coming back here), running the full diagnostic
     * again would interleave with the outer's pending writes line-by-line.
     * crash_recursive_check detects that, appends a one-line marker
     * (scause/sepc/stval), and calls crash_rendezvous() directly — NOT
     * returning. If we're not yet in crash mode it returns normally and the
     * diagnostic below runs as the first time.
     */
    crash_recursive_check(scause, sepc, stval);

    /* Enter per-CPU crash mode for the diagnostic torrent below. Every kprintf
     * on this CPU from now until crash_rendezvous() appends to a private 4 KiB
     * staging buffer instead of touching the global kprintf lock or the
     * console. Sibling CPUs that are simultaneously crashing do the same — each
     * in its own row of the per-CPU buffer. At the end, crash_rendezvous()
     * picks one CPU as the printer and dumps every buffer in CPU-id order, so
     * the diagnostics never interleave on the console.
     *
     * This replaces the older "IPI_PARKIT all peers + force-release the kprintf
     * lock" approach: the IPI grace period was racy (a peer in WFI with SIE off
     * didn't see it in time) and the force- release left the kprintf state
     * inconsistent across CPUs. The rendezvous needs neither.
     */
    kprintf_crash_mode_on();

    int from_umode = !(regs->sstatus & SSTATUS_SPP);
    thread_t *act = get_active_thread(get_cpunum());

    /* Defensive deref. During a crash, "act" (this CPU's pc_curthread) may be a
     * *freed* thread soul: _sfree poisons freed heap with 0x6d (alloc.c:246),
     * so act->process reads back 0x6d6d6d6d6d6d6d6d, survives a plain NULL
     * check, and the ->pid load (offset 8) faults at 0x6d6d6d6d6d6d6d75 — a
     * recursive load fault that fires *during* this very kprintf and destroys
     * the evidence of the ORIGINAL crash. A live kernel soul is 8-aligned and
     * >= KERNEL_VIRT_BASE; the 0x6d poison, user pointers, and other garbage
     * all fail that test, so we range-check before each deref and flag the
     * freed-active case instead of re-trapping. See lifecycle.c's freed-active
     * guard.
     */
    int act_ok = act && ((uintptr_t) act & 7) == 0 &&
                 (uintptr_t) act >= KERNEL_VIRT_BASE;
    process_t *aprp = act_ok ? act->process : NULL;
    int proc_ok = aprp && ((uintptr_t) aprp & 7) == 0 &&
                  (uintptr_t) aprp >= KERNEL_VIRT_BASE;
    kprintf("\n*** trap from %s on cpu %d (act=%p tid%d pid%d%s) ***\n",
            from_umode ? "U-mode" : "S-mode", get_cpunum(), (void *) act,
            act_ok ? (int) act->tid : -1, proc_ok ? (int) aprp->pid : -1,
            (act_ok && !proc_ok) ? " <ACT FREED/POISON>" : "");

    kprintf("  scause: %lu (%s)\n", scause, scause_name(scause));
    kprintf("  sepc:   0x%lx\n", sepc);
    kprintf("  stval:  0x%lx\n", stval);

    /* Walk satp's pgdir for the faulting VA so we can tell unmapped vs.
     * mapped-with-wrong-bits at a glance.
     */
    if (scause == RISCV_EXC_LOAD_PAGE_FAULT ||
        scause == RISCV_EXC_STORE_PAGE_FAULT ||
        scause == RISCV_EXC_INSN_PAGE_FAULT) {
        uint64_t satp = r_satp();
        uint64_t pgdir_pa = (satp & ((1UL << 44) - 1)) << 12;
        pagetable_t pgdir = (pagetable_t) (pgdir_pa + KERNEL_VIRT_BASE);
        pte_t *pte = cpu_pte_lookup(pgdir, (uintptr_t) stval);
        if (!pte) {
            unsigned l2_idx = (unsigned) ((stval >> 30) & 0x1ff);
            unsigned l1_idx = (unsigned) ((stval >> 21) & 0x1ff);
            unsigned long l2e = (unsigned long) pgdir[l2_idx];
            kprintf("  pgwalk: va=%lx UNMAPPED (pgdir@%p L2[%u]=%lx)\n",
                    (unsigned long) stval, pgdir, l2_idx, l2e);
            if (l2e & PTE_V) {
                pagetable_t l1 =
                    (pagetable_t) ((PTE2PA(l2e)) + KERNEL_VIRT_BASE);
                unsigned long l1e = (unsigned long) l1[l1_idx];
                kprintf("  pgwalk:   L1@%p[%u]=%lx\n", l1, l1_idx, l1e);
                if ((l1e & PTE_V) && !(l1e & (PTE_R | PTE_W | PTE_X))) {
                    pagetable_t l0 =
                        (pagetable_t) ((PTE2PA(l1e)) + KERNEL_VIRT_BASE);
                    unsigned l0_idx = (unsigned) ((stval >> 12) & 0x1ff);
                    unsigned long l0e = (unsigned long) l0[l0_idx];
                    kprintf("  pgwalk:   L0@%p[%u]=%lx\n", l0, l0_idx, l0e);

                    /* Show the neighbours too — guard pages are usually
                     * isolated zeros in a sea of valid leaves.
                     */
                    for (int d = -2; d <= 2; d++) {
                        if (d == 0)
                            continue;
                        int idx = (int) l0_idx + d;
                        if (idx < 0 || idx >= 512)
                            continue;
                        kprintf("  pgwalk:   L0[%d]=%lx\n", idx,
                                (unsigned long) l0[idx]);
                    }
                }
            }
        } else {
            unsigned long pv = (unsigned long) *pte;
            kprintf("  pgwalk: va=%lx pte=%lx pa=%lx [%c%c%c%c%c%c%c%c]\n",
                    (unsigned long) stval, pv, (unsigned long) PTE2PA(*pte),
                    (pv & PTE_V) ? 'V' : '-', (pv & PTE_R) ? 'R' : '-',
                    (pv & PTE_W) ? 'W' : '-', (pv & PTE_X) ? 'X' : '-',
                    (pv & PTE_U) ? 'U' : '-', (pv & (1UL << 5)) ? 'G' : '-',
                    (pv & (1UL << 6)) ? 'A' : '-',
                    (pv & (1UL << 7)) ? 'D' : '-');
        }
    }
    kprintf("  ra:     0x%lx\n", regs->gpr[RISCV_REG_RA]);
    kprintf("  sp:     0x%lx\n", regs->gpr[RISCV_REG_SP]);
    kprintf("  gp:     0x%lx\n", regs->gpr[RISCV_REG_GP]);
    kprintf("  tp:     0x%lx\n", regs->gpr[RISCV_REG_TP]);
    kprintf("  s0:     0x%lx\n", regs->gpr[RISCV_REG_FP]);
    kprintf("  s1:     0x%lx\n", regs->gpr[RISCV_REG_S1]);
    kprintf("  a0:     0x%lx  a1: 0x%lx  a2: 0x%lx\n", regs->gpr[RISCV_REG_A0],
            regs->gpr[RISCV_REG_A1], regs->gpr[RISCV_REG_A2]);
    kprintf("  a3:     0x%lx  a4: 0x%lx  a5: 0x%lx\n", regs->gpr[RISCV_REG_A3],
            regs->gpr[RISCV_REG_A4], regs->gpr[RISCV_REG_A5]);
    kprintf("  a6:     0x%lx  a7: 0x%lx\n", regs->gpr[RISCV_REG_A6],
            regs->gpr[RISCV_REG_A7]);
    kprintf("  t0:     0x%lx  t1: 0x%lx  t2: 0x%lx\n", regs->gpr[RISCV_REG_T0],
            regs->gpr[RISCV_REG_T1], regs->gpr[RISCV_REG_T2]);
    kprintf("  t3:     0x%lx  t4: 0x%lx  t5: 0x%lx  t6: 0x%lx\n",
            regs->gpr[RISCV_REG_T3], regs->gpr[RISCV_REG_T4],
            regs->gpr[RISCV_REG_T5], regs->gpr[RISCV_REG_T6]);
    kprintf("  satp:   0x%lx\n", r_satp());
    kprintf("  sstatus:0x%lx (saved)\n", regs->sstatus);

    /* Per-CPU function-entry trace ring dump. See <trace-ring.h>; adds zero
     * overhead when no TRACE_FN call sites are active in the build. Highly
     * useful for SMP timing-bug hunts: shows the last N functions entered on
     * the trapping CPU, in order.
     */
    trace_ring_dump(get_cpunum());

    /* Wide kernel-stack dump for the saved-ra-corruption hunt.
     *
     * Fires on S-mode page faults (scause 12/13/15) — those are the
     * fingerprints of "kernel ret jumped to a bad address". Walk the thread's
     * kernel stack from sp upward and print every 8-byte slot, annotated for
     * values that look like kernel pointers, user pointers, or known sentinels.
     * We expect to see the corrupting value (sepc/ra) in at least one slot —
     * that tells us which function's saved-ra got clobbered.
     *
     * Upper bound: the per-CPU kernel stack top from ksys_stack[cpu]. (An
     * earlier version used "tp" as the upper bound — wrong, tp is the kernel
     * TLS pointer in S-mode, not the stack top, and was typically tens of KiB
     * above sp. The 16 KiB cap then suppressed the dump on every real fault.
     * Stack top is the right ceiling.)
     */
    if (!from_umode && (scause == RISCV_EXC_LOAD_PAGE_FAULT ||
                        scause == RISCV_EXC_STORE_PAGE_FAULT ||
                        scause == RISCV_EXC_INSN_PAGE_FAULT)) {
        uint64_t sp_v = regs->gpr[RISCV_REG_SP];
        uint64_t tp_v = regs->gpr[RISCV_REG_TP];
        uint64_t bad_ra = regs->gpr[RISCV_REG_RA];
        uint64_t klo = 0xffffffc080000000UL;
        uint64_t khi = 0xffffffc100000000UL;

        /* Per-CPU stack top: ksys_stack[cpu] is the SP value loaded at trap
         * entry (top of stack, grows down). Walk from sp up to
         * min(ksys_stack[cpu], sp + 16 KiB). 16 KiB is the RTMux per-CPU kernel
         * stack size; matching it here gives a full-frame dump at most without
         * runaway prints.
         */
        unsigned cpu = get_cpunum();
        uint64_t top = (uint64_t) ksys_stack[cpu];
        uint64_t cap = sp_v + 0x4000UL;
        uint64_t end_v;
        if (top >= klo && top < khi && top > sp_v) {
            end_v = top < cap ? top : cap;
        } else {
            end_v = cap;
        }

        if (sp_v >= klo && sp_v < khi && end_v > sp_v) {
            kprintf(
                "\n--- kstack window [sp..end] cpu=%u sp=%lx end=%lx top=%lx "
                "tp=%lx (window=%lu bytes) ---\n",
                cpu, sp_v, end_v, top, tp_v, end_v - sp_v);
            uint64_t *p = (uint64_t *) sp_v;
            uint64_t *end = (uint64_t *) end_v;
            unsigned off = 0;
            while (p < end) {
                uint64_t v = *p;
                const char *tag = "    ";
                if (v == bad_ra)
                    tag = "<RA "; /* matches faulting ra */
                else if (v == tp_v)
                    tag = "<TP "; /* equals tp */
                else if (v == top)
                    tag = "<TOP"; /* equals stack top */
                else if (v == 0x40116)
                    tag = "<116"; /* old fingerprint */
                else if (v >= klo && v < khi)
                    tag = "<k  "; /* kernel pointer */
                else if (v != 0 && v < 0x100000000UL)
                    tag = "<u  "; /* low VA / user-ish */
                kprintf("  sp+0x%-4x %p  %016lx %s\n", off, (void *) p, v, tag);
                p++;
                off += 8;
            }
            kprintf("--- end kstack window ---\n");

            /* Also dump 64 bytes BELOW sp — that's where the trap frame itself
             * was just built (regs->* came from there) and where the next outer
             * frame's saved area begins.
             */
            kprintf("--- 64 bytes below sp (trap-frame head) ---\n");
            uint64_t *q = (uint64_t *) (sp_v - 64);
            for (int i = 0; i < 8; i++) {
                kprintf("  sp-0x%02x %p  %016lx\n", 64 - i * 8, (void *) q, *q);
                q++;
            }
            kprintf("--- end below-sp ---\n");

            /* Saved-RA digest: scan the same window for any 8-byte slot whose
             * value, taken as a kernel-text address, corresponds to "the byte
             * after a "jal" / "jalr" / "auipc; jalr" sequence" — i.e., a
             * plausible saved-RA. Print those alongside their offset so the
             * offending function's frame is identifiable in one glance even
             * when the corrupting RA is the only oddly-colored slot.
             */
            kprintf("--- saved-RA candidates in window ---\n");

            /* .text on riscv-virtio RTMux starts at 0xffffffc080200000; upper
             * bound covered with a generous 256 KiB cap so any commit that
             * grows the kernel still gets all RAs. If the upper bound is too
             * tight, bump it; spurious matches are cheap.
             */
            uint64_t txt_lo = 0xffffffc080200000UL;
            uint64_t txt_hi = 0xffffffc080240000UL;
            p = (uint64_t *) sp_v;
            off = 0;
            while (p < (uint64_t *) end_v) {
                uint64_t v = *p;
                if (v >= txt_lo && v < txt_hi) {
                    kprintf("  sp+0x%-4x %016lx (kernel text)\n", off, v);
                }
                p++;
                off += 8;
            }
            kprintf("--- end RA candidates ---\n");
        } else {
            kprintf(
                "\n[kstack dump skipped: sp out of kernel range or "
                "no valid window (sp=%lx end=%lx top=%lx tp=%lx)]\n",
                sp_v, end_v, top, tp_v);
        }
    }

    /* Dump the tracelogger ring buffer (if CONFIG_INSTRUMENTATION is on and a
     * buffer has been allocated). This gives us the last ~hundred trace events
     * leading up to the crash — invaluable for diagnosing SMP races.
     */
    {
        extern void tr_dump_to_console(void);
        tr_dump_to_console();
    }

    /* Drain the per-CPU staging buffer (and any sibling CPU that also crashed
     * and accumulated into its own buffer). The single elected printer dumps
     * every buffer in CPU-id order; no other CPU's bytes can interleave with
     * ours.
     */
    crash_rendezvous();
    /* NOTREACHED */
}
