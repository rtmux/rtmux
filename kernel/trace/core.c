/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdarg.h>
#include <core/globals.h>

#if defined(CONFIG_INSTRUMENTATION)

#include <unistd.h>
#include <string.h>

/* Event/interrupt handler invocation (generic, no arch-specific asm needed).
 * The handler has already been cast to the proper function-pointer type at each
 * call site (usually int(*)(event_data_t *); in the buffer-invoke path it's
 * uint32_t(*)(int)). "area" is passed through as-is.
 */
#define _TRACE_EH_INVOKE(handler, area, cpus) ((uint32_t) (*(handler))(area))

/* Local macro definitions of conditional execution of the event handler Check
 * handlers e1/e2 and invoke exe_*_event_h*() with uniform uint32_t arg slots.
 * Explicit (uint32_t) casts at the macro level absorb the mixed
 * pid_t/int/uint32_t call-site types — the trace wire format takes 32-bit
 * values regardless.
 */
#define _TRACE_CKH_EXE_PT_EH(e1, e2, a1, a2, a3, a4, a5)                      \
    ((!(e1) ||                                                                \
      exe_pt_event_h((e1), (uint32_t) (a1), (uint32_t) (a2), (uint32_t) (a3), \
                     (uint32_t) (a4), (uint32_t) (a5))) &&                    \
     (!(e2) ||                                                                \
      exe_pt_event_h((e2), (uint32_t) (a1), (uint32_t) (a2), (uint32_t) (a3), \
                     (uint32_t) (a4), (uint32_t) (a5))))

#define _TRACE_CKH_EXE_EH(e1, e2, a1, a2, a3)                                  \
    ((!(e1) ||                                                                 \
      exe_event_h((e1), (uint32_t) (a1), (uint32_t) (a2), (uint32_t) (a3))) && \
     (!(e2) ||                                                                 \
      exe_event_h((e2), (uint32_t) (a1), (uint32_t) (a2), (uint32_t) (a3))))

#define _TRACE_CKH_EXE_PT_EHB(e1, e2, a1, a2, a3, a4, a5)                      \
    ((!(e1) || exe_pt_event_h_buff((e1), (uint32_t) (a1), (uint32_t) (a2),     \
                                   (uint32_t) (a3), (a4), (uint32_t) (a5))) && \
     (!(e2) || exe_pt_event_h_buff((e2), (uint32_t) (a1), (uint32_t) (a2),     \
                                   (uint32_t) (a3), (a4), (uint32_t) (a5))))

#define _TRACE_CKH_EXE_EHB(e1, e2, a1, a2, a3)                              \
    ((!(e1) ||                                                              \
      exe_event_h_buff(1, (e1), (uint32_t) (a1), (a2), (uint32_t) (a3))) && \
     (!(e2) ||                                                              \
      exe_event_h_buff(1, (e2), (uint32_t) (a1), (a2), (uint32_t) (a3))))

#define _TRACE_CKH_EXE_EHBNL(e1, e2, a1, a2, a3)                            \
    ((!(e1) ||                                                              \
      exe_event_h_buff(0, (e1), (uint32_t) (a1), (a2), (uint32_t) (a3))) && \
     (!(e2) ||                                                              \
      exe_event_h_buff(0, (e2), (uint32_t) (a1), (a2), (uint32_t) (a3))))

#define _TRACE_CKH_EXE_EHNL(e1, e2, a1, a2, a3)                            \
    ((!(e1) || exe_event_h_no_lock((e1), (uint32_t) (a1), (uint32_t) (a2), \
                                   (uint32_t) (a3))) &&                    \
     (!(e2) || exe_event_h_no_lock((e2), (uint32_t) (a1), (uint32_t) (a2), \
                                   (uint32_t) (a3))))

/* Local macro definition clears the buffer */
#define _TRACE_CLR_BUFF(b_p)                 \
    {                                        \
        dropped_buffers++;                   \
        dropped_events += b_p->h.num_events; \
        b_p->h.tail_ptr = b_p->h.begin_ptr;  \
        b_p->h.num_events = 0U;              \
        b_p->h.flags = 0U;                   \
    }

static unsigned dropped_events;
static unsigned dropped_buffers;

/* Triggers pseudo-interrupt handler */
int em_event(tracebuf_t *t_b_p)
{
    interrupt_t *itp;

    if (dropped_events != 0) {
        if (kopt.loglevel > 2)
            kprintf("INSTR: dropped %u events!\n", dropped_events);
        dropped_events = 0;
    }
    if (dropped_buffers != 0) {
        if (kopt.loglevel > 2)
            kprintf("INSTR: dropped %u buffers!\n", dropped_buffers);
        dropped_buffers = 0;
    }
    if ((itp = interrupt_level[HOOK_TO_LEVEL(RTMUX_HOOK_TRACE)].queue)) {
        process_t *pprp;
        uint32_t r_vlocal;
        unsigned cpu = get_cpunum();
        thread_t *thp = itp->thread;

        trace_force_flush = 0;


        t_b_p->h.begin_ptr->data[2] = t_b_p->h.num_events;
        pprp = pcpu_array[cpu].pc_as_prp;
        if (thp->as_prp && thp->as_prp != pprp) {
            vm_ops->as(thp->as_prp, &pcpu_array[cpu].pc_as_prp);
        }

        /* invoke pseudo interrupt handler pass trace buffer and sequence number
         * as first argument ASSUMPTION: kernel trace buffers are contiguous
         */
        if ((r_vlocal = _TRACE_EH_INVOKE(
                 (uint32_t (*)(int)) itp->handler,
                 (int) (t_b_p->h.ls13_rb_num | (t_b_p->h.seq_buff_num & 0x7ff)),
                 itp->cpu))) {
            /* Deliver inline via sigevent_deliver_inline(). The trace handler
             * runs in kernel extension context (from a trace point inside a
             * kernel extension), so direct delivery is correct.
             */
            sigevent_deliver_inline(
                TRACE_PACK_PTR(const struct sigevent *, r_vlocal), thp, 0);
        }
        if (pprp && pprp != pcpu_array[cpu].pc_as_prp) {
            vm_ops->as(pprp, &pcpu_array[cpu].pc_as_prp);
        }
        return EOK;
    }
    return ENOTSUP;
}

/* Executes an event handler with two data arguments and pid/tid */
uint32_t exe_pt_event_h(ehandler_data_t *stor_ptr,
                        uint32_t header,
                        uint32_t pid,
                        uint32_t tid,
                        uint32_t d1,
                        uint32_t d2)
{
    event_data_t *area = stor_ptr->area;
    unsigned cpu = get_cpunum();
    thread_t *thp = stor_ptr->thp;
    process_t *pp = pcpu_array[cpu].pc_as_prp;
    uint32_t r_vlocal;
    unsigned long state;
    void *save_tls;
    struct cpupage_entry *cpu_p = cpupageptr[cpu];

    interrupt_disable();
    state = cpu_p->state;
    cpu_p->state = state | 0x2;
    save_tls = cpu_p->tls;
    cpu_p->tls = &intr_tls;
    interrupt_enable();
    if (thp->as_prp && thp->as_prp != pp) {
        vm_ops->as(thp->as_prp, &pcpu_array[cpu].pc_as_prp);
    }

    area->header = (__traceentry) header;
    area->el_num = 2U;
    area->feature_mask = RTMUX_TRACE_FMPID | RTMUX_TRACE_FMTID;
    ((uint32_t *) area->feature)[RTMUX_TRACE_FIPID] = (uint32_t) pid;
    ((uint32_t *) area->feature)[RTMUX_TRACE_FITID] = (uint32_t) tid;
    ((uint32_t *) area->data_array)[0] = d1;
    ((uint32_t *) area->data_array)[1] = d2;

    r_vlocal = _TRACE_EH_INVOKE(stor_ptr->handler, area, stor_ptr->cpu);
    if (pp && pp != pcpu_array[cpu].pc_as_prp) {
        vm_ops->as(pp, &pcpu_array[cpu].pc_as_prp);
    }
    interrupt_disable();
    cpu_p->state = state;
    cpu_p->tls = save_tls;
    interrupt_enable();
    return r_vlocal;
}

/* Executes an event handler with two data arguments */
uint32_t exe_event_h(ehandler_data_t *stor_ptr,
                     uint32_t header,
                     uint32_t d1,
                     uint32_t d2)
{
    event_data_t *area = stor_ptr->area;
    unsigned cpu = get_cpunum();
    thread_t *thp = stor_ptr->thp;
    process_t *pp = pcpu_array[cpu].pc_as_prp;
    uint32_t r_vlocal;
    unsigned long state;
    void *save_tls;
    struct cpupage_entry *cpu_p = cpupageptr[cpu];

    interrupt_disable();
    state = cpu_p->state;
    cpu_p->state = state | 0x2;
    save_tls = cpu_p->tls;
    cpu_p->tls = &intr_tls;
    interrupt_enable();
    if (thp->as_prp && thp->as_prp != pp) {
        vm_ops->as(thp->as_prp, &pcpu_array[cpu].pc_as_prp);
    }

    area->header = (__traceentry) header;
    area->el_num = 2U;
    area->feature_mask = RTMUX_TRACE_FMEMPTY;
    ((uint32_t *) area->data_array)[0] = d1;
    ((uint32_t *) area->data_array)[1] = d2;

    r_vlocal = _TRACE_EH_INVOKE(stor_ptr->handler, area, stor_ptr->cpu);
    if (pp && pp != pcpu_array[cpu].pc_as_prp) {
        vm_ops->as(pp, &pcpu_array[cpu].pc_as_prp);
    }
    interrupt_disable();
    cpu_p->state = state;
    cpu_p->tls = save_tls;
    interrupt_enable();
    return r_vlocal;
}

/* Executes an event handler with buffer type arguments and pid/tid */
uint32_t exe_pt_event_h_buff(ehandler_data_t *stor_ptr,
                             uint32_t header,
                             uint32_t pid,
                             uint32_t tid,
                             void *buff,
                             uint32_t size)
{
    event_data_t *area = stor_ptr->area;
    unsigned cpu = get_cpunum();
    thread_t *thp = stor_ptr->thp;
    process_t *pp = pcpu_array[cpu].pc_as_prp;
    uint32_t r_vlocal;
    unsigned long state;
    void *save_tls;
    struct cpupage_entry *cpu_p = cpupageptr[cpu];

    interrupt_disable();
    state = cpu_p->state;
    cpu_p->state = state | 0x2;
    save_tls = cpu_p->tls;
    cpu_p->tls = &intr_tls;
    interrupt_enable();
    if (thp->as_prp && thp->as_prp != pp) {
        vm_ops->as(thp->as_prp, &pcpu_array[cpu].pc_as_prp);
    }

    area->header = (__traceentry) header;
    area->el_num = (uint32_t) _TRACE_ROUND_UP(size);
    area->feature_mask = RTMUX_TRACE_FMPID | RTMUX_TRACE_FMTID;
    ((uint32_t *) area->feature)[RTMUX_TRACE_FIPID] = (uint32_t) pid;
    ((uint32_t *) area->feature)[RTMUX_TRACE_FITID] = (uint32_t) tid;
    (void) memcpy((void *) area->data_array, buff, (size_t) size);

    r_vlocal = _TRACE_EH_INVOKE(stor_ptr->handler, area, stor_ptr->cpu);
    if (pp && pp != pcpu_array[cpu].pc_as_prp) {
        vm_ops->as(pp, &pcpu_array[cpu].pc_as_prp);
    }
    interrupt_disable();
    cpu_p->state = state;
    cpu_p->tls = save_tls;
    interrupt_enable();
    return r_vlocal;
}

/* Executes an event handler with buffer type arguments */
uint32_t exe_event_h_buff(int locked,
                          ehandler_data_t *stor_ptr,
                          uint32_t header,
                          void *buff,
                          uint32_t size)
{
    event_data_t *area = stor_ptr->area;
    unsigned cpu = get_cpunum();
    thread_t *thp = stor_ptr->thp;
    process_t *pp = pcpu_array[cpu].pc_as_prp;
    uint32_t r_vlocal;
    unsigned long state;
    void *save_tls;
    struct cpupage_entry *cpu_p = cpupageptr[cpu];

    (void) locked; /* unused */
    interrupt_disable();
    state = cpu_p->state;
    cpu_p->state = state | 0x2;
    save_tls = cpu_p->tls;
    cpu_p->tls = &intr_tls;
    interrupt_enable();
    if (thp->as_prp && thp->as_prp != pp) {
        vm_ops->as(thp->as_prp, &pcpu_array[cpu].pc_as_prp);
    }

    area->header = (__traceentry) header;
    area->el_num = (uint32_t) _TRACE_ROUND_UP(size);
    area->feature_mask = RTMUX_TRACE_FMEMPTY;
    (void) memcpy((void *) area->data_array, buff, (size_t) size);

    r_vlocal = _TRACE_EH_INVOKE(stor_ptr->handler, area, stor_ptr->cpu);
    if (pp && pp != pcpu_array[cpu].pc_as_prp) {
        vm_ops->as(pp, &pcpu_array[cpu].pc_as_prp);
    }
    interrupt_disable();
    cpu_p->state = state;
    cpu_p->tls = save_tls;
    interrupt_enable();
    return r_vlocal;
}

/* Executes an event handler with buffer type arguments and no locking */
static uint32_t exe_event_h_no_lock(ehandler_data_t *stor_ptr,
                                    uint32_t header,
                                    uint32_t d1,
                                    uint32_t d2)
{
    event_data_t *area = stor_ptr->area;
    unsigned cpu = get_cpunum();
    thread_t *thp = stor_ptr->thp;
    process_t *pp = pcpu_array[cpu].pc_as_prp;
    uint32_t r_vlocal;
    unsigned long state;
    void *save_tls;
    struct cpupage_entry *cpu_p = cpupageptr[cpu];

    interrupt_disable();
    state = cpu_p->state;
    cpu_p->state = state | 0x2;
    save_tls = cpu_p->tls;
    cpu_p->tls = &intr_tls;
    interrupt_enable();
    if (thp->as_prp && thp->as_prp != pp) {
        vm_ops->as(thp->as_prp, &pcpu_array[cpu].pc_as_prp);
    }

    area->header = (__traceentry) header;
    area->el_num = 2U;
    area->feature_mask = RTMUX_TRACE_FMEMPTY;
    ((uint32_t *) area->data_array)[0] = d1;
    ((uint32_t *) area->data_array)[1] = d2;

    r_vlocal = _TRACE_EH_INVOKE(stor_ptr->handler, area, stor_ptr->cpu);
    if (pp && pp != pcpu_array[cpu].pc_as_prp) {
        vm_ops->as(pp, &pcpu_array[cpu].pc_as_prp);
    }
    interrupt_disable();
    cpu_p->state = state;
    cpu_p->tls = save_tls;
    interrupt_enable();

    return r_vlocal;
}

/* Destroys event handler */
void destroy_eh(process_t *prp)
{
    if (prp) {
        uint32_t i;

        interrupt_lock(&trace_masks.eh_spin);
        for (i = 0; trace_masks.eh_num && i < _TRACE_MAX_EV_HANDLER_NUM; ++i) {
            if (trace_masks.eh_storage[i].process == prp) {
                *trace_masks.eh_storage[i].location = NULL;
                trace_masks.eh_storage[i].location = NULL;
                trace_masks.eh_storage[i].area = NULL;
                trace_masks.eh_storage[i].thp = NULL;
                trace_masks.eh_storage[i].process = NULL;
                trace_masks.eh_storage[i].handler = NULL;
                --trace_masks.eh_num;
            }
        }
        interrupt_unlock(&trace_masks.eh_spin);
    }

    return;
}

/* Emits time control events */
void time_em(uint32_t msb, uint32_t lsb)
{
    uint32_t header = _TRACE_MAKE_CODE(get_cpunum(), _TRACE_STRUCT_S,
                                       _TRACE_CONTROL_C, _TRACE_CONTROL_TIME);
    if (_TRACE_CKH_EXE_EH(trace_masks.class_control_ehd_p,
                          trace_masks.control_time_ehd_p, header, msb, lsb)) {
        (void) add_trace_event(header, 0, msb, lsb);
    }

    return;
}

static int force_emit[_TRACE_MAX_SMP_CPU_NUM];


/* Generates time control events */
static uint32_t get_time_off(int init)
{
    static union {
        uint64_t timestamp;
        uint32_t bits[2];
    } ts[_TRACE_MAX_SMP_CPU_NUM];
    uint32_t lsb;
    unsigned cpu = get_cpunum();
    int force;

    force = init || force_emit[cpu];
    force_emit[cpu] = 0;

#if (_TRACE_CLOCK_SWAP != 0)
    if (init)
        memset(ts, 0, sizeof(ts));
#endif
    lsb = ts[cpu].bits[_TRACE_CLOCK_LSB];
    ts[cpu].timestamp = cpu_clock_ticks();
#if CONFIG_PROCESSORS_MAX > 1 && defined(CONFIG_INSTRUMENTATION)
    ts[cpu].timestamp -= pcpu_array[cpu].pc_clockcycles_offset;
#endif
    if (ts[cpu].bits[_TRACE_CLOCK_LSB] == lsb) {
        /* Make sure the timestamps are unique */
        ts[cpu].bits[_TRACE_CLOCK_LSB]++;
    }
    if (force || ts[cpu].bits[_TRACE_CLOCK_LSB] < lsb) {
#if (_TRACE_CLOCK_SWAP != 0)
        static uint32_t msb[_TRACE_MAX_SMP_CPU_NUM];
        if (init) {
            memset(msb, 0, sizeof(msb));
        }
        if (ts[cpu].bits[_TRACE_CLOCK_LSB] < lsb) {
            ++msb[cpu];
        }
        ts[cpu].bits[_TRACE_CLOCK_MSB] = msb[cpu];
#endif
        _TRACE_EMIT_CONTROL_TIME(ts[cpu].bits[_TRACE_CLOCK_MSB],
                                 ts[cpu].bits[_TRACE_CLOCK_LSB]);
    }

    return ts[cpu].bits[_TRACE_CLOCK_LSB];
}

int trace_flushbuffer(void)
{
    tracebuf_t *tracebuf = privateptr->tracebuf;

    if (tracebuf) {
        if (trace_masks.ring_mode) {
            if (!trace_masks.main_flags) {
                (void) em_event(tracebuf);
            } else {
                return ECANCELED;
            }
        } else {
            interrupt_disable();
            SPINLOCK(&trace_masks.state_spin);
            SPINLOCK(&tracebuf->h.spin);
            if (!(tracebuf->h.flags & _TRACE_FLAGS_FLUSH)) {
                tracebuf_t *b_p = tracebuf->h.next;

                tracebuf->h.flags |= _TRACE_FLAGS_FLUSH;

                privateptr->tracebuf = b_p;

                SPINUNLOCK(&tracebuf->h.spin);
                SPINUNLOCK(&trace_masks.state_spin);

                memset(&force_emit, 0xff, sizeof(force_emit));
                (void) em_event(tracebuf);
            } else {
                SPINUNLOCK(&tracebuf->h.spin);
                SPINUNLOCK(&trace_masks.state_spin);
            }
            interrupt_enable();
        }

        return EOK;
    } else {
        return EFAULT;
    }
}

/* Intercepts thread states */
void th_em_st(thread_t *thp, uint32_t s, unsigned cpu)
{
    unsigned bufsize;
    CRASHCHECK(TYPE_MASK(thp->type) != TYPE_THREAD);
    if (_TRACE_THPTID(thp, s)) {
        struct buf {
            uint32_t pid;
            uint32_t tid;
            uint32_t priority;
            uint32_t policy;

            /* remaining fields present only if and external scheduler is loaded
             */
            uint32_t partition_id;
            uint32_t sched_flags; /* to indicate if threads runs or is billed
                                     critical. see macros.h:: CRITICAL_RUN_*
                                     flags.
                                     */
        } buf;
        uint32_t header =
            _TRACE_MAKE_CODE(cpu, _TRACE_STRUCT_S, _TRACE_PR_TH_C, s);
        buf.pid = (uint32_t) thp->process->pid;
        buf.tid = (uint32_t) thp->tid + 1;

        if (_TRACE_THREAD_ARG_WIDE & trace_masks.th_mask) {
            buf.priority = thp->priority;
            buf.policy = thp->policy;
            if (scheduler_type != SCHEDULER_TYPE_DEFAULT) {
                buf.partition_id = (uint32_t) thp->dpp->id;
                buf.sched_flags = thp->sched_flags;
                bufsize = sizeof(buf);
            } else {
                bufsize = offsetof(struct buf, partition_id);
            }
            if (_TRACE_CKH_EXE_PT_EHB(
                    trace_masks.class_thread_ehd_p, trace_masks.thread_ehd_p[s],
                    header, buf.pid, buf.tid, (void *) &buf, bufsize)) {
                add_trace_buffer(header, (void *) &buf,
                                 _TRACE_ROUND_UP(bufsize));
            }
        } else {
            if (_TRACE_CKH_EXE_PT_EH(trace_masks.class_thread_ehd_p,
                                     trace_masks.thread_ehd_p[s], header,
                                     buf.pid, buf.tid, buf.pid, buf.tid)) {
                (void) add_trace_event(header, 0, buf.pid, buf.tid);
            }
        }
    }

    return;
}

/* Intercepts thread creation/destruction */
void th_em_cd(thread_t *thp, uint32_t s, uint32_t c)
{
    CRASHCHECK(TYPE_MASK(thp->type) != TYPE_THREAD);
    if (_TRACE_THPTID(thp, s)) {
        uint32_t pid = (uint32_t) thp->process->pid;
        uint32_t tid = (uint32_t) thp->tid + 1;
        uint32_t header =
            _TRACE_MAKE_CODE(get_cpunum(), _TRACE_STRUCT_S, _TRACE_PR_TH_C, c);

        if (_TRACE_CKH_EXE_PT_EH(trace_masks.class_thread_ehd_p,
                                 trace_masks.thread_ehd_p[s], header, pid, tid,
                                 pid, tid)) {
            (void) add_trace_event(header, 0, pid, tid);
        }
    }

    return;
}

/* Intercepts process creation */
void pr_em_create(process_t *prp, pid_t pid)
{
    uint32_t ppid = prp->parent ? (uint32_t) prp->parent->pid : 0;
    uint32_t header = _TRACE_MAKE_CODE(get_cpunum(), _TRACE_STRUCT_S,
                                       _TRACE_PR_TH_C, _TRACE_PR_TH_CREATE_P);

    if (_TRACE_CKH_EXE_EH(trace_masks.class_process_ehd_p,
                          trace_masks.pr_create_ehd_p, header, ppid, pid)) {
        (void) add_trace_event(header, 0, ppid, (uint32_t) pid);
    }

    return;
}

/* Intercepts process destruction */
void pr_em_destroy(process_t *prp, pid_t pid)
{
    uint32_t ppid = prp->parent ? (uint32_t) prp->parent->pid : 0;
    uint32_t header = _TRACE_MAKE_CODE(get_cpunum(), _TRACE_STRUCT_S,
                                       _TRACE_PR_TH_C, _TRACE_PR_TH_DESTROY_P);

    if (_TRACE_CKH_EXE_EH(trace_masks.class_process_ehd_p,
                          trace_masks.pr_destroy_ehd_p, header, ppid, pid)) {
        (void) add_trace_event(header, 0, ppid, (uint32_t) pid);
    }

    return;
}

/* Intercepts "process name" creation */
void pr_em_create_name(process_t *prp)
{
    struct {
        uint32_t ppid;
        uint32_t pid;
        char name[_TRACE_MAX_STR_SIZE];
    } buf;

    /* "debug_name" lives in root_process_t (roottask side) and is not fetchable
     * from prp here, so emit "?" as a placeholder (as event.c, shutdown.c and
     * process.c do). Trace consumers that want the real name read it from
     * /proc/<pid>/name (roottask).
     */
    const char *str_p = "?";
    uint32_t header = _TRACE_MAKE_CODE(get_cpunum(), 0, _TRACE_PR_TH_C,
                                       _TRACE_PR_TH_CREATE_P_NAME);

    buf.ppid = prp->parent ? (uint32_t) prp->parent->pid : 0;
    buf.pid = (uint32_t) prp->pid;
    STRLCPY(buf.name, str_p, sizeof(buf.name));
    if (_TRACE_CKH_EXE_EHB(
            trace_masks.class_process_ehd_p, trace_masks.pr_create_name_ehd_p,
            header, &buf,
            sizeof(buf.ppid) + sizeof(buf.pid) + strlen(buf.name) + 1)) {
        add_trace_d2_string(header,
                            prp->parent ? (uint32_t) prp->parent->pid : 0,
                            (uint32_t) prp->pid, "%s", str_p);
    }

    return;
}

/* Assumes that if you call this with a NULL thread name, you still want an
 * event (ie for name reset)
 */
void th_em_name(thread_t *thp)
{
    struct {
        uint32_t pid;
        uint32_t tid;
        char name[_TRACE_MAX_STR_SIZE];
    } buf;
    uint32_t header;

    header =
        _TRACE_MAKE_CODE(get_cpunum(), 0, _TRACE_PR_TH_C, _TRACE_PR_TH_NAME_T);
    buf.pid = (uint32_t) thp->process->pid;
    buf.tid = (uint32_t) thp->tid + 1;
    if (thp->name) {
        STRLCPY(buf.name, thp->name, sizeof(buf.name));
    } else {
        buf.name[0] = '\0';
    }

    if (_TRACE_CKH_EXE_EHB(
            trace_masks.class_process_ehd_p, trace_masks.th_name_ehd_p, header,
            &buf, sizeof(buf.pid) + sizeof(buf.tid) + strlen(buf.name) + 1)) {
        add_trace_d2_string(header, buf.pid, buf.tid, "%s", buf.name);
    }
}

/* Intercepts "process name" destruction */
void pr_em_destroy_name(process_t *prp)
{
    struct {
        uint32_t ppid;
        uint32_t pid;
        char name[_TRACE_MAX_STR_SIZE];
    } buf;

    /* See pr_em_create_name above — debug_name lives in root_process_t now;
     * trace event keeps a "?" placeholder.
     */
    const char *str_p = "?";
    uint32_t header = _TRACE_MAKE_CODE(get_cpunum(), 0, _TRACE_PR_TH_C,
                                       _TRACE_PR_TH_DESTROY_P_NAME);

    buf.ppid = prp->parent ? (uint32_t) prp->parent->pid : 0;
    buf.pid = (uint32_t) prp->pid;
    STRLCPY(buf.name, str_p, sizeof(buf.name));
    if (_TRACE_CKH_EXE_EHB(
            trace_masks.class_process_ehd_p, trace_masks.pr_destroy_name_ehd_p,
            header, &buf,
            sizeof(buf.ppid) + sizeof(buf.pid) + strlen(buf.name) + 1)) {
        add_trace_d2_string(header,
                            prp->parent ? (uint32_t) prp->parent->pid : 0,
                            (uint32_t) prp->pid, "%s", str_p);
    }

    return;
}

/* Intercepts vthread creation/destruction */
void vth_em_cd(thread_t *vthp, uint32_t s, uint32_t c)
{
    CRASHCHECK(TYPE_MASK(vthp->type) != TYPE_VTHREAD);
    if (_TRACE_VTHPTID(vthp, s)) {
        uint32_t pid = (uint32_t) vthp->process->pid;
        uint32_t tid = (uint32_t) vthp->tid + 1;
        uint32_t header =
            _TRACE_MAKE_CODE(get_cpunum(), _TRACE_STRUCT_S, _TRACE_PR_TH_C, c);

        if (_TRACE_CKH_EXE_PT_EH(trace_masks.class_vthread_ehd_p,
                                 trace_masks.vthread_ehd_p[s], header, pid, tid,
                                 pid, tid)) {
            (void) add_trace_event(header, 0, pid, tid);
        }
    }

    return;
}

/* Intercepts vthread states */
void vth_em_st(thread_t *vthp, uint32_t s)
{
    CRASHCHECK(TYPE_MASK(vthp->type) != TYPE_VTHREAD);
    if (_TRACE_VTHPTID(vthp, s)) {
        uint32_t pid = (uint32_t) vthp->process->pid;
        uint32_t tid = (uint32_t) vthp->tid + 1;
        uint32_t header =
            _TRACE_MAKE_CODE(get_cpunum(), _TRACE_STRUCT_S, _TRACE_PR_TH_C,
                             _TRACE_MAX_TH_STATE_NUM + s);

        if (_TRACE_CKH_EXE_PT_EH(trace_masks.class_vthread_ehd_p,
                                 trace_masks.vthread_ehd_p[s], header, pid, tid,
                                 pid, tid)) {
            (void) add_trace_event(header, 0, pid, tid);
        }
    }

    return;
}

/* Intercepts communication info of "exe events" */
void comm_exe_em(connect_t *cop, uint32_t srcvid, uint32_t ev)
{
    uint32_t pid;
    uint32_t header =
        _TRACE_MAKE_CODE(get_cpunum(), _TRACE_STRUCT_S, _TRACE_COMM_C, ev);

    switch (ev) {
    case RTMUX_TRACE_COMM_SNOTIFICATION_EXE:
    case RTMUX_TRACE_COMM_SNOTIFICATION_DIS:
    case RTMUX_TRACE_COMM_SNOTIFICATION_DEA:
    case RTMUX_TRACE_COMM_SNOTIFICATION_UN: {
        if (cop->channel == remote_ipc.chp) {
            pid = 0U;
        } else if ((cop->channel) && (cop->channel->process)) {
            pid = (uint32_t) cop->channel->process->pid;
        } else {
            pid = ~0U;
        }
        break;
    }
    case RTMUX_TRACE_COMM_SNOTIFICATION_QUN: {
        pid = 0U;
        break;
    }
    default:
        return;
    }

    if ((!trace_masks.comm_pid[ev] || trace_masks.comm_pid[ev] == pid) &&
        _TRACE_CKH_EXE_EH(trace_masks.class_comm_ehd_p,
                          trace_masks.comm_ehd_p[ev], header, srcvid, pid)) {
        (void) add_trace_event(header, 0, srcvid, pid);
    }

    return;
}

static void xfer_trace_fault_h(thread_t *act,
                               CPU_REGISTERS *regs,
                               unsigned flags)
{
    if (_TRACE_GETSYSCALL(act->syscall) == __KSYS_MSG_RECEIVENOTIFICATIONV) {
        (void) add_trace_event(
            _TRACE_MAKE_CODE(get_cpunum(), _TRACE_STRUCT_S, _TRACE_COMM_C,
                             RTMUX_TRACE_COMM_RNOTIFICATION),
            0, ~0U, ~0U);
    } else {
        (void) add_trace_event(
            _TRACE_MAKE_CODE(get_cpunum(), _TRACE_STRUCT_S, _TRACE_COMM_C,
                             RTMUX_TRACE_COMM_RMSG),
            0, ~0U, ~0U);
    }
    act->syscall &= ~_TRACE_COMM_IPC_EXIT;
    act->syscall |= _TRACE_COMM_CALL_EXIT;

    return;
}

const static struct fault_handlers xfer_trace_fault_handler = {
    xfer_trace_fault_h, NULL};

/* Intercepts communication info of "ipc events" */
void comm_em(thread_t *thp, connect_t *cop, uint32_t srcvid, uint32_t ev)
{
    if (_TRACE_IPCPTID(thp, ev)) {
        uint32_t pid;
        uint32_t header;

        switch (ev) {
        case RTMUX_TRACE_COMM_SMSG:
        case RTMUX_TRACE_COMM_SNOTIFICATION: {
            if (cop->channel == remote_ipc.chp) {
                pid = 0U;
            } else if ((cop->channel) && (cop->channel->process)) {
                pid = (uint32_t) cop->channel->process->pid;
            } else {
                pid = ~0U;
            }
            break;
        }
        case RTMUX_TRACE_COMM_REPLY:
        case RTMUX_TRACE_COMM_ERROR: {
            pid = (uint32_t) thp->process->pid;
            break;
        }
        case RTMUX_TRACE_COMM_RMSG: {
            if (!(thp->flags & RTMUX_FLG_THR_KCALL_ERR_SET)) {
                pid = (uint32_t) thp->process->pid;
                if ((srcvid = (uint32_t) KSTATUS(thp)) == 0U) {
                    if (!(trace_masks
                              .comm_mask[RTMUX_TRACE_COMM_RNOTIFICATION] &
                          _TRACE_ENTER_COMM))
                        return;
                    SET_XFER_HANDLER(&xfer_trace_fault_handler);
                    ev = RTMUX_TRACE_COMM_RNOTIFICATION;
                    if (((struct kcall_args_msg_receivev *) _TRACE_ARGPTR(thp))
                            ->rparts < 0) {
                        srcvid =
                            (uint32_t) ((struct _notification
                                             *) ((struct kcall_args_msg_receivev
                                                      *) _TRACE_ARGPTR(thp))
                                            ->rmsg)
                                ->scoid;
                    } else {
                        srcvid =
                            (uint32_t) ((struct _notification *) GETIOVBASE(
                                            ((struct kcall_args_msg_receivev *)
                                                 _TRACE_ARGPTR(thp))
                                                ->rmsg))
                                ->scoid;
                    }
                    SET_XFER_HANDLER(NULL);
                } else {
                    if (!(trace_masks.comm_mask[RTMUX_TRACE_COMM_RMSG] &
                          _TRACE_ENTER_COMM))
                        return;
                }
            } else {
                if (_TRACE_GETSYSCALL(thp->syscall) == __KSYS_MSG_RECEIVEV) {
                    if (!(trace_masks.comm_mask[RTMUX_TRACE_COMM_RMSG] &
                          _TRACE_ENTER_COMM))
                        return;
                    ev = RTMUX_TRACE_COMM_RMSG;
                } else {
                    if (!(trace_masks
                              .comm_mask[RTMUX_TRACE_COMM_RNOTIFICATION] &
                          _TRACE_ENTER_COMM))
                        return;
                    ev = RTMUX_TRACE_COMM_RNOTIFICATION;
                }
                pid = ~0U;
                srcvid = ~0U;
            }
            break;
        }
        case (unsigned) ~RTMUX_TRACE_COMM_RMSG:
        case (unsigned) ~RTMUX_TRACE_COMM_RNOTIFICATION:
            if (_TRACE_IPCPTID(thp, ~ev)) {
                if (!(thp->flags & RTMUX_FLG_THR_KCALL_ERR_SET) &&
                    _TRACE_GETARGSFLAG(thp)) {
                    pid = (uint32_t) thp->process->pid;
                    if ((srcvid = (uint32_t) KSTATUS(thp)) == 0U) {
                        if (!(trace_masks
                                  .comm_mask[RTMUX_TRACE_COMM_RNOTIFICATION] &
                              _TRACE_ENTER_COMM))
                            return;
                        specret_kernel();
                        SET_XFER_HANDLER(&xfer_trace_fault_handler);
                        ev = RTMUX_TRACE_COMM_RNOTIFICATION;
                        if (((struct kcall_args_msg_receivev *) _TRACE_ARGPTR(
                                 thp))
                                ->rparts < 0) {
                            srcvid =
                                (uint32_t) ((struct _notification
                                                 *) ((struct
                                                      kcall_args_msg_receivev *)
                                                         _TRACE_ARGPTR(thp))
                                                ->rmsg)
                                    ->scoid;
                        } else {
                            srcvid =
                                (uint32_t) ((struct _notification *) GETIOVBASE(
                                                ((struct kcall_args_msg_receivev
                                                      *) _TRACE_ARGPTR(thp))
                                                    ->rmsg))
                                    ->scoid;
                        }
                        SET_XFER_HANDLER(NULL);
                        unspecret_kernel();
                    } else {
                        if (!(trace_masks.comm_mask[RTMUX_TRACE_COMM_RMSG] &
                              _TRACE_ENTER_COMM))
                            return;
                        ev = ~ev;
                    }
                } else {
                    pid = ~0U;
                    srcvid = ~0U;
                }
                thp->syscall &= ~_TRACE_COMM_IPC_EXIT;
                thp->syscall |= _TRACE_COMM_CALL_EXIT;
                break;
            }
            /* fall through */
        default:
            return;
        }

        header =
            _TRACE_MAKE_CODE(get_cpunum(), _TRACE_STRUCT_S, _TRACE_COMM_C, ev);
        if (_TRACE_CKH_EXE_PT_EH(
                trace_masks.class_comm_ehd_p, trace_masks.comm_ehd_p[ev],
                header, thp->process->pid, thp->tid + 1, srcvid, pid)) {
            (void) add_trace_event(header, 0, srcvid, pid);
        }
    }

    return;
}

/* Intercepts "signals" */
void comm_em_signal(thread_t *thp, siginfo_t *s_i)
{
    uint32_t header = _TRACE_MAKE_CODE(get_cpunum(), _TRACE_STRUCT_S,
                                       _TRACE_COMM_C, RTMUX_TRACE_COMM_SIGNAL);

    if (_TRACE_IPCPTID(thp, RTMUX_TRACE_COMM_SIGNAL)) {
        if (_TRACE_COMM_ARG_WIDE &
            trace_masks.comm_mask[RTMUX_TRACE_COMM_SIGNAL]) {
            uint32_t arg_arr[10];

            arg_arr[0] = (uint32_t) s_i->si_signo;
            arg_arr[1] = (uint32_t) s_i->si_code;
            arg_arr[2] = (uint32_t) s_i->si_errno;
            arg_arr[3] = (uint32_t) s_i->__pad[0];
            arg_arr[4] = (uint32_t) s_i->__pad[1];
            arg_arr[5] = (uint32_t) s_i->__pad[2];
            arg_arr[6] = (uint32_t) s_i->__pad[3];
            arg_arr[7] = (uint32_t) s_i->__pad[4];
            arg_arr[8] = (uint32_t) s_i->__pad[5];
            arg_arr[9] = (uint32_t) s_i->__pad[6];

            if (_TRACE_CKH_EXE_PT_EHB(
                    trace_masks.class_comm_ehd_p,
                    trace_masks.comm_ehd_p[RTMUX_TRACE_COMM_SIGNAL], header,
                    thp->process->pid, thp->tid + 1, (void *) arg_arr,
                    sizeof(uint32_t) * NUM_ELTS(arg_arr))) {
                add_trace_buffer(header, arg_arr, NUM_ELTS(arg_arr));
            }
        } else {
            if (_TRACE_CKH_EXE_PT_EH(
                    trace_masks.class_comm_ehd_p,
                    trace_masks.comm_ehd_p[RTMUX_TRACE_COMM_SIGNAL], header,
                    thp->process->pid, thp->tid + 1, (uint32_t) s_i->si_signo,
                    (uint32_t) s_i->si_code)) {
                (void) add_trace_event(header, 0, (uint32_t) s_i->si_signo,
                                       (uint32_t) s_i->si_code);
            }
        }
    }

    return;
}

/* Intercepts interrupt entries */
void add_ktrace_int_handler_enter(intr_level_t *lp, interrupt_t *isr)
{
    uint32_t l = _TRACE_INT_LEV(lp);

    if (_TRACE_ENTER_INT & trace_masks.int_handler_masks[l]) {
        struct _buf {
            uint32_t pid;
            uint32_t i_n;
            uint32_t ip;
            uint32_t area;
        } buf;
        unsigned cpu = get_cpunum();
        uint32_t header = _TRACE_MAKE_CODE(cpu, _TRACE_STRUCT_S, _TRACE_INT_C,
                                           _TRACE_INT_HANDLER_ENTRY);

        buf.pid = (uint32_t) isr->thread->process->pid;
        buf.i_n = _TRACE_INT_NUM(lp);
        buf.ip = TRACE_PTR_LOW32(isr->handler);
        buf.area = TRACE_PTR_LOW32(isr->area);

        if (_TRACE_CKH_EXE_EHBNL(trace_masks.class_int_handler_enter_ehd_p,
                                 trace_masks.int_handler_enter_ehd_p[l], header,
                                 &buf, sizeof(buf))) {
            add_trace_buffer(header, (uint32_t *) &buf,
                             sizeof(buf) / sizeof(uint32_t));
        }
    }

    return;
}

/* Intercepts interrupt exits */
void add_ktrace_int_handler_exit(intr_level_t *lp, const struct sigevent *ev)
{
    uint32_t l = _TRACE_INT_LEV(lp);

    if (_TRACE_EXIT_INT & trace_masks.int_handler_masks[l]) {
        struct _buf {
            uint32_t i_n;
            struct sigevent ev;
        } buf;
        unsigned cpu = get_cpunum();
        uint32_t header = _TRACE_MAKE_CODE(cpu, _TRACE_STRUCT_S, _TRACE_INT_C,
                                           _TRACE_INT_HANDLER_EXIT);

        buf.i_n = _TRACE_INT_NUM(lp);
        if (ev)
            memcpy(&buf.ev, ev, sizeof(*ev));
        else
            memset(&buf.ev, 0, sizeof(buf.ev));
        if (_TRACE_CKH_EXE_EHBNL(trace_masks.class_int_handler_exit_ehd_p,
                                 trace_masks.int_handler_exit_ehd_p[l], header,
                                 &buf, sizeof(buf))) {
            add_trace_buffer(header, (uint32_t *) &buf,
                             sizeof(buf) / sizeof(uint32_t));
        }
    }

    return;
}

/* Intercepts interrupt entries */
void add_ktrace_int_enter(intr_level_t *lp)
{
    uint32_t l = _TRACE_INT_LEV(lp);

    if (_TRACE_ENTER_INT & trace_masks.int_masks[l]) {
        uint32_t i_n = _TRACE_INT_NUM(lp);
        unsigned cpu = get_cpunum();
        uint32_t ip = (uint32_t) REGIP(&get_active_thread(cpu)->reg);
        uint32_t header = _TRACE_MAKE_CODE(cpu, _TRACE_STRUCT_S, _TRACE_INT_C,
                                           _TRACE_INT_ENTRY);

        if (_TRACE_CKH_EXE_EHNL(trace_masks.class_int_enter_ehd_p,
                                trace_masks.int_enter_ehd_p[l], header, i_n,
                                ip)) {
            (void) add_trace_event(header, 0, i_n, ip);
        }
    }

    return;
}

/* Intercepts interrupt exits */
void add_ktrace_int_exit(intr_level_t *lp)
{
    uint32_t l = _TRACE_INT_LEV(lp);

    if (_TRACE_EXIT_INT & trace_masks.int_masks[l]) {
        uint32_t i_n = _TRACE_INT_NUM(lp);
        uint32_t ink = get_inkernel();
        unsigned cpu = get_cpunum();
        uint32_t header = _TRACE_MAKE_CODE(cpu, _TRACE_STRUCT_S, _TRACE_INT_C,
                                           _TRACE_INT_EXIT);

        if (_TRACE_CKH_EXE_EHNL(trace_masks.class_int_exit_ehd_p,
                                trace_masks.int_exit_ehd_p[l], header, i_n,
                                ink)) {
            (void) add_trace_event(header, 0, i_n, ink);
        }
    }

    return;
}

void trace_emit_irq_claim(intr_level_t *lp, interrupt_t *itp)
{
    uint32_t l = _TRACE_INT_LEV(lp);

    if (_TRACE_ENTER_INT & trace_masks.int_masks[l]) {
        uint32_t pid = 0;
        unsigned cpu = get_cpunum();
        uint32_t header = _TRACE_MAKE_CODE(cpu, _TRACE_STRUCT_S, _TRACE_INT_C,
                                           _TRACE_IRQ_CLAIM_EVENT);

        if (itp && itp->thread && itp->thread->process)
            pid = (uint32_t) itp->thread->process->pid;
        (void) add_trace_event(header, 0, _TRACE_INT_NUM(lp), pid);
    }
}

void trace_emit_ist_dispatch(interrupt_t *itp, thread_t *thp)
{
    uint32_t enabled = 0;
    uint32_t pid = 0;
    uint32_t tid = 0;
    unsigned cpu;
    uint32_t header;
    uint32_t i;

    if (itp) {
        intr_level_t *lp = &interrupt_level[itp->level];

        enabled = trace_masks.int_masks[_TRACE_INT_LEV(lp)];
    } else {
        for (i = 0; i < _TRACE_MAX_INT_NUM; i++) {
            enabled |= trace_masks.int_masks[i];
            if (enabled & _TRACE_EXIT_INT)
                break;
        }
    }
    if (!(_TRACE_EXIT_INT & enabled))
        return;

    if (thp) {
        if (thp->process)
            pid = (uint32_t) thp->process->pid;
        tid = (uint32_t) thp->tid + 1;
    }
    cpu = get_cpunum();
    header = _TRACE_MAKE_CODE(cpu, _TRACE_STRUCT_S, _TRACE_INT_C,
                              _TRACE_IST_DISPATCH_EVENT);
    (void) add_trace_event(header, 0, pid, tid);
}

void trace_emit_pi_boost(thread_t *thp, unsigned old_prio, unsigned new_prio)
{
    uint32_t header;
    uint32_t pid_tid = 0;
    uint32_t old_new;

    if (thp && thp->process) {
        pid_tid = (((uint32_t) thp->process->pid & 0xffffU) << 16) |
                  (((uint32_t) thp->tid + 1U) & 0xffffU);
    }
    old_new = ((uint32_t) (old_prio & 0xffffU) << 16) |
              ((uint32_t) (new_prio & 0xffffU));
    header = _TRACE_MAKE_CODE(get_cpunum(), _TRACE_STRUCT_S, _TRACE_SYSTEM_C,
                              RTMUX_TRACE_SYS_PI_BOOST);
    (void) add_trace_event(header, 0, pid_tid, old_new);
}

/* Emit a KER_CALL-class exit event carrying the kernel-side cost of one call.
 *
 * The event number is _TRACE_MAX_KER_CALL_NUM + the kcall number, which is the
 * encoding the trace wire format already reserved for a kcall exit. d_1 is pid
 * and tid+1 packed the way the pi_boost event packs them; d_2 is the elapsed
 * time-CSR tick count measured around the handler in the trap dispatcher, so
 * the ring's own lock and buffer write are outside the window rather than
 * inside it. A self-contained duration rather than a paired timestamp also
 * means the sample survives the consumer seeing events out of order across ring
 * buffers.
 *
 * pid and tid arrive as values rather than as a thread pointer: the self-reap
 * kcalls free the calling thread_t and still return normally, so the caller
 * snapshots identity before the handler runs and nothing here dereferences it.
 * They arrive full width because the SETCLASSPID / SETEVENTPID filters below
 * store a whole pid_t and a live pid routinely exceeds 16 bits; only the d_1
 * payload is narrowed, after filtering.
 *
 * One sample is one ecall execution, not one logical kernel call: KCALL_RESTART
 * rewinds the user PC so a preempted call returns to user space and re-enters
 * as a fresh trap, emitting one event per chunk. A handler that leaves through
 * kernel_exit() instead of returning (the xfer fault paths) emits nothing.
 *
 * The RTMUX_TRACE_SETCLASSPID / SETCLASSTID filters and the class and per-call
 * event handlers apply here, so the trace control API means the same thing for
 * a kcall event as for any other class. The handler sees exactly the two data
 * words the record carries, as at every other emit site in this file.
 * Parameters:
 *   pid   : calling process id, or 0 when identity was unavailable
 *   tid   : calling thread id + 1, or 0
 *   num   : kcall number, already validated below __KSYS_BAD by the caller
 *   ticks : elapsed time-CSR ticks across the handler
 */
void trace_emit_kcall(uint32_t pid, uint32_t tid, unsigned num, uint64_t ticks)
{
    uint32_t header;
    uint32_t pid_tid;

    if (trace_masks.ksys_call_pid[num] &&
        (trace_masks.ksys_call_pid[num] != pid ||
         (trace_masks.ksys_call_tid[num] &&
          trace_masks.ksys_call_tid[num] != tid))) {
        return;
    }

    /* d_1 holds both, so each is truncated to 16 bits, the same packing the
     * pi_boost event uses. Lossy for a pid above 65535: the generation bits
     * pid_unique adds are dropped, so two pids in the same slot alias. The
     * consumer uses d_1 for attribution only, never for pairing, and the filter
     * above already ran on the full values.
     */
    pid_tid = ((pid & 0xffffU) << 16) | (tid & 0xffffU);
    header = _TRACE_MAKE_CODE(get_cpunum(), _TRACE_STRUCT_S, _TRACE_KER_CALL_C,
                              _TRACE_MAX_KER_CALL_NUM + num);
    if (_TRACE_CKH_EXE_EH(trace_masks.class_ker_call_exit_ehd_p,
                          trace_masks.ksys_call_exit_ehd_p[num], header,
                          pid_tid, (uint32_t) ticks)) {
        (void) add_trace_event(header, 0, pid_tid, (uint32_t) ticks);
    }
}


/* Yuck - we should really have a per-cpu buffer system, but hey... */
int add_trace_event(uint32_t header, uint32_t d_0, uint32_t d_1, uint32_t d_2)
{
    tracebuf_t *t_b_p;
    traceevent_t *e_p;
    uint32_t skip;
    int retry = 2, want_emit = 0, want_next = 0;

    /* Tracing off: leave before touching the global state lock. Every hart
     * serializes on trace_masks.state_spin for every event, so an unlocked
     * pre-check keeps a kernel that merely has CONFIG_INSTRUMENTATION compiled
     * in off that lock entirely. Racy by design and only ever an early exit:
     * the locked test below stays authoritative, so a concurrent START at worst
     * drops the event that raced it, which the ring already permits (see
     * dropped_events).
     */
    if (__atomic_load_n(&trace_masks.main_flags, __ATOMIC_RELAXED) == 0)
        return EOK;

    /* This needs to be out here, before we lock anything down, since it may
     * actually call add_trace_event itself!
     */
    if (!_TRACE_GET_STRUCT(header)) {
        d_0 = get_time_off(0);
    }
    /* interrupt_disable(); */
begin:
    interrupt_disable();

    /* Until we've decided which buffer we're going to use, we can't let anyone
     * mess with the global buffer pointer
     */
    SPINLOCK(&trace_masks.state_spin);
    skip = trace_masks.skip_buff;

    if (trace_masks.main_flags == 0) {
        /* We're not actually tracing, bail out */
        SPINUNLOCK(&trace_masks.state_spin);
        interrupt_enable();
        return EOK;
    }
    t_b_p = privateptr->tracebuf;
    if (!t_b_p) {
        /* No Trace Buffer allocated */
        SPINUNLOCK(&trace_masks.state_spin);
        interrupt_enable();
        return ECANCELED;
    }
    SPINLOCK(&t_b_p->h.spin);

    /* If the current trace buffer is already scheduled for flushing... */
    if (t_b_p->h.flags & _TRACE_FLAGS_FLUSH) {
        /* if we have retries available or we have specified via skip_buff, then
         * ...
         */
        if (retry && ((t_b_p->h.flags & _TRACE_FLAGS_WRITING) || skip)) {
            skip = 0;
            /* skip to next buffer */
            privateptr->tracebuf = t_b_p->h.next;
            SPINUNLOCK(&t_b_p->h.spin);
            SPINUNLOCK(&trace_masks.state_spin);
            interrupt_enable();
            retry--;
            goto begin;
        } else {
            if (t_b_p->h.flags & _TRACE_FLAGS_WRITING) {
                SPINUNLOCK(&trace_masks.state_spin);
                SPINUNLOCK(&t_b_p->h.spin);
                dropped_events++;
                interrupt_enable();
                return EOK;
            }
            /* Drop that buffer! */
            _TRACE_CLR_BUFF(t_b_p);
            SPINUNLOCK(&trace_masks.state_spin);
        }
    } else {
        /* We've got our man! */
        SPINUNLOCK(&trace_masks.state_spin);
    }

    /* If this is the first event of the buffer, intialize with buffer event */
    if (t_b_p->h.num_events == 0) {
        t_b_p->h.num_events++;
        e_p = (traceevent_t *) t_b_p->h.tail_ptr;
        t_b_p->h.seq_buff_num = ++(trace_masks.send_buff_num);
        t_b_p->h.tail_ptr++;
        e_p->header = _TRACE_MAKE_CODE(get_cpunum(), _TRACE_STRUCT_S,
                                       _TRACE_CONTROL_C, _TRACE_CONTROL_BUFFER);
        e_p->data[0] = d_0;
        e_p->data[1] = t_b_p->h.seq_buff_num;
        e_p->data[2] = 0;
    }
    if (t_b_p->h.num_events < _TRACELEMENTS) {
        /* ok, we have room in the buffer to add the event */
        t_b_p->h.num_events++;
        e_p = (traceevent_t *) t_b_p->h.tail_ptr;
        t_b_p->h.tail_ptr++;
        e_p->header = header;
        e_p->data[0] = d_0;
        e_p->data[1] = d_1;
        e_p->data[2] = d_2;

        if (t_b_p->h.num_events >= trace_masks.max_events) {
            /* If we're getting low on space... */
            if (trace_masks.ring_mode) {
                want_next = 1;
                /* Nada? */
            } else {
                if ((get_inkernel() & INKERNEL_INTRMASK) == 0) {
                    want_next = 1;
                    /* Schedule a flush */
                    want_emit = 1;
                } else {
                    trace_force_flush = 1;
                }
            }
        }
    } else {
        dropped_events++;
    }
    SPINUNLOCK(&t_b_p->h.spin);
    interrupt_enable();

    /* At this point we're done logging the event, but we still need to check to
     * see if we need to emit this buffer, or simply move to the next buffer in
     * ring mode.
     */
    if (want_next) {
        interrupt_disable();
        SPINLOCK(&trace_masks.state_spin);
        if (t_b_p == privateptr->tracebuf) {
            tracebuf_t *b_p = t_b_p->h.next;
            SPINLOCK(&t_b_p->h.spin);
            if (trace_masks.ring_mode) {
                SPINLOCK(&b_p->h.spin);
                b_p->h.tail_ptr = b_p->h.begin_ptr;
                b_p->h.num_events = 0;
                SPINUNLOCK(&b_p->h.spin);
            } else if (want_emit) {
                t_b_p->h.flags |= _TRACE_FLAGS_FLUSH;
            }
            SPINUNLOCK(&t_b_p->h.spin);
            privateptr->tracebuf = b_p;
            SPINUNLOCK(&trace_masks.state_spin);
            memset(&force_emit, 0xff, sizeof(force_emit));
            if (want_emit) {
                (void) em_event(t_b_p);
            }
        } else {
            /* Someone else moved it? WTF? Assume that they flushed it too! */
            SPINUNLOCK(&trace_masks.state_spin);
        }
        interrupt_enable();
    }
    return EOK;
}

/* Adds buffer in the form of events. */
void add_trace_buffer(uint32_t header, uint32_t *b_p, unsigned len)
{
    uint32_t t_s = get_time_off(0);
    int i;

    if (len > (_TRACEDATANUM * 2)) {
        _TRACE_SET_STRUCT(header, _TRACE_STRUCT_CB);
        (void) add_trace_event(header, t_s, b_p[0], b_p[1]);
        _TRACE_SET_STRUCT(header, _TRACE_STRUCT_CC);
        for (i = _TRACEDATANUM, len -= _TRACEDATANUM; i < len;
             i += _TRACEDATANUM) {
            (void) add_trace_event(header, t_s, b_p[i], b_p[i + 1]);
        }
        _TRACE_SET_STRUCT(header, _TRACE_STRUCT_CE);
        if ((len + _TRACEDATANUM) > (i + 1)) {
            (void) add_trace_event(header, t_s, b_p[i], b_p[i + 1]);
        } else {
            (void) add_trace_event(header, t_s, b_p[i], 0);
        }
    } else if (len > _TRACEDATANUM) {
        _TRACE_SET_STRUCT(header, _TRACE_STRUCT_CB);
        (void) add_trace_event(header, t_s, b_p[0], b_p[1]);
        _TRACE_SET_STRUCT(header, _TRACE_STRUCT_CE);
        if (len == (_TRACEDATANUM + 1)) {
            (void) add_trace_event(header, t_s, b_p[2], 0);
        } else {
            (void) add_trace_event(header, t_s, b_p[2], b_p[3]);
        }
    } else if (len > 0) {
        _TRACE_SET_STRUCT(header, _TRACE_STRUCT_S);
        if (len == _TRACEDATANUM) {
            (void) add_trace_event(header, t_s, b_p[0], b_p[1]);
        } else {
            (void) add_trace_event(header, t_s, b_p[0], 0);
        }
    } else {
        _TRACE_SET_STRUCT(header, _TRACE_STRUCT_S);
        (void) add_trace_event(header, t_s, 0, 0);
    }

    return;
}

/* Initilizes the "trace state" structure ("stream") */
void add_trace_b_init(trace_state_t *st_p)
{
    memset(st_p, 0, sizeof(trace_state_t));
    st_p->time_off = get_time_off(0);
}

/* Ends the writting process ("stream") */
void add_trace_b_end(trace_state_t *st_p, uint32_t header)
{
    /* If no event has been emitted, and not data added, do nothing */
    if (st_p->data_emitted == 0 && st_p->di == 0) {
        return;
    }

    /* If no data has been emitted, but we have data, simple event If we have
     * emitted data then this is a terminating event
     */
    _TRACE_SET_STRUCT(
        header, (st_p->data_emitted) ? _TRACE_STRUCT_CE : _TRACE_STRUCT_S);
    (void) add_trace_event(header, st_p->time_off, st_p->data.idata[0],
                           st_p->data.idata[1]);
}

/* Adds buffers as an event "stream" */
void add_trace_b_add(trace_state_t *st_p,
                     uint32_t header,
                     char *b_p,
                     unsigned len)
{
    unsigned bufamt, bi;
    unsigned dataamt, amt;

    bi = 0;

    while (bi < len) {
        /* We want to put more data out, and we have a full buffer. This means
         * we are either the begin or continuation, but not simple
         */
        if (st_p->di == sizeof(st_p->data)) {
            _TRACE_SET_STRUCT(header, (st_p->data_emitted) ? _TRACE_STRUCT_CC
                                                           : _TRACE_STRUCT_CB);
            (void) add_trace_event(header, st_p->time_off, st_p->data.idata[0],
                                   st_p->data.idata[1]);

            /* Set-up for the next event that is going out */
            st_p->data_emitted = 1;
            st_p->di = 0;
            memset(st_p->data.cdata, 0, sizeof(st_p->data));
        }
        /* Determine how much data we can put in, and copy to a local buffer */
        bufamt = len - bi;
        dataamt = sizeof(st_p->data) - st_p->di;
        amt = (bufamt <= dataamt) ? bufamt : dataamt;

        memcpy(st_p->data.cdata + st_p->di, b_p + bi, amt);
        bi += amt;
        st_p->di += amt;
    }
}

void add_trace_iovs(uint32_t header, iov_t *iovs, unsigned iovlen)
{
    trace_state_t state;
    int i;

    add_trace_b_init(&state);

    for (i = 0; i < iovlen; i++) {
        add_trace_b_add(&state, header, GETIOVBASE(iovs + i),
                        (unsigned) GETIOVLEN(iovs + i));
    }

    add_trace_b_end(&state, header);
}

/* Double (in/out) user interface
 *
 * Note: all parameters must have been validated by this point. This routine is
 * called locked from ksys_trace_event and also called directly from interrupt
 * service routines, so any segfault as a result of accessing a bad parameter
 * will result in a kernel crash!
 */
int trace_event(uint32_t *a_p)
{
    uint32_t code;
    switch (_TRACE_GET_FLAG(*a_p)) {
    case _TRACE_GET_FLAG(RTMUX_TRACE_INSERTSUSEREVENT): {
        /* Note - simply masking off the user event code here may seem dubious,
         * but I can't change it since I've actually seen users relying on this
         * effect. *SIGH*
         */
        code = *(a_p + 1) & RTMUX_TRACE_USERLAST;
        if (trace_masks.user_mask[code]) {
            (void) add_trace_event(
                _TRACE_MAKE_CODE(get_cpunum(), _TRACE_STRUCT_S, _TRACE_USER_C,
                                 code),
                0, *(a_p + 2), *(a_p + 3));
        }

        return EOK;
    }
    case _TRACE_GET_FLAG(RTMUX_TRACE_INSERTCUSEREVENT): {
        code = *(a_p + 1) & RTMUX_TRACE_USERLAST;
        if (trace_masks.user_mask[code]) {
            add_trace_buffer(
                _TRACE_MAKE_CODE(get_cpunum(), 0, _TRACE_USER_C, code),
                TRACE_UNPACK_PTR(uint32_t *, *(a_p + 2), *(a_p + 3)),
                (uint32_t) *(a_p + 4));
        }

        return EOK;
    }
    /* The class is _TRACE_USER_C, not the caller's word shifted left by 10.
     * The event code was masked but the class was not, so any process could
     * shift an arbitrary value into the class, struct, and CPU fields of the
     * header and inject a record that reads back as a kernel-call sample:
     * a fabricated duration in scripts/rt-trace-sched.py output and hence in
     * the per-kernel-call rows of docs/rt-budgets.md. These two opcodes only
     * ever emit user records, so the class is a constant, not caller data.
     */
    case _TRACE_GET_FLAG(RTMUX_TRACE_INSERTSCLASSEVENT): {
        code = *(a_p + 2) & RTMUX_TRACE_USERLAST;
        if (trace_masks.user_mask[code]) {
            (void) add_trace_event(_TRACE_MAKE_CODE(get_cpunum(),
                                                    _TRACE_STRUCT_S,
                                                    _TRACE_USER_C, code),
                                   0, *(a_p + 3), *(a_p + 4));
        }

        return EOK;
    }
    case _TRACE_GET_FLAG(RTMUX_TRACE_INSERTCCLASSEVENT): {
        code = *(a_p + 2) & RTMUX_TRACE_USERLAST;
        if (trace_masks.user_mask[code]) {
            add_trace_buffer(
                _TRACE_MAKE_CODE(get_cpunum(), 0, _TRACE_USER_C, code),
                TRACE_UNPACK_PTR(uint32_t *, *(a_p + 3), *(a_p + 4)),
                (uint32_t) *(a_p + 5));
        }

        return EOK;
    }
    case _TRACE_GET_FLAG(RTMUX_TRACE_INSERTUSRSTREVENT): {
        code = *(a_p + 1) & RTMUX_TRACE_USERLAST;
        if (trace_masks.user_mask[code]) {
            trace_state_t t_s;
            uint32_t h = _TRACE_MAKE_CODE(get_cpunum(), 0, _TRACE_USER_C, code);
            char *str = TRACE_UNPACK_PTR(char *, *(a_p + 2), *(a_p + 3));
            uint32_t l = (uint32_t) strlen(str) + 1;

            add_trace_b_init(&t_s);
            add_trace_b_add(&t_s, h, str, l);
            add_trace_b_end(&t_s, h);
        }

        return EOK;
    }
    case _TRACE_GET_FLAG(RTMUX_TRACE_INSERTEVENT): {
        (void) add_trace_event(*(a_p + 1), *(a_p + 2), *(a_p + 3), *(a_p + 4));

        return EOK;
    }
    case _TRACE_GET_FLAG(RTMUX_TRACE_STOP): {
        if (trace_masks.main_flags && trace_masks.ring_mode) {
            tracebuf_t *t_b_p = privateptr->tracebuf;

            if (trace_masks.main_flags && t_b_p) {
                (void) em_event(t_b_p);
            } else {
                return ECANCELED;
            }
        }
        trace_masks.main_flags = 0;

        return EOK;
    }
    case _TRACE_GET_FLAG(RTMUX_TRACE_STARTNOSTATE): {
        if (!privateptr->tracebuf)
            return ECANCELED;
        if (trace_masks.main_flags)
            return EOK;
        trace_masks.main_flags = 1;

        return EOK;
    }
    case _TRACE_GET_FLAG(RTMUX_TRACE_START): {
        thread_t *thp;
        process_t *prp;
        unsigned i_1;
        unsigned i_2;

        if (!privateptr->tracebuf)
            return ECANCELED;
        if (trace_masks.main_flags)
            return EOK;
        trace_masks.main_flags = 1;
        (void) get_time_off(1);
        sched_trace_initial_parms();
        for (i_1 = 1; i_1 < process_vector.nentries;
             ++i_1) { /* Dump sys. state including */
            if (VECP(prp, &process_vector,
                     i_1)) { /* process names and task IDs */
                /* debug_name lives in root_process_t and the "_NAME" emitter
                 * always sends "?", so always take the with-name path for a
                 * consistent event sequence — trace consumers correlate the pid
                 * with /proc/<pid>/name (roottask) for the real name.
                 */
                _TRACE_PR_EMIT_CREATE_NAME(prp);
                for (i_2 = 0; i_2 < prp->threads.nentries; ++i_2) {
                    if (VECP(thp, &prp->threads, i_2)) {
                        _TRACE_TH_EMIT_CREATE(thp);
                        _TRACE_TH_EMIT_STATE_ONCPU(thp, thp->state,
                                                   thp->state == STATE_RUNNING
                                                       ? thp->runcpu
                                                       : get_cpunum());
                        if (thp->name) {
                            _TRACE_TH_EMIT_NAME(thp);
                        }
                    }
                }
            }
        }

        return EOK;
    }
    default: {
        return ENOTSUP;
    }
    }
}

/* Inserts a string event into the trace buffer */
void add_trace_string(uint32_t header, const char *fmt, ...)
{
    va_list ap;
    uint32_t buff[_TRACE_ROUND_UP(_TRACE_MAX_STR_SIZE)];
    uint32_t l;

    va_start(ap, fmt);
    l = (uint32_t) kvsnprintf((char *) buff, sizeof(buff) - 1, fmt, ap);
    add_trace_buffer(header, buff, _TRACE_ROUND_UP(1 + l));
    va_end(ap);

    return;
}

/* Inserts "one data argument" and string event into the trace buffer */
void add_trace_d1_string(uint32_t header, uint32_t d_1, const char *fmt, ...)
{
    va_list ap;
    uint32_t buff[_TRACE_ROUND_UP(_TRACE_MAX_STR_SIZE) + 1];
    uint32_t l;

    buff[0] = d_1;
    va_start(ap, fmt);
    l = (uint32_t) kvsnprintf((char *) (buff + 1),
                              sizeof(buff) - (sizeof(buff[0]) + 1), fmt, ap);
    add_trace_buffer(header, buff, 1 + _TRACE_ROUND_UP(1 + l));
    va_end(ap);

    return;
}

/* Inserts "two data arguments" and string event into the trace buffer */
void add_trace_d2_string(uint32_t header,
                         uint32_t d_1,
                         uint32_t d_2,
                         const char *fmt,
                         ...)
{
    va_list ap;
    uint32_t buff[_TRACE_ROUND_UP(_TRACE_MAX_STR_SIZE) + 2];
    uint32_t l;

    buff[0] = d_1;
    buff[1] = d_2;
    va_start(ap, fmt);
    l = (uint32_t) kvsnprintf((char *) (buff + 2),
                              sizeof(buff) - ((2 * sizeof(buff[0])) + 1), fmt,
                              ap);
    add_trace_buffer(header, buff, 2 + _TRACE_ROUND_UP(1 + l));
    va_end(ap);

    return;
}

void trace_emit_address(thread_t *thp, uint32_t vaddr)
{
    uint32_t header;

    header = _TRACE_MAKE_CODE(get_cpunum(), _TRACE_STRUCT_S, _TRACE_SYSTEM_C,
                              RTMUX_TRACE_SYS_ADDRESS);
    if (_TRACE_CKH_EXE_EH(trace_masks.class_system_ehd_p,
                          trace_masks.system_ehd_p[RTMUX_TRACE_SYS_ADDRESS],
                          header, vaddr, 0)) {
        (void) add_trace_event(header, 0, vaddr, 0);
    }
}

#else

void trace_emit_pi_boost(thread_t *thp, unsigned old_prio, unsigned new_prio)
{
    (void) thp;
    (void) old_prio;
    (void) new_prio;
}

int trace_event(uint32_t *data)
{
    interrupt_t *itp;
    struct sigevent *evp;
    struct sigevent *(*func)(void *);

    if ((itp = interrupt_level[HOOK_TO_LEVEL(RTMUX_HOOK_TRACE)].queue)) {
        thread_t *thp;

        thp = itp->thread;
        if (thp->as_prp && thp->as_prp != pcpu_array[get_cpunum()].pc_as_prp) {
            vm_ops->as(thp->as_prp, &pcpu_array[get_cpunum()].pc_as_prp);
        }

        func = (struct sigevent * (*) (void *) ) itp->handler;
        if ((evp = func(data))) {
            /* Inline delivery (kernel extension context). */
            sigevent_deliver_inline(evp, thp, 0);
        }

        return EOK;
    }

    return ENOTSUP;
}

#endif

void trace_emit_th_state(thread_t *thp, int state)
{
    _TRACE_TH_EMIT_ANY_STATE(thp, (unsigned) state);
    return;
}

/* Fault-safe read of a caller-supplied trace buffer.
 *
 * ksys_trace_event arms no xfer fault handler, so probing the caller's pointer
 * with a bare load turns a null or unmapped argument into a kernel page fault.
 * trace_event is reachable by any process, so that was an unprivileged kernel
 * crash in both the instrumented and the stub build. Range-check first, then
 * copy through xfer_memcpy, which is documented to return an error rather than
 * fault. Callers work on the kernel copy, which also removes the
 * time-of-check-to-time-of-use window on the caller's data. Must be called with
 * no lock held: the recovery path does not release a spinlock. Parameters:
 *   act   : calling thread
 *   user  : caller's buffer
 *   out   : kernel destination
 *   len   : bytes to read
 * Return value: EOK, or EFAULT if the buffer is not fully readable.
 */
int trace_read_user(thread_t *act, const void *user, void *out, size_t len)
{
    if (!user || !act || !act->process)
        return EFAULT;
    if ((uintptr_t) user + len < (uintptr_t) user ||
        !WITHIN_BOUNDARY((uintptr_t) user, (uintptr_t) user + len,
                         act->process->boundary_addr))
        return EFAULT;
    if (len && xfer_memcpy(out, user, len))
        return EFAULT;

    return EOK;
}

/* Read a command array of `words` 32-bit words. */
int trace_read_cmd(thread_t *act,
                   const uint32_t *user,
                   uint32_t *out,
                   unsigned words)
{
    return trace_read_user(act, user, out, (size_t) words * sizeof(uint32_t));
}

/* Copy a NUL-terminated caller string into `out` (cap bytes, terminator
 * included) without ever reading past the terminator, so a short string that
 * ends at the edge of its mapping does not fault. Copies up to a page boundary
 * at a time and stops at the first NUL. Same no-lock rule as trace_read_user.
 * Return value: EOK, EFAULT if the string runs into unreadable memory, EINVAL
 * if no terminator lies within `cap` bytes.
 */
int trace_read_str(thread_t *act, const char *user, char *out, size_t cap)
{
    size_t done = 0;

    while (done < cap) {
        uintptr_t addr = (uintptr_t) user + done;
        size_t n = PGSIZE - (addr & (PGSIZE - 1));
        int rc;

        if (n > cap - done)
            n = cap - done;
        rc = trace_read_user(act, (const char *) addr, out + done, n);
        if (rc != EOK)
            return rc;
        for (size_t i = 0; i < n; i++) {
            if (!out[done + i])
                return EOK;
        }
        done += n;
    }

    return EINVAL;
}
