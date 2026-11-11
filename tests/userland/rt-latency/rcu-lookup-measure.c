/*
 * rcu-lookup-measure - latency probe for kernel lookup hot paths.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/rtmux-core.h>
#include <sys/siginfo.h>

#define DEFAULT_SAMPLES 1000
#define DEFAULT_BATCH 64
#define SERVER_STOP_MSG 0x73746f70
#define THREAD_DESTROY_STATS_WAIT_NS 30000000000ULL

static volatile int server_ready;
static pthread_attr_t join_attr;
static pthread_attr_t detached_attr;
static struct sched_param saved_sched_param;
static int saved_sched_policy;

static int monotonic_ns(uint64_t *out)
{
    if (__clock_time(CLOCK_MONOTONIC, NULL, out) == -1)
        return -1;
    return 0;
}

static int cmp_u64(const void *a, const void *b)
{
    uint64_t av = *(const uint64_t *) a;
    uint64_t bv = *(const uint64_t *) b;

    return (av > bv) - (av < bv);
}

static void report(const char *name, uint64_t *samples, int n, int batch)
{
    qsort(samples, (size_t) n, sizeof(samples[0]), cmp_u64);
    printf("rcu-lookup-measure: %s samples=%d batch=%d min-ns=%llu p50-ns=%llu "
           "p99-ns=%llu max-ns=%llu\n",
           name, n, batch, (unsigned long long) samples[0],
           (unsigned long long) samples[n / 2],
           (unsigned long long) samples[((n - 1) * 99) / 100],
           (unsigned long long) samples[n - 1]);
}

static int thread_destroy_stats_reset(void)
{
    struct _thread_destroy_stats stats;

    memset(&stats, 0, sizeof(stats));
    stats.reset = 1;
    return thread_ctl(RTMUX_TCTL_THREAD_DESTROY_STATS, &stats);
}

static int thread_destroy_stats_disable(void)
{
    struct _thread_destroy_stats stats;

    memset(&stats, 0, sizeof(stats));
    stats.reset = 2;
    return thread_ctl(RTMUX_TCTL_THREAD_DESTROY_STATS, &stats);
}

static int thread_destroy_expect_active_claim(const char *label)
{
    return strcmp(label, "thread-join-post-store-self") == 0 ||
           strcmp(label, "thread-join-post-store-pid") == 0;
}

static int thread_destroy_stats_report(const char *label, unsigned long calls)
{
    struct _thread_destroy_stats stats;
    uint64_t post_ready_named_ns;
    uint64_t post_ready_residual_ns;
    uint64_t start;
    uint64_t now;

    if (monotonic_ns(&start) == -1)
        return -1;

    for (;;) {
        memset(&stats, 0, sizeof(stats));
        if (thread_ctl(RTMUX_TCTL_THREAD_DESTROY_STATS, &stats) == -1)
            return -1;
        if (stats.calls >= calls)
            break;
        if (monotonic_ns(&now) == -1)
            return -1;
        if (now - start > THREAD_DESTROY_STATS_WAIT_NS)
            break;
        (void) __sched_yield();
    }
    post_ready_named_ns = stats.rcu_wait_ns + stats.pin_wait_ns +
        stats.active_scan_ns + stats.object_free_ns +
        stats.destroy_post_vector_ns + stats.destroy_post_cpu_ns +
        stats.destroy_post_name_ns;
    post_ready_residual_ns =
        post_ready_named_ns <= stats.destroy_join_post_ready_ns ?
        stats.destroy_join_post_ready_ns - post_ready_named_ns : (uint64_t) 0;
    if (stats.calls != calls || stats.rcu_wait_ns == 0 ||
        stats.pin_wait_ns == 0 || stats.active_scan_ns == 0 ||
        stats.object_free_ns == 0 ||
        stats.object_free_heap + stats.object_free_cache != calls ||
        stats.object_free_heap_ns + stats.object_free_cache_ns !=
            stats.object_free_ns ||
        stats.object_free_cache_clear_ns + stats.object_free_cache_link_ns >
            stats.object_free_cache_ns ||
        (stats.object_free_cache != 0 &&
         stats.object_free_cache_clear_ns + stats.object_free_cache_link_ns ==
             0) ||
        stats.pin_waited + stats.pin_skipped != calls ||
        (thread_destroy_expect_active_claim(label) &&
         (stats.destroy_claim_active != calls ||
          stats.destroy_claim_reserved != 0 ||
          stats.destroy_join_wake != calls ||
          stats.destroy_join_wake_ns == 0 ||
          stats.destroy_join_post_ready != calls ||
          stats.destroy_join_post_ready_ns == 0 ||
          stats.destroy_post_vector_ns == 0 ||
          stats.destroy_post_cpu_ns == 0 ||
          post_ready_named_ns > stats.destroy_join_post_ready_ns)) ||
        stats.rcu_waited + stats.rcu_skipped != calls ||
        stats.rcu_waited_ns + stats.rcu_skipped_ns != stats.rcu_wait_ns ||
        stats.rcu_enroll_ns + stats.rcu_scan_ns + stats.rcu_post_scan_ns ==
            0 ||
        stats.rcu_enroll_ns + stats.rcu_scan_ns + stats.rcu_post_scan_ns >
            stats.rcu_wait_ns ||
        stats.rcu_busy_peer_sum < stats.rcu_waited ||
        stats.rcu_busy_thread + stats.rcu_busy_msg + stats.rcu_busy_sys +
        stats.rcu_busy_channel + stats.rcu_busy_connect +
        stats.rcu_busy_interrupt + stats.rcu_busy_sync +
        stats.rcu_busy_sched + stats.rcu_busy_time + stats.rcu_busy_idle +
        stats.rcu_busy_dispatch + stats.rcu_busy_timer +
        stats.rcu_busy_fault + stats.rcu_busy_priv +
        stats.rcu_busy_heartbeat + stats.rcu_busy_priv_system +
        stats.rcu_busy_priv_vm + stats.rcu_busy_priv_ipc +
        stats.rcu_busy_priv_process + stats.rcu_busy_priv_thread +
        stats.rcu_busy_priv_pa + stats.rcu_busy_priv_pa_alloc +
        stats.rcu_busy_priv_pa_free + stats.rcu_busy_priv_pa_zero +
        stats.rcu_busy_priv_pa_info + stats.rcu_busy_priv_pa_xlat +
        stats.rcu_busy_priv_pa_pq +
        stats.rcu_busy_unstamped + stats.rcu_busy_other !=
            stats.rcu_busy_peer_sum ||
        stats.destroy_claim_active + stats.destroy_claim_reserved != calls ||
        (stats.rcu_waited != 0 && stats.rcu_busy_peer_max == 0)) {
        fprintf(stderr,
                "rcu-lookup-measure: thread-destroy-stats invalid label=%s "
                "expected=%lu calls=%llu rcu-wait-ns=%llu "
                "rcu-enroll-ns=%llu rcu-scan-ns=%llu "
                "rcu-post-scan-ns=%llu pin-wait-ns=%llu "
                "active-scan-ns=%llu object-free-ns=%llu heap=%llu "
                "cache=%llu heap-ns=%llu cache-ns=%llu "
                "cache-clear-ns=%llu cache-link-ns=%llu "
                "pin-waited=%llu pin-skipped=%llu "
                "rcu-waited=%llu rcu-skipped=%llu rcu-waited-ns=%llu "
                "rcu-skipped-ns=%llu busy-sum=%llu busy-max=%llu "
                "busy-thread=%llu busy-msg=%llu busy-sys=%llu "
                "busy-channel=%llu busy-connect=%llu busy-interrupt=%llu "
                "busy-sync=%llu busy-sched=%llu busy-time=%llu "
                "busy-idle=%llu busy-dispatch=%llu "
                "busy-timer=%llu busy-fault=%llu busy-priv=%llu "
                "busy-heartbeat=%llu busy-priv-system=%llu "
                "busy-priv-vm=%llu busy-priv-ipc=%llu "
                "busy-priv-process=%llu busy-priv-thread=%llu "
                "busy-priv-pa=%llu busy-priv-pa-alloc=%llu "
                "busy-priv-pa-free=%llu busy-priv-pa-zero=%llu "
                "busy-priv-pa-info=%llu busy-priv-pa-xlat=%llu "
                "busy-priv-pa-pq=%llu "
                "busy-unstamped=%llu busy-other=%llu "
                "claim-active=%llu claim-reserved=%llu "
                "join-wake=%llu join-wake-ns=%llu "
                "join-post-ready=%llu join-post-ready-ns=%llu "
                "post-vector-ns=%llu post-cpu-ns=%llu post-name-ns=%llu "
                "post-notify-ns=%llu post-ready-named-ns=%llu "
                "post-ready-residual-ns=%llu\n",
                label, calls, (unsigned long long) stats.calls,
                (unsigned long long) stats.rcu_wait_ns,
                (unsigned long long) stats.rcu_enroll_ns,
                (unsigned long long) stats.rcu_scan_ns,
                (unsigned long long) stats.rcu_post_scan_ns,
                (unsigned long long) stats.pin_wait_ns,
                (unsigned long long) stats.active_scan_ns,
                (unsigned long long) stats.object_free_ns,
                (unsigned long long) stats.object_free_heap,
                (unsigned long long) stats.object_free_cache,
                (unsigned long long) stats.object_free_heap_ns,
                (unsigned long long) stats.object_free_cache_ns,
                (unsigned long long) stats.object_free_cache_clear_ns,
                (unsigned long long) stats.object_free_cache_link_ns,
                (unsigned long long) stats.pin_waited,
                (unsigned long long) stats.pin_skipped,
                (unsigned long long) stats.rcu_waited,
                (unsigned long long) stats.rcu_skipped,
                (unsigned long long) stats.rcu_waited_ns,
                (unsigned long long) stats.rcu_skipped_ns,
                (unsigned long long) stats.rcu_busy_peer_sum,
                (unsigned long long) stats.rcu_busy_peer_max,
                (unsigned long long) stats.rcu_busy_thread,
                (unsigned long long) stats.rcu_busy_msg,
                (unsigned long long) stats.rcu_busy_sys,
                (unsigned long long) stats.rcu_busy_channel,
                (unsigned long long) stats.rcu_busy_connect,
                (unsigned long long) stats.rcu_busy_interrupt,
                (unsigned long long) stats.rcu_busy_sync,
                (unsigned long long) stats.rcu_busy_sched,
                (unsigned long long) stats.rcu_busy_time,
                (unsigned long long) stats.rcu_busy_idle,
                (unsigned long long) stats.rcu_busy_dispatch,
                (unsigned long long) stats.rcu_busy_timer,
                (unsigned long long) stats.rcu_busy_fault,
                (unsigned long long) stats.rcu_busy_priv,
                (unsigned long long) stats.rcu_busy_heartbeat,
                (unsigned long long) stats.rcu_busy_priv_system,
                (unsigned long long) stats.rcu_busy_priv_vm,
                (unsigned long long) stats.rcu_busy_priv_ipc,
                (unsigned long long) stats.rcu_busy_priv_process,
                (unsigned long long) stats.rcu_busy_priv_thread,
                (unsigned long long) stats.rcu_busy_priv_pa,
                (unsigned long long) stats.rcu_busy_priv_pa_alloc,
                (unsigned long long) stats.rcu_busy_priv_pa_free,
                (unsigned long long) stats.rcu_busy_priv_pa_zero,
                (unsigned long long) stats.rcu_busy_priv_pa_info,
                (unsigned long long) stats.rcu_busy_priv_pa_xlat,
                (unsigned long long) stats.rcu_busy_priv_pa_pq,
                (unsigned long long) stats.rcu_busy_unstamped,
                (unsigned long long) stats.rcu_busy_other,
                (unsigned long long) stats.destroy_claim_active,
                (unsigned long long) stats.destroy_claim_reserved,
                (unsigned long long) stats.destroy_join_wake,
                (unsigned long long) stats.destroy_join_wake_ns,
                (unsigned long long) stats.destroy_join_post_ready,
                (unsigned long long) stats.destroy_join_post_ready_ns,
                (unsigned long long) stats.destroy_post_vector_ns,
                (unsigned long long) stats.destroy_post_cpu_ns,
                (unsigned long long) stats.destroy_post_name_ns,
                (unsigned long long) stats.destroy_post_notify_ns,
                (unsigned long long) post_ready_named_ns,
                (unsigned long long) post_ready_residual_ns);
        errno = EINVAL;
        return -1;
    }
    printf("rcu-lookup-measure: thread-destroy-stats label=%s calls=%llu "
           "rcu-wait-ns=%llu pin-wait-ns=%llu active-scan-ns=%llu "
           "object-free-ns=%llu object-free-heap=%llu object-free-cache=%llu "
           "rcu-waited=%llu rcu-skipped=%llu\n",
           label, (unsigned long long) stats.calls,
           (unsigned long long) stats.rcu_wait_ns,
           (unsigned long long) stats.pin_wait_ns,
           (unsigned long long) stats.active_scan_ns,
           (unsigned long long) stats.object_free_ns,
           (unsigned long long) stats.object_free_heap,
           (unsigned long long) stats.object_free_cache,
           (unsigned long long) stats.rcu_waited,
           (unsigned long long) stats.rcu_skipped);
    printf("rcu-lookup-measure: thread-destroy-object-free-ns label=%s "
           "object-free-heap-ns=%llu object-free-cache-ns=%llu\n",
           label,
           (unsigned long long) stats.object_free_heap_ns,
           (unsigned long long) stats.object_free_cache_ns);
    printf("rcu-lookup-measure: thread-destroy-object-free-cache label=%s "
           "clear-ns=%llu link-ns=%llu\n",
           label,
           (unsigned long long) stats.object_free_cache_clear_ns,
           (unsigned long long) stats.object_free_cache_link_ns);
    printf("rcu-lookup-measure: thread-destroy-pin label=%s "
           "pin-waited=%llu pin-skipped=%llu\n",
           label,
           (unsigned long long) stats.pin_waited,
           (unsigned long long) stats.pin_skipped);
    printf("rcu-lookup-measure: thread-destroy-rcu-ns label=%s "
           "rcu-waited-ns=%llu rcu-skipped-ns=%llu\n",
           label,
           (unsigned long long) stats.rcu_waited_ns,
           (unsigned long long) stats.rcu_skipped_ns);
    printf("rcu-lookup-measure: thread-destroy-rcu-detail label=%s "
           "enroll-ns=%llu scan-ns=%llu post-scan-ns=%llu\n",
           label,
           (unsigned long long) stats.rcu_enroll_ns,
           (unsigned long long) stats.rcu_scan_ns,
           (unsigned long long) stats.rcu_post_scan_ns);
    printf("rcu-lookup-measure: thread-destroy-rcu-peers label=%s "
           "busy-peer-sum=%llu busy-peer-max=%llu\n",
           label,
           (unsigned long long) stats.rcu_busy_peer_sum,
           (unsigned long long) stats.rcu_busy_peer_max);
    printf("rcu-lookup-measure: thread-destroy-rcu-busy label=%s "
           "thread=%llu msg=%llu sys=%llu channel=%llu connect=%llu "
           "interrupt=%llu sync=%llu sched=%llu time=%llu idle=%llu "
           "dispatch=%llu timer=%llu fault=%llu priv=%llu "
           "heartbeat=%llu unstamped=%llu other=%llu\n",
           label,
           (unsigned long long) stats.rcu_busy_thread,
           (unsigned long long) stats.rcu_busy_msg,
           (unsigned long long) stats.rcu_busy_sys,
           (unsigned long long) stats.rcu_busy_channel,
           (unsigned long long) stats.rcu_busy_connect,
           (unsigned long long) stats.rcu_busy_interrupt,
           (unsigned long long) stats.rcu_busy_sync,
           (unsigned long long) stats.rcu_busy_sched,
           (unsigned long long) stats.rcu_busy_time,
           (unsigned long long) stats.rcu_busy_idle,
           (unsigned long long) stats.rcu_busy_dispatch,
           (unsigned long long) stats.rcu_busy_timer,
           (unsigned long long) stats.rcu_busy_fault,
           (unsigned long long) stats.rcu_busy_priv,
           (unsigned long long) stats.rcu_busy_heartbeat,
           (unsigned long long) stats.rcu_busy_unstamped,
           (unsigned long long) stats.rcu_busy_other);
    printf("rcu-lookup-measure: thread-destroy-rcu-priv label=%s "
           "system=%llu vm=%llu ipc=%llu process=%llu thread=%llu pa=%llu\n",
           label,
           (unsigned long long) stats.rcu_busy_priv_system,
           (unsigned long long) stats.rcu_busy_priv_vm,
           (unsigned long long) stats.rcu_busy_priv_ipc,
           (unsigned long long) stats.rcu_busy_priv_process,
           (unsigned long long) stats.rcu_busy_priv_thread,
           (unsigned long long) stats.rcu_busy_priv_pa);
    printf("rcu-lookup-measure: thread-destroy-rcu-priv-pa label=%s "
           "alloc=%llu free=%llu zero=%llu info=%llu xlat=%llu pq=%llu\n",
           label,
           (unsigned long long) stats.rcu_busy_priv_pa_alloc,
           (unsigned long long) stats.rcu_busy_priv_pa_free,
           (unsigned long long) stats.rcu_busy_priv_pa_zero,
           (unsigned long long) stats.rcu_busy_priv_pa_info,
           (unsigned long long) stats.rcu_busy_priv_pa_xlat,
           (unsigned long long) stats.rcu_busy_priv_pa_pq);
    printf("rcu-lookup-measure: thread-destroy-claim label=%s "
           "active=%llu reserved=%llu\n",
           label,
           (unsigned long long) stats.destroy_claim_active,
           (unsigned long long) stats.destroy_claim_reserved);
    printf("rcu-lookup-measure: thread-destroy-join-wake label=%s "
           "count=%llu ns=%llu\n",
           label,
           (unsigned long long) stats.destroy_join_wake,
           (unsigned long long) stats.destroy_join_wake_ns);
    printf("rcu-lookup-measure: thread-destroy-join-post-ready label=%s "
           "count=%llu ns=%llu\n",
           label,
           (unsigned long long) stats.destroy_join_post_ready,
           (unsigned long long) stats.destroy_join_post_ready_ns);
    printf("rcu-lookup-measure: thread-destroy-post-ready-detail label=%s "
           "vector-ns=%llu cpu-ns=%llu name-ns=%llu notify-ns=%llu\n",
           label,
           (unsigned long long) stats.destroy_post_vector_ns,
           (unsigned long long) stats.destroy_post_cpu_ns,
           (unsigned long long) stats.destroy_post_name_ns,
           (unsigned long long) stats.destroy_post_notify_ns);
    printf("rcu-lookup-measure: thread-destroy-post-ready-residual label=%s "
           "named-ns=%llu residual-ns=%llu notify-ns=%llu\n",
           label,
           (unsigned long long) post_ready_named_ns,
           (unsigned long long) post_ready_residual_ns,
           (unsigned long long) stats.destroy_post_notify_ns);
    return thread_destroy_stats_disable();
}

static int measure_batch(const char *name, uint64_t *samples, int n, int batch,
                         int (*fn)(void *), void *arg)
{
    uint64_t start, end;

    for (int i = 0; i < n; i++) {
        if (monotonic_ns(&start) == -1)
            goto fail;
        for (int j = 0; j < batch; j++) {
            if (fn(arg) == -1)
                goto fail;
        }
        if (monotonic_ns(&end) == -1)
            goto fail;
        samples[i] = (end - start) / (uint64_t) batch;
    }
    report(name, samples, n, batch);
    return 0;

fail:
    fprintf(stderr, "rcu-lookup-measure: %s failed: %s\n", name,
            strerror(errno));
    return -1;
}

static int measure(const char *name, uint64_t *samples, int n,
                   int (*fn)(void *), void *arg)
{
    return measure_batch(name, samples, n, DEFAULT_BATCH, fn, arg);
}

static int measure_prepared(const char *name, uint64_t *samples, int n,
                            int (*prepare)(void *), int (*fn)(void *),
                            void *arg)
{
    uint64_t start, end;

    for (int i = 0; i < n; i++) {
        int prepared = 0;
        int joined = 0;

        if (prepare(arg) == -1)
            goto fail;
        prepared = 1;
        if (monotonic_ns(&start) == -1)
            goto fail;
        if (fn(arg) == -1)
            goto fail;
        joined = 1;
        if (monotonic_ns(&end) == -1)
            goto fail;
        samples[i] = end - start;
        continue;

fail:
        if (prepared && !joined)
            (void) fn(arg);
        fprintf(stderr, "rcu-lookup-measure: %s failed: %s\n", name,
                strerror(errno));
        return -1;
    }
    report(name, samples, n, 1);
    return 0;
}

static int run_prepared(const char *name, int n, int (*prepare)(void *),
                        int (*fn)(void *), void *arg)
{
    for (int i = 0; i < n; i++) {
        int prepared = 0;
        int joined = 0;

        if (prepare(arg) == -1)
            goto fail;
        prepared = 1;
        if (fn(arg) == -1)
            goto fail;
        joined = 1;
        continue;

fail:
        if (prepared && !joined)
            (void) fn(arg);
        fprintf(stderr, "rcu-lookup-measure: %s failed: %s\n", name,
                strerror(errno));
        return -1;
    }
    return 0;
}

static int do_noop(void *arg)
{
    (void) arg;
    return 0;
}

static void *server_thread(void *arg)
{
    int chid = *(int *) arg;
    int msg;
    int rcvid;

    __atomic_store_n(&server_ready, 1, __ATOMIC_RELEASE);
    for (;;) {
        rcvid = msg_receive(chid, &msg, sizeof(msg), NULL);
        if (rcvid > 0) {
            (void) msg_reply(rcvid, EOK, &msg, sizeof(msg));
            if (msg == SERVER_STOP_MSG)
                break;
        }
    }
    return NULL;
}

static int do_attach_detach(void *arg)
{
    int chid = *(int *) arg;
    uint64_t cap;

    cap = connect_attach_cap(0, getpid(), chid, RTMUX_SIDE_CHANNEL, 0);
    if (cap == 0)
        return -1;
    return __connect_detach(cap);
}

static int do_msgsend_cap(void *arg)
{
    uint64_t cap = *(uint64_t *) arg;
    int msg = 0x726375;
    int reply = 0;
    int rc;

    rc = msg_send_cap(cap, &msg, sizeof(msg), &reply, sizeof(reply));
    if (rc == -1 || reply != msg) {
        if (rc != -1)
            errno = EIO;
        return -1;
    }
    return 0;
}

static int stop_server(uint64_t cap)
{
    int msg = SERVER_STOP_MSG;
    int reply = 0;
    int rc;

    rc = msg_send_cap(cap, &msg, sizeof(msg), &reply, sizeof(reply));
    if (rc == -1 || reply != msg) {
        if (rc != -1)
            errno = EIO;
        return -1;
    }
    return 0;
}

static int do_msgsend_coid(void *arg)
{
    int coid = *(int *) arg;
    int msg = 0x636f6964;
    int reply = 0;
    int rc;

    rc = msg_send(coid, &msg, sizeof(msg), &reply, sizeof(reply));
    if (rc == -1 || reply != msg) {
        if (rc != -1)
            errno = EIO;
        return -1;
    }
    return 0;
}

static int do_connect_server_info_pid(void *arg)
{
    struct _server_info info;
    uint64_t cap = *(uint64_t *) arg;

    return connect_server_info(getpid(), cap, &info) == -1 ? -1 : 0;
}

static int do_connect_server_info_self(void *arg)
{
    struct _server_info info;
    uint64_t cap = *(uint64_t *) arg;

    return connect_server_info(0, cap, &info) == -1 ? -1 : 0;
}

static int do_connect_flags_pid(void *arg)
{
    uint64_t cap = *(uint64_t *) arg;

    return connect_flags(getpid(), cap, 0, 0) == -1 ? -1 : 0;
}

static int do_connect_flags_self(void *arg)
{
    uint64_t cap = *(uint64_t *) arg;

    return connect_flags(0, cap, 0, 0) == -1 ? -1 : 0;
}

static int do_sched_current_thread(void *arg)
{
    struct sched_param sp;

    (void) arg;
    return sched_getparam(0, &sp);
}

static int do_sched_info_pid(void *arg)
{
    struct _sched_info info;

    (void) arg;
    return __sched_info(getpid(), SCHED_RR, &info) == -1 ? -1 : 0;
}

static int do_sched_info_self(void *arg)
{
    struct _sched_info info;

    (void) arg;
    return __sched_info(0, SCHED_RR, &info) == -1 ? -1 : 0;
}

static int do_sched_pid_tid(void *arg)
{
    struct sched_param sp;
    pid_t pid = getpid();
    pthread_t tid = pthread_self();

    (void) arg;
    return __sched_get(pid, tid, &sp) == -1 ? -1 : 0;
}

static int do_sched_set_self(void *arg)
{
    (void) arg;
    return __sched_set(0, 0, saved_sched_policy, &saved_sched_param);
}

static int do_sched_set_pid_tid(void *arg)
{
    pid_t pid = getpid();
    pthread_t tid = pthread_self();

    (void) arg;
    return __sched_set(pid, tid, saved_sched_policy, &saved_sched_param);
}

static int do_sched_get_set_pid_tid(void *arg)
{
    struct sched_param sp;
    pid_t pid = getpid();
    pthread_t tid = pthread_self();
    int policy;

    (void) arg;
    policy = __sched_get(pid, tid, &sp);
    if (policy == -1)
        return -1;
    return __sched_set(pid, tid, policy, &sp);
}

static int do_timer_info_pid(void *arg)
{
    struct _timer_info info;
    timer_t timerid = *(timer_t *) arg;

    return __timer_info(getpid(), timerid, 0, &info);
}

static int do_timer_info_self(void *arg)
{
    struct _timer_info info;
    timer_t timerid = *(timer_t *) arg;

    return __timer_info(0, timerid, 0, &info);
}

static int do_clock_id_pid_tid(void *arg)
{
    (void) arg;
    return __clock_id(getpid(), pthread_self()) == -1 ? -1 : 0;
}

static int do_clock_id_self(void *arg)
{
    (void) arg;
    return __clock_id(0, pthread_self()) == -1 ? -1 : 0;
}

static void *thread_create_noop(void *arg)
{
    int *done = arg;

    __atomic_store_n(done, 1, __ATOMIC_RELEASE);
    return NULL;
}

static void *thread_create_return_noop(void *arg)
{
    (void) arg;
    return NULL;
}

struct join_post_store_arg {
    int explicit_pid;
    int done;
    int tid;
};

static int prepare_thread_join_post_store(void *arg)
{
    struct join_post_store_arg *jp = arg;
    pid_t pid = jp->explicit_pid ? getpid() : 0;

    jp->done = 0;
    jp->tid = thread_create_r(pid, thread_create_noop, &jp->done, &join_attr);
    if (jp->tid < 0) {
        errno = -jp->tid;
        jp->tid = -1;
        return -1;
    }
    while (!__atomic_load_n(&jp->done, __ATOMIC_ACQUIRE))
        (void) __sched_yield();
    return 0;
}

static int do_thread_join_post_store(void *arg)
{
    struct join_post_store_arg *jp = arg;
    int rc = thread_join(jp->tid, NULL);

    if (rc == EOK)
        jp->tid = -1;
    return rc;
}

static int do_thread_create_return_pid(void *arg)
{
    int tid;

    (void) arg;
    tid = thread_create_r(getpid(), thread_create_return_noop, NULL,
                          &detached_attr);
    if (tid < 0) {
        errno = -tid;
        return -1;
    }
    return 0;
}

static int do_thread_create_return_self(void *arg)
{
    int tid;

    (void) arg;
    tid = thread_create_r(0, thread_create_return_noop, NULL, &detached_attr);
    if (tid < 0) {
        errno = -tid;
        return -1;
    }
    return 0;
}

static int do_thread_create_pid(void *arg)
{
    int done = 0;
    int tid;

    (void) arg;
    tid = thread_create_r(getpid(), thread_create_noop, &done, &detached_attr);
    if (tid < 0) {
        errno = -tid;
        return -1;
    }
    while (!__atomic_load_n(&done, __ATOMIC_ACQUIRE))
        (void) __sched_yield();
    return 0;
}

static int do_thread_create_self(void *arg)
{
    int done = 0;
    int tid;

    (void) arg;
    tid = thread_create_r(0, thread_create_noop, &done, &detached_attr);
    if (tid < 0) {
        errno = -tid;
        return -1;
    }
    while (!__atomic_load_n(&done, __ATOMIC_ACQUIRE))
        (void) __sched_yield();
    return 0;
}

static int do_thread_create_join_pid(void *arg)
{
    int tid;

    (void) arg;
    tid = thread_create_r(getpid(), thread_create_return_noop, NULL,
                          &join_attr);
    if (tid < 0) {
        errno = -tid;
        return -1;
    }
    return thread_join(tid, NULL);
}

static int do_thread_create_join_self(void *arg)
{
    int tid;

    (void) arg;
    tid = thread_create_r(0, thread_create_return_noop, NULL, &join_attr);
    if (tid < 0) {
        errno = -tid;
        return -1;
    }
    return thread_join(tid, NULL);
}

int main(int argc, char **argv)
{
    uint64_t *samples;
    pthread_t server;
    struct sigevent ev;
    uint64_t cap;
    struct join_post_store_arg join_post_store_self = {0, 0, -1};
    struct join_post_store_arg join_post_store_pid = {1, 0, -1};
    int coid = -1;
    int samples_n = DEFAULT_SAMPLES;
    timer_t timerid = (timer_t) -1;
    int new_timerid;
    int chid;
    int rc = EXIT_FAILURE;
    int err;

    if (argc > 1) {
        samples_n = atoi(argv[1]);
        if (samples_n <= 0) {
            fprintf(stderr, "usage: rcu-lookup-measure [samples]\n");
            return EXIT_FAILURE;
        }
    }
    samples = calloc((size_t) samples_n, sizeof(samples[0]));
    if (!samples) {
        perror("calloc");
        return EXIT_FAILURE;
    }
    err = pthread_attr_init(&detached_attr);
    if (err != EOK) {
        fprintf(stderr, "pthread_attr_init: %s\n", strerror(err));
        goto out_samples;
    }
    err = pthread_attr_init(&join_attr);
    if (err != EOK) {
        fprintf(stderr, "pthread_attr_init(join): %s\n", strerror(err));
        goto out_detached_attr;
    }
    err = pthread_attr_setdetachstate(&detached_attr, PTHREAD_CREATE_DETACHED);
    if (err != EOK) {
        fprintf(stderr, "pthread_attr_setdetachstate: %s\n", strerror(err));
        goto out_join_attr;
    }

    chid = channel_create(0);
    if (chid == -1) {
        perror("channel_create");
        goto out_join_attr;
    }
    cap = connect_attach_cap(0, getpid(), chid, RTMUX_SIDE_CHANNEL, 0);
    if (cap == 0) {
        perror("connect_attach_cap");
        goto out_channel;
    }
    coid = connect_attach(0, getpid(), chid, RTMUX_SIDE_CHANNEL, 0);
    if (coid == -1) {
        perror("connect_attach");
        goto out_cap_only;
    }
    SIGEV_NOTIFICATION_INIT(&ev, ROOTTASK_COID, 10, 0, NULL);
    new_timerid = __timer_create(CLOCK_MONOTONIC, &ev);
    if (new_timerid == -1) {
        perror("__timer_create");
        goto out_coid_only;
    }
    timerid = (timer_t) new_timerid;
    err = pthread_create(&server, NULL, server_thread, &chid);
    if (err != EOK) {
        fprintf(stderr, "pthread_create: %s\n", strerror(err));
        goto out_timer;
    }
    while (!__atomic_load_n(&server_ready, __ATOMIC_ACQUIRE))
        (void) __sched_yield();
    saved_sched_policy = __sched_get(0, 0, &saved_sched_param);
    if (saved_sched_policy == -1) {
        perror("__sched_get");
        goto out_cap;
    }

    if (measure("measure-overhead", samples, samples_n, do_noop, NULL) == -1 ||
        measure("cap-attach-detach", samples, samples_n, do_attach_detach,
                &chid) == -1 ||
        measure("msgsend-cap-roundtrip", samples, samples_n, do_msgsend_cap,
                &cap) == -1 ||
        measure("msgsend-coid-roundtrip", samples, samples_n, do_msgsend_coid,
                &coid) == -1 ||
        measure("connect-server-info-self", samples, samples_n,
                do_connect_server_info_self, &cap) == -1 ||
        measure("connect-server-info-pid", samples, samples_n,
                do_connect_server_info_pid, &cap) == -1 ||
        measure("connect-flags-self", samples, samples_n,
                do_connect_flags_self, &cap) == -1 ||
        measure("connect-flags-pid", samples, samples_n,
                do_connect_flags_pid, &cap) == -1 ||
        measure("sched-current-thread", samples, samples_n,
                do_sched_current_thread, NULL) == -1 ||
        measure("sched-info-self", samples, samples_n, do_sched_info_self,
                NULL) == -1 ||
        measure("sched-info-pid", samples, samples_n, do_sched_info_pid,
                NULL) == -1 ||
        measure("sched-pid-tid", samples, samples_n, do_sched_pid_tid,
                NULL) == -1 ||
        measure("sched-set-self", samples, samples_n, do_sched_set_self,
                NULL) == -1 ||
        measure("sched-set-pid-tid", samples, samples_n, do_sched_set_pid_tid,
                NULL) == -1 ||
        measure("sched-get-set-pid-tid", samples, samples_n,
                do_sched_get_set_pid_tid,
                NULL) == -1 ||
        measure("timer-info-pid", samples, samples_n,
                do_timer_info_pid,
                &timerid) == -1 ||
        measure("timer-info-self", samples, samples_n,
                do_timer_info_self,
                &timerid) == -1 ||
        measure("clock-id-self", samples, samples_n,
                do_clock_id_self,
                NULL) == -1 ||
        measure("clock-id-pid-tid", samples, samples_n,
                do_clock_id_pid_tid,
                NULL) == -1)
        goto out_cap;

    if (thread_destroy_stats_disable() == -1 ||
        measure_prepared("thread-join-post-store-self", samples, samples_n,
                         prepare_thread_join_post_store,
                         do_thread_join_post_store,
                         &join_post_store_self) == -1 ||
        thread_destroy_stats_reset() == -1 ||
        run_prepared("thread-join-post-store-self-stats", samples_n,
                     prepare_thread_join_post_store,
                     do_thread_join_post_store,
                     &join_post_store_self) == -1 ||
        thread_destroy_stats_report("thread-join-post-store-self",
                                    (unsigned long) samples_n) == -1 ||
        thread_destroy_stats_disable() == -1 ||
        measure_prepared("thread-join-post-store-pid", samples, samples_n,
                         prepare_thread_join_post_store,
                         do_thread_join_post_store,
                         &join_post_store_pid) == -1 ||
        thread_destroy_stats_reset() == -1 ||
        run_prepared("thread-join-post-store-pid-stats", samples_n,
                     prepare_thread_join_post_store,
                     do_thread_join_post_store,
                     &join_post_store_pid) == -1 ||
        thread_destroy_stats_report("thread-join-post-store-pid",
                                    (unsigned long) samples_n) == -1)
        goto out_cap;

    if (measure_batch("thread-create-return-self", samples, samples_n, 1,
                      do_thread_create_return_self, NULL) == -1 ||
        measure_batch("thread-create-return-pid", samples, samples_n, 1,
                      do_thread_create_return_pid, NULL) == -1 ||
        measure_batch("thread-create-self", samples, samples_n, 1,
                      do_thread_create_self, NULL) == -1 ||
        measure_batch("thread-create-pid", samples, samples_n, 1,
                      do_thread_create_pid, NULL) == -1 ||
        measure_batch("thread-create-join-self", samples, samples_n, 1,
                      do_thread_create_join_self, NULL) == -1 ||
        measure_batch("thread-create-join-pid", samples, samples_n, 1,
                      do_thread_create_join_pid, NULL) == -1)
        goto out_cap;

    rc = EXIT_SUCCESS;

out_cap:
    (void) thread_destroy_stats_disable();
    if (stop_server(cap) == -1)
        (void) pthread_cancel(server);
    (void) pthread_join(server, NULL);
out_timer:
    (void) __timer_destroy(timerid);
out_coid_only:
    if (coid != -1)
        (void) __connect_detach((uint64_t) coid);
out_cap_only:
    (void) __connect_detach(cap);
out_channel:
    (void) channel_destroy(chid);
out_join_attr:
    (void) pthread_attr_destroy(&join_attr);
out_detached_attr:
    (void) pthread_attr_destroy(&detached_attr);
out_samples:
    free(samples);
    return rc;
}
