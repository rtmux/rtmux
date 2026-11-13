/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef _KERNEL_DECLS_H
#define _KERNEL_DECLS_H

#include <arch/cpu-attrs.h>
#include <vm-hooks.h>
#include <debug.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <log.h>

#ifndef STRINGIZE
#define STRINGIZE_(x) #x
#define STRINGIZE(x) STRINGIZE_(x)
#endif

/* The kernel-internal declarations below take pointers to types that are gated
 * as kernel-private in include/objects.h (process_t, thread_t, connect_t,
 * credential_t, sched_info_t, ...). Compiling them in a context that does not
 * define KERNEL_INTERNAL would yield "unknown type name" errors, so the whole
 * body is gated. Roottask should be reaching for the small public-declarations
 * header (kernel/include/private/kernel-util.h) for the handful of kernel
 * utility functions it legitimately needs (object_alloc, vector_*, etc.); a
 * stray "#include <decls.h>" from roottask code now gives an empty file rather
 * than struct-deref errors.
 */
#ifdef KERNEL_INTERNAL

/* Kernel declarations */

/* Core scheduler/thread calls */
void rdecl block(void);
void rdecl unready(thread_t *thp, int newstate);
void __attribute__((noreturn)) rdecl
unready_release_and_exit(thread_t *thp, int newstate, kspinlock_t *to_release);
void rdecl stop_threads(process_t *prp, unsigned flags, unsigned iflags);
void rdecl cont_threads(process_t *prp, unsigned flags);
int rdecl stop_one_thread(process_t *prp,
                          int tid,
                          unsigned flags,
                          unsigned iflags);
int rdecl cont_one_thread(process_t *prp, int tid, unsigned flags);
int rdecl set_runmask(thread_t *thp, unsigned runmask);
void *rdecl object_alloc(process_t *prp, soul_t *soulp);
void *rdecl object_alloc_reserved(process_t *prp, soul_t *soulp);
bool rdecl object_reserve(soul_t *soulp);
void rdecl object_unreserve(soul_t *soulp);
bool rdecl object_grow(soul_t *soulp);
void rdecl object_free(process_t *prp, soul_t *soulp, void *ptr);
void rdecl object_free_track_heap(process_t *prp,
                                  soul_t *soulp,
                                  void *ptr,
                                  bool *heap_returned);
void rdecl object_free_track_heap_detail(process_t *prp,
                                         soul_t *soulp,
                                         void *ptr,
                                         bool *heap_returned,
                                         uint64_t *cache_clear_cycles,
                                         uint64_t *cache_link_cycles);
unsigned rdecl object_register_data(soul_t *soulp, size_t size);
#define object_to_data(o, c) ((void *) ((char *) (o) + (c)))
#define object_from_data(o, c) ((void *) ((char *) (o) - (c)))
void *rdecl vector_lookup(vector_t *vec, unsigned id);
void *rdecl vector_lookup2(vector_t *vec, unsigned id);
void *rdecl vector_search(vector_t *vec, unsigned id, unsigned *found);
int rdecl vector_add(vector_t *vec, void *object, unsigned index);
void *rdecl vector_rem(vector_t *vec, unsigned index);
int rdecl vector_flag(vector_t *vec, unsigned index, int flag);
void rdecl vector_free(vector_t *vec);
void *nano_query(unsigned type,
                 unsigned index1,
                 int subtype,
                 unsigned index2,
                 unsigned *next,
                 void *objbuf,
                 size_t objsize);
int rdecl force_ready(thread_t *thp, unsigned err);
void specialret_attr specialret(thread_t *thp);
void rdecl deliver_fault(thread_t *thp, siginfo_t *info);
uint64_t rlimit_cpu_to_ns(uint64_t secs);
void rdecl usr_fault(int code_signo, thread_t *act, uintptr_t addr);
void rdecl timer_init(void);
ktimer_t *rdecl timer_alloc(process_t *);
void rdecl timer_free(process_t *, ktimer_t *);
void rdecl timer_period(void);
void rdecl timer_remaining(ktimer_t *tip, struct _itimer *left);
int rdecl timer_past(int clockid, int flags, uint64_t *firetime);
void rdecl timer_activate(ktimer_t *tip);
int rdecl timer_deactivate(ktimer_t *tip);
void rdecl timer_next(uint64_t *nexttime);
void rdecl timer_next_deadline(uint64_t *nexttime);
void rdecl timer_deadline_invalidate(void);

/* Composite next-deadline owner tags, reported by clock_next_deadline() for
 * diagnostics only.
 */
#define CLOCK_OWNER_NONE 0
#define CLOCK_OWNER_TIMER 1
#define CLOCK_OWNER_RR 2
#define CLOCK_OWNER_SS 3
#define CLOCK_OWNER_TOD 4
#define CLOCK_OWNER_STALL 5

/* Earliest absolute monotonic-ns deadline idle timer programming must not sleep
 * past on the given hart, across every owner that still needs a wakeup there.
 *
 * The timer wheel, sporadic-server replenishment, TOD adjustment, and the stall
 * detector are all serviced only by the boot CPU, so they contribute only when
 * cpu == boot_cpunum; the round-robin timeslice is per-hart and always uses the
 * given cpu's running thread.
 *
 * Returns all-ones when no owner needs a wakeup on that hart. When owner is
 * non-NULL it receives the winning CLOCK_OWNER_* tag. When cands is non-NULL it
 * is a caller-initialized array indexed by CLOCK_OWNER_* that receives each
 * evaluated owner's candidate (for diagnostics). Read-only and allocation-free.
 */
uint64_t rdecl clock_next_deadline(systime_t *qtp,
                                   unsigned cpu,
                                   unsigned *owner,
                                   uint64_t *cands);
void rdecl timer_expiry(systime_t *qtp);
void rdecl timer_pending(void);

/* Deferral flag. Set by timer_expiry when it has work that cannot run from trap
 * context; checked by kernel_exit (and cpu_idle) which calls timer_pending() to
 * drain. Single-writer at trap time + best-effort readers; volatile is
 * sufficient.
 *
 * timer_drain_busy is a C11 atomic CAS-claim flag — see timer-core.c for the
 * exact pattern. Ensures at most one hart enters timer_pending at a time,
 * avoiding a multi-hart livelock.
 */
extern volatile uint8_t timer_ops_delayed;
extern atomic_int timer_drain_busy;
void rdecl thread_init(void);
int rdecl thread_create(thread_t *act,
                        process_t *prp,
                        const struct sigevent *evp,
                        unsigned thread_create_flags,
                        const struct _thread_attr *kattr);
int rdecl kernel_thread_create(void (*entry)(void *),
                               void *arg,
                               unsigned priority,
                               unsigned runmask,
                               thread_t **out);
int rdecl kernel_thread_checkpoint(CPU_REGISTERS *regs);
void rdecl thread_destroy(thread_t *thp);
void rdecl thread_destroyall(thread_t *thp);
void rdecl thread_cancel(thread_t *thp);
int rdecl process_pin(process_t *prp);
void rdecl process_unpin(process_t *prp);
int rdecl tthread_pin(thread_t *thp);
int thread_active_hart(thread_t *thp);
int exit_ready_stopped(thread_t *thp);
void rdecl tthread_unpin(thread_t *thp);
int rdecl thread_set_as_prp(thread_t *thp, process_t *prp);
void rdecl thread_clear_as_prp_if(thread_t *thp, process_t *prp);
void rdecl sync_thread_cleanup(thread_t *thp);
uint32_t rdecl keygen(const unsigned char *buf, size_t n, uint64_t secret);
uint64_t keydata_draw(uintptr_t salt);
void keydata_stir(void);
void rdecl get_rcvinfo(thread_t *thp,
                       int tid,
                       connect_t *cop,
                       struct _msg_info *rep);
void rdecl connect_detach(connect_t *cop, unsigned prio);
int rdecl connect_client_drop(connect_t *cop, unsigned prio);
void endpoint_cap_revoke_if_present(connect_t *cop);
void cap_revoke_object_all_tables(const void *object);
void connect_soul_free(process_t *prp, connect_t *cop);
void connect_unreserve(connect_t *cop);
void rdecl connect_coid_disconnect(connect_t *cop,
                                   channel_t *chp,
                                   unsigned priority);
vthread_t *rdecl remote_ipc_vthread(unsigned vtid, struct _vtid_info *vtp);
connect_t *rdecl remote_ipc_send(kcall_args_t *kap,
                                 unsigned vtid,
                                 connect_t *cop,
                                 thread_t *thp);
void rdecl snap_time_wakeup(void);
void rdecl snap_time(uint64_t *tsp, int incl_tod);
int rdecl cred_set(credential_t **pcrp, struct _cred_info *cip);
void rdecl cred_dirty(process_t *prp);

/* memory object allocator calls
 *
 * Heap selectors for _sreallocfunc. The kernel maintains two free lists, both
 * protected by alloc_slock: a main list (ksys_free_list) and a small
 * pre-reserved critical list (ksys_crit_list) used when alloc() fails on the
 * main list. The U-mode roottask uses its own libc malloc and never touches the
 * kernel allocator.
 */
enum heap_choice {
    HEAP_KER,      /* kernel main free list (alloc_slock)            */
    HEAP_KER_CRIT, /* kernel critical free list (alloc_slock)        */
};

void *malloc(size_t size);
void *calloc(size_t nmemb, size_t size);
void *realloc(void *data, size_t size);
void free(void *data);
void *_smalloc(size_t size);
void *_scalloc(size_t size);
void *_scalloc_pgalign(size_t size);
void *_scalloc_page(void);
void _sfree_page(void *p);
void *_srealloc(void *data, size_t old_size, size_t new_size);
void *_sreallocfunc(void *data,
                    size_t old_size,
                    size_t new_size,
                    size_t (*alloc)(size_t size, void **addr),
                    enum heap_choice heap);
void _sfree(void *data, size_t size);
int purger_register(int (*purge)(size_t), unsigned prio);
int purger_invoke(size_t amount);

void shutdown(unsigned sigcode, const CPU_REGISTERS *reg);

process_t *rdecl lookup_pid(pid_t pid);
process_t *rdecl
lookup_pid_rcu(pid_t pid); /* opens RCU-sched section; caller unlocks */
connect_t *rdecl lookup_connect(uint64_t coid);
void rdecl lookup_rcvid_open(void);
connect_t *rdecl lookup_rcvid(kcall_args_t *kap, int rcvid, thread_t **thpp);

/* lookup_rcvid_release — close the nested RCU-sched read sections that
 * lookup_rcvid opened on entry. Every lookup_rcvid call MUST be paired with one
 * release, including paths that received NULL (the sections are opened
 * unconditionally). Call after the last dereference of cop / thp.
 */
void rdecl lookup_rcvid_release(void);
limits_t *rdecl lookup_limits(uid_t uid);
ksync_t *rdecl sync_create(process_t *bill_prp, sync_t *sync, unsigned flags);
int rdecl sync_destroy(process_t *prp, sync_t *sync);
ksync_t *rdecl sync_lookup(sync_t *sync, unsigned create);
void rdecl sync_wakeup(ksync_t *syp, int all);
int rdecl sync_chain_force_ready(thread_t *thp);

notification_t *rdecl notification_add(process_t *prp,
                                       pril_head_t *queue,
                                       unsigned priority,
                                       int code,
                                       uintptr_t value,
                                       intptr_t id);
notification_t *rdecl notification_add_reserved(process_t *prp,
                                                pril_head_t *queue,
                                                unsigned priority,
                                                int code,
                                                uintptr_t value,
                                                intptr_t id);
notification_t *rdecl notification_replace(process_t *prp,
                                           pril_head_t *queue,
                                           int priority,
                                           int code,
                                           uintptr_t value,
                                           intptr_t id);
int rdecl notification_deliver(channel_t *chp,
                               unsigned priority,
                               int code,
                               uintptr_t value,
                               intptr_t id,
                               unsigned notification_flags);
void rdecl notification_remove(process_t *prp,
                               pril_head_t *queue,
                               notification_t *pup);

void rdecl remove_unblock(thread_t *thp, connect_t *cop, int rcvid);

/* Add support for sporatic cleanup */
void rdecl sched_ss_cleanup(thread_t *thp);
void rdecl sched_account_current(uint64_t now);
void rdecl sched_account_restart(void);
uint64_t rdecl sched_account_read(thread_t *thp, uint64_t now);
int rdecl sched_ss_adjust(void);
void rdecl sched_ss_update(thread_t *thp,
                           sched_info_t *ssinfo,
                           sched_replenish_t *ssrepl,
                           int drop);
void rdecl sched_ss_block(thread_t *thp, int preempted);
void rdecl sched_ss_queue(thread_t *thp);

int rdecl sched_thread(thread_t *thp, int policy, struct sched_param *param);

void rdecl timeout_start(thread_t *thp);
void rdecl timeout_start_deferred(thread_t *thp);
void rdecl timeout_deferred_fire(void);
void rdecl timeout_stop(thread_t *thp);

/* reserved != 0 routes a SIGEV_NOTIFICATION through the allocation-free reserve
 * (notification_add_reserved); only IRQ/timer sources that called
 * object_reserve pass it. Every other caller passes 0.
 */
int rdecl sigevent_exe(const struct sigevent *evp,
                       thread_t *thp,
                       int verify,
                       int reserved);

/* Direct-from-trap notification delivery: invoked from trap context (PLIC +
 * STIMER) and from kernel-extension context (timer activation). See event.c for
 * the helper body and the invariants it relies on. reserved != 0 selects the
 * allocation-free reserve for the SIGEV_NOTIFICATION case.
 */
void sigevent_deliver_inline(const struct sigevent *evp,
                             thread_t *target,
                             int reserved);
int rdecl sigevent_proc(const struct sigevent *evp);
int rdecl sigevent_notification_roottask(int code,
                                         unsigned priority,
                                         uintptr_t value);

/* There is no intrevent_pending queue. The two surviving entry points:
 *   - intrevent_preemption: writes per-CPU pending_preempt_prio.
 * - intrevent_flush: cross-CPU INKERNEL_INTRMASK spin (no drain).
 */
void intrevent_preemption(unsigned);
void intrevent_flush(void);

int viov_to_piov(process_t *prp,
                 iov_t *piop,
                 int pparts,
                 iov_t *viop,
                 int vparts,
                 int offset);

int rdecl debug_process(process_t *prp, debug_process_t *dpp);
int rdecl debug_thread(process_t *prp, thread_t *thp, debug_thread_t *dtp);
int rdecl debug_stop(process_t *prp);
int rdecl debug_run(process_t *prp, debug_run_t *drp);
int rdecl debug_break(process_t *pid, debug_break_t *dbp);
int rdecl debug_break_list(process_t *pid, debug_breaklist_t *dbp);

unsigned kdebug_enter(process_t *prp, unsigned sigcode, CPU_REGISTERS *reg);

void rdecl send_ipi(unsigned cpu, int cmd);
void rdecl smp_flush_tlb(void);
void cpu_icache_sync_all(void);

struct asinfo_entry;
int walk_asinfo(const char *name,
                int (*func)(struct asinfo_entry *, char *, void *),
                void *data);

/* STRLCPY(dst, src, len)
 *
 * BSD 'strlcpy()' - google it NOTE however that unlike the BSD version, this
 * macro (currently) does
 * 	not return anything
 *
 * In general ... a more efficient and safer implementation of strncpy() (non
 * debug)
 * - <dst> is guaranteed to be null terminated
 * - copying stops at first '\0' in <src>
 * - debug version will identify truncated strings
 */
#ifndef NDEBUG
#define STR_OVERFLOW(f, line, s, d, l)                                  \
    kprintf(                                                            \
        "%s:%u - strncpy overflow, src truncated to %d bytes\n\tsrc - " \
        "'%s'\n\tdst  - '%s'\n",                                        \
        (f), (line), (l) - 1, (s), (d))
#else
#define STR_OVERFLOW(f, line, s, d, l)
#endif /* NDEBUG */

#define STRLCPY(d, s, l)                                                     \
    ({                                                                       \
        CRASHCHECK((size_t) (l) >                                            \
                   4096); /* what we believe to be a reasonable value */     \
        if (!memccpy((d), (s), '\0', (l))) {                                 \
            (d)[((l) > 0) ? (l) - 1 : (l)] = '\0';                           \
            /* STR_OVERFLOW is already NDEBUG-gated.  Gating it              \
             * further on the kernel-only kopt symbol would pull             \
             * kopt into every TU that uses STRLCPY, including               \
             * roottask ones.  Truncation is rare and worth knowing          \
             * about — let it print. */                                    \
            STR_OVERFLOW(STRINGIZE(__BASE_FILE__), __LINE__, (s), (d), (l)); \
        }                                                                    \
    })

/* Architecture-dependent declarations */
int arch_timer_reload(systime_t *);
int arch_timer_load(systime_t *);
uint64_t arch_timer_now_ns(systime_t *);

#endif /* KERNEL_INTERNAL */

#endif /* _KERNEL_DECLS_H */
