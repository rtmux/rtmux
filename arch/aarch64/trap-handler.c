/* SPDX-License-Identifier: Apache-2.0 */
#include <core/globals.h>
#include <stdatomic.h>
#include <options.h>
#include <sys/fault.h>
_Static_assert(sizeof(CPU_REGISTERS) == 816, "AArch64 assembly frame size");
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

static void peripheral_interrupts(void)
{
    volatile uint32_t *ic = (void *) (KERNEL_VIRT_BASE + 0x3f00b200);
    uint32_t pending[] = {ic[1], ic[2], ic[0] & 0xff};
    for (unsigned bank = 0; bank < 3; bank++) {
        while (pending[bank]) {
            unsigned bit = (unsigned) __builtin_ctz(pending[bank]);
            pending[bank] &= pending[bank] - 1;
            unsigned vector = 32 + bank * 32 + bit;
            int level = get_interrupt_level(NULL, vector);
            if (level < 0)
                continue;
            intr_level_t *ilp = &interrupt_level[level];
            /* A level remains masked until its owner clears the device and
             * explicitly unmasks it; BCM2835 has no PLIC claim/complete. */
            interrupt_mask((unsigned) level, NULL);
            CRASHCHECK(intr_get());
            for (interrupt_t *itp = ilp->queue; itp; itp = itp->next) {
                itp->fire_count++;
                _TRACE_IRQ_CLAIM(ilp, itp);
                CRASHCHECK(intr_get());
                if (itp->kind == INTR_KIND_THREAD && itp->thread) {
                    thread_t *target = itp->thread;
                    if (target == get_active_thread(get_cpunum()) ||
                        target->state != STATE_INTR)
                        atomic_or((volatile unsigned *) &target->flags,
                                  RTMUX_FLG_THR_INTR_PENDING);
                    else
                        ready(target);
                } else if (itp->kind == INTR_KIND_EVENT) {
                    sigevent_deliver_inline(&itp->event, itp->thread, itp->nresv);
                }
            }
        }
    }
}

void aarch64_exception(CPU_REGISTERS *r, unsigned irq)
{
    unsigned cpu = get_cpunum();
    thread_t *act = get_active_thread(cpu);
    bool user = (r->pstate & 15) == 0;
    uint64_t esr, far;
    __asm__ volatile("mrs %0,esr_el1; mrs %1,far_el1" : "=r"(esr), "=r"(far));
    unsigned ec = (unsigned) (esr >> 26);
    if (user && act)
        act->reg = *r;
    if (irq) {
        atomic_add(&inkernel, 1);
        uint32_t source =
            *(volatile uint32_t *) (KERNEL_VIRT_BASE + 0x40000060 + cpu * 4);
        if (source & 16) {
            *(volatile uint32_t *) (KERNEL_VIRT_BASE + 0x400000c0 + cpu * 16) =
                ~0U;
            __asm__ volatile("dsb sy" ::: "memory");
        }
        unsigned cmds = atomic_exchange_explicit(&pcpu_array[cpu].pc_ipicmds, 0,
                                                 memory_order_acquire);
        if (cmds)
            pcpu_array[cpu].pc_ipi_handler_count++;
        if (cmds & IPI_TLB_FLUSH) {
            unsigned req = atomic_load_explicit(
                &pcpu_array[cpu].pc_tlb_flush_req, memory_order_acquire);
            cpu_tlb_flush();
            atomic_store_explicit(&pcpu_array[cpu].pc_tlb_flush_done, req,
                                  memory_order_release);
        }
        if (cmds & IPI_TLB_SAFE) {
            extern void set_safe_as(unsigned);
            set_safe_as(cpu);
        }
        if (cmds & IPI_CLOCK_LOAD)
            arch_timer_reload(qtimeptr);
        if (cmds & IPI_CRASH)
            crash_quiesce_self();
        if (cmds & IPI_PARKIT)
            halt();
        if (cmds & IPI_RESCHED && act)
            atomic_fetch_or_explicit(&act->async_flags, RTMUX_ATF_SMP_RESCHED,
                                     memory_order_release);
        if (source & 8) {
            extern _Atomic uint64_t stimer_intr_count[];
            atomic_fetch_add_explicit(&stimer_intr_count[cpu], 1,
                                      memory_order_relaxed);
            if (in_kernel_context() || !pcpu_array[cpu].pc_alive)
                arch_timer_reload(qtimeptr);
            else {
                for (interrupt_t *itp = interrupt_level[0].queue; itp;
                     itp = itp->next) {
                    itp->fire_count++;
                    if (itp->handler)
                        (void) itp->handler(itp->area, itp->id);
                }
            }
        }
        if (source & (1U << 8)) {
            interrupt_disable();
            peripheral_interrupts();
        }
        atomic_sub(&inkernel, 1);
        if (user) {
            in_kcall_inc();
            in_kcall_phase(IN_KCALL_PHASE_DISPATCH);
            kernel_exit();
        }
        return;
    }
    if (ec == 0x15 && user && act) {
        unsigned num = (unsigned) r->gpr[8];
        union kcall_args kargs;
        memset(&kargs, 0, sizeof(kargs));
        uintptr_t *kap = (uintptr_t *) &kargs;
        for (unsigned i = 0; i < 6; i++)
            kap[i] = act->kcall_c_args[i] = r->gpr[i];
        in_kcall_inc();
        if (num >= __KSYS_BAD) {
            SETREGSTATUS(&act->reg, -ENOSYS);
            kernel_exit();
        }
        act->syscall = (int) num;
        in_kcall_phase(num == __KSYS_PRIV ? priv_call_phase((unsigned) kap[0])
                                          : (int) num);
        __atomic_store_n(&pcpu_array[cpu].pc_syscall, (int) num,
                         __ATOMIC_RELEASE);
        SETREGSTATUS(&act->reg, 0);
        REGTYPE(&act->reg) = num;
        atomic_clr(&act->flags,
                   RTMUX_FLG_THR_KCALL_ERR_SET | RTMUX_FLG_THR_BUFF_MSG);
        interrupt_enable();
        /* Match the shared RISC-V kcall trace contract: snapshot identity
         * before a handler may reap its caller, and exclude peer wait time. */
        unsigned kcall_tr = _TRACE_KCALL_ARMED(num);
        uint32_t kcall_pid = kcall_tr ? _TRACE_KCALL_PID(act) : 0U;
        uint32_t kcall_tid = kcall_tr ? _TRACE_KCALL_TID(act) : 0U;
        uint64_t kcall_t0 = _TRACE_KCALL_TICKS(kcall_tr);
        int ret = ksys_call_table[num](act, &kargs);
        _TRACE_KCALL_EMIT(kcall_pid, kcall_tid, num, kcall_t0);
        lookup_rcvid_release();
        if (ret != ENOERROR && get_active_thread(cpu) == act &&
            act->state == STATE_RUNNING)
            SETREGSTATUS(&act->reg, -(long) ret);
        kernel_exit();
    }
    if (!user && (ec == 0x25 || ec == 0x21) && far < KERNEL_VIRT_BASE) {
        const struct fault_handlers *fh = pcpu_array[cpu].pc_xfer_handlers;
        if (fh && fh->fault)
            fh->fault(act, r, (esr & (1UL << 6)) ? SIGCODE_STORE : 0);
    }
    if (user && act) {
        kprintf("AArch64 user fault pid=%d tid=%u pc=%lx far=%lx esr=%lx\n",
                act->process->pid, act->tid, r->pc, far, esr);
        int sig = (ec == 0x20 || ec == 0x24) ? SIGSEGV : SIGILL;
        in_kcall_inc();
        in_kcall_phase(IN_KCALL_PHASE_FAULT);
        usr_fault(sig | (SEGV_MAPERR << 8) | (FLTPAGE << 16), act, far);
        kernel_exit();
    }
    kprintf(
        "AArch64 KERNEL FAULT cpu=%u pc=%lx lr=%lx sp=%lx far=%lx esr=%lx\n",
        cpu, r->pc, r->gpr[30], r->gpr[31], far, esr);
    halt();
}
