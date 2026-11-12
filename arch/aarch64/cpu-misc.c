/* SPDX-License-Identifier: Apache-2.0 */
#include <core/globals.h>
#include <stdatomic.h>
#include <options.h>
extern _Atomic int timer_drain_busy;
extern void aarch64_restore(CPU_REGISTERS *, uintptr_t)
    __attribute__((noreturn));
static void aarch64_set_timer(uint64_t when)
{
    __asm__ volatile(
        "msr cntv_cval_el0, %0; msr cntv_ctl_el0, %1; isb" ::"r"(when), "r"(1UL)
        : "memory");
}
void cpu_force_fpu_save(thread_t *t)
{
    (void) t;
}
void cpu_mutex_adjust(thread_t *t)
{
    (void) t;
}
uintptr_t next_instruction(CPU_REGISTERS *r)
{
    return r->pc + 4;
}
uint64_t clock_cycles(void)
{
    return r_time();
}
uint64_t cpu_clock_ticks(void)
{
    return r_time();
}
void halt(void)
{
    intr_disable();
    for (;;)
        cpu_halt();
}
void cpu_shutdown(void)
{
    halt();
}
void cpu_reboot(void)
{
    halt();
}
void cpu_start_others(uintptr_t start)
{
    if (start == ~0UL)
        return;
    start -= KERNEL_VIRT_BASE;
    for (unsigned cpu = 1; cpu < num_processors; cpu++) {
        volatile uint64_t *release =
            (void *) (KERNEL_VIRT_BASE + 0xd8 + cpu * 8);
        *release = start;
        __asm__ volatile("dc cvac,%0; dsb sy; sev" ::"r"(release) : "memory");
    }
}
void send_ipi(unsigned cpu, int cmd)
{
    atomic_fetch_or_explicit(&pcpu_array[cpu].pc_ipicmds, (unsigned) cmd,
                             memory_order_release);
    __asm__ volatile("dsb ishst" ::: "memory");
    *(volatile uint32_t *) (KERNEL_VIRT_BASE + 0x40000080 + cpu * 16) = 1;
}
void cpu_icache_sync_all(void)
{
    cpu_icache_sync();
}
void smp_flush_tlb(void)
{
    cpu_tlb_flush();
}
void idle_run_thread(thread_t *t)
{
    aarch64_restore(&t->reg, ksys_stack[get_cpunum()]);
}
void cpu_syspage_init(void) {}

void cpu_thread_init(thread_t *act, thread_t *thp, int align)
{
    (void) act;
    (void) align;

    memset(&thp->reg, 0, sizeof(thp->reg));

    thp->reg.tls = (uintptr_t) thp->un.lcl.tls;

    thp->reg.pstate = 5;
}

void cpu_thread_waaa(thread_t *thp)
{
    thp->reg.tls = (uintptr_t) thp->un.lcl.tls;

    thp->reg.gpr[AARCH64_REG_A0] = (uintptr_t) thp->un.lcl.tls->__arg;

    if (thp->un.lcl.tls->__exitfunc)
        thp->reg.gpr[AARCH64_REG_RA] = (uintptr_t) thp->un.lcl.tls->__exitfunc;
}

void cpu_thread_priv(thread_t *thp)
{
    (void) thp;
}

void cpu_thread_destroy(thread_t *thp)
{
    (void) thp;
}

void cpu_thread_align_fault(thread_t *thp)
{
    (void) thp;
}

void cpu_intr_attach(interrupt_t *itp, thread_t *thp)
{
    (void) thp;
    /* BCM2836 has one route for the entire BCM2835 peripheral controller.
     * Keep every peripheral IST on CPU0, rather than silently moving other
     * devices when a new driver attaches on a different CPU. */
    if (interrupt_level[itp->level].info->vector_base == 32)
        itp->owner_hart = 0;
}

int aarch64_console_irq_owned(void)
{
    if (!interrupt_level)
        return 0;
    int level = get_interrupt_level(NULL, 32 + 57);
    /* Only test the head for presence, without dereferencing it. Detachment
     * releases console input back to the boot console after the last owner. */
    return level >= 0 &&
           __atomic_load_n(&interrupt_level[level].queue, __ATOMIC_ACQUIRE) != NULL;
}

void cpu_greg_load(thread_t *thp, CPU_REGISTERS *regs)
{
    thp->reg = *regs;
}

void cpu_invoke_func(thread_t *thp, uintptr_t addr)
{
    SETKIP(thp, addr);
}

void cpu_thread_enter_umode(thread_t *thp)
{
    uintptr_t saved_pc = thp->reg.pc;
    uintptr_t saved_sp = REGSP(&thp->reg);

    memset(&thp->reg.gpr, 0, sizeof(thp->reg.gpr));

    thp->reg.pc = saved_pc;
    SETREGSP(&thp->reg, saved_sp);
    thp->reg.gpr[0] = saved_sp;

    thp->reg.tls = (uintptr_t) thp->un.lcl.tls;

    thp->reg.pstate = 0;
    memset(thp->reg.q, 0, sizeof(thp->reg.q));
    thp->reg.fpcr = thp->reg.fpsr = 0;

    if (thp->fpudata) {
        fpu_drop_claims(thp);
        memset(FPUDATA_PTR(thp->fpudata), 0, sizeof(FPU_REGISTERS));
    }
}

unsigned arch_timer_value(struct qtime_entry *qtp)
{
    (void) qtp;
    return 0;
}

static uint64_t timer_base_cycle;
static uint64_t timer_base_nsec;

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

#define TIMER_BACKLOG_DELTA_NS 100000ULL
extern volatile uint8_t timer_backlog;

#define IDLE_MAX_SLEEP_NS 50000000ULL

static void arch_timer_program_next(struct qtime_entry *qtp, int idle)
{
    uint64_t delta_ns = qtp->nsec_inc;
    uint64_t next;

    if (idle) {
        uint64_t now = arch_timer_now_ns(qtp);
        uint64_t horizon = now + IDLE_MAX_SLEEP_NS;

        next = clock_next_deadline(qtp, get_cpunum(), NULL, NULL);
        if (next == ~(uint64_t) 0 || next > horizon)
            next = horizon;

        delta_ns = next > now ? next - now : TIMER_BACKLOG_DELTA_NS;
        atomic_or_64(&qtp->flags, QTIME_FLAG_TICKLESS);
        aarch64_set_timer(r_time() + timer_delta_cycles(qtp, delta_ns));
        return;
    }

    if (get_cpunum() == boot_cpunum) {
        uint64_t now = arch_timer_now_ns(qtp);
        timer_next_deadline(&next);

        if (next != ~(uint64_t) 0 && next > now && next - now < delta_ns)
            delta_ns = next - now;

        if (timer_backlog && delta_ns > TIMER_BACKLOG_DELTA_NS)
            delta_ns = TIMER_BACKLOG_DELTA_NS;
    }

    atomic_clr_64(&qtp->flags, QTIME_FLAG_TICKLESS);
    aarch64_set_timer(r_time() + timer_delta_cycles(qtp, delta_ns));
}

int arch_timer_reload(struct qtime_entry *qtp)
{
    if (!kopt.disable_timer)
        arch_timer_program_next(qtp, 0);
    return 1;
}

int arch_timer_load(struct qtime_entry *qtp)
{
    if (!kopt.disable_timer) {
        int en = intr_get();
        interrupt_disable();

        atomic_fetch_add_explicit(&timer_base_seq, 1u, memory_order_release);
        atomic_thread_fence(memory_order_release);
        timer_base_cycle = r_time();
        timer_base_nsec = qtp->nsec;
        atomic_thread_fence(memory_order_release);
        atomic_fetch_add_explicit(&timer_base_seq, 1u, memory_order_release);
        arch_timer_program_next(qtp, 0);

        if (en)
            interrupt_enable();
    }
    return 1;
}

void cpu_idle(unsigned cpu)
{
    /* A blocked syscall can reach idle with IRQs enabled. A timer between
     * idle_pick_thread/set_active_thread and in_kcall_inc would dispatch on
     * partially installed state and lose the selected thread. */
    interrupt_disable();
    cpu_tlb_flush();
    pcpu_array[cpu].pc_alive = true;
    for (;;) {
        if (pcpu_array[cpu].pc_idle_thp)
            pcpu_array[cpu].pc_idle_thp->state = STATE_RUNNING;

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
            }
        }

        thread_t *thp = idle_pick_thread(cpu);
        CRASHCHECK(intr_get());
        if (thp) {
            if (!thp->process) {
                kprintf(
                    "FATAL: cpu_idle cpu=%u picked thread %p tid=%d "
                    "process=NULL\n",
                    cpu, (void *) thp, thp->tid);
                crash();
            }

            if (pcpu_array[cpu].pc_idle_thp)
                pcpu_array[cpu].pc_idle_thp->state = STATE_READY;

            sched_account_restart();
            set_active_thread(cpu, thp);
            pcpu_array[cpu].pc_active_prp = thp->process;

            if (!kopt.disable_timer)
                arch_timer_reload(qtimeptr);

            in_kcall_inc();
            in_kcall_phase(IN_KCALL_PHASE_DISPATCH);
            kernel_exit();

        } else {
            if (pcpu_array[cpu].pc_idle_thp)
                set_active_thread(cpu, pcpu_array[cpu].pc_idle_thp);

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

            pcpu_array[cpu].pc_pending_preempt_prio = 0;

            if (roottask_prp && roottask_prp->memory &&
                pcpu_array[cpu].pc_as_prp != roottask_prp) {
                vm_ops->as(roottask_prp, &pcpu_array[cpu].pc_as_prp);
            }

            if (!kopt.disable_timer)
                arch_timer_program_next(qtimeptr, 1);

            {
                thread_t *late = idle_pick_thread(cpu);
                CRASHCHECK(intr_get());
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
                    kernel_exit();
                }
            }

            rcu_sched_quiescent();
            __asm__ volatile("wfi" ::: "memory");
            interrupt_enable();
            interrupt_disable();
        }
    }
}

void mmu_set_domain(unsigned domain)
{
    (void) domain;
}

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

unsigned num_processors;
void cpu_board_interrupt_init(void) {}
void aarch64_secondary_entry(unsigned cpu)
{
    extern void init_traps(void), init_cpu(void);
    cpu_set_kernel_tp(&pcpu_array[cpu]);
    pcpu_array[cpu].pc_hart = cpu;
    init_traps();
    init_cpu();
    extern void aarch64_idle_enter(unsigned, uintptr_t)
        __attribute__((noreturn));
    aarch64_idle_enter(cpu, ksys_stack[cpu]);
}
