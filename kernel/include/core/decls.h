/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* Declarations that are private to the kernel itself. */

int kdecl ksys_bad(thread_t *act, struct kcall_args_null *kap);
int kdecl ksys_nop(thread_t *act, struct kcall_args_null *kap);
int kdecl ksys_sys_cpupage_get(thread_t *act,
                               struct kcall_args_sys_cpupage_get *kap);
int kdecl ksys_sys_cpupage_set(thread_t *act,
                               struct kcall_args_sys_cpupage_set *kap);
int kdecl ksys_dbgprint(thread_t *act, struct kcall_args_dbgprint *kap);

int kdecl ksys_channel_create(thread_t *act,
                              struct kcall_args_channel_create *kap);
int kdecl ksys_channel_destroy(thread_t *act,
                               struct kcall_args_channel_destroy *kap);

int kdecl ksys_connect_attach(thread_t *act,
                              struct kcall_args_connect_attach *kap);
int kdecl ksys_connect_detach(thread_t *act,
                              struct kcall_args_connect_detach *kap);
int kdecl ksys_connect_client_info(thread_t *act,
                                   struct kcall_args_connect_client_info *kap);
int kdecl ksys_connect_server_info(thread_t *act,
                                   struct kcall_args_connect_server_info *kap);
int kdecl ksys_connect_flags(thread_t *act,
                             struct kcall_args_connect_flags *kap);

int kdecl ksys_msg_sendv(thread_t *act, struct kcall_args_msg_sendv *kap);
int kdecl ksys_msg_receivev(thread_t *act, struct kcall_args_msg_receivev *kap);
int kdecl ksys_msg_replyv(thread_t *act, struct kcall_args_msg_replyv *kap);
int kdecl
ksys_msg_sendnotification(thread_t *act,
                          struct kcall_args_msg_sendnotification *kap);
int kdecl ksys_msg_error(thread_t *act, struct kcall_args_msg_error *kap);
int kdecl ksys_msg_readv(thread_t *act, struct kcall_args_msg_readv *kap);
/* ksys_msg_readwritev removed — slot bound to ksys_bad. */
int kdecl ksys_msg_readiov(thread_t *act, struct kcall_args_msg_readiov *kap);
int kdecl ksys_msg_writev(thread_t *act, struct kcall_args_msg_writev *kap);
int kdecl ksys_msg_info(thread_t *act, struct kcall_args_msg_info *kap);
int kdecl ksys_msg_deliver_event(thread_t *act,
                                 struct kcall_args_msg_deliver_event *kap);
int kdecl ksys_msg_verify_event(thread_t *act,
                                struct kcall_args_msg_verify_event *kap);
int kdecl ksys_msg_keydata(thread_t *act, struct kcall_args_msg_keydata *kap);
int kdecl ksys_msg_current(thread_t *act, struct kcall_args_msg_current *kap);

int kdecl ksys_thread_create(thread_t *act,
                             struct kcall_args_thread_create *kap);
int kdecl ksys_thread_destroy(thread_t *act,
                              struct kcall_args_thread_destroy *kap);
int kdecl ksys_thread_destroyall(thread_t *act, struct kcall_args_null *kap);
int kdecl ksys_thread_detach(thread_t *act,
                             struct kcall_args_thread_detach *kap);
int kdecl ksys_thread_join(thread_t *act, struct kcall_args_thread_join *kap);
int kdecl ksys_thread_cancel(thread_t *act,
                             struct kcall_args_thread_cancel *kap);
int kdecl ksys_thread_ctl(thread_t *act, struct kcall_args_thread_ctl *kap);
int kcall_op_thread_ctl(thread_t *act,
                        thread_t *op,
                        struct kcall_args_thread_ctl *kap);
void rdecl thread_destroy_stats_snapshot(struct _thread_destroy_stats *out,
                                         int reset);

int kdecl
ksys_interrupt_attach_thread(thread_t *act,
                             struct kcall_args_interrupt_attach_thread *kap);
int kdecl
ksys_interrupt_attach_event(thread_t *act,
                            struct kcall_args_interrupt_attach_event *kap);
int kdecl ksys_interrupt_detach(thread_t *act,
                                struct kcall_args_interrupt_detach *kap);
int kdecl ksys_interrupt_wait(thread_t *act,
                              struct kcall_args_interrupt_wait *kap);
int kdecl ksys_interrupt_mask(thread_t *act,
                              struct kcall_args_interrupt_mask *kap);
int kdecl ksys_interrupt_unmask(thread_t *act,
                                struct kcall_args_interrupt_unmask *kap);

int kdecl ksys_sched_get(thread_t *act, struct kcall_args_sched_get *kap);
int kdecl ksys_sched_set(thread_t *act, struct kcall_args_sched_set *kap);
int kdecl ksys_sched_yield(thread_t *act, struct kcall_args_null *kap);
int kdecl ksys_sched_info(thread_t *act, struct kcall_args_sched_info *kap);

int kdecl ksys_clock_time(thread_t *act, struct kcall_args_clock_time *kap);
int kdecl ksys_clock_period(thread_t *act, struct kcall_args_clock_period *kap);
int kdecl ksys_clock_id(thread_t *act, struct kcall_args_clock_id *kap);

int kdecl ksys_timer_create(thread_t *act, struct kcall_args_timer_create *kap);
int kdecl ksys_timer_destroy(thread_t *act,
                             struct kcall_args_timer_destroy *kap);
int kdecl ksys_timer_settime(thread_t *act,
                             struct kcall_args_timer_settime *kap);
int kdecl ksys_timer_info(thread_t *act, struct kcall_args_timer_info *kap);
int kdecl ksys_timer_timeout(thread_t *act,
                             struct kcall_args_timer_timeout *kap);

int kdecl ksys_sync_create(thread_t *act, struct kcall_args_sync_create *kap);
int kdecl ksys_sync_destroy(thread_t *act, struct kcall_args_sync_destroy *kap);
int kdecl ksys_sync_mutex_lock(thread_t *act,
                               struct kcall_args_sync_mutex_lock *kap);
int kdecl ksys_sync_mutex_unlock(thread_t *act,
                                 struct kcall_args_sync_mutex_unlock *kap);
int kdecl ksys_sync_mutex_revive(thread_t *act,
                                 struct kcall_args_sync_mutex_revive *kap);
int kdecl ksys_sync_ctl(thread_t *act, struct kcall_args_sync_ctl *kap);
int kdecl ksys_sync_condvar_wait(thread_t *act,
                                 struct kcall_args_sync_condvar_wait *kap);
int kdecl ksys_sync_condvar_signal(thread_t *act,
                                   struct kcall_args_sync_condvar_signal *kap);
int kdecl ksys_sync_sem_wait(thread_t *act,
                             struct kcall_args_sync_sem_wait *kap);
int kdecl ksys_sync_sem_post(thread_t *act,
                             struct kcall_args_sync_sem_post *kap);

int kdecl ksys_trace_event(thread_t *act, struct kcall_args_trace_event *kap);

int kdecl ksys_priv(thread_t *act, struct kcall_args_priv *kap);
int priv_check(process_t *prp, unsigned id);
int priv_dispatch(process_t *prp, unsigned id, void *arg);

void rdecl kcall_err(thread_t *thp, int err);
bool rdecl cred_is_root(thread_t *thp);
bool rdecl cred_may_access(thread_t *thp, process_t *dst);

int rdecl ksched_ok(thread_t *thp, int policy, const struct sched_param *param);
int kcall_attach(unsigned callnum, ksys_call_t *func);
void ksys_start(void);
void sync_mutex_lock(thread_t *act, sync_t *sync, int flags);
void mutex_holdlist_add(thread_t *, ksync_t *);
void mutex_holdlist_rem(ksync_t *);
/* _locked variants: caller already holds sync_slock. */
void mutex_holdlist_add_locked(thread_t *, ksync_t *);
void mutex_holdlist_rem_locked(ksync_t *);
void rdecl thread_specret(thread_t *thp);

/* Notification-based replacement for SIGCHLD — implemented in roottask */
void notification_notify_proc_terminated(process_t *parent, process_t *prp);

/* Unconditional process kill (replaces SIGKILL) — implemented in roottask */
void process_destroy(process_t *prp, pid_t killer_pid);

void heap_init(size_t);
int crit_sfree(void *p, size_t size);

const struct sigevent *clock_handler(void *, int id);
struct sigevent *fpu_exception();
struct sigevent *fpu_switch();

void init_cpu(void);
void init_traps(void);
void init_objects(void);
void init_vm_ops(void);
void mm_init_mem(int phase);
void init_smp(void);
kdispatch_t *init_scheduler_default();

void rdecl clock_resolution(unsigned long nsec);
void clock_start(unsigned long nsec);

void idle(void);
void ksys_start_roottask(void);

void _smpstart(void);
void cpu_idle(unsigned cpu);
void __attribute__((noreturn)) idle_run_thread(thread_t *thp);
thread_t *idle_pick_thread(unsigned cpu);

void rdecl interrupt_init(void);
int rdecl interrupt_attach(unsigned level,
                           const struct sigevent *(*handler)(void *area,
                                                             int id),
                           void *area,
                           unsigned flags);
int rdecl interrupt_attach_event(unsigned level,
                                 const struct sigevent *event,
                                 unsigned flags);
void rdecl interrupt_detach_entry(process_t *prp, unsigned index);
int rdecl interrupt_mask(unsigned level, interrupt_t *itp);
int rdecl interrupt_unmask(unsigned level, interrupt_t *itp);
int rdecl get_interrupt_level(thread_t *act, unsigned vector);

unsigned(xferiov_pos)(CPU_REGISTERS *regs);
int(xferiov)(thread_t *sthp,
             iov_t *dst,
             iov_t *src,
             int dparts,
             int sparts,
             unsigned doff,
             unsigned soff);

int(xfermsg)(thread_t *dthp, thread_t *sthp, unsigned doff, unsigned soff);
int(xfernotification)(thread_t *dthp,
                      iov_t *dst,
                      int parts,
                      uint32_t code,
                      uintptr_t value,
                      int32_t scoid);
int(xferlen)(thread_t *thp, iov_t *iov, int parts);
int(xfer_memcpy)(void *dst, const void *src, size_t len);
int xfer_probe(void *loc, unsigned words, int write);
int xfer_atomic_or_user(volatile unsigned *uaddr, unsigned bits);
int xfer_fetch_or_user(volatile unsigned *uaddr, unsigned bits, unsigned *oldp);
int(xfer_cpy_diov)(thread_t *thpd,
                   iov_t *dst,
                   uint8_t *saddr,
                   int dparts,
                   size_t slen);
int(xfer_memchk)(uintptr_t bound, const iov_t *iov, size_t iov_len);
void xfer_restart(thread_t *thp, CPU_REGISTERS *regs);
void xfer_async_restart(thread_t *thp, CPU_REGISTERS *regs);

void __attribute__((noreturn)) kernel_exit(void);

void kernel_init(int argc, char *argv[], char *environ[]);

int rdecl cpu_debug_sstep(debug_t *dep, thread_t *thp);
int rdecl cpu_debug_brkpt(debug_t *dep, breakpoint_t *bpp);
void rdecl cpu_debug_attach_brkpts(debug_t *dep);
void rdecl cpu_debug_detach_brkpts(debug_t *dep);
int rdecl cpu_debug_fault(debug_t *dep,
                          thread_t *thp,
                          siginfo_t *info,
                          unsigned *pflags);
int rdecl cpu_debug_get_altregs(thread_t *thp, debug_altreg_t *regs);
int rdecl cpu_debug_set_altregs(thread_t *thp, debug_altreg_t *regs);
int rdecl cpu_debug_set_perfregs(thread_t *thp, debug_perfreg_t *regs);
int rdecl cpu_debug_get_perfregs(thread_t *thp, debug_perfreg_t *regs);

void halt(void);

int rdecl synchash_add(path_object_t *obp, paddr_t addr, ksync_t *syp);
ksync_t *rdecl synchash_lookup(path_object_t *obp, paddr_t addr);
void rdecl synchash_rem(paddr_t addr,
                        path_object_t *obp,
                        paddr_t addr1,
                        paddr_t addr2,
                        process_t *prp,
                        void *vaddr);

uint32_t exe_pt_event_h(ehandler_data_t *,
                        uint32_t,
                        uint32_t,
                        uint32_t,
                        uint32_t,
                        uint32_t);
uint32_t exe_event_h(ehandler_data_t *, uint32_t, uint32_t, uint32_t);
uint32_t exe_pt_event_h_buff(ehandler_data_t *,
                             uint32_t,
                             uint32_t,
                             uint32_t,
                             void *,
                             uint32_t);
uint32_t exe_event_h_buff(int, ehandler_data_t *, uint32_t, void *, uint32_t);
int trace_event(uint32_t *);
void add_ktrace_int_handler_enter(intr_level_t *, interrupt_t *);
void add_ktrace_int_handler_exit(intr_level_t *, const struct sigevent *);
void add_ktrace_int_enter(intr_level_t *);
void add_ktrace_int_exit(intr_level_t *);
void trace_emit_irq_claim(intr_level_t *, interrupt_t *);
void trace_emit_ist_dispatch(interrupt_t *, thread_t *);
void trace_emit_pi_boost(thread_t *, unsigned, unsigned);
void trace_emit_kcall(uint32_t, uint32_t, unsigned, uint64_t);
int trace_read_cmd(thread_t *, const uint32_t *, uint32_t *, unsigned);
int trace_read_user(thread_t *, const void *, void *, size_t);
int trace_read_str(thread_t *, const char *, char *, size_t);
void time_em(uint32_t, uint32_t);
int trace_flushbuffer(void);
void destroy_eh(process_t *);
void add_trace_string(uint32_t header, const char *fmt, ...);
void add_trace_d1_string(uint32_t header, uint32_t d_1, const char *fmt, ...);
void add_trace_d2_string(uint32_t header,
                         uint32_t d_1,
                         uint32_t d_2,
                         const char *fmt,
                         ...);
int add_trace_event(unsigned code,
                    unsigned data_0,
                    unsigned data_1,
                    unsigned data_2);
void add_trace_buffer(uint32_t header, uint32_t *b_p, unsigned len);
void add_trace_iovs(uint32_t header, iov_t *iovs, unsigned iovlen);
void th_em_st(thread_t *thp, uint32_t s, unsigned cpu);
void th_em_cd(thread_t *thp, uint32_t s, uint32_t c);
void th_em_name(thread_t *thp);
void vth_em_cd(thread_t *vthp, uint32_t s, uint32_t c);
void vth_em_st(thread_t *vthp, uint32_t s);
void pr_em_create(process_t *prp, pid_t pid);
void pr_em_destroy(process_t *prp, pid_t pid);
void pr_em_create_name(process_t *prp);
void pr_em_destroy_name(process_t *prp);
void comm_em(thread_t *thp, connect_t *cop, uint32_t id, uint32_t ev);
void comm_exe_em(connect_t *cop, uint32_t id, uint32_t ev);
/* comm_em_signal removed — signals eliminated */
void trace_emit_th_state(thread_t *thp, int state);
void trace_emit_address(thread_t *thp, unsigned vaddr);

void waitpage_status_get(thread_t *thp, debug_thread_t *dtp);


uint64_t get_nsec_since_block(thread_t *thp); /* in thread-debug.c */

/* CPU specific things */
void cpu_syspage_init(void);
void cpu_interrupt_init(unsigned);
void cpu_thread_init(thread_t *act, thread_t *thp, int align);
void cpu_thread_waaa(thread_t *thp);
void cpu_thread_priv(thread_t *thp);
void cpu_thread_align_fault(thread_t *thp);
void cpu_thread_destroy(thread_t *thp);
/* cpu_signal_save/restore removed — signals eliminated */
void cpu_intr_attach(interrupt_t *itp, thread_t *thp);
void cpu_greg_load(thread_t *thp, CPU_REGISTERS *regs);
void cpu_reboot(void);
void cpu_shutdown(void);
void cpu_start_others(uintptr_t start);
void plic_init(void);
void plic_inithart(void);
void cpu_force_fpu_save(thread_t *thp);
#ifdef CONFIG_DEBUG_INJECT
int inject_should_fail_alloc(process_t *prp, soul_t *soulp); /* debug/inject.c */
#endif
void fpu_drop_claims(thread_t *thp);
/* User FP context: save/restore f0-f31 and fcsr (arch/riscv/fpcontext.S). */
void cpu_fpu_save(FPU_REGISTERS *fp);
void cpu_fpu_restore(const FPU_REGISTERS *fp);
void cpu_invoke_func(thread_t *, uintptr_t);
void cpu_mutex_adjust(thread_t *);
void cpu_thread_enter_umode(thread_t *thp);

struct sigevent *hook_trace(void *, interrupt_t *);

int rdecl remote_ipc_msg(thread_t *act, connect_t *cop, unsigned prio);
int rdecl remote_ipc_notify(thread_t *act,
                            connect_t *cop,
                            unsigned coid,
                            unsigned prio,
                            int code,
                            int value);
/* net_deliver_signal removed — signals eliminated */

unsigned cpu_perfreg_id(void);
void rdecl cpu_save_perfregs(void *);
void rdecl cpu_restore_perfregs(void *);
void rdecl cpu_free_perfregs(thread_t *thp);
void rdecl cpu_debug_init_perfregs(void);
/* int  rdecl    cpu_alloc_perfregs(thread_t *thp); */

int(rcvmsg)(thread_t *dthp,
            process_t *sprp,
            void *destp,
            int destparts,
            void *srcp,
            int srcparts);

#undef MMF
#define MMF(r, f, p, e) MMF_PROTO(r, f, p, e)
MM_FUNCS(emm)

#ifdef CONFIG_DEBUG_PRIL
void *rdecl pril_first(pril_head_t *);
void *rdecl pril_next(void *);
#else
#define pril_first(ph) ((void *) (ph)->data)
#define pril_next(p) ((void *) (p)->next.pril)
#endif

void *rdecl pril_next_prio(void *);
void *rdecl pril_find_insertion(pril_head_t *, unsigned);
void rdecl pril_add(pril_head_t *, void *);
void rdecl pril_rem(pril_head_t *, void *);
void rdecl pril_update_register(pril_head_t *, struct pril_update *);
void rdecl pril_update_unregister(pril_head_t *, struct pril_update *);

/* arch-provided */
uint64_t clock_cycles(void);
uint64_t cpu_clock_ticks(void);

#if defined(CONFIG_INSTRUMENTATION)
/* section 18 interrupt-disabled window stats dump (globals.c). */
void nonpreempt_report(void);
#endif
