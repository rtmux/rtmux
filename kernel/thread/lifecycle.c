/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <core/globals.h>
#include <sys/procmsg.h>

/* Pool-thread stack free list. Heads a singly-linked list using the first 8
 * bytes of each cached stack page as the link. Populated by the
 * __KSYS_PRIV_REGISTER_POOL_STACK kernel extension (called from roottask's
 * message_start after as_mmap'ing each stack into its own user VA), consumed by
 * roottask_stack_alloc on the WAAA path of any new thread of pid 1 (roottask).
 */
void **roottask_stack_cache;
size_t roottask_stack_size;
#define NO_PURGER (~(size_t) 0)

static kspinlock_t thread_as_slock;
static int roottask_stack_purge(size_t amount);
extern void proc_threads_destroyed(process_t *prp, struct sigevent *evp);
extern void kernel_thread_exit_fallback(void *arg);

static inline void thread_cpu_relax(void)
{
    /* RISC-V Zihintpause PAUSE encoding, kept raw for older assemblers. */
    __asm__ volatile(".word 0x0100000f" ::: "memory");
}

static void thread_recycle_reset(thread_t *thp)
{
    memset(thp, 0, __builtin_offsetof(thread_t, real_priority));
    thp->state = STATE_DEAD;
    thp->reserved_word = 0;
    thp->runcpu = 0;
    thp->internal_flags = 0;
    thp->timestamp_last_block = 0;
    thp->as_prp = NULL;
    atomic_store_explicit(&thp->async_flags, 0, memory_order_relaxed);
    thp->dpp = NULL;
    thp->timeout_flags = 0;
    thp->flags = 0;
    thp->sched_flags = 0;
    thp->syscall = -1;
    thp->process = NULL;
    thp->tid = 0;
    thp->orig_dpp = NULL;
    thp->schedinfo.rr_ticks = 0;
    thp->un.lcl.tls = NULL;
    thp->un.lcl.stackaddr = NULL;
    thp->un.lcl.stacksize = 0;
    thp->client = NULL;
    thp->blocked_on = NULL;
    thp->blocked_on_turnstile = NULL;
    thp->owned_turnstiles = NULL;
    thp->restart = NULL;
    memset(&thp->args.wa, 0, sizeof(thp->args.wa));
    thp->runmask = 0;
    thp->default_runmask = 0;
    thp->name = NULL;
    thp->key = 0;
    thp->last_chid = -1;
    thp->status = NULL;
    thp->timeout = NULL;
    thp->join = NULL;
    thp->mutex_holdlist = NULL;
    thp->start_time = 0;
    thp->fpudata = NULL;
    thp->ticker_using = 0;
    thp->running_time = 0;
    thp->link3_link.next = NULL;
    thp->link3_link.prev = NULL;
    memset(&thp->cpu, 0, sizeof(thp->cpu));
    memset(&thp->reg, 0, sizeof(thp->reg));
    thp->trap_aux.kernel_sp = 0;
    thp->trap_aux.kernel_tp = 0;
    memset(thp->kcall_c_args, 0, sizeof(thp->kcall_c_args));
    thp->destroy_claim = 0;
    thp->pin_count = 0;
    memset(thp->held_ceilings, 0, sizeof(thp->held_ceilings));
    thp->held_ceiling_n = 0;
    thp->held_ceiling_floor = 0;
    thp->held_ceiling_saturated = false;
    thp->stall_reported_ts = 0;
}

static void thread_join_wait_ready(thread_t *thp)
{
    while (thp->internal_flags & RTMUX_ITF_WAIT_PENDING)
        thread_cpu_relax();
    cpu_smp_mb();
}

static volatile unsigned long thread_destroy_calls;
static volatile unsigned long thread_destroy_rcu_wait_ns;
static volatile unsigned long thread_destroy_rcu_enroll_ns;
static volatile unsigned long thread_destroy_rcu_scan_ns;
static volatile unsigned long thread_destroy_rcu_post_scan_ns;
static volatile unsigned long thread_destroy_pin_wait_ns;
static volatile unsigned long thread_destroy_active_scan_ns;
static volatile unsigned long thread_destroy_object_free_ns;
static volatile unsigned long thread_destroy_object_free_heap;
static volatile unsigned long thread_destroy_object_free_cache;
static volatile unsigned long thread_destroy_object_free_heap_ns;
static volatile unsigned long thread_destroy_object_free_cache_ns;
static volatile unsigned long thread_destroy_object_free_cache_clear_ns;
static volatile unsigned long thread_destroy_object_free_cache_link_ns;
static volatile unsigned long thread_destroy_pin_waited;
static volatile unsigned long thread_destroy_pin_skipped;
static volatile unsigned long thread_destroy_rcu_waited;
static volatile unsigned long thread_destroy_rcu_skipped;
static volatile unsigned long thread_destroy_rcu_waited_ns;
static volatile unsigned long thread_destroy_rcu_skipped_ns;
static volatile unsigned long thread_destroy_rcu_busy_peer_sum;
static volatile unsigned long thread_destroy_rcu_busy_peer_max;
static volatile unsigned long thread_destroy_rcu_busy_thread;
static volatile unsigned long thread_destroy_rcu_busy_msg;
static volatile unsigned long thread_destroy_rcu_busy_sys;
static volatile unsigned long thread_destroy_rcu_busy_channel;
static volatile unsigned long thread_destroy_rcu_busy_connect;
static volatile unsigned long thread_destroy_rcu_busy_interrupt;
static volatile unsigned long thread_destroy_rcu_busy_sync;
static volatile unsigned long thread_destroy_rcu_busy_sched;
static volatile unsigned long thread_destroy_rcu_busy_time;
static volatile unsigned long thread_destroy_rcu_busy_idle;
static volatile unsigned long thread_destroy_rcu_busy_dispatch;
static volatile unsigned long thread_destroy_rcu_busy_timer;
static volatile unsigned long thread_destroy_rcu_busy_fault;
static volatile unsigned long thread_destroy_rcu_busy_priv;
static volatile unsigned long thread_destroy_rcu_busy_heartbeat;
static volatile unsigned long thread_destroy_rcu_busy_priv_system;
static volatile unsigned long thread_destroy_rcu_busy_priv_vm;
static volatile unsigned long thread_destroy_rcu_busy_priv_ipc;
static volatile unsigned long thread_destroy_rcu_busy_priv_process;
static volatile unsigned long thread_destroy_rcu_busy_priv_thread;
static volatile unsigned long thread_destroy_rcu_busy_priv_pa;
static volatile unsigned long thread_destroy_rcu_busy_priv_pa_alloc;
static volatile unsigned long thread_destroy_rcu_busy_priv_pa_free;
static volatile unsigned long thread_destroy_rcu_busy_priv_pa_zero;
static volatile unsigned long thread_destroy_rcu_busy_priv_pa_info;
static volatile unsigned long thread_destroy_rcu_busy_priv_pa_xlat;
static volatile unsigned long thread_destroy_rcu_busy_priv_pa_pq;
static volatile unsigned long thread_destroy_rcu_busy_unstamped;
static volatile unsigned long thread_destroy_rcu_busy_other;
static volatile unsigned long thread_destroy_claim_active;
static volatile unsigned long thread_destroy_claim_reserved;
static volatile unsigned long thread_destroy_join_wake;
static volatile unsigned long thread_destroy_join_wake_ns;
static volatile unsigned long thread_destroy_join_post_ready;
static volatile unsigned long thread_destroy_join_post_ready_ns;
static volatile unsigned long thread_destroy_post_vector_ns;
static volatile unsigned long thread_destroy_post_cpu_ns;
static volatile unsigned long thread_destroy_post_name_ns;
static volatile unsigned long thread_destroy_post_notify_ns;
static volatile unsigned thread_destroy_stats_enabled;

static uint64_t thread_destroy_cycles_to_ns(uint64_t cycles_delta);

void rdecl thread_destroy_stats_snapshot(struct _thread_destroy_stats *out,
                                         int reset)
{
    if (reset) {
        __atomic_store_n(&thread_destroy_stats_enabled, 0, __ATOMIC_RELEASE);
        out->rcu_wait_ns = __atomic_exchange_n(&thread_destroy_rcu_wait_ns, 0,
                                               __ATOMIC_RELAXED);
        out->rcu_enroll_ns = __atomic_exchange_n(&thread_destroy_rcu_enroll_ns,
                                                 0, __ATOMIC_RELAXED);
        out->rcu_scan_ns = __atomic_exchange_n(&thread_destroy_rcu_scan_ns, 0,
                                               __ATOMIC_RELAXED);
        out->rcu_post_scan_ns = __atomic_exchange_n(
            &thread_destroy_rcu_post_scan_ns, 0, __ATOMIC_RELAXED);
        out->pin_wait_ns = __atomic_exchange_n(&thread_destroy_pin_wait_ns, 0,
                                               __ATOMIC_RELAXED);
        out->active_scan_ns = __atomic_exchange_n(
            &thread_destroy_active_scan_ns, 0, __ATOMIC_RELAXED);
        out->object_free_ns = __atomic_exchange_n(
            &thread_destroy_object_free_ns, 0, __ATOMIC_RELAXED);
        out->object_free_heap = __atomic_exchange_n(
            &thread_destroy_object_free_heap, 0, __ATOMIC_RELAXED);
        out->object_free_cache = __atomic_exchange_n(
            &thread_destroy_object_free_cache, 0, __ATOMIC_RELAXED);
        out->object_free_heap_ns = __atomic_exchange_n(
            &thread_destroy_object_free_heap_ns, 0, __ATOMIC_RELAXED);
        out->object_free_cache_ns = __atomic_exchange_n(
            &thread_destroy_object_free_cache_ns, 0, __ATOMIC_RELAXED);
        out->object_free_cache_clear_ns = __atomic_exchange_n(
            &thread_destroy_object_free_cache_clear_ns, 0, __ATOMIC_RELAXED);
        out->object_free_cache_link_ns = __atomic_exchange_n(
            &thread_destroy_object_free_cache_link_ns, 0, __ATOMIC_RELAXED);
        out->pin_waited = __atomic_exchange_n(&thread_destroy_pin_waited, 0,
                                              __ATOMIC_RELAXED);
        out->pin_skipped = __atomic_exchange_n(&thread_destroy_pin_skipped, 0,
                                               __ATOMIC_RELAXED);
        out->rcu_waited = __atomic_exchange_n(&thread_destroy_rcu_waited, 0,
                                              __ATOMIC_RELAXED);
        out->rcu_skipped = __atomic_exchange_n(&thread_destroy_rcu_skipped, 0,
                                               __ATOMIC_RELAXED);
        out->rcu_waited_ns = __atomic_exchange_n(&thread_destroy_rcu_waited_ns,
                                                 0, __ATOMIC_RELAXED);
        out->rcu_skipped_ns = __atomic_exchange_n(
            &thread_destroy_rcu_skipped_ns, 0, __ATOMIC_RELAXED);
        out->rcu_busy_peer_sum = __atomic_exchange_n(
            &thread_destroy_rcu_busy_peer_sum, 0, __ATOMIC_RELAXED);
        out->rcu_busy_peer_max = __atomic_exchange_n(
            &thread_destroy_rcu_busy_peer_max, 0, __ATOMIC_RELAXED);
        out->rcu_busy_thread = __atomic_exchange_n(
            &thread_destroy_rcu_busy_thread, 0, __ATOMIC_RELAXED);
        out->rcu_busy_msg = __atomic_exchange_n(&thread_destroy_rcu_busy_msg, 0,
                                                __ATOMIC_RELAXED);
        out->rcu_busy_sys = __atomic_exchange_n(&thread_destroy_rcu_busy_sys, 0,
                                                __ATOMIC_RELAXED);
        out->rcu_busy_channel = __atomic_exchange_n(
            &thread_destroy_rcu_busy_channel, 0, __ATOMIC_RELAXED);
        out->rcu_busy_connect = __atomic_exchange_n(
            &thread_destroy_rcu_busy_connect, 0, __ATOMIC_RELAXED);
        out->rcu_busy_interrupt = __atomic_exchange_n(
            &thread_destroy_rcu_busy_interrupt, 0, __ATOMIC_RELAXED);
        out->rcu_busy_sync = __atomic_exchange_n(&thread_destroy_rcu_busy_sync,
                                                 0, __ATOMIC_RELAXED);
        out->rcu_busy_sched = __atomic_exchange_n(
            &thread_destroy_rcu_busy_sched, 0, __ATOMIC_RELAXED);
        out->rcu_busy_time = __atomic_exchange_n(&thread_destroy_rcu_busy_time,
                                                 0, __ATOMIC_RELAXED);
        out->rcu_busy_idle = __atomic_exchange_n(&thread_destroy_rcu_busy_idle,
                                                 0, __ATOMIC_RELAXED);
        out->rcu_busy_dispatch = __atomic_exchange_n(
            &thread_destroy_rcu_busy_dispatch, 0, __ATOMIC_RELAXED);
        out->rcu_busy_timer = __atomic_exchange_n(
            &thread_destroy_rcu_busy_timer, 0, __ATOMIC_RELAXED);
        out->rcu_busy_fault = __atomic_exchange_n(
            &thread_destroy_rcu_busy_fault, 0, __ATOMIC_RELAXED);
        out->rcu_busy_priv = __atomic_exchange_n(&thread_destroy_rcu_busy_priv,
                                                 0, __ATOMIC_RELAXED);
        out->rcu_busy_heartbeat = __atomic_exchange_n(
            &thread_destroy_rcu_busy_heartbeat, 0, __ATOMIC_RELAXED);
        out->rcu_busy_priv_system = __atomic_exchange_n(
            &thread_destroy_rcu_busy_priv_system, 0, __ATOMIC_RELAXED);
        out->rcu_busy_priv_vm = __atomic_exchange_n(
            &thread_destroy_rcu_busy_priv_vm, 0, __ATOMIC_RELAXED);
        out->rcu_busy_priv_ipc = __atomic_exchange_n(
            &thread_destroy_rcu_busy_priv_ipc, 0, __ATOMIC_RELAXED);
        out->rcu_busy_priv_process = __atomic_exchange_n(
            &thread_destroy_rcu_busy_priv_process, 0, __ATOMIC_RELAXED);
        out->rcu_busy_priv_thread = __atomic_exchange_n(
            &thread_destroy_rcu_busy_priv_thread, 0, __ATOMIC_RELAXED);
        out->rcu_busy_priv_pa = __atomic_exchange_n(
            &thread_destroy_rcu_busy_priv_pa, 0, __ATOMIC_RELAXED);
        out->rcu_busy_priv_pa_alloc = __atomic_exchange_n(
            &thread_destroy_rcu_busy_priv_pa_alloc, 0, __ATOMIC_RELAXED);
        out->rcu_busy_priv_pa_free = __atomic_exchange_n(
            &thread_destroy_rcu_busy_priv_pa_free, 0, __ATOMIC_RELAXED);
        out->rcu_busy_priv_pa_zero = __atomic_exchange_n(
            &thread_destroy_rcu_busy_priv_pa_zero, 0, __ATOMIC_RELAXED);
        out->rcu_busy_priv_pa_info = __atomic_exchange_n(
            &thread_destroy_rcu_busy_priv_pa_info, 0, __ATOMIC_RELAXED);
        out->rcu_busy_priv_pa_xlat = __atomic_exchange_n(
            &thread_destroy_rcu_busy_priv_pa_xlat, 0, __ATOMIC_RELAXED);
        out->rcu_busy_priv_pa_pq = __atomic_exchange_n(
            &thread_destroy_rcu_busy_priv_pa_pq, 0, __ATOMIC_RELAXED);
        out->rcu_busy_unstamped = __atomic_exchange_n(
            &thread_destroy_rcu_busy_unstamped, 0, __ATOMIC_RELAXED);
        out->rcu_busy_other = __atomic_exchange_n(
            &thread_destroy_rcu_busy_other, 0, __ATOMIC_RELAXED);
        out->destroy_claim_active = __atomic_exchange_n(
            &thread_destroy_claim_active, 0, __ATOMIC_RELAXED);
        out->destroy_claim_reserved = __atomic_exchange_n(
            &thread_destroy_claim_reserved, 0, __ATOMIC_RELAXED);
        out->destroy_join_wake =
            __atomic_exchange_n(&thread_destroy_join_wake, 0, __ATOMIC_RELAXED);
        out->destroy_join_wake_ns = __atomic_exchange_n(
            &thread_destroy_join_wake_ns, 0, __ATOMIC_RELAXED);
        out->destroy_join_post_ready = __atomic_exchange_n(
            &thread_destroy_join_post_ready, 0, __ATOMIC_RELAXED);
        out->destroy_join_post_ready_ns = __atomic_exchange_n(
            &thread_destroy_join_post_ready_ns, 0, __ATOMIC_RELAXED);
        out->destroy_post_vector_ns = __atomic_exchange_n(
            &thread_destroy_post_vector_ns, 0, __ATOMIC_RELAXED);
        out->destroy_post_cpu_ns = __atomic_exchange_n(
            &thread_destroy_post_cpu_ns, 0, __ATOMIC_RELAXED);
        out->destroy_post_name_ns = __atomic_exchange_n(
            &thread_destroy_post_name_ns, 0, __ATOMIC_RELAXED);
        out->destroy_post_notify_ns = __atomic_exchange_n(
            &thread_destroy_post_notify_ns, 0, __ATOMIC_RELAXED);
        out->calls =
            __atomic_exchange_n(&thread_destroy_calls, 0, __ATOMIC_ACQUIRE);
        if (reset == 1)
            __atomic_store_n(&thread_destroy_stats_enabled, 1,
                             __ATOMIC_RELEASE);
    } else {
        out->calls = __atomic_load_n(&thread_destroy_calls, __ATOMIC_ACQUIRE);
        out->rcu_wait_ns =
            __atomic_load_n(&thread_destroy_rcu_wait_ns, __ATOMIC_RELAXED);
        out->rcu_enroll_ns =
            __atomic_load_n(&thread_destroy_rcu_enroll_ns, __ATOMIC_RELAXED);
        out->rcu_scan_ns =
            __atomic_load_n(&thread_destroy_rcu_scan_ns, __ATOMIC_RELAXED);
        out->rcu_post_scan_ns =
            __atomic_load_n(&thread_destroy_rcu_post_scan_ns, __ATOMIC_RELAXED);
        out->pin_wait_ns =
            __atomic_load_n(&thread_destroy_pin_wait_ns, __ATOMIC_RELAXED);
        out->active_scan_ns =
            __atomic_load_n(&thread_destroy_active_scan_ns, __ATOMIC_RELAXED);
        out->object_free_ns =
            __atomic_load_n(&thread_destroy_object_free_ns, __ATOMIC_RELAXED);
        out->object_free_heap =
            __atomic_load_n(&thread_destroy_object_free_heap, __ATOMIC_RELAXED);
        out->object_free_cache = __atomic_load_n(
            &thread_destroy_object_free_cache, __ATOMIC_RELAXED);
        out->object_free_heap_ns = __atomic_load_n(
            &thread_destroy_object_free_heap_ns, __ATOMIC_RELAXED);
        out->object_free_cache_ns = __atomic_load_n(
            &thread_destroy_object_free_cache_ns, __ATOMIC_RELAXED);
        out->object_free_cache_clear_ns = __atomic_load_n(
            &thread_destroy_object_free_cache_clear_ns, __ATOMIC_RELAXED);
        out->object_free_cache_link_ns = __atomic_load_n(
            &thread_destroy_object_free_cache_link_ns, __ATOMIC_RELAXED);
        out->pin_waited =
            __atomic_load_n(&thread_destroy_pin_waited, __ATOMIC_RELAXED);
        out->pin_skipped =
            __atomic_load_n(&thread_destroy_pin_skipped, __ATOMIC_RELAXED);
        out->rcu_waited =
            __atomic_load_n(&thread_destroy_rcu_waited, __ATOMIC_RELAXED);
        out->rcu_skipped =
            __atomic_load_n(&thread_destroy_rcu_skipped, __ATOMIC_RELAXED);
        out->rcu_waited_ns =
            __atomic_load_n(&thread_destroy_rcu_waited_ns, __ATOMIC_RELAXED);
        out->rcu_skipped_ns =
            __atomic_load_n(&thread_destroy_rcu_skipped_ns, __ATOMIC_RELAXED);
        out->rcu_busy_peer_sum = __atomic_load_n(
            &thread_destroy_rcu_busy_peer_sum, __ATOMIC_RELAXED);
        out->rcu_busy_peer_max = __atomic_load_n(
            &thread_destroy_rcu_busy_peer_max, __ATOMIC_RELAXED);
        out->rcu_busy_thread =
            __atomic_load_n(&thread_destroy_rcu_busy_thread, __ATOMIC_RELAXED);
        out->rcu_busy_msg =
            __atomic_load_n(&thread_destroy_rcu_busy_msg, __ATOMIC_RELAXED);
        out->rcu_busy_sys =
            __atomic_load_n(&thread_destroy_rcu_busy_sys, __ATOMIC_RELAXED);
        out->rcu_busy_channel =
            __atomic_load_n(&thread_destroy_rcu_busy_channel, __ATOMIC_RELAXED);
        out->rcu_busy_connect =
            __atomic_load_n(&thread_destroy_rcu_busy_connect, __ATOMIC_RELAXED);
        out->rcu_busy_interrupt = __atomic_load_n(
            &thread_destroy_rcu_busy_interrupt, __ATOMIC_RELAXED);
        out->rcu_busy_sync =
            __atomic_load_n(&thread_destroy_rcu_busy_sync, __ATOMIC_RELAXED);
        out->rcu_busy_sched =
            __atomic_load_n(&thread_destroy_rcu_busy_sched, __ATOMIC_RELAXED);
        out->rcu_busy_time =
            __atomic_load_n(&thread_destroy_rcu_busy_time, __ATOMIC_RELAXED);
        out->rcu_busy_idle =
            __atomic_load_n(&thread_destroy_rcu_busy_idle, __ATOMIC_RELAXED);
        out->rcu_busy_dispatch = __atomic_load_n(
            &thread_destroy_rcu_busy_dispatch, __ATOMIC_RELAXED);
        out->rcu_busy_timer =
            __atomic_load_n(&thread_destroy_rcu_busy_timer, __ATOMIC_RELAXED);
        out->rcu_busy_fault =
            __atomic_load_n(&thread_destroy_rcu_busy_fault, __ATOMIC_RELAXED);
        out->rcu_busy_priv =
            __atomic_load_n(&thread_destroy_rcu_busy_priv, __ATOMIC_RELAXED);
        out->rcu_busy_heartbeat = __atomic_load_n(
            &thread_destroy_rcu_busy_heartbeat, __ATOMIC_RELAXED);
        out->rcu_busy_priv_system = __atomic_load_n(
            &thread_destroy_rcu_busy_priv_system, __ATOMIC_RELAXED);
        out->rcu_busy_priv_vm =
            __atomic_load_n(&thread_destroy_rcu_busy_priv_vm, __ATOMIC_RELAXED);
        out->rcu_busy_priv_ipc = __atomic_load_n(
            &thread_destroy_rcu_busy_priv_ipc, __ATOMIC_RELAXED);
        out->rcu_busy_priv_process = __atomic_load_n(
            &thread_destroy_rcu_busy_priv_process, __ATOMIC_RELAXED);
        out->rcu_busy_priv_thread = __atomic_load_n(
            &thread_destroy_rcu_busy_priv_thread, __ATOMIC_RELAXED);
        out->rcu_busy_priv_pa =
            __atomic_load_n(&thread_destroy_rcu_busy_priv_pa, __ATOMIC_RELAXED);
        out->rcu_busy_priv_pa_alloc = __atomic_load_n(
            &thread_destroy_rcu_busy_priv_pa_alloc, __ATOMIC_RELAXED);
        out->rcu_busy_priv_pa_free = __atomic_load_n(
            &thread_destroy_rcu_busy_priv_pa_free, __ATOMIC_RELAXED);
        out->rcu_busy_priv_pa_zero = __atomic_load_n(
            &thread_destroy_rcu_busy_priv_pa_zero, __ATOMIC_RELAXED);
        out->rcu_busy_priv_pa_info = __atomic_load_n(
            &thread_destroy_rcu_busy_priv_pa_info, __ATOMIC_RELAXED);
        out->rcu_busy_priv_pa_xlat = __atomic_load_n(
            &thread_destroy_rcu_busy_priv_pa_xlat, __ATOMIC_RELAXED);
        out->rcu_busy_priv_pa_pq = __atomic_load_n(
            &thread_destroy_rcu_busy_priv_pa_pq, __ATOMIC_RELAXED);
        out->rcu_busy_unstamped = __atomic_load_n(
            &thread_destroy_rcu_busy_unstamped, __ATOMIC_RELAXED);
        out->rcu_busy_other =
            __atomic_load_n(&thread_destroy_rcu_busy_other, __ATOMIC_RELAXED);
        out->destroy_claim_active =
            __atomic_load_n(&thread_destroy_claim_active, __ATOMIC_RELAXED);
        out->destroy_claim_reserved =
            __atomic_load_n(&thread_destroy_claim_reserved, __ATOMIC_RELAXED);
        out->destroy_join_wake =
            __atomic_load_n(&thread_destroy_join_wake, __ATOMIC_RELAXED);
        out->destroy_join_wake_ns =
            __atomic_load_n(&thread_destroy_join_wake_ns, __ATOMIC_RELAXED);
        out->destroy_join_post_ready =
            __atomic_load_n(&thread_destroy_join_post_ready, __ATOMIC_RELAXED);
        out->destroy_join_post_ready_ns = __atomic_load_n(
            &thread_destroy_join_post_ready_ns, __ATOMIC_RELAXED);
        out->destroy_post_vector_ns =
            __atomic_load_n(&thread_destroy_post_vector_ns, __ATOMIC_RELAXED);
        out->destroy_post_cpu_ns =
            __atomic_load_n(&thread_destroy_post_cpu_ns, __ATOMIC_RELAXED);
        out->destroy_post_name_ns =
            __atomic_load_n(&thread_destroy_post_name_ns, __ATOMIC_RELAXED);
        out->destroy_post_notify_ns =
            __atomic_load_n(&thread_destroy_post_notify_ns, __ATOMIC_RELAXED);
    }
    out->reset = 0;
    out->reserved = 0;
}

static void thread_destroy_stats_add(uint64_t rcu_wait_ns,
                                     uint64_t pin_wait_ns,
                                     uint64_t active_scan_ns,
                                     uint64_t object_free_ns,
                                     uint64_t object_free_cache_clear_ns,
                                     uint64_t object_free_cache_link_ns,
                                     bool pin_waited,
                                     bool object_free_heap,
                                     bool destroy_claim_reserved,
                                     bool join_woke,
                                     uint64_t join_wake_ns,
                                     uint64_t join_post_ready_ns,
                                     uint64_t post_vector_ns,
                                     uint64_t post_cpu_ns,
                                     uint64_t post_name_ns,
                                     uint64_t post_notify_ns,
                                     const struct rcu_sched_busy *busy)
{
    unsigned rcu_busy_peers = busy->peers;
    bool rcu_waited = rcu_busy_peers != 0;
    unsigned long old_max;

    if (!__atomic_load_n(&thread_destroy_stats_enabled, __ATOMIC_ACQUIRE))
        return;
    __atomic_fetch_add(&thread_destroy_rcu_wait_ns, (unsigned long) rcu_wait_ns,
                       __ATOMIC_RELAXED);
    __atomic_fetch_add(
        &thread_destroy_rcu_enroll_ns,
        (unsigned long) thread_destroy_cycles_to_ns(busy->enroll_cycles),
        __ATOMIC_RELAXED);
    __atomic_fetch_add(
        &thread_destroy_rcu_scan_ns,
        (unsigned long) thread_destroy_cycles_to_ns(busy->scan_cycles),
        __ATOMIC_RELAXED);
    __atomic_fetch_add(
        &thread_destroy_rcu_post_scan_ns,
        (unsigned long) thread_destroy_cycles_to_ns(busy->post_scan_cycles),
        __ATOMIC_RELAXED);
    __atomic_fetch_add(&thread_destroy_pin_wait_ns, (unsigned long) pin_wait_ns,
                       __ATOMIC_RELAXED);
    __atomic_fetch_add(&thread_destroy_active_scan_ns,
                       (unsigned long) active_scan_ns, __ATOMIC_RELAXED);
    __atomic_fetch_add(&thread_destroy_object_free_ns,
                       (unsigned long) object_free_ns, __ATOMIC_RELAXED);
    __atomic_fetch_add(object_free_heap ? &thread_destroy_object_free_heap_ns
                                        : &thread_destroy_object_free_cache_ns,
                       (unsigned long) object_free_ns, __ATOMIC_RELAXED);
    __atomic_fetch_add(&thread_destroy_object_free_cache_clear_ns,
                       (unsigned long) object_free_cache_clear_ns,
                       __ATOMIC_RELAXED);
    __atomic_fetch_add(&thread_destroy_object_free_cache_link_ns,
                       (unsigned long) object_free_cache_link_ns,
                       __ATOMIC_RELAXED);
    __atomic_fetch_add(object_free_heap ? &thread_destroy_object_free_heap
                                        : &thread_destroy_object_free_cache,
                       1, __ATOMIC_RELAXED);
    __atomic_fetch_add(
        pin_waited ? &thread_destroy_pin_waited : &thread_destroy_pin_skipped,
        1, __ATOMIC_RELAXED);
    __atomic_fetch_add(
        rcu_waited ? &thread_destroy_rcu_waited : &thread_destroy_rcu_skipped,
        1, __ATOMIC_RELAXED);
    __atomic_fetch_add(rcu_waited ? &thread_destroy_rcu_waited_ns
                                  : &thread_destroy_rcu_skipped_ns,
                       (unsigned long) rcu_wait_ns, __ATOMIC_RELAXED);
    __atomic_fetch_add(&thread_destroy_rcu_busy_peer_sum, rcu_busy_peers,
                       __ATOMIC_RELAXED);
    __atomic_fetch_add(&thread_destroy_rcu_busy_thread, busy->thread,
                       __ATOMIC_RELAXED);
    __atomic_fetch_add(&thread_destroy_rcu_busy_msg, busy->msg,
                       __ATOMIC_RELAXED);
    __atomic_fetch_add(&thread_destroy_rcu_busy_sys, busy->sys,
                       __ATOMIC_RELAXED);
    __atomic_fetch_add(&thread_destroy_rcu_busy_channel, busy->channel,
                       __ATOMIC_RELAXED);
    __atomic_fetch_add(&thread_destroy_rcu_busy_connect, busy->connect,
                       __ATOMIC_RELAXED);
    __atomic_fetch_add(&thread_destroy_rcu_busy_interrupt, busy->interrupt,
                       __ATOMIC_RELAXED);
    __atomic_fetch_add(&thread_destroy_rcu_busy_sync, busy->sync,
                       __ATOMIC_RELAXED);
    __atomic_fetch_add(&thread_destroy_rcu_busy_sched, busy->sched,
                       __ATOMIC_RELAXED);
    __atomic_fetch_add(&thread_destroy_rcu_busy_time, busy->time,
                       __ATOMIC_RELAXED);
    __atomic_fetch_add(&thread_destroy_rcu_busy_idle, busy->idle,
                       __ATOMIC_RELAXED);
    __atomic_fetch_add(&thread_destroy_rcu_busy_dispatch, busy->dispatch,
                       __ATOMIC_RELAXED);
    __atomic_fetch_add(&thread_destroy_rcu_busy_timer, busy->timer,
                       __ATOMIC_RELAXED);
    __atomic_fetch_add(&thread_destroy_rcu_busy_fault, busy->fault,
                       __ATOMIC_RELAXED);
    __atomic_fetch_add(&thread_destroy_rcu_busy_priv, busy->priv,
                       __ATOMIC_RELAXED);
    __atomic_fetch_add(&thread_destroy_rcu_busy_heartbeat, busy->heartbeat,
                       __ATOMIC_RELAXED);
    __atomic_fetch_add(&thread_destroy_rcu_busy_priv_system, busy->priv_system,
                       __ATOMIC_RELAXED);
    __atomic_fetch_add(&thread_destroy_rcu_busy_priv_vm, busy->priv_vm,
                       __ATOMIC_RELAXED);
    __atomic_fetch_add(&thread_destroy_rcu_busy_priv_ipc, busy->priv_ipc,
                       __ATOMIC_RELAXED);
    __atomic_fetch_add(&thread_destroy_rcu_busy_priv_process,
                       busy->priv_process, __ATOMIC_RELAXED);
    __atomic_fetch_add(&thread_destroy_rcu_busy_priv_thread, busy->priv_thread,
                       __ATOMIC_RELAXED);
    __atomic_fetch_add(&thread_destroy_rcu_busy_priv_pa, busy->priv_pa,
                       __ATOMIC_RELAXED);
    __atomic_fetch_add(&thread_destroy_rcu_busy_priv_pa_alloc,
                       busy->priv_pa_alloc, __ATOMIC_RELAXED);
    __atomic_fetch_add(&thread_destroy_rcu_busy_priv_pa_free,
                       busy->priv_pa_free, __ATOMIC_RELAXED);
    __atomic_fetch_add(&thread_destroy_rcu_busy_priv_pa_zero,
                       busy->priv_pa_zero, __ATOMIC_RELAXED);
    __atomic_fetch_add(&thread_destroy_rcu_busy_priv_pa_info,
                       busy->priv_pa_info, __ATOMIC_RELAXED);
    __atomic_fetch_add(&thread_destroy_rcu_busy_priv_pa_xlat,
                       busy->priv_pa_xlat, __ATOMIC_RELAXED);
    __atomic_fetch_add(&thread_destroy_rcu_busy_priv_pa_pq, busy->priv_pa_pq,
                       __ATOMIC_RELAXED);
    __atomic_fetch_add(&thread_destroy_rcu_busy_unstamped, busy->unstamped,
                       __ATOMIC_RELAXED);
    __atomic_fetch_add(&thread_destroy_rcu_busy_other, busy->other,
                       __ATOMIC_RELAXED);
    __atomic_fetch_add(destroy_claim_reserved ? &thread_destroy_claim_reserved
                                              : &thread_destroy_claim_active,
                       1, __ATOMIC_RELAXED);
    if (join_woke) {
        __atomic_fetch_add(&thread_destroy_join_wake, 1, __ATOMIC_RELAXED);
        __atomic_fetch_add(&thread_destroy_join_wake_ns, join_wake_ns,
                           __ATOMIC_RELAXED);
        __atomic_fetch_add(&thread_destroy_join_post_ready, 1,
                           __ATOMIC_RELAXED);
        __atomic_fetch_add(&thread_destroy_join_post_ready_ns,
                           join_post_ready_ns, __ATOMIC_RELAXED);
        __atomic_fetch_add(&thread_destroy_post_vector_ns, post_vector_ns,
                           __ATOMIC_RELAXED);
        __atomic_fetch_add(&thread_destroy_post_cpu_ns, post_cpu_ns,
                           __ATOMIC_RELAXED);
        __atomic_fetch_add(&thread_destroy_post_name_ns, post_name_ns,
                           __ATOMIC_RELAXED);
        __atomic_fetch_add(&thread_destroy_post_notify_ns, post_notify_ns,
                           __ATOMIC_RELAXED);
    }
    old_max =
        __atomic_load_n(&thread_destroy_rcu_busy_peer_max, __ATOMIC_RELAXED);
    while (old_max < rcu_busy_peers &&
           !__atomic_compare_exchange_n(&thread_destroy_rcu_busy_peer_max,
                                        &old_max, rcu_busy_peers, 0,
                                        __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
    }
    __atomic_fetch_add(&thread_destroy_calls, 1, __ATOMIC_RELEASE);
}

static uint64_t thread_destroy_cycles_to_ns(uint64_t cycles_delta)
{
    uint64_t freq = qtimeptr->cycles_per_sec;

    if (!freq)
        return 0;
    return (cycles_delta * 1000000000ULL) / freq;
}

static void thread_destroy_wait_inactive(thread_t *thp, process_t *prp)
{
    extern unsigned hart_ids[];
    unsigned self = get_cpunum();
    unsigned spins = 0;

    for (;;) {
        unsigned hi;
        int active_hart = -1;
        unsigned depth = 0;

        for (hi = 0; hi < num_processors; hi++) {
            unsigned hart = hart_ids[hi];
            thread_t *cur;

            if (hart == self)
                continue;
            cur = __atomic_load_n(&pcpu_array[hart].pc_curthread,
                                  __ATOMIC_ACQUIRE);
            if (cur == thp) {
                active_hart = (int) hart;
                depth = __atomic_load_n(&in_kcall[hart], __ATOMIC_ACQUIRE);
                break;
            }
        }
        if (active_hart < 0)
            return;
        if (++spins > 1000000) {
            kprintf(
                "FATAL: thread_destroy thp=%p tid=%d pid=%d state=%d "
                "runcpu=%u stuck active on hart %d (in_kcall=%u, %s)\n",
                (void *) thp, thp->tid, prp ? prp->pid : -1, thp->state,
                thp->runcpu, active_hart, depth,
                depth ? "in-kernel" : "U-mode");
            crash();
        }
        SENDIPI((unsigned) active_hart, IPI_RESCHED);
        thread_cpu_relax();
    }
}

/* Revoke every capability naming this dying thread before its soul recycles,
 * mirroring the process and connection soul-free paths (see
 * cap_revoke_object_all_tables). The caller holds no spinlock here; the scan
 * itself takes process_vector_slock and each cap-table lock, and it does not
 * dereference thp, so the revoke is safe to run before object_free regardless
 * of thp's run state. It finds nothing until a THREAD cap is first minted; it
 * exists so that mint can never open a use-after-free window. The
 * rcu_sched_synchronize already on the reap paths dwarfs this bounded scan.
 *
 * Revoke marks a pinned cap slot REVOKING but leaves slot->object live until
 * its last unpin, so this fan-out alone does not protect a cap consumer that is
 * mid-dereference. The matching invariant for any future THREAD-cap lookup: it
 * must promote the resolved thread_t to a real object pin (tthread_pin) before
 * dropping its cap pin, so a concurrent reap here cannot recycle a thread_t the
 * consumer still holds. No THREAD cap is minted today, so nothing relies on it
 * yet, but a minter must land that promotion together with its lookup.
 */
static void thread_soul_free(process_t *prp, thread_t *thp)
{
    cap_revoke_object_all_tables(thp);
    object_free(prp, &thread_souls, thp);
}

static int thread_active_hart(thread_t *thp)
{
    extern unsigned hart_ids[];

    for (unsigned hi = 0; hi < num_processors; hi++) {
        unsigned hart = hart_ids[hi];
        thread_t *cur;

        cur = __atomic_load_n(&pcpu_array[hart].pc_curthread, __ATOMIC_ACQUIRE);
        if (cur == thp)
            return (int) hart;
    }
    return -1;
}

static uint8_t thread_state_load_acquire(thread_t *thp)
{
    return __atomic_load_n(&thp->state, __ATOMIC_ACQUIRE);
}

/* roottask_stack_alloc — pop a pre-cached user-VA stack for the new roottask
 * pool thread.
 *
 * The cache is populated by roottask itself in message_start() via the
 * __KSYS_PRIV_REGISTER_POOL_STACK kernel extension (one push per as_mmap'd
 * stack page). This is a kernel-internal function that runs from thread_specret
 * on the WAAA path of pid=1 threads only; non-roottask threads take the
 * SIGEV_NOTIFICATION path to roottask's proc_stack handler instead.
 *
 * If the cache is empty or the size doesn't match, fail with EAGAIN — the
 * previous fallback to vm_ops->mmap landed on the kernel heap (HEAP_KER), which
 * after the PTE_U=0 revert can no longer be used as a U-mode thread stack.
 */
static int roottask_stack_alloc(thread_t *thp)
{
    /* Pop from the free list under roottask_stack_slock. Without the lock, two
     * concurrent allocs on different CPUs could both read the same head and
     * both pop it, double-handing one stack to two threads. See core/globals.h
     * for the lock declaration.
     */
    kspinlock_lock(&roottask_stack_slock);
    if (roottask_stack_cache && thp->un.lcl.stacksize == roottask_stack_size) {
        thp->un.lcl.stackaddr = roottask_stack_cache;
        roottask_stack_cache = *roottask_stack_cache;
        kspinlock_unlock(&roottask_stack_slock);
        return EOK;
    }
    kspinlock_unlock(&roottask_stack_slock);
    return EAGAIN;
}


/* Return a roottask pool thread's stack to the cache.
 *
 * Pushes the stack page onto the free list rather than unmapping it (a
 * self-destroying thread is still running on it), recording the stack size on
 * first use. Parameters:
 *   thp : thread whose stack is being released
 */
static void roottask_stack_free(thread_t *thp)
{
    void **stk;

    if (!thp->process || thp->process->pid != ROOTTASK_PID)
        return;

    stk = thp->un.lcl.stackaddr;

    /* Guard against NULL stackaddr. A pool thread's thread_t.un is a union —
     * its "lcl.stackaddr" slot is reused for IPC-receive bookkeeping (un.ms,
     * un.rcv) once the thread is past startup. If the thread is destroyed while
     * blocked mid-receive, stackaddr can read back as NULL even though
     * ALLOCED_STACK is set. Skipping the cache push for a NULL stack is correct
     * — there is nothing to free into the cache; we just lose a slot.
     */
    if (!stk)
        return;

    /* The probe checks the page is mapped+writable, but an unaligned stk would
     * still trap on the *stk store below (misaligned S-mode store), faulting
     * inside the kernel. A roottask thread can supply any stackaddr via
     * pthread_attr_setstack, so reject what we cannot store to.
     */
    if (((uintptr_t) stk & (sizeof(*stk) - 1)) != 0)
        return;
    if (!WR_PROBE_PTR(thp, stk, sizeof(*stk)))
        return;

    /* Always cache roottask stacks on the free list — never munmap them.
     *
     * In the pinned-thread model (no kernel_exit), a self-destroying thread
     * (tid=0) is still running on this very stack when thread_destroy() calls
     * us. If we mm_munmap() it, _sfree() → _sreallocfunc() will memset(0x6d)
     * the block (debug poison), corrupting our own live stack frames and
     * crashing.
     *
     * Caching is always safe: the memory stays mapped and can be reused. The
     * "leak" is bounded by max_threads * stack_size (< 100 KB).
     *
     * Also: thread_create() allocates stacks via _scalloc(), not
     * roottask_stack_alloc(), so roottask_stack_size may never be set. We set
     * it here on first free so future allocs can reuse cached stacks. Push onto
     * the free list under roottask_stack_slock. Without the lock, a concurrent
     * push on another CPU can orphan a stack (both readers see the same head,
     * both writes overwrite each other).
     */
    kspinlock_lock(&roottask_stack_slock);
    if (roottask_stack_size == 0)
        roottask_stack_size = thp->un.lcl.stacksize;

    *stk = roottask_stack_cache;
    roottask_stack_cache = stk;
    kspinlock_unlock(&roottask_stack_slock);
}



/* Kext entry that purges the cached roottask stacks. Parameters:
 *   p : the purge amount, passed through to roottask_stack_purge
 */
void kext_stack_purge(void *p)
{
    int r;

    CRASHCHECK(get_inkernel() & INKERNEL_INTRMASK);
    r = roottask_stack_purge((size_t) p);
    SETKSTATUS(get_active_thread(get_cpunum()), r);
}


/* Reclaim cached roottask stacks under memory pressure.
 *
 * From user context, forwards to roottask via __KSYS_PRIV_STACK_PURGE; in
 * kernel context, detaches the whole free list under the slock then unmaps each
 * stack with the lock dropped. Parameters:
 *   amount : caller-requested purge amount (currently the whole list)
 * Return value: non-zero if anything was reclaimed, 0 otherwise.
 */
static int roottask_stack_purge(size_t amount)
{
    void **stk;

    if (!in_kernel_context()) {
        return (int) syscall_priv(__KSYS_PRIV_STACK_PURGE, (void *) amount);
    }

    if (!roottask_stack_cache)
        return 0;

    /* The drain loop must be atomic with respect to push/pop on other CPUs.
     * Detach the entire list under the slock (single store), then walk and
     * munmap unlocked — munmap can take other heap locks and we must not hold
     * roottask_stack_slock across vm_ops->munmap (lock-rank inversion risk).
     */
    kspinlock_lock(&roottask_stack_slock);
    stk = roottask_stack_cache;
    roottask_stack_cache = NULL;
    kspinlock_unlock(&roottask_stack_slock);

    while (stk) {
        void **next = *stk;
        (void) vm_ops->munmap(0, NULL, (uintptr_t) stk, roottask_stack_size, 0);
        stk = next;
    }
    return 1;
}


/* Promote a thread_t pointer that is already protected by tthread_rcu (or by a
 * stronger subsystem lock) into an explicit lifetime pin. The pin does not
 * discover objects on its own; callers must first obtain thp through a valid
 * lookup/protection path.
 */
int rdecl tthread_pin(thread_t *thp)
{
    unsigned count;

    if (!thp || TYPE_MASK(thp->type) != TYPE_THREAD) {
        return ESRCH;
    }

    count = __atomic_load_n(&thp->pin_count, __ATOMIC_RELAXED);
    for (;;) {
        if (count == UINT_MAX) {
            return EAGAIN;
        }
        if (__atomic_compare_exchange_n(&thp->pin_count, &count, count + 1, 0,
                                        __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
            return EOK;
        }
    }
}


void rdecl tthread_unpin(thread_t *thp)
{
    unsigned old;

    old = __atomic_fetch_sub(&thp->pin_count, 1, __ATOMIC_RELEASE);
    if (old == 0) {
        crash();
    }
}


static bool tthread_wait_unpinned(thread_t *thp)
{
    bool waited = false;

    while (__atomic_load_n(&thp->pin_count, __ATOMIC_ACQUIRE) != 0) {
        waited = true;
        __asm__ __volatile__("" ::: "memory");
    }
    return waited;
}

int rdecl thread_set_as_prp(thread_t *thp, process_t *prp)
{
    process_t *old;
    int status;

    if (!thp)
        return ESRCH;
    kspinlock_lock(&thread_as_slock);
    if (thp->as_prp == prp)
        goto out;
    if (prp && prp != thp->process) {
        status = process_pin(prp);
        if (status != EOK) {
            kspinlock_unlock(&thread_as_slock);
            return status;
        }
    }
    old = thp->as_prp;
    thp->as_prp = prp;
    if (old && old != thp->process)
        process_unpin(old);
out:
    kspinlock_unlock(&thread_as_slock);
    return EOK;
}

void rdecl thread_clear_as_prp_if(thread_t *thp, process_t *prp)
{
    if (!thp)
        return;

    kspinlock_lock(&thread_as_slock);
    if (thp->as_prp == prp) {
        thp->as_prp = NULL;
        if (prp && prp != thp->process)
            process_unpin(prp);
    }
    kspinlock_unlock(&thread_as_slock);
}


/* Initialize the thread subsystem at boot. */
void rdecl thread_init(void)
{
    kspinlock_init(&thread_as_slock, LOCK_CLASS_THREAD, "thread_as_slock");

    /* Do not register roottask_stack_purge as a global memory-pressure purger:
     * the cache stores roottask user-VA stack pages, and a purger can run while
     * some other process address space is active. Walking the free-list link
     * through that user VA can fault in S-mode.
     *
     * Keeping the cache is bounded by max_threads * stack_size (< 100 KiB) and
     * avoids unmapping a self-destroying roottask pool thread's live stack.
     */
}

int rdecl kernel_thread_create(void (*entry)(void *),
                               void *arg,
                               unsigned priority,
                               unsigned runmask,
                               thread_t **out)
{
    thread_t *thp;
    uintptr_t stack_top;
    int tid;

    if (!entry || !kernel_threads_prp || priority == 0 || priority >= NUM_PRI)
        return EINVAL;

    thp = object_alloc(NULL, &thread_souls);
    if (!thp)
        return EAGAIN;
    thread_recycle_reset(thp);

    thp->process = kernel_threads_prp;
    thp->as_prp = NULL;
    thp->type = TYPE_THREAD;
    thp->state = STATE_DEAD;
    thp->policy = SCHED_RR;
    thp->priority = thp->real_priority = (uint8_t) priority;
    thp->dpp = thp->orig_dpp = kernel_threads_prp->default_dpp;
    thp->runmask = thp->default_runmask = runmask;
    thp->last_chid = -1;
    thp->flags = RTMUX_FLG_THR_NOMULTISIG | RTMUX_FLG_THR_DETACHED;
    thp->start_time = kernel_threads_prp->start_time;

    thp->un.lcl.stacksize = KSYS_STACK_NBYTES;
    thp->un.lcl.stackaddr = _scalloc(thp->un.lcl.stacksize);
    thp->un.lcl.tls = _scalloc(sizeof(*thp->un.lcl.tls));
    if (!thp->un.lcl.stackaddr || !thp->un.lcl.tls) {
        if (thp->un.lcl.stackaddr)
            _sfree(thp->un.lcl.stackaddr, thp->un.lcl.stacksize);
        if (thp->un.lcl.tls)
            _sfree(thp->un.lcl.tls, sizeof(*thp->un.lcl.tls));
        object_free(NULL, &thread_souls, thp);
        return EAGAIN;
    }

    thp->un.lcl.tls->__errptr = &thp->un.lcl.tls->__errval;
    thp->un.lcl.tls->__flags = PTHREAD_CANCEL_DISABLE;
    thp->un.lcl.tls->__pid = kernel_threads_prp->pid;

    kspinlock_lock(&kernel_threads_prp->vec_slock);
    tid = vector_add(&kernel_threads_prp->threads, thp, 0);
    kspinlock_unlock(&kernel_threads_prp->vec_slock);
    if (tid == -1) {
        _sfree(thp->un.lcl.stackaddr, thp->un.lcl.stacksize);
        _sfree(thp->un.lcl.tls, sizeof(*thp->un.lcl.tls));
        object_free(NULL, &thread_souls, thp);
        return EAGAIN;
    }
    thp->tid = (uint32_t) tid;
    __atomic_add_fetch(&kernel_threads_prp->num_active_threads, 1,
                       __ATOMIC_ACQ_REL);
    thp->un.lcl.tls->__tid = tid + 1;
    thp->un.lcl.tls->__owner =
        (unsigned) ((thp->un.lcl.tls->__pid << 16) | thp->un.lcl.tls->__tid) &
        ~RTMUX_SYNC_WAITING;

    cpu_thread_init(NULL, thp, kopt.align_fault);
    stack_top =
        STACK_INIT((uintptr_t) thp->un.lcl.stackaddr, thp->un.lcl.stacksize);
    SETKSP(thp, stack_top);
    SETKIP_FUNC(thp, entry);
    CPU_REG_ARG(&thp->reg, 0) = (uintptr_t) arg;
    CPU_REG_RA(&thp->reg) = (uintptr_t) kernel_thread_exit_fallback;

    ready(thp);
    if (out)
        *out = thp;
    return EOK;
}

/* Create a new thread.
 *
 * Allocates and initializes a thread_t (address space, priority, scheduler
 * partition, runmask, stack flag), adds it to the process thread vector, and
 * arms the WAAA special-return path so thread_specret() can validate the
 * attributes (a kernel copy, or a user pointer for SIGEV_THREAD) once it can
 * fault. Optionally blocks the parent until the create is confirmed.
 * Parameters:
 *   act                 : the creating (active) thread
 *   prp                 : process the new thread belongs to
 *   evp                 : sigevent carrying the entry point and attributes
 *   kattr               : kernel copy of the caller's attributes, validated in
 *                         the caller's context; NULL to read evp's user pointer
 *                         in thread_specret (SIGEV_THREAD only: the pointer is
 *                         valid in prp, never use it cross-process)
 *   thread_create_flags : THREAD_CREATE_*_FLAG bits (block, critical event)
 * Return value: ENOERROR on success, or an errno on failure.
 */
int rdecl thread_create(thread_t *act,
                        process_t *prp,
                        const struct sigevent *evp,
                        unsigned thread_create_flags,
                        const struct _thread_attr *kattr)
{
    /* thread_create_flags are an or-ing of macros.h : THREAD_CREATE_*_FLAG */
    thread_t *thp;
    int tid;
    int align;

    /* Allocate a thread object. */
    if (!(thp = object_alloc(prp, &thread_souls))) {
        return EAGAIN;
    }
    thread_recycle_reset(thp);
    thp->process = prp;
    thp->type = TYPE_THREAD;
    thp->last_chid = -1;
    thp->syscall = -1;
    thp->client = 0;
    thp->blocked_on_turnstile = NULL;
    thp->owned_turnstiles = NULL;
    thp->held_ceiling_n = 0;
    thp->held_ceiling_floor = 0;
    thp->held_ceiling_saturated = false;
    thp->stall_reported_ts = 0;
    thp->destroy_claim = 0; /* see banner in objects.h */
    thp->pin_count = 0;
    snap_time(&thp->start_time, 1);
    /* no need to init thp->timestamp_since_block since we start READY */

    /* Determine the new thread's address space. Three cases:
     *   (a) sibling thread (act->process == prp): inherit caller's
     *       as.  Common case for pthread_create within a process.
     *   (b) cross-process create into a non-LOADING target: target
     *       has its own ELF image fully mapped at its low VA; new
     *       thread runs in target's pgdir.  (Rare in practice;
     *       mostly only the special cases below trigger cross-
     *       process creates.)
     *   (c) cross-process create into a PROC_LOADING target: this is the
     *       roottask spawn-loader path.  The loader function lives in
     *       ROOTTASK's text at low VA; the child's pgdir doesn't map
     *       roottask text there yet, so run the loader thread in CALLER's
     *       pgdir (roottask) until kext_process_startup promotes it to the
     *       child's first user thread and switches satp to the child's pgdir.
     */
    int loader_path = (prp != act->process) &&
                      (prp->flags & RTMUX_FLG_PROC_LOADING) && (act->as_prp);
    if (loader_path) {
        /* The loader thread runs in the CHILD's pgdir from the start. The
         * child's pgdir is freshly created by mm_mcreate (kernel L2
         * + L2[1] syspage); install a view of ROOTTASK's image at L2[3] so the
         * loader can execute roottask text and reach roottask globals while
         * also touching the child's mmap'd PT_LOAD buffers — all in one pgdir,
         * no EFAULT on proc_read.
         *
         * kext_process_startup tears down the L2[3] view before the satp-switch
         * into the child's first U-mode thread, so the user binary runs fully
         * isolated from roottask.
         */
        extern int mm_roottask_view_install(process_t *);
        int rc = mm_roottask_view_install(prp);
        if (rc != EOK) {
            object_free(prp, &thread_souls, thp);
            return rc;
        }
        thp->as_prp = prp;
    } else {
        process_t *as_prp = (act->process == prp) ? act->as_prp : prp;

        if (!as_prp)
            as_prp = prp;
        int status = thread_set_as_prp(thp, as_prp);
        if (status != EOK) {
            object_free(prp, &thread_souls, thp);
            return status;
        }
    }

    /* Start the new thread at the current threads priority. Because we can
     * block active, we don't want priority inversion if the new thread is a
     * lower priority.
     */
    thp->priority = thp->real_priority = act->real_priority;
    thp->policy = act->policy;

    /* Inherit scheduler partition.
     *
     * If the creating thread(act) and child thread(thp) are in the same
     * process, the child inherits both the dynamic partition (->dpp) and the
     * home partition(->orig_dpp) from the creating thread.
     *
     * Otherwise, roottask is creating a thread for another process, so set both
     * dynamic and home partitions of the child to the default partition of the
     * parent's process.
     */
    if (prp == act->process) {
        thp->dpp = act->dpp;
        thp->orig_dpp = act->orig_dpp;
    } else {
        thp->dpp = prp->default_dpp;
        thp->orig_dpp = prp->default_dpp;
    }

    /* set child to start running critical if triggered by critical
     * sigev_thread. Child's critical state will last only up until it goes
     * received blocked.
     */
    thp->sched_flags = 0;
    if (thread_create_flags & THREAD_CREATE_CRITICAL_FLAG)
        CRITICAL_MARK_THREAD(thp);

    /* Inherit parent runmask */
    thp->runmask = thp->default_runmask = act->default_runmask;

    /* Add a new schedinfo.ss_info object for every thread we create, even if it
     * isn't needed (delete later). We are temporarily inheriting the priority,
     * so it only makes sense.
     */
    if (IS_SCHED_SS(thp)) {
        if (!(thp->schedinfo.ss_info = object_alloc(prp, &ssinfo_souls))) {
            (void) thread_set_as_prp(thp, NULL);
            object_free(prp, &thread_souls, thp);
            return EAGAIN;
        }
        memset(thp->schedinfo.ss_info, 0, sizeof(*thp->schedinfo.ss_info));
        /* Transfer the inherited attributes across, may be reset later */
        thp->schedinfo.ss_info->init_budget =
            act->schedinfo.ss_info->init_budget;
        thp->schedinfo.ss_info->repl_period =
            act->schedinfo.ss_info->repl_period;
        thp->schedinfo.ss_info->max_repl = act->schedinfo.ss_info->max_repl;
        thp->schedinfo.ss_info->low_priority =
            act->schedinfo.ss_info->low_priority;
        thp->schedinfo.ss_info->replenishment.thp = thp;

        /* Ensure that we have enough information set-up to allow us to run with
         * a valid sporadic configuration (ie non-zero execution budget, snapped
         * activation time) Leaving the other fields (repl_count, consumed) as 0
         * is fine
         */
        snap_time(&thp->schedinfo.ss_info->activation_time, 0);
        thp->schedinfo.ss_info->curr_budget =
            thp->schedinfo.ss_info->init_budget;
    } else {
        RR_RESET_TICK(thp);
    }

    /* Add the object to the process thread vector.
     *
     * vector_add/vector_rem mutate prp->threads' intrusive free list —
     * concurrent callers across CPUs corrupt it (check_vec catches this at
     * vector.c:35). Per-process vec_slock serializes this region. The same lock
     * is taken by the matching vector_rem in thread_destroy below, and by the
     * iteration walks in kext_process_shutdown / *_destroyall.
     */
    kspinlock_lock(&prp->vec_slock);
    tid = vector_add(&prp->threads, thp, 0);
    if (tid != -1 && !prp->valid_thp) {
        prp->valid_thp = thp;
    }
    kspinlock_unlock(&prp->vec_slock);
    if (tid == -1) {
        (void) thread_set_as_prp(thp, NULL);
        object_free(prp, &thread_souls, thp);
        return EAGAIN;
    }
    thp->tid = (uint32_t) tid; /* always >= 0 after vector_add succeeds */


    /* Give the vendor extension a chance to take first dibs */
    if (kcall_op_thread_create_hook) {
        int r = kcall_op_thread_create_hook(act, prp, evp, thread_create_flags,
                                            thp);
        if (r != EOK) {
            /* Unlike the rollback paths above, thp is already published in
             * prp->threads (vector_add above), so it is a live target: remove
             * the slot under vec_slock before freeing, or a later tid lookup
             * would resolve to the recycled soul. The thread never ran, so no
             * hart holds it active, but a concurrent reader may have looked it
             * up before the vector_rem: drain in-flight readers and wait out
             * any pin before thread_soul_free (revoke then free), the same
             * lifetime barrier the true reaps use minus the active-hart wait.
             */
            kspinlock_lock(&prp->vec_slock);
            (void) vector_rem(&prp->threads, (unsigned) tid);
            if (prp->valid_thp == thp)
                prp->valid_thp = NULL;
            kspinlock_unlock(&prp->vec_slock);
            (void) thread_set_as_prp(thp, NULL);
            rcu_sched_synchronize();
            (void) tthread_wait_unpinned(thp);
            thread_soul_free(prp, thp);
            return r;
        }
    }
    /* Figure out the default aligment */
    if (prp != act->process) {
        align = kopt.align_fault;
    } else if (act->flags & RTMUX_FLG_THR_ALIGN_FAULT) {
        align = +1;
    } else {
        align = -1;
    }
    cpu_thread_init(act, thp, align);

    /* U-mode entry decision. cpu_thread_init() defaults to S-mode (SPP=1);
     * clear SPP for user processes so sret drops to U-mode.
     *
     * The gate is PROC_LOADING, not PROC_RING0: roottask threads (which keep
     * PROC_RING0 for syscall_priv capability) still run in U-mode. A
     * still-loading process's threads stay S-mode unless they take the loader
     * path — that loader runs in the CALLER's pgdir (roottask, low VA, PTE_U=1)
     * so it MUST be U-mode, since an S-mode fetch from a PTE_U=1 page is
     * illegal regardless of SUM. kext_process_startup later re-applies
     * cpu_thread_enter_umode on the same thread with the child's entry-point
     * PC; subsequent pthread_create'd threads of a loaded process fall through
     * here too.
     */
    if (!(prp->flags & RTMUX_FLG_PROC_LOADING) || loader_path) {
        cpu_thread_enter_umode(thp);
    }

    atomic_or(&thp->flags, (act->flags & RTMUX_FLG_THR_IOPRIV));

    /* Stash some stuff for use in the WAAA special return code */
    thp->args.wa.not_sigev_thread =
        (thread_create_flags & THREAD_CREATE_BLOCK_FLAG) != 0;
    if (kattr) {
        thp->args.wa.kattr = *kattr;
        thp->args.wa.attr = &thp->args.wa.kattr;
    } else {
        thp->args.wa.attr = evp->sigev_notify_attributes;
    }
    thp->args.wa.real_attr = thp->args.wa.attr;
    SETKIP_FUNC(thp, evp->sigev_notify_function);
    thp->args.wa.arg = evp->sigev_value.sival_ptr;

    /* We cannot initalize the thread private data at this time because we may
     * not have addressability to it. We set a flag which will initalize it when
     * it runs for the first time (its birth cry).
     */
    atomic_or(&thp->flags, RTMUX_FLG_THR_WAAA | RTMUX_FLG_THR_DETACHED);

    if (prp->num_active_threads == 0) {
        /* get a default value for process/termination priority */
        prp->terming_priority = act->priority;
        prp->process_priority = 0; /* Set later on in the specret */
    }

    __atomic_add_fetch(&prp->num_active_threads, 1, __ATOMIC_ACQ_REL);

    /* Make thread lookups invalid */
    vector_flag(&prp->threads, thp->tid, 1);
    _TRACE_TH_EMIT_CREATE(thp);
    if (thread_create_flags & THREAD_CREATE_BLOCK_FLAG) {
        /* If the parent needs to know if the create succeeded, we block it. Set
         * join and blocked_on BEFORE block_and_ready — on single CPU,
         * block_and_ready immediately switches to thp, which checks thp->join
         * during WAAA to wake the parent.
         */
        act->blocked_on = thp;
        SETKSTATUS(act, tid + 1);
        thp->join = act;
        act->state = STATE_WAITTHREAD;
        _TRACE_TH_EMIT_STATE(act, WAITTHREAD);
        block_and_ready(thp);
    } else {
        ready(thp);
    }

    return ENOERROR;
}

/* Drop every hart's claim on thp's live FP registers (arch/riscv/exit.c lets
 * only the recorded owner resume with FP on). A compare-exchange per hart, so a
 * hart that has since loaded another thread's registers is never clobbered.
 */
void fpu_drop_claims(thread_t *thp)
{
    for (unsigned c = 0; c < CONFIG_PROCESSORS_MAX; c++) {
        struct thread_entry *me = thp;

        __atomic_compare_exchange_n(&pcpu_array[c].pc_active_fpu, &me, NULL, 0,
                                    __ATOMIC_ACQ_REL, __ATOMIC_RELAXED);
    }
}

/* Destroy a thread.
 *
 * Atomically claims the thread, defers destruction of a victim still running a
 * kcall on another hart, then tears it down: removes it from the run queues,
 * releases its FPU/timer/stack/interrupt/mutex resources, retargets valid_thp,
 * wakes any joiner, RCU-drains and frees the soul, and notifications roottask
 * if it was the process's last active thread.
 *
 * By design this is called by a thread in the same process as the one being
 * destroyed — possibly the thread destroying itself. Parameters:
 *   thp : thread to destroy
 */
void rdecl thread_destroy(thread_t *thp)
{
    process_t *prp = thp->process;
    unsigned i;
    ksync_t *syp;
    thread_t *jthp = NULL;
    int join_owned = 0;
    int last_active = 0; /* set iff this reap emptied prp->threads */
    unsigned stats_enabled;
    int join_woke = 0;
    uint64_t destroy_entry_cycles = 0;
    uint64_t join_wake_cycles = 0;
    uint64_t post_vec_end_cycles = 0;
    uint64_t post_vec_cycles = 0;
    uint64_t post_cpu_cycles = 0;
    uint64_t post_name_cycles = 0;
    uint64_t post_notify_cycles = 0;
    uint64_t stats_destroy_t0 = 0;
    uint64_t stats_destroy_t1 = 0;
    uint64_t stats_destroy_t2 = 0;
    uint64_t stats_destroy_t3 = 0;
    uint64_t stats_destroy_t4 = 0;
    uint64_t stats_clear_cycles = 0;
    uint64_t stats_link_cycles = 0;
    bool stats_object_free_heap = false;
    bool stats_pin_waited = false;
    bool stats_destroy_claim_reserved = false;
    bool stats_have = false;
    struct rcu_sched_busy stats_busy;

    /* Is this a self-reap? thp is this hart's active thread, here via its own
     * KILLSELF specret. A self-reap is the UNIQUE freer of thp (the quiescence
     * gate below guarantees no remote hart ever frees a thread that is some
     * hart's active) AND it is running NOW — so it must be NON-ABORTABLE: it
     * cannot bail back to U-mode, or the thread resumes user execution at
     * whatever pc it had (pc=0 for a not-yet-started main thread → the rc12
     * ip=0 FAULT). Compute it before the claim so the claim acquisition can
     * wait rather than bail for the self path.
     */
    int self_reap = (thp == get_active_thread(get_cpunum()));

    /* A terminating process cannot wait for a live server's UNBLOCK reply.
     * In particular, roottask deliberately never replies to _PROC_EXIT. An
     * IPC claim can make exit's second teardown pass defer, leaving the last
     * thread REPLY-blocked with KILLSELF and UNBLOCK_QUEUED forever. Bypass
     * the handshake, but retain force_ready's channel lock / transfer-claim
     * coordination before unlinking and dispatching the self-reaper. */
    unsigned force_flags = _FORCE_KILL_SELF;
    if (prp->flags & (RTMUX_FLG_PROC_TERMING | RTMUX_FLG_PROC_DESTROYALL))
        force_flags |= _FORCE_NO_UNBLOCK;

    /* Destroy-claim — see banner on thread_t::destroy_claim in objects.h.
     * Atomically claim destruction of this thread.
     *
     * A REQUEST caller (remote/blocked path) that loses the CAS bails: the
     * current holder is already steering thp toward its self-reap.
     *
     * A SELF-REAP must NOT bail. A losing CAS here means a remote
     * thread_destroy holds the claim transiently while it sets KILLSELF and
     * sends an IPI/force_ready (the quiescence gate's steer branch) — it will
     * release shortly and it will NOT free us (only we can, from our own hart).
     * So wait for the claim and proceed; aborting would send this running
     * thread back to U-mode undescheduled.
     */
    {
        unsigned reserved = THREAD_DESTROY_CLAIM_RESERVED_BASE + get_cpunum();
        unsigned expected = 0;
        unsigned claim_spins = 0;
        if (__atomic_load_n(&thp->destroy_claim, __ATOMIC_ACQUIRE) !=
            reserved) {
            while (!__atomic_compare_exchange_n(
                &thp->destroy_claim, &expected, THREAD_DESTROY_CLAIM_ACTIVE, 0,
                __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
                if (!self_reap) {
                    return;
                }
                if (++claim_spins > 1000000) {
                    kprintf(
                        "FATAL: thread_destroy claim stuck thp=%p pid=%d "
                        "tid=%d state=%d flags=%x\n",
                        (void *) thp, prp ? prp->pid : -1, thp->tid, thp->state,
                        thp->flags);
                    crash();
                }
                expected = 0;
                thread_cpu_relax();
            }
        }
    }
    stats_enabled =
        __atomic_load_n(&thread_destroy_stats_enabled, __ATOMIC_ACQUIRE);
    if (stats_enabled)
        destroy_entry_cycles = cpu_clock_ticks();

    /* Quiescence gate (rc12 — pure self-reap model). The teardown below, and in
     * particular the object_free at the end, may run ONLY when thp is
     * quiescable on THIS hart:
     *
     *   - it is this hart's active thread (a self-reap): unready->block
     *     below moves pc_curthread off it before the free, and a runqueue
     *     entry is removed by exactly one LINK3_REM under one dpp->lock, so
     *     no other hart can simultaneously own it; OR
     *   - it is an already-quiesced STATE_DEAD zombie: off every dispatch
     *     queue, named by no hart's pc_curthread (it ran this same path on
     *     its own hart when it exited, stopping at the zombie gate below).
     *
     * For ANY other thp — running on a remote hart, ready, or blocked — we must
     * NOT free it here. A remote select_thread could install it as pc_curthread
     * between our checks and the free; that is the freed-active use-after-free.
     * Instead flag KILLSELF so it self-destructs on its own hart when it next
     * runs, steer it toward being dispatched, release the claim, and return.
     * This single rule replaces the old G1 (remote pc_curthread + in_kcall>0)
     * and G2 (STATE_RUNNING && !local) deferrals, both of which read
     * pc_curthread/state UNLOCKED and so could not close the TOCTOU.
     */
    {
        unsigned self = get_cpunum();
        int active_hart = thread_active_hart(thp);
        int active_remote = active_hart >= 0 && active_hart != (int) self;
        unsigned dead_active_spins = 0;
        uint8_t state = thread_state_load_acquire(thp);

        while (state == STATE_DEAD && active_remote) {
            if (++dead_active_spins > 1000000) {
                kprintf(
                    "FATAL: thread_destroy dead thp=%p pid=%d tid=%d "
                    "stuck active on hart %d\n",
                    (void *) thp, prp ? prp->pid : -1, thp->tid, active_hart);
                crash();
            }
            SENDIPI((unsigned) active_hart, IPI_RESCHED);
            thread_cpu_relax();
            active_hart = thread_active_hart(thp);
            active_remote = active_hart >= 0 && active_hart != (int) self;
            state = thread_state_load_acquire(thp);
        }

        if ((thp != get_active_thread(self) && state != STATE_DEAD) ||
            active_remote) {
            atomic_or(&thp->flags,
                      (RTMUX_FLG_THR_KILLSELF | RTMUX_FLG_THR_ONLYME));
            if (active_hart >= 0) {
                SENDIPI((unsigned) active_hart, IPI_RESCHED);
            } else if (state == STATE_RUNNING) {
                SENDIPI(thp->runcpu, IPI_RESCHED);
            } else if (state != STATE_DEAD) {
                /* Blocked or ready: get it onto a run queue so it is dispatched
                 * and self-reaps. force_ready returns 0 for states another
                 * mechanism will ready (WAITPAGE/STACK, REPLY/SEND mid-xfer) —
                 * that is fine: the pager / xfer completion readies it, and the
                 * KILLSELF flag makes it self-reap then.
                 */
                force_ready(thp, force_flags);
            }
            __atomic_store_n(&thp->destroy_claim, 0, __ATOMIC_RELEASE);
            return;
        }
    }

    _TRACE_TH_EMIT_DESTROY(thp);
    if (thread_state_load_acquire(thp) != STATE_DEAD) {
        /* Remove thread from any queues. */
        if (!force_ready(thp, force_flags)) {
            /* Couldn't kill this thread right now, do it later */
            __atomic_store_n(&thp->destroy_claim, 0, __ATOMIC_RELEASE);
            return;
        }

        /* Remove any fpu buffer before the thread becomes dead */
        if (thp->fpudata) {
            FPU_REGISTERS *fpudata = FPUDATA_PTR(thp->fpudata);

            if (FPUDATA_INUSE(thp->fpudata)) {
                if (FPUDATA_CPU(thp->fpudata) != get_cpunum()) {
                    /* Context still in use on another CPU; need to flush it out
                     */
                    atomic_or(&thp->flags,
                              (RTMUX_FLG_THR_KILLSELF | RTMUX_FLG_THR_ONLYME));
                    SENDIPI(FPUDATA_CPU(thp->fpudata), IPI_CONTEXT_SAVE);
                    __atomic_store_n(&thp->destroy_claim, 0, __ATOMIC_RELEASE);
                    return;
                }
            }

            fpu_drop_claims(thp);
            atomic_order(thp->fpudata = NULL);
            object_free(NULL, &fpu_souls, fpudata);
        }

        if (thp->flags & RTMUX_FLG_THR_RCVINFO) {
            thp->args.ri.thp->restart = 0;
        }
        if (thp->flags & RTMUX_FLG_THR_SHORT_MSG) {
            ((thread_t *) thp->blocked_on)->restart = 0;
        }

        thp->timeout_flags = 0;
        if (thp->timeout) {
            timer_free(prp, thp->timeout);
            thp->timeout = NULL;
        }


        /* Now remove thread from the ready queue. Call unready() instead of
         * block() the thread may not be active.
         */
        unready(thp, STATE_DEAD);


        /* The thread is definitely no longer in actives[]. */

        /* Give the vendor extension a chance to take first dibs */
        if (kcall_op_thread_destroy_hook) {
            kcall_op_thread_destroy_hook(thp);
        }

        /* purge mutex hold list */
        while ((syp = thp->mutex_holdlist)) {
            thread_t *waiting = pril_first(&syp->waiting);
            CRASHCHECK(waiting->args.mu.owner == 0);
            mutex_holdlist_rem(syp);
            waiting->args.mu.owner = 0;
        }
        sync_thread_cleanup(thp);

        /* clear client field */
        if (thp->client != 0) {
            /* need to clear client's server field */
            thp->client->args.ms.server = 0;
            thp->client = 0;
        }

        /* Release the stack if it was dynamically allocated */
        if (thp->flags & RTMUX_FLG_THR_ALLOCED_STACK) {
            if (prp->flags &
                (RTMUX_FLG_PROC_TERMING | RTMUX_FLG_PROC_DESTROYALL)) {
                /* Process teardown unmaps the whole address space after the
                 * last thread is gone. Do not park exiting threads in
                 * STATE_STACK waiting for per-thread stack munmap, or the
                 * last-thread notification can be lost and the parent waitpid()
                 * stays blocked.
                 */
                atomic_clr(&thp->flags, RTMUX_FLG_THR_ALLOCED_STACK);
            } else if (prp->pid != ROOTTASK_PID &&
                       proc_hooks->proc_stack_code) {
                if (thp->state != STATE_STACK) {
                    /* Must do modification of user address spaces at process
                     * time
                     */
                    thp->state = STATE_STACK;
                    if (!(thp->flags & RTMUX_FLG_THR_DETACHED))
                        ITF_SET(thp, RTMUX_ITF_EXIT_STACK);

                    _TRACE_TH_EMIT_STATE(thp, STACK);
                    atomic_or(&thp->flags,
                              (RTMUX_FLG_THR_KILLSELF | RTMUX_FLG_THR_ONLYME));
                    atomic_clr(&thp->flags, (RTMUX_FLG_THR_TO_BE_STOPPED |
                                             RTMUX_FLG_THR_WAAA));

                    /* Direct notification — see banner of
                     * sigevent_notification_roottask() in kernel/ipc/event.c.
                     * The sigevent_proc → ROOTTASK_COID route goes through the
                     * unwired kernel_threads_prp->chancons[0] and returns
                     * ESRCH.
                     */
                    if (sigevent_notification_roottask(
                            proc_hooks->proc_stack_code, thp->priority,
                            (uintptr_t) SYNC_OWNER(thp))) {
                        /* Very bad; we are out of notifications... This means
                         * there's absolutely no memory left in the system.
                         */
                        CRASHCHECK(1);
                    }
                }
                __atomic_store_n(&thp->destroy_claim, 0, __ATOMIC_RELEASE);
                return;
            }
            roottask_stack_free(thp);
            atomic_clr(&thp->flags, RTMUX_FLG_THR_ALLOCED_STACK);
        }

        /* Make thread lookups invalid */
        vector_flag(&prp->threads, thp->tid, 1);

        /* If there is still active threads retarget valid_thp.
         *
         * Atomic decrement: two threads of this process can reach here on
         * different harts (destroy_claim serializes per-thread, not
         * per-process), so a plain -- could lose an update or let both observe
         * 0. __atomic_sub_fetch keeps num_active_threads coherent. NOTE: this
         * is NOT the "last thread out" signal — pass-2 of
         * kext_exit_destroy_threads also decrements num_active_threads manually
         * for REPLY/SEND threads, so the reach-zero here may be missed. The
         * notification trigger is the empty-thread-vector check at the
         * vector_rem below, which every reap path passes through.
         */
        unsigned remaining = (unsigned) __atomic_sub_fetch(
            &prp->num_active_threads, 1, __ATOMIC_ACQ_REL);
        if (remaining && prp->valid_thp == thp) {
            thread_t *thp2, *thp3 = NULL;

            for (i = 0; i < prp->threads.nentries; i++) {
                /* VECP() will not match our thread. */
                if (VECP(thp2, &prp->threads, i)) {
                    prp->valid_thp = thp2;
                    break;
                } else if ((thp2 = VECP2(thp2, &prp->threads, i)) &&
                           thp2->state != STATE_DEAD) {
                    thp3 = thp2;
                }
            }

            /* Could happen if we only have threads starting up left */
            if (prp->valid_thp == thp) {
                prp->valid_thp = thp3;
            }
        } else if (!remaining) {
            prp->valid_thp = NULL;
        }

        /* Deactivate any timers targeting this thread */
        for (i = 0; i < prp->timers.nentries; ++i) {
            ktimer_t *tip;

            if (VECP(tip, &prp->timers, i) && (tip->flags & RTMUX_TI_ACTIVE) &&
                (prp->num_active_threads == 0 || tip->thread == thp)) {
                if ((tip->flags & RTMUX_TI_TARGET_PROCESS) &&
                    (prp->valid_thp)) {
                    /* have to retarget to a new thread */
                    tip->thread = prp->valid_thp;
                } else {
                    timer_deactivate(tip);
                }
            }
        }
        if (prp->alarm) {
            ktimer_t *tip = prp->alarm;

            /* Alarm timers are always process based. */
            if (tip->thread == thp) {
                tip->thread = prp->valid_thp;
            }
        }

        /* Clean up after a SPORADIC thread */
        if (thp->policy == SCHED_SPORADIC) {
            sched_ss_cleanup(thp);
        }

        /* Release any attached interrupts */
        if (prp->flags & RTMUX_FLG_PROC_CHECK_INTR) {
            interrupt_t *itp;

            for (i = 0; i < interrupt_vector.nentries; ++i) {
                if (VECP(itp, &interrupt_vector, i) &&
                    itp->thread->process == prp) {
                    /* Only detach interrupt if bound to thread, or all threads
                     * are inactive
                     */
                    if (prp->num_active_threads == 0) {
                        interrupt_detach_entry(prp, i);
                    } else if (itp->thread != thp) {
                        /* nothing to do */
                    } else if (itp->flags & RTMUX_INTR_FLAGS_PROCESS) {
                        itp->thread = prp->valid_thp;
                    } else {
                        interrupt_detach_entry(prp, i);
                    }
                }
            }
        }

        /* timers/interrupts have been detached, so flush the interrupt queues
         * to get rid of anything pending that's pointing at this thread
         */
        intrevent_flush();

        /* If requested send a death notification to a channel. */
        if (prp->death_chp &&
            (prp->death_chp->flags & RTMUX_FLG_CHF_THREAD_DEATH)) {
            notification_deliver(prp->death_chp, thp->real_priority,
                                 _NOTIFICATION_CODE_THREADDEATH, thp->tid + 1,
                                 -1, 0);
        }
    }

    (void) thread_set_as_prp(thp, NULL);

    kspinlock_lock(&prp->vec_slock);
    jthp = thp->join;
    if (jthp && tthread_pin(jthp) != EOK)
        crash();
    if (!jthp && (thp->flags & RTMUX_FLG_THR_DETACHED) == 0 &&
        (prp->threads.nentries - 1 != prp->threads.nfree)) {
        kspinlock_unlock(&prp->vec_slock);

        /* Release the destroy claim: the thread is now a zombie, and the
         * eventual pthread_join (or process exit) will re-enter thread_destroy
         * to finish the teardown. The next entry must be able to acquire.
         */
        __atomic_store_n(&thp->destroy_claim, 0, __ATOMIC_RELEASE);
        return;
    }
    kspinlock_unlock(&prp->vec_slock);


    /* Wakeup any thread waiting to join on me. */
    if (jthp) {
        thread_join_wait_ready(jthp);
        kspinlock_lock(&prp->vec_slock);
        if (thp->join == jthp) {
            thp->join = NULL;
            join_owned = 1;
        }
        if (!join_owned && (thp->flags & RTMUX_FLG_THR_DETACHED) == 0 &&
            (prp->threads.nentries - 1 != prp->threads.nfree)) {
            kspinlock_unlock(&prp->vec_slock);
            __atomic_store_n(&thp->destroy_claim, 0, __ATOMIC_RELEASE);
            tthread_unpin(jthp);
            return;
        }
        kspinlock_unlock(&prp->vec_slock);

        if (join_owned) {
            if (jthp->state == STATE_WAITTHREAD) {
                /* This thread was being created.... */
                kcall_err(jthp, (int) (intptr_t) thp->status);
            } else {
                /* This is a normal thread death... */
                jthp->args.jo.status = thp->status;
                atomic_or(&jthp->flags, RTMUX_FLG_THR_JOIN);
                SETKSTATUS(jthp, EOK);
            }
            ready(jthp);
            if (stats_enabled) {
                join_woke = 1;
                join_wake_cycles = cpu_clock_ticks();
            }
        }
        tthread_unpin(jthp);
    }

    /* A thread that exited on its own was already STATE_DEAD when it reached
     * the cleanup above, so its FP save area can still be here. The thread is
     * no longer running anywhere, so drop every hart's claim on its live
     * registers (a compare-exchange: never clobber a hart that has since loaded
     * another thread) and free the area.
     */
    if (thp->fpudata) {
        FPU_REGISTERS *fpudata = FPUDATA_PTR(thp->fpudata);

        fpu_drop_claims(thp);
        atomic_order(thp->fpudata = NULL);
        object_free(NULL, &fpu_souls, fpudata);
    }

    /* Likewise its timeout timer. A thread already STATE_DEAD here (process
     * exit stamps REPLY/SEND-blocked threads dead without force_ready) skipped
     * the free in the body above, so every exiting process that had ever slept
     * leaked one timer. That reply-blocked thread may still have its reply
     * timeout armed (RTMUX_FLG_TIMEOUT_ACTIVE): timeout_stop() deactivates it,
     * as force_ready would have, because timer_free() requires an idle timer.
     */
    if (thp->timeout) {
        timeout_stop(thp);
        timer_free(prp, thp->timeout);
        thp->timeout = NULL;
    }

    /* Remove thread from process thread vector. vec_slock — see the paired
     * vector_add in thread_create.
     */
    {
        unsigned __tid = thp->tid;
        uint64_t t0 = stats_enabled && join_woke ? cpu_clock_ticks() : 0;
        uint64_t t1;
        kspinlock_lock(&prp->vec_slock);
        thp = vector_rem(&prp->threads, __tid);

        /* "Last thread out": the thread vector is now empty. This is the single
         * robust trigger for the proc_threads_destroyed notification (fired
         * below) — EVERY reap path reaches this vector_rem exactly once (the
         * normal body reap AND the pass-2 STATE_DEAD reap in
         * kext_exit_destroy_threads), whereas num_active_threads is decremented
         * in two places and so cannot reliably name the last reaper. Captured
         * under vec_slock, so exactly one reaper sees the empty vector and
         * fires once.
         */
        last_active = (prp->threads.nentries == prp->threads.nfree);
        kspinlock_unlock(&prp->vec_slock);
        if (stats_enabled && join_woke) {
            t1 = cpu_clock_ticks();
            post_vec_cycles = t1 - t0;
            post_vec_end_cycles = t1;
        }
    }
    if (!thp) {
        crash();
    }

    /* Remove any CPU-specific save buffers */
    if (stats_enabled && join_woke)
        post_vec_end_cycles = cpu_clock_ticks();
    cpu_thread_destroy(thp);
    if (stats_enabled && join_woke) {
        uint64_t t1 = cpu_clock_ticks();
        post_cpu_cycles = t1 - post_vec_end_cycles;
    }

    /* Remember the priority for the notifications below */
    i = thp->real_priority;

    thp->schedinfo.rr_ticks =
        0; /* sanity check before we return it to the pool */

    /* Free the name if it was allocated */
    if (thp->name) {
        uint64_t t0 = stats_enabled && join_woke ? cpu_clock_ticks() : 0;
        size_t name_len = strlen(thp->name) + 1;
        if (name_len >= THREAD_NAME_FIXED_SIZE) {
            _sfree(thp->name, name_len);
        } else {
            object_free(thp->process, &threadname_souls, thp->name);
        }
        thp->name = NULL;
        if (stats_enabled && join_woke)
            post_name_cycles = cpu_clock_ticks() - t0;
    }

    /* Reaping is synchronous on every path: join_woke is stats-only attribution
     * and never selects a deferred or worker-thread reclaim handoff.
     *
     * RCU drain. At this point thp has been removed from prp->threads
     * (vector_rem above) so no NEW reader can find it. But a concurrent
     * BKL-less kcall on another CPU may still hold a thp pointer from a
     * vector_lookup performed BEFORE our vector_rem. Wait here for every
     * in-flight tthread_rcu read section to exit before recycling the memory;
     * this closes the lookup_rcvid → thp deref UAF that surfaced as the
     * 0x6d6d... poison load in ksys_msg_readv.
     */
    if (stats_enabled) {
        stats_destroy_t0 = cpu_clock_ticks();
        (void) rcu_sched_synchronize_detail(&stats_busy);
        stats_destroy_t1 = cpu_clock_ticks();
        stats_pin_waited = tthread_wait_unpinned(thp);

        /* Freed-active gate. The RCU drain above covers in-flight kernel
         * readers, but a hart that has thp as its pc_curthread and is running
         * it in U-mode is NOT an RCU reader — the drain does not see it. If any
         * OTHER hart still calls thp its active thread, freeing now is the
         * freed-active use-after-free: that hart's next trap would dispatch a
         * kcall on a recycled "act".
         *
         * Self is expected and safe: a KILLSELF self-destruct is still its own
         * hart's pc_curthread until kernel_exit dispatches the next thread.
         * Release thread object back to the free queue.
         */
        stats_destroy_t2 = cpu_clock_ticks();
        thread_destroy_wait_inactive(thp, prp);
        stats_destroy_t3 = cpu_clock_ticks();
        stats_destroy_claim_reserved =
            __atomic_load_n(&thp->destroy_claim, __ATOMIC_RELAXED) >=
            THREAD_DESTROY_CLAIM_RESERVED_BASE;

        /* Same revoke-before-free invariant as thread_soul_free; the
         * instrumented path frees through object_free_track_heap_detail, so it
         * cannot share that helper.
         */
        cap_revoke_object_all_tables(thp);
        object_free_track_heap_detail(prp, &thread_souls, thp,
                                      &stats_object_free_heap,
                                      &stats_clear_cycles, &stats_link_cycles);
        stats_destroy_t4 = cpu_clock_ticks();
        stats_have = true;
    } else {
        rcu_sched_synchronize();
        (void) tthread_wait_unpinned(thp);
        thread_destroy_wait_inactive(thp, prp);
        thread_soul_free(prp, thp);
    }

    /* If this was the last active thread, notify roottask so it can clean up
     * process resources (FDs, channels, memory) and wake the parent's
     * waitpid(). Without this, abnormal termination (e.g. ldso failure →
     * DESTROYALL) leaves the process as a resource-leaking zombie and the
     * parent blocked forever.
     *
     * Route directly to roottask via sigevent_notification_roottask rather than
     * sigevent_proc: the latter resolves ROOTTASK_COID through the dangling
     * "kernel_threads_prp->chancons[0]" and returns ESRCH, so the notification
     * never arrives and the dead process's parent stays blocked in STATE_REPLY.
     * The resident proc_threads_destroyed() callback fills the sigevent fields,
     * then sigevent_notification_roottask() delivers straight into roottask's
     * process channel — no connection-table indirection. See the helper's
     * banner in event.c. rc12 — this is now the SOLE trigger for process
     * resource cleanup, for BOTH normal exit and abnormal death. The TERMING
     * suppression is gone: normal exit (proc_exit) only marks TERMING + steers
     * the threads to self-reap and returns; the last thread out fires this
     * notification, and the single cleanup path in proc_terminate frees
     * channels / timers / FDs / memory / soul and notifies the parent.
     * Resources are thus freed only after every thread is gone — by causality,
     * not by the old hopeful in-line sequencing that raced still-reaping
     * threads.
     */
    if (last_active) {
        struct sigevent ev;
        uint64_t t0 = stats_enabled && join_woke ? cpu_clock_ticks() : 0;
        proc_threads_destroyed(prp, &ev);
        sigevent_notification_roottask(ev.sigev_code,
                                       (unsigned) ev.sigev_priority,
                                       (uintptr_t) ev.sigev_value.sival_int);
        if (stats_enabled && join_woke)
            post_notify_cycles = cpu_clock_ticks() - t0;
    }
    if (stats_have)
        thread_destroy_stats_add(
            thread_destroy_cycles_to_ns(stats_destroy_t1 - stats_destroy_t0),
            thread_destroy_cycles_to_ns(stats_destroy_t2 - stats_destroy_t1),
            thread_destroy_cycles_to_ns(stats_destroy_t3 - stats_destroy_t2),
            thread_destroy_cycles_to_ns(stats_destroy_t4 - stats_destroy_t3),
            thread_destroy_cycles_to_ns(stats_clear_cycles),
            thread_destroy_cycles_to_ns(stats_link_cycles), stats_pin_waited,
            stats_object_free_heap, stats_destroy_claim_reserved, join_woke,
            join_woke ? thread_destroy_cycles_to_ns(join_wake_cycles -
                                                    destroy_entry_cycles)
                      : 0,
            join_woke ? thread_destroy_cycles_to_ns(stats_destroy_t4 -
                                                    join_wake_cycles)
                      : 0,
            join_woke ? thread_destroy_cycles_to_ns(post_vec_cycles) : 0,
            join_woke ? thread_destroy_cycles_to_ns(post_cpu_cycles) : 0,
            join_woke ? thread_destroy_cycles_to_ns(post_name_cycles) : 0,
            join_woke ? thread_destroy_cycles_to_ns(post_notify_cycles) : 0,
            &stats_busy);
}

/* Fault handler for a fault while touching a new thread's stack.
 *
 * Fails the thread creation (ENOMEM/EINVAL) and self-kills the thread.
 * Parameters:
 *   thp   : thread whose stack faulted
 *   reg   : register context at the fault
 *   flags : fault flags (unused)
 */
static void threadstack_fault(thread_t *thp, CPU_REGISTERS *reg, unsigned flags)
{
    /* This will cause the parent thread to return from __thread_create with an
     * ENOMEM or EINVAL error.
     */
    thp->status = (thp->flags & RTMUX_FLG_THR_ALLOCED_STACK)
                      ? (void *) (uintptr_t) ENOMEM
                      : (void *) (uintptr_t) EINVAL;
    atomic_or(&thp->flags, (RTMUX_FLG_THR_KILLSELF | RTMUX_FLG_THR_ONLYME));

    /* Leave the special-return context, as xfer_preempt does: with the per-CPU
     * flag still set, the specialret that reaps this KILLSELF would hit its
     * re-entry crash().
     */
    SET_XFER_HANDLER(NULL);
    unspecret_kernel();
    kernel_exit();
}

static const struct fault_handlers threadstack_fault_handlers = {
    threadstack_fault, 0};

/* Complete thread creation in a context where user memory is accessible and
 * faults are recoverable.
 *
 * Validates the user thread attributes, allocates or arranges the stack (via
 * the roottask proc_stack notification for non-roottask processes), initializes
 * the thread-local storage block, applies the scheduling policy, and releases
 * the blocked parent. Bad user pointers fault into threadstack_fault, which
 * fails the create. Parameters:
 *   thp : the new thread being brought up (its WAAA args carry the request)
 */
void rdecl thread_specret(thread_t *thp)
{
    struct _thread_local_storage *tsp;
    const struct _thread_attr *attr;
    void *init_cc;
    uintptr_t stack_top;
    uintptr_t new_sp;
    int verify;

    thp->status = (void *) (uintptr_t) EFAULT;
    if ((attr = thp->args.wa.attr)) {
        /* RD_VERIFY_PTR(act, attr, sizeof(*attr)); RD_PROBE_INT(act, attr,
         * sizeof(*attr) / sizeof(int));
         */

        /* Check for attributes which we do not support. If there is a stack
         * addr there must be a stack size. If there is a stack size it must be
         * at lease PTHREAD_STACK_MIN. If EXPLICIT sched, make sure policy and
         * priority are valid.
         *                    add validation of the sporadic server attributes
         */
        if (attr->flags & PTHREAD_SCOPE_PROCESS) {
            verify = ENOTSUP;
        } else if ((attr->stackaddr || attr->stacksize) &&
                   attr->stacksize < PTHREAD_STACK_MIN) {
            verify = EINVAL;
        } else if (attr->flags & PTHREAD_EXPLICIT_SCHED) {
            verify = ksched_ok(thp, attr->policy,
                               (struct sched_param *) &attr->sched_param);
        } else {
            verify = EOK;
        }

        if (verify != EOK) {
            /* The flags update is atomic_or for cross-CPU safety. */
            thp->status = (void *) (uintptr_t) verify;
            atomic_or((volatile unsigned *) &thp->flags,
                      RTMUX_FLG_THR_KILLSELF | RTMUX_FLG_THR_ONLYME);
            return;
            /* RUSH3: this comes out in loader_exit() but EINTR overridden */
        }
    }

    /* Check if we need to allocate a stack */
    if (!(thp->flags & RTMUX_FLG_THR_ALLOCED_STACK)) {
        uintptr_t guardsize = 0;
        unsigned prealloc = 0;

        if (attr) {
            /* Get the user requested values. */
            thp->un.lcl.stackaddr = attr->stackaddr;
            thp->un.lcl.stacksize = attr->stacksize;
            if (attr->stackaddr && !WR_PROBE_PTR(thp, thp->un.lcl.stackaddr,
                                                 thp->un.lcl.stacksize)) {
                thp->status = (void *) (uintptr_t) EINVAL;
                atomic_or((volatile unsigned *) &thp->flags,
                          RTMUX_FLG_THR_KILLSELF | RTMUX_FLG_THR_ONLYME);
                return;
            }
            guardsize = attr->guardsize;
            prealloc = attr->prealloc;
        }
        if (thp->un.lcl.stacksize == 0) {
            if (__cpu_flags & CPU_FLAG_MMU) {
                thp->un.lcl.stacksize = DEF_VIRTUAL_THREAD_STACKSIZE;
            } else {
                thp->un.lcl.stacksize = DEF_PHYSICAL_THREAD_STACKSIZE;
            }
        }
        if (!thp->un.lcl.stackaddr) {
            if (thp->process->pid != ROOTTASK_PID &&
                proc_hooks->proc_stack_code) {
                unspecret_kernel();

                if (thp->state != STATE_STACK) {
                    /* Must do modification of user address spaces at process
                     * time
                     */
                    CRASHCHECK(thp != get_active_thread(get_cpunum()));

                    /* Order matters on SMP. Roottask's notification handler
                     * (proc_stack → kext_stack_wait_info) requires
                     *   thp->state == STATE_STACK
                     * to find the waiter, and reads guardsize/flags/prealloc
                     * out of thp->prev.thread / thp->next.thread / thp->status.
                     *
                     * The notification send may schedule a higher-priority pool
                     * thread on another CPU (or this one) which races into
                     * stack_wait_info before we've parked thp. If the lookup
                     * misses, the thread sits in STATE_RUNNING with no stack
                     * and a later kernel_exit dispatches on a garbage SP.
                     *
                     * So park thp + stash params FIRST, then send the
                     * notification. The destroy-side branch does the same.
                     */
                    unready(thp, STATE_STACK);
                    thp->prev.thread = (void *) guardsize;
                    thp->next.thread = NULL;
                    thp->status = (void *) (uintptr_t) prealloc;

                    /* Direct notification to roottask's process channel. The
                     * sigevent_proc(SIGEV_NOTIFICATION coid=ROOTTASK_COID)
                     * route goes through sigevent_exe's connection lookup in
                     * the dangling "kernel_threads_prp->chancons[0]". See the
                     * helper's banner in kernel/ipc/event.c.
                     */
                    if (sigevent_notification_roottask(
                            proc_hooks->proc_stack_code, thp->priority,
                            (uintptr_t) SYNC_OWNER(thp))) {
                        /* Notification failed (notification pool exhausted).
                         * Roll back: restore RUNNING state and arrange for
                         * self-kill.
                         */
                        thp->state = STATE_RUNNING;
                        thp->status = (void *) (uintptr_t) EAGAIN;
                        atomic_or(&thp->flags, (RTMUX_FLG_THR_KILLSELF |
                                                RTMUX_FLG_THR_ONLYME));
                        return;
                    }
                }
                return;
            }

            /* guardsize: roottask_stack_alloc now passes __PAGESIZE to as_mmap
             * as boff, creating a proper guard page
             */
            if (roottask_stack_alloc(thp) != EOK) {
                thp->status = (void *) (uintptr_t) EAGAIN;
                atomic_or(&thp->flags,
                          (RTMUX_FLG_THR_KILLSELF | RTMUX_FLG_THR_ONLYME));
                return;
            }
            atomic_or(&thp->flags, RTMUX_FLG_THR_ALLOCED_STACK);
            SPECRET_PREEMPT(thp);
        }
    }

    /* Inherit or assign a scheduling policy and params. */
    if (attr) {
        thp->args.wa.exitfunc = attr->exitfunc;
    }

    /* Clear detach state if there is a parent */

    /* Get the *real* attribute structure pointer - we may have NULL'd out
     * thp->args.wa.attr and then been preempted
     */
    attr = thp->args.wa.real_attr;
    if (thp->join && (!attr || !(attr->flags & PTHREAD_CREATE_DETACHED))) {
        atomic_clr(&thp->flags, RTMUX_FLG_THR_DETACHED);
    }

    /* Make thread lookups valid */
    vector_flag(&thp->process->threads, thp->tid, 0);
    thp->args.wa.attr = 0;

    if (get_active_thread(get_cpunum()) != thp) {
        return;
    }

    /* Load the necessary registers for the thread to start execution. */
    stack_top =
        STACK_INIT((uintptr_t) thp->un.lcl.stackaddr, thp->un.lcl.stacksize);
    STACK_ALLOC(thp->un.lcl.tls, new_sp, stack_top, sizeof *thp->un.lcl.tls);
    STACK_ALLOC(init_cc, new_sp, new_sp, STACK_INITIAL_CALL_CONVENTION_USAGE);
    SETKSP(thp, new_sp);

    /* Could fault again while setting tls in stack... */
    SPECRET_PREEMPT(thp);

    SET_XFER_HANDLER(&threadstack_fault_handlers);

    tsp = thp->un.lcl.tls;
    {
        /* Preserve __reserved2[0] (cpupage pointer used by get_cpunum()) across
         * the memset. For the idle thread, tsp == tp — the active kernel TLS.
         * Zeroing it destroys the cpupage pointer.
         */
        void *saved_cpupage = tsp->__reserved2[0];
        memset(tsp, 0, sizeof(*tsp));
        tsp->__reserved2[0] = saved_cpupage;
    }

    /* Set the inital calling convention usage section to zero - will help any
     * stack traceback code to determine when it has hit the top of the stack.
     */
    memset(init_cc, 0, STACK_INITIAL_CALL_CONVENTION_USAGE);

    if (attr) {
        tsp->__flags = attr->flags & (PTHREAD_CSTATE_MASK | PTHREAD_CTYPE_MASK);
    }
    tsp->__arg = thp->args.wa.arg;
    tsp->__exitfunc = thp->args.wa.exitfunc;
    if (!tsp->__exitfunc && thp->process->valid_thp) {
        /* We don't have thread termination (exitfunc) for this thread. Likely
         * it was created with SIGEV_THREAD. Use the same one as for the
         * valid_thp's. This mostly works since all threads created via
         * pthread_create have the same exit function.
         */
        tsp->__exitfunc = thp->process->valid_thp->un.lcl.tls->__exitfunc;
    }

    tsp->__errptr = &tsp->__errval;
    if (thp->process->pid == ROOTTASK_PID) {
        tsp->__stackaddr = (uint8_t *) thp->un.lcl.stackaddr;
    } else {
        tsp->__stackaddr =
            (uint8_t *) thp->un.lcl.stackaddr + ((!attr) ? 0 : attr->guardsize);
    }
    tsp->__pid = thp->process->pid;
    tsp->__tid = (int) (thp->tid + 1); /* 1-based POSIX tid */
    tsp->__owner = SYNC_OWNER(thp);

    /* TODO: touch additional stack pages if attr->guaranteedstacksize is set */

    SET_XFER_HANDLER(NULL);

    cpu_thread_waaa(thp);

    /* Let the parent continue. The tid was stuffed during thread_create(). */
    if (thp->join && thp->join->state == STATE_WAITTHREAD) {
        ready(thp->join);
        thp->join = NULL;
    }

    /* Don't change priority until parent thread freed to run again
     * - we might get a priority inversion otherwise.
     */
    if ((attr) && (attr->flags & PTHREAD_EXPLICIT_SCHED)) {
        if (sched_thread(thp, attr->policy,
                         (struct sched_param *) &attr->sched_param) != EOK) {
            /* We should have some error handling if sched_thread() fails ...
             * thp->status = (void *)EAGAIN; atomic_or(&thp->flags,
             * (RTMUX_FLG_THR_KILLSELF | RTMUX_FLG_THR_ONLYME)); return;
             */
        }
    }

    /* Only done once for the first thread running */
    if (thp->process->process_priority == 0) {
        thp->process->process_priority = thp->priority;
    }

    /* a thread is born unto a STOPPED process - make sure it stops too! */
    if (thp->process->flags &
        (RTMUX_FLG_PROC_DEBUG_STOPPED | RTMUX_FLG_PROC_STOPPED)) {
        atomic_or(&thp->flags, RTMUX_FLG_THR_TO_BE_STOPPED);
    }
    atomic_clr(&thp->flags, RTMUX_FLG_THR_WAAA);
}

/* Mark every thread of a process for self-destruction.
 *
 * Flags each thread DETACHED | KILLSELF (clearing stop/stack flags) and sets
 * RTMUX_FLG_PROC_DESTROYALL on the process. Parameters:
 *   thp : any thread of the process to destroy
 */
void rdecl thread_destroyall(thread_t *thp)
{
    process_t *prp = thp->process;
    thread_t *thp2;
    int tid;

    for (tid = 0; tid < prp->threads.nentries; ++tid) {
        if (VECP(thp2, &prp->threads, tid)) {
            atomic_or(&thp2->flags,
                      RTMUX_FLG_THR_DETACHED | RTMUX_FLG_THR_KILLSELF);
            /* Knock down the stack flag, faster to do it when we unmap all */
            atomic_clr(&thp2->flags, (RTMUX_FLG_THR_TO_BE_STOPPED |
                                      RTMUX_FLG_THR_ALLOCED_STACK));
        }
    }

    prp->flags |= RTMUX_FLG_PROC_DESTROYALL;
}


/* Cancel a thread.
 *
 * If the thread runs on another CPU it is flagged for self-cancellation and
 * kicked; otherwise it is redirected to its process cancellation stub (deferred
 * via CANCELSELF when mid message-delivery). Parameters:
 *   thp : thread to cancel
 */
void rdecl thread_cancel(thread_t *thp)
{
    if (thp->state == STATE_RUNNING && thp != get_active_thread(get_cpunum())) {
        /* The thread is running on another processor in an SMP system. */
        atomic_or(&thp->flags, RTMUX_FLG_THR_CANCELSELF);
        SENDIPI(thp->runcpu, IPI_RESCHED);
        return;
    }

#if CONFIG_PROCESSORS_MAX > 1 && defined(CONFIG_SMP_MSGOPT)
    if (thp->internal_flags & RTMUX_ITF_MSG_DELIVERY) {
        atomic_or(&thp->flags, RTMUX_FLG_THR_CANCELSELF);
    } else {
        atomic_or(&thp->flags, RTMUX_FLG_THR_KCALL_ERR_SET);
        SETKIP_FUNC(thp, thp->process->canstub);
    }
#else
    atomic_or(&thp->flags, RTMUX_FLG_THR_KCALL_ERR_SET);
    SETKIP_FUNC(thp, thp->process->canstub);
#endif
}
