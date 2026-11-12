#!/bin/sh
set -eu

need_order()
{
    file=$1
    first=$2
    second=$3
    msg=$4

    if ! awk -v first="$first" -v second="$second" '
        index($0, first) && !a { a = NR }
        index($0, second) && a && !b { b = NR }
        END { exit !(a && b && a < b) }
    ' "$file"; then
        echo "RCU-sched: $msg" >&2
        exit 1
    fi
}

need_order arch/riscv/cpu-misc.c \
    'rcu_sched_quiescent();' \
    'wfi' \
    'idle entry must publish a quiescent state before WFI'

need_order arch/riscv/exit.c \
    'if (!(thp->reg.sstatus & SSTATUS_SPP))' \
    'rcu_sched_quiescent();' \
    'U-mode return path must publish a quiescent state'

need_order arch/riscv/exit.c \
    'rcu_sched_quiescent();' \
    'set_active_thread(cpu, NULL);' \
    'dead-thread exit loop must publish a quiescent state before clearing active thread'

need_order kernel/sched/dispatch.c \
    'if (old != new)' \
    'rcu_sched_quiescent();' \
    'context switch must publish a quiescent state'

need_order kernel/sched/dispatch.c \
    'rcu_sched_quiescent();' \
    'set_active_thread(cpu, NULL);' \
    'scheduler block paths must publish a quiescent state before clearing active thread'

if ! awk '
    /set_active_thread\(.*NULL\);/ {
        if (prev !~ /rcu_sched_quiescent\(\);/)
            bad = 1
        count++
    }
    { prev = $0 }
    END { exit !(count == 3 && !bad) }
' kernel/sched/dispatch.c; then
    echo "RCU-sched: every scheduler NULL active-thread path needs an immediate QS" >&2
    exit 1
fi

if ! awk '
    /void rdecl unready_release\(thread_t \*thp,/ { in_unready_release = 1 }
    in_unready_release && /thp != get_active_thread\(get_cpunum\(\)\)/ {
        saw_guard = NR
    }
    in_unready_release && /SCHED_LOCK_LOCAL\(\);/ {
        saw_lock = NR
    }
    in_unready_release && /^}/ { in_unready_release = 0 }
    END { exit !(saw_guard && saw_lock && saw_guard < saw_lock) }
' kernel/sched/dispatch.c; then
    echo "RCU-sched: unready_release must fail before blocking a non-active thread" >&2
    exit 1
fi

if ! awk '
    /unready_release_and_exit\(thread_t \*thp,/ { in_sleep_exit = 1 }
    in_sleep_exit && /unready_release\(thp, newstate, to_release\);/ {
        saw_release = NR
    }
    in_sleep_exit && /in_kcall_inc\(\);/ && saw_release {
        saw_inc = NR
    }
    in_sleep_exit && /in_kcall_phase\(IN_KCALL_PHASE_DISPATCH\);/ &&
        saw_inc {
        saw_phase = NR
    }
    in_sleep_exit && /kernel_exit\(\);/ && saw_phase {
        saw_exit = NR
    }
    in_sleep_exit && /^}/ { in_sleep_exit = 0 }
    END {
        exit !(saw_release && saw_inc && saw_phase && saw_exit &&
               saw_release < saw_inc && saw_inc < saw_phase &&
               saw_phase < saw_exit)
    }
' kernel/sched/dispatch.c; then
    echo "RCU-sched: S-mode worker sleep must block then enter kernel_exit" >&2
    exit 1
fi

if ! grep -q '^          sync/wait sync/rcu \\' kernel/objects.mk; then
    echo "RCU-sched: kernel/sync/rcu.c must be built" >&2
    exit 1
fi

if [ -e kernel/include/smr.h ] || [ -e kernel/sync/smr.c ]; then
    echo "RCU-sched: retired SMR compatibility files must stay removed" >&2
    exit 1
fi

if grep -q '#include "smr.h"' kernel/include/rcu.h ||
   grep -q 'typedef smr_seq_t rcu_epoch_t' kernel/include/rcu.h ||
   grep -q 'SMR_SEQ_' kernel/sync/rcu.c ||
   grep -q 'sync/smr' kernel/objects.mk ||
   grep -q 'typedef smr_t rcu_t' kernel/include/rcu.h ||
   grep -q 'smr_enter(rcu)' kernel/include/rcu.h ||
   grep -q 'smr_synchronize(rcu)' kernel/include/rcu.h; then
    echo "RCU-sched: public RCU backend must not depend on SMR" >&2
    exit 1
fi

if grep -Rqw 'synchronize_rcu' kernel; then
    echo "RCU-sched: retired synchronize_rcu writer API must stay removed" >&2
    exit 1
fi

if ! grep -q 'static inline void rcu_sched_read_lock(void)' \
        kernel/include/rcu.h ||
   ! grep -q 'static inline void rcu_sched_read_unlock(void)' \
        kernel/include/rcu.h; then
    echo "RCU-sched: direct read-side backend helpers must stay public" >&2
    exit 1
fi

if grep -Rqw 'pc_rcu_depth\|rcu_gp_goal\|rcu_gp_done\|rcu_epoch_bump_to' \
    kernel arch include; then
    echo "RCU-sched: rejected reader-depth/grace-period batching experiments must stay out of live code" >&2
    exit 1
fi

if grep -Rqw 'thread_reaper_worker\|thread_reaper_wait\|rcu_reap_next' \
    kernel arch include; then
    echo "RCU-sched: rejected S-mode reaper worker must stay out until pc_curthread binding is proved" >&2
    exit 1
fi

if grep -Rqw 'thread_reclaim_pending\|THREAD_RECLAIM_PENDING' \
    kernel arch include; then
    echo "RCU-sched: rejected broad reclaim queue cap must stay out of live code" >&2
    exit 1
fi

if ! awk '
    /int rdecl kernel_thread_create\(void \(\*entry\)\(void \*\),/ {
        in_create = 1
    }
    in_create && /thread_recycle_reset\(thp\);/ { saw_reset = NR }
    in_create && /thp->process = kernel_threads_prp;/ && saw_reset {
        saw_proc = NR
    }
    in_create && /cpu_thread_init\(NULL, thp, kopt\.align_fault\);/ &&
        saw_proc { saw_cpu = NR }
    in_create && /SETKIP_FUNC\(thp, entry\);/ && saw_cpu { saw_ip = NR }
    in_create && /ready\(thp\);/ && saw_ip { saw_ready = NR }
    in_create && /RTMUX_FLG_THR_WAAA/ { saw_waaa = NR }
    in_create && /^}/ { in_create = 0 }
    END { exit !(saw_reset && saw_proc && saw_cpu && saw_ip && saw_ready &&
                  saw_reset < saw_proc && saw_proc < saw_cpu &&
                  saw_cpu < saw_ip && saw_ip < saw_ready && !saw_waaa) }
' kernel/thread/lifecycle.c; then
    echo "RCU-sched: kernel workers must use the S-mode ready-queue constructor, not WAAA" >&2
    exit 1
fi

if ! grep -q '^ARCH_SRC = .* kernel-thread \\' arch/riscv/Makefile ||
   ! grep -q '^kernel_thread_checkpoint:' arch/riscv/kernel-thread.S ||
   ! grep -Eq 'sd[[:space:]]+ra,[[:space:]]+REG_PC\(a0\)' \
        arch/riscv/kernel-thread.S ||
   ! grep -Eq 'sd[[:space:]]+t0,[[:space:]]+REG_A0\(a0\)' \
        arch/riscv/kernel-thread.S ||
   ! grep -Fq 'SSTATUS_SPP | SSTATUS_SPIE | SSTATUS_SUM' \
        arch/riscv/kernel-thread.S ||
   ! grep -Fq 'int rdecl kernel_thread_checkpoint(CPU_REGISTERS *regs);' \
        kernel/include/decls.h; then
    echo "RCU-sched: S-mode worker continuation checkpoint must stay wired" >&2
    exit 1
fi

kernel_thread_create_uses=$(
    grep -Rnw 'kernel_thread_create[[:space:]]*(' kernel arch include |
        grep -v '^kernel/thread/lifecycle.c:[0-9][0-9]*:int rdecl kernel_thread_create' |
        grep -v '^kernel/include/decls.h:[0-9][0-9]*:int rdecl kernel_thread_create' ||
        true
)
if [ -n "$kernel_thread_create_uses" ]; then
    echo "RCU-sched: kernel_thread_create must stay unused until S-mode continuation save/restore is proved" >&2
    echo "$kernel_thread_create_uses" >&2
    exit 1
fi

if grep -Rqw 'thread_reclaim_worker\|thread_reclaim_worker_ensure\|thread_reclaim_enqueue\|thread_reclaim_finish\|thread_reclaim_slock\|reclaim_next\|reclaim_prp\|reclaim_last_active' \
    kernel arch include; then
    echo "RCU detail: reclaim handoff" >&2
    echo "RCU-sched: thread reap must stay synchronous; no deferred reclaim worker handoff" >&2
    exit 1
fi

if ! awk '
    /^void rdecl thread_destroy\(/ { in_destroy = 1 }
    in_destroy && /RCU drain\. At this point/ { in_reap = 1 }
    in_reap && /^    if \(stats_enabled\) {$/ { stats_branch = NR }
    in_reap && stats_branch && /rcu_sched_synchronize_detail\(&stats_busy\)/ { stats_rcu = NR }
    in_reap && stats_branch && /stats_pin_waited = tthread_wait_unpinned\(thp\)/ { stats_pin = NR }
    in_destroy && /thread_destroy_wait_inactive\(thp, prp\)/ && stats_pin && !stats_active {
        stats_active = NR
    }
    in_destroy && /object_free_track_heap_detail\(prp, &thread_souls, thp,/ {
        stats_free = NR
    }
    in_reap && /cap_revoke_object_all_tables\(thp\);/ { stats_revoke = NR }
    in_reap && /^    } else {$/ && stats_branch { in_else = 1 }
    in_destroy && in_else && /rcu_sched_synchronize\(\);/ { saw_rcu = NR }
    in_destroy && in_else && /tthread_wait_unpinned\(thp\);/ && saw_rcu {
        saw_pin = NR
    }
    in_destroy && in_else && /thread_destroy_wait_inactive\(thp, prp\);/ &&
        saw_pin { saw_active = NR }
    in_destroy && in_else && /thread_soul_free\(prp, thp\);/ && saw_active {
        saw_free = NR
    }
    in_destroy && /^}/ { in_destroy = 0 }
    END { exit !(stats_branch && stats_rcu && stats_pin && stats_active && stats_revoke && stats_free &&
                 stats_branch < stats_rcu && stats_rcu < stats_pin && stats_pin < stats_active &&
                 stats_active < stats_revoke && stats_revoke < stats_free &&
                 saw_rcu && saw_pin && saw_active && saw_free) }
' kernel/thread/lifecycle.c; then
    echo "RCU detail: sync reap" >&2
    echo "RCU-sched: thread reap must drain RCU, pins, and active harts before freeing" >&2
    exit 1
fi

if grep -Eq 'kernel/(sync/)?smr\.c|Use SMR|SMR \(RCU-equivalent\)' \
    README.md AGENTS.md; then
    echo "RCU-sched: current-facing docs must not point to retired SMR" >&2
    exit 1
fi

if grep -q 'endpoint_cap_to_context' kernel/ipc/connect.c; then
    echo "RCU-sched: endpoint cap users must keep generated caps pinned through validation" >&2
    exit 1
fi

if ! awk '
    /int kdecl ksys_sched_info\(thread_t \*act,/ {
        in_info = 1
    }
    in_info && /kap->pid == 0 \|\| kap->pid == act->process->pid/ { saw_self = NR }
    in_info && /lookup_pid_rcu\(kap->pid\)/ { saw_rcu = NR }
    in_info && /rcu_sched_read_unlock\(\);/ { saw_unlock = NR }
    in_info && /if \(prp != act->process\)/ { saw_local = NR }
    in_info && /return EPERM;/ && NR == saw_local + 1 { saw_denied = NR }
    in_info && /user_slice_init\(act, kap->info, sizeof\(si\), sizeof\(si\), USER_SLICE_WR,/ { saw_validate = NR }
    in_info && /&info_slice\)\)/ { saw_slice = NR }
    in_info && /return EFAULT;/ && NR == saw_slice + 1 { saw_fault = NR }
    in_info && /user_slice_copyout\(&info_slice, sip\)/ && !saw_copy { saw_copy = NR }
    in_info && /lookup_pid\(kap->pid\)/ { saw_plain = NR }
    in_info && /^}/ { in_info = 0 }
    END {
        if (!saw_self) print "RCU detail: info self" > "/dev/stderr"
        if (!saw_rcu) print "RCU detail: info lookup" > "/dev/stderr"
        if (!saw_unlock) print "RCU detail: info unlock" > "/dev/stderr"
        if (!saw_local) print "RCU detail: info local guard" > "/dev/stderr"
        if (!saw_denied) print "RCU detail: info denial" > "/dev/stderr"
        if (!saw_validate) print "RCU detail: info validation" > "/dev/stderr"
        if (!saw_fault) print "RCU detail: info fault" > "/dev/stderr"
        exit !(saw_self && saw_rcu && saw_unlock && saw_local && saw_denied &&
               saw_validate && saw_slice && saw_fault && saw_copy &&
               saw_self < saw_rcu && saw_rcu < saw_unlock && saw_unlock < saw_local &&
               saw_local < saw_denied && saw_denied < saw_validate &&
               saw_validate < saw_slice && saw_slice < saw_fault &&
               saw_fault < saw_copy && !saw_plain)
    }
' kernel/sched/policy.c; then
    echo "RCU-sched: sched_info must bypass self pid and use RCU for foreign pid" >&2
    exit 1
fi

if ! grep -q 'measure("sched-info-pid"' \
    tests/userland/rt-latency/rcu-lookup-measure.c; then
    echo "RCU-sched: lookup benchmark must cover sched_info pid lookup" >&2
    exit 1
fi

if ! grep -q 'measure("measure-overhead"' \
    tests/userland/rt-latency/rcu-lookup-measure.c; then
    echo "RCU-sched: lookup benchmark must include measurement overhead row" >&2
    exit 1
fi

if ! grep -q '#define DEFAULT_BATCH 64' \
    tests/userland/rt-latency/rcu-lookup-measure.c ||
   grep -q 'batch=\[0-9\]\[0-9\]\*' scripts/qemu-regress.sh; then
    echo "RCU-sched: fast lookup benchmark batch must be pinned to 64" >&2
    exit 1
fi

for row in connect-server-info-self connect-flags-self sched-info-self \
           sched-set-self sched-set-pid-tid timer-info-self clock-id-self; do
    if ! grep -q "measure(\"$row\"" \
        tests/userland/rt-latency/rcu-lookup-measure.c; then
        echo "RCU-sched: lookup benchmark must cover $row" >&2
        exit 1
    fi
done

if ! grep -q '__clock_id(0, pthread_self())' \
    tests/userland/rt-latency/rcu-lookup-measure.c; then
    echo "RCU-sched: clock-id self row must compare pid 0 with current tid" >&2
    exit 1
fi

if ! grep -q 'measure_batch("thread-create-return-self"' \
    tests/userland/rt-latency/rcu-lookup-measure.c ||
   ! grep -q 'measure_batch("thread-create-self"' \
    tests/userland/rt-latency/rcu-lookup-measure.c ||
   ! grep -q 'measure_batch("thread-create-join-self"' \
    tests/userland/rt-latency/rcu-lookup-measure.c ||
   ! grep -q 'measure_batch("thread-create-join-pid"' \
    tests/userland/rt-latency/rcu-lookup-measure.c ||
   ! grep -q 'measure_prepared("thread-join-post-store-self"' \
    tests/userland/rt-latency/rcu-lookup-measure.c ||
   ! grep -q 'measure_prepared("thread-join-post-store-pid"' \
    tests/userland/rt-latency/rcu-lookup-measure.c; then
    echo "RCU-sched: thread-create benchmark must separate self-pid and join rows" >&2
    exit 1
fi

if ! grep -q 'thread_destroy_stats_snapshot(&tds' kernel/sched/thread.c; then
    echo "RCU detail: destroy stats" >&2
    echo "RCU-sched: thread destroy attribution must stay wired into thread_ctl and lookup benchmark" >&2
    exit 1
fi

if ! grep -q 'RTMUX_TCTL_THREAD_DESTROY_STATS' internal/include/sys/rtmux-core.h ||
   ! grep -q 'uint64_t pin_waited;' internal/include/sys/rtmux-core.h ||
   ! grep -q 'uint64_t pin_skipped;' internal/include/sys/rtmux-core.h ||
   ! grep -q 'uint64_t active_scan_ns;' internal/include/sys/rtmux-core.h ||
   ! grep -q 'uint64_t object_free_heap_ns;' internal/include/sys/rtmux-core.h ||
   ! grep -q 'uint64_t object_free_cache_ns;' internal/include/sys/rtmux-core.h ||
   ! grep -q 'uint64_t object_free_cache_clear_ns;' internal/include/sys/rtmux-core.h ||
   ! grep -q 'uint64_t object_free_cache_link_ns;' internal/include/sys/rtmux-core.h ||
   ! grep -q 'uint64_t rcu_busy_idle;' internal/include/sys/rtmux-core.h ||
   ! grep -q 'uint64_t rcu_busy_sys;' internal/include/sys/rtmux-core.h ||
   ! grep -q 'uint64_t rcu_busy_channel;' internal/include/sys/rtmux-core.h ||
   ! grep -q 'uint64_t rcu_busy_connect;' internal/include/sys/rtmux-core.h ||
   ! grep -q 'uint64_t rcu_busy_interrupt;' internal/include/sys/rtmux-core.h ||
   ! grep -q 'uint64_t rcu_busy_sync;' internal/include/sys/rtmux-core.h ||
   ! grep -q 'uint64_t rcu_busy_dispatch;' internal/include/sys/rtmux-core.h ||
   ! grep -q 'uint64_t rcu_busy_timer;' internal/include/sys/rtmux-core.h ||
   ! grep -q 'uint64_t rcu_busy_fault;' internal/include/sys/rtmux-core.h ||
   ! grep -q 'uint64_t rcu_busy_priv;' internal/include/sys/rtmux-core.h ||
   ! grep -q 'uint64_t rcu_busy_heartbeat;' internal/include/sys/rtmux-core.h ||
   ! grep -q 'uint64_t rcu_busy_priv_system;' internal/include/sys/rtmux-core.h ||
   ! grep -q 'uint64_t rcu_busy_priv_vm;' internal/include/sys/rtmux-core.h ||
   ! grep -q 'uint64_t rcu_busy_priv_ipc;' internal/include/sys/rtmux-core.h ||
   ! grep -q 'uint64_t rcu_busy_priv_process;' internal/include/sys/rtmux-core.h ||
   ! grep -q 'uint64_t rcu_busy_priv_thread;' internal/include/sys/rtmux-core.h ||
   ! grep -q 'uint64_t rcu_busy_priv_pa;' internal/include/sys/rtmux-core.h ||
   ! grep -q 'uint64_t rcu_busy_priv_pa_alloc;' internal/include/sys/rtmux-core.h ||
   ! grep -q 'uint64_t rcu_busy_priv_pa_free;' internal/include/sys/rtmux-core.h ||
   ! grep -q 'uint64_t rcu_busy_priv_pa_zero;' internal/include/sys/rtmux-core.h ||
   ! grep -q 'uint64_t rcu_busy_priv_pa_info;' internal/include/sys/rtmux-core.h ||
   ! grep -q 'uint64_t rcu_busy_priv_pa_xlat;' internal/include/sys/rtmux-core.h ||
   ! grep -q 'uint64_t rcu_busy_priv_pa_pq;' internal/include/sys/rtmux-core.h ||
   ! grep -q 'uint64_t rcu_busy_unstamped;' internal/include/sys/rtmux-core.h ||
   ! grep -q 'uint64_t destroy_claim_active;' internal/include/sys/rtmux-core.h ||
   ! grep -q 'uint64_t destroy_claim_reserved;' internal/include/sys/rtmux-core.h ||
   ! grep -q 'uint64_t destroy_join_wake;' internal/include/sys/rtmux-core.h ||
   ! grep -q 'uint64_t destroy_join_wake_ns;' internal/include/sys/rtmux-core.h ||
   ! grep -q 'uint64_t destroy_join_post_ready;' \
    internal/include/sys/rtmux-core.h ||
   ! grep -q 'uint64_t destroy_join_post_ready_ns;' \
    internal/include/sys/rtmux-core.h ||
   ! grep -q 'uint64_t destroy_post_vector_ns;' \
    internal/include/sys/rtmux-core.h ||
   ! grep -q 'uint64_t destroy_post_cpu_ns;' \
    internal/include/sys/rtmux-core.h ||
   ! grep -q 'uint64_t destroy_post_name_ns;' \
    internal/include/sys/rtmux-core.h ||
   ! grep -q 'uint64_t destroy_post_notify_ns;' \
    internal/include/sys/rtmux-core.h ||
   [ "$(grep -c 'thread_destroy_stats_add(' kernel/thread/lifecycle.c)" -lt 2 ] ||
   ! grep -q 'thread_destroy_pin_waited' kernel/thread/lifecycle.c ||
   ! grep -q 'thread_destroy_pin_skipped' kernel/thread/lifecycle.c ||
   ! grep -q 'thread_destroy_active_scan_ns' kernel/thread/lifecycle.c ||
   ! grep -q 'thread_destroy_object_free_heap_ns' kernel/thread/lifecycle.c ||
   ! grep -q 'thread_destroy_object_free_cache_ns' kernel/thread/lifecycle.c ||
   ! grep -q 'thread_destroy_object_free_cache_clear_ns' \
    kernel/thread/lifecycle.c ||
   ! grep -q 'thread_destroy_object_free_cache_link_ns' \
    kernel/thread/lifecycle.c ||
   ! grep -q 'thread_destroy_rcu_busy_idle' kernel/thread/lifecycle.c ||
   ! grep -q 'thread_destroy_rcu_busy_sys' kernel/thread/lifecycle.c ||
   ! grep -q 'thread_destroy_rcu_busy_channel' kernel/thread/lifecycle.c ||
   ! grep -q 'thread_destroy_rcu_busy_connect' kernel/thread/lifecycle.c ||
   ! grep -q 'thread_destroy_rcu_busy_interrupt' kernel/thread/lifecycle.c ||
   ! grep -q 'thread_destroy_rcu_busy_sync' kernel/thread/lifecycle.c ||
   ! grep -q 'thread_destroy_rcu_busy_dispatch' kernel/thread/lifecycle.c ||
   ! grep -q 'thread_destroy_rcu_busy_timer' kernel/thread/lifecycle.c ||
   ! grep -q 'thread_destroy_rcu_busy_fault' kernel/thread/lifecycle.c ||
   ! grep -q 'thread_destroy_rcu_busy_priv' kernel/thread/lifecycle.c ||
   ! grep -q 'thread_destroy_rcu_busy_heartbeat' kernel/thread/lifecycle.c ||
   ! grep -q 'thread_destroy_rcu_busy_priv_system' kernel/thread/lifecycle.c ||
   ! grep -q 'thread_destroy_rcu_busy_priv_vm' kernel/thread/lifecycle.c ||
   ! grep -q 'thread_destroy_rcu_busy_priv_ipc' kernel/thread/lifecycle.c ||
   ! grep -q 'thread_destroy_rcu_busy_priv_process' kernel/thread/lifecycle.c ||
   ! grep -q 'thread_destroy_rcu_busy_priv_thread' kernel/thread/lifecycle.c ||
   ! grep -q 'thread_destroy_rcu_busy_priv_pa' kernel/thread/lifecycle.c ||
   ! grep -q 'thread_destroy_rcu_busy_priv_pa_alloc' kernel/thread/lifecycle.c ||
   ! grep -q 'thread_destroy_rcu_busy_priv_pa_free' kernel/thread/lifecycle.c ||
   ! grep -q 'thread_destroy_rcu_busy_priv_pa_zero' kernel/thread/lifecycle.c ||
   ! grep -q 'thread_destroy_rcu_busy_priv_pa_info' kernel/thread/lifecycle.c ||
   ! grep -q 'thread_destroy_rcu_busy_priv_pa_xlat' kernel/thread/lifecycle.c ||
   ! grep -q 'thread_destroy_rcu_busy_priv_pa_pq' kernel/thread/lifecycle.c ||
   ! grep -q 'thread_destroy_rcu_busy_unstamped' kernel/thread/lifecycle.c ||
   ! grep -q 'thread_destroy_claim_active' kernel/thread/lifecycle.c ||
   ! grep -q 'thread_destroy_claim_reserved' kernel/thread/lifecycle.c ||
   ! grep -q 'thread_destroy_join_wake' kernel/thread/lifecycle.c ||
   ! grep -q 'thread_destroy_join_wake_ns' kernel/thread/lifecycle.c ||
   ! grep -q 'thread_destroy_join_post_ready' kernel/thread/lifecycle.c ||
   ! grep -q 'thread_destroy_join_post_ready_ns' kernel/thread/lifecycle.c ||
   ! grep -q 'thread_destroy_post_vector_ns' kernel/thread/lifecycle.c ||
   ! grep -q 'thread_destroy_post_cpu_ns' kernel/thread/lifecycle.c ||
   ! grep -q 'thread_destroy_post_name_ns' kernel/thread/lifecycle.c ||
   ! grep -q 'thread_destroy_post_notify_ns' kernel/thread/lifecycle.c ||
   ! grep -q 'THREAD_DESTROY_CLAIM_RESERVED_BASE' kernel/thread/lifecycle.c ||
   ! grep -q 'thread_destroy_stats_enabled' kernel/thread/lifecycle.c ||
   ! grep -q 'if (reset == 1)' kernel/thread/lifecycle.c ||
   ! grep -q 'thread_destroy_stats_disable' \
    tests/userland/rt-latency/rcu-lookup-measure.c ||
   ! grep -q 'stats.reset = 2' \
    tests/userland/rt-latency/rcu-lookup-measure.c ||
   ! grep -q 'object_free_track_heap_detail(' \
    kernel/thread/lifecycle.c ||
   ! grep -q 'object_free(prp, &thread_souls, thp)' kernel/thread/lifecycle.c ||
   grep -q 'thread_destroy_will_heap_free' kernel/thread/lifecycle.c ||
   ! grep -q 'void rdecl object_free_track_heap' kernel/object/core.c ||
   ! grep -q 'void rdecl object_free_track_heap_detail' kernel/object/core.c ||
   ! grep -q 'object_free_track_heap(prp, soulp, ptr, NULL)' \
    kernel/object/core.c ||
   ! grep -q 'act->process->cred->info.euid != 0' kernel/sched/thread.c ||
   ! grep -q 'thread-destroy-object-free-ns label=%s' \
    tests/userland/rt-latency/rcu-lookup-measure.c ||
   ! grep -q 'thread-destroy-object-free-cache label=%s' \
    tests/userland/rt-latency/rcu-lookup-measure.c ||
   ! grep -q 'run_prepared("thread-join-post-store-self-stats"' \
    tests/userland/rt-latency/rcu-lookup-measure.c ||
   ! grep -q 'run_prepared("thread-join-post-store-pid-stats"' \
    tests/userland/rt-latency/rcu-lookup-measure.c ||
   ! grep -q 'rcu-post-scan-ns=%llu pin-wait-ns=%llu' \
    tests/userland/rt-latency/rcu-lookup-measure.c ||
   ! grep -q 'active-scan-ns=%llu object-free-ns=%llu' \
    tests/userland/rt-latency/rcu-lookup-measure.c ||
   ! grep -q 'object-free-ns=%llu heap=%llu' \
    tests/userland/rt-latency/rcu-lookup-measure.c ||
   ! grep -q 'thread-destroy-pin label=%s' \
    tests/userland/rt-latency/rcu-lookup-measure.c ||
   ! grep -q 'thread-destroy-rcu-detail label=%s' \
    tests/userland/rt-latency/rcu-lookup-measure.c ||
   ! grep -q 'enroll-ns=%llu scan-ns=%llu post-scan-ns=%llu' \
    tests/userland/rt-latency/rcu-lookup-measure.c ||
   ! grep -q 'stats.rcu_enroll_ns + stats.rcu_scan_ns + stats.rcu_post_scan_ns' \
    tests/userland/rt-latency/rcu-lookup-measure.c ||
   ! grep -q 'idle=%llu "' \
    tests/userland/rt-latency/rcu-lookup-measure.c ||
   ! grep -q 'sys=%llu channel=%llu connect=%llu "' \
    tests/userland/rt-latency/rcu-lookup-measure.c ||
   ! grep -q 'interrupt=%llu sync=%llu sched=%llu' \
    tests/userland/rt-latency/rcu-lookup-measure.c ||
   ! grep -q 'dispatch=%llu timer=%llu fault=%llu priv=%llu "' \
    tests/userland/rt-latency/rcu-lookup-measure.c ||
   ! grep -q 'heartbeat=%llu unstamped=%llu other=%llu' \
    tests/userland/rt-latency/rcu-lookup-measure.c ||
   ! grep -q 'thread-destroy-rcu-priv label=%s' \
    tests/userland/rt-latency/rcu-lookup-measure.c ||
   ! grep -q 'system=%llu vm=%llu ipc=%llu process=%llu thread=%llu pa=%llu' \
    tests/userland/rt-latency/rcu-lookup-measure.c ||
   ! grep -q 'thread-destroy-rcu-priv-pa label=%s' \
    tests/userland/rt-latency/rcu-lookup-measure.c ||
   ! grep -q 'alloc=%llu free=%llu zero=%llu info=%llu xlat=%llu pq=%llu' \
    tests/userland/rt-latency/rcu-lookup-measure.c ||
   ! grep -q 'stats.destroy_claim_active + stats.destroy_claim_reserved != calls' \
    tests/userland/rt-latency/rcu-lookup-measure.c ||
   ! grep -q 'thread_destroy_expect_active_claim(label)' \
    tests/userland/rt-latency/rcu-lookup-measure.c ||
   ! grep -q 'stats.destroy_claim_active != calls' \
    tests/userland/rt-latency/rcu-lookup-measure.c ||
   ! grep -q 'stats.destroy_claim_reserved != 0' \
    tests/userland/rt-latency/rcu-lookup-measure.c ||
   ! grep -q 'stats.destroy_join_wake != calls' \
    tests/userland/rt-latency/rcu-lookup-measure.c ||
   ! grep -q 'stats.destroy_join_wake_ns == 0' \
    tests/userland/rt-latency/rcu-lookup-measure.c ||
   ! grep -q 'stats.destroy_join_post_ready != calls' \
    tests/userland/rt-latency/rcu-lookup-measure.c ||
   ! grep -q 'stats.destroy_join_post_ready_ns == 0' \
    tests/userland/rt-latency/rcu-lookup-measure.c ||
   ! grep -q 'stats.destroy_post_vector_ns == 0' \
    tests/userland/rt-latency/rcu-lookup-measure.c ||
	   ! grep -q 'stats.destroy_post_cpu_ns == 0' \
	    tests/userland/rt-latency/rcu-lookup-measure.c ||
	   ! grep -q 'post_ready_named_ns > stats.destroy_join_post_ready_ns' \
	    tests/userland/rt-latency/rcu-lookup-measure.c ||
	   ! grep -q 'post_ready_residual_ns =' \
	    tests/userland/rt-latency/rcu-lookup-measure.c ||
	   ! grep -q 'thread-destroy-claim label=%s' \
	    tests/userland/rt-latency/rcu-lookup-measure.c ||
   ! grep -q 'active=%llu reserved=%llu' \
    tests/userland/rt-latency/rcu-lookup-measure.c ||
   ! grep -q 'thread-destroy-join-wake label=%s' \
    tests/userland/rt-latency/rcu-lookup-measure.c ||
   ! grep -q 'count=%llu ns=%llu' \
    tests/userland/rt-latency/rcu-lookup-measure.c ||
   ! grep -q 'thread-destroy-join-post-ready label=%s' \
    tests/userland/rt-latency/rcu-lookup-measure.c ||
	   ! grep -q 'thread-destroy-post-ready-detail label=%s' \
	    tests/userland/rt-latency/rcu-lookup-measure.c ||
	   ! grep -q 'vector-ns=%llu cpu-ns=%llu name-ns=%llu notify-ns=%llu' \
	    tests/userland/rt-latency/rcu-lookup-measure.c ||
	   ! grep -q 'thread-destroy-post-ready-residual label=%s' \
	    tests/userland/rt-latency/rcu-lookup-measure.c ||
	   ! grep -q 'named-ns=%llu residual-ns=%llu notify-ns=%llu' \
	    tests/userland/rt-latency/rcu-lookup-measure.c ||
	   ! grep -q 'unstamped=%llu other=%llu' \
	    tests/userland/rt-latency/rcu-lookup-measure.c ||
   ! grep -q 'thread_destroy_stats_report("thread-join-post-store-self"' \
    tests/userland/rt-latency/rcu-lookup-measure.c ||
   ! grep -q 'thread_destroy_stats_report("thread-join-post-store-pid"' \
    tests/userland/rt-latency/rcu-lookup-measure.c ||
   ! grep -q '(unsigned long) samples_n' \
    tests/userland/rt-latency/rcu-lookup-measure.c; then
    echo "RCU-sched: thread destroy attribution must stay wired into thread_ctl and lookup benchmark" >&2
    exit 1
fi

if ! grep -q 'thread_recycle_reset' kernel/thread/lifecycle.c; then
    if ! awk '
        /object_free_track_heap_common\(process_t \*prp,/ { in_common = 1 }
        in_common && /memset\(ptr, 0, soulp->size \+ soulp->align\);/ {
            saw_clear = NR
        }
        in_common && /soulp->next = ptr;/ && saw_clear {
            saw_link_after_clear = NR
        }
        END {
            exit !(saw_clear && saw_link_after_clear &&
                   saw_clear < saw_link_after_clear)
        }
    ' kernel/object/core.c; then
        echo "RCU-sched: cached soul zeroing cannot be weakened without a thread_recycle_reset proof" >&2
        exit 1
    fi
else
    if ! grep -q 'SOUL_NOCLEAR' kernel/include/core/globals.h ||
       ! grep -q 'thread_recycle_reset(thp)' kernel/thread/lifecycle.c ||
       ! grep -q 'skip_clear = (soulp->flags & SOUL_NOCLEAR)' \
        kernel/object/core.c ||
       ! grep -q 'soulp != &thread_souls' kernel/object/core.c ||
       ! grep -Fq 'TYPE_MASK(((thread_t *) ptr)->type) == TYPE_THREAD' \
        kernel/object/core.c; then
        echo "RCU-sched: thread_souls no-clear needs reset and TYPE_THREAD guard" >&2
        exit 1
    fi
fi

if ! awk '
    /thread_destroy_stats_disable\(\) == -1/ && !saw_measure_self {
        saw_disable_self = NR
    }
    /measure_prepared\("thread-join-post-store-self"/ { saw_measure_self = NR }
    /thread_destroy_stats_reset\(\) == -1/ && saw_measure_self && !saw_reset_self {
        saw_reset_self = NR
    }
    /run_prepared\("thread-join-post-store-self-stats"/ {
        saw_stats_self = NR
    }
    /thread_destroy_stats_disable\(\) == -1/ && saw_stats_self &&
        !saw_measure_pid {
        saw_disable_pid = NR
    }
    /measure_prepared\("thread-join-post-store-pid"/ { saw_measure_pid = NR }
    /thread_destroy_stats_reset\(\) == -1/ && saw_measure_pid && !saw_reset_pid {
        saw_reset_pid = NR
    }
    /run_prepared\("thread-join-post-store-pid-stats"/ {
        saw_stats_pid = NR
    }
    END {
        exit !(saw_disable_self && saw_measure_self && saw_reset_self &&
               saw_stats_self && saw_disable_self < saw_measure_self &&
               saw_measure_self < saw_reset_self &&
               saw_reset_self < saw_stats_self &&
               saw_disable_pid && saw_measure_pid && saw_reset_pid &&
               saw_stats_pid && saw_stats_self < saw_disable_pid &&
               saw_disable_pid < saw_measure_pid && saw_measure_pid < saw_reset_pid &&
               saw_reset_pid < saw_stats_pid)
    }
' tests/userland/rt-latency/rcu-lookup-measure.c; then
    echo "RCU-sched: post-store latency rows must run before destroy stats instrumentation" >&2
    exit 1
fi

if ! awk '
    /int kdecl ksys_sched_get\(thread_t \*act,/ {
        in_get = 1
    }
    in_get && /kap->pid == 0 \|\| kap->pid == act->process->pid/ { saw_self = NR }
    in_get && /lookup_pid_rcu\(kap->pid\)/ { saw_prp_rcu = NR }
    in_get && /rcu_sched_read_unlock\(\);/ { saw_prp_unlock = NR }
    in_get && /if \(prp != act->process\)/ { saw_local = NR }
    in_get && /return EPERM;/ && NR == saw_local + 1 { saw_denied = NR }
    in_get && /rcu_read_lock\(&tthread_rcu\);/ { saw_thp_lock = NR }
    in_get && /vector_lookup\(&prp->threads, \(unsigned\) \(kap->tid - 1\)\)/ {
        saw_vec = NR
    }
    in_get && /rcu_read_unlock\(&tthread_rcu\);/ { saw_thp_unlock = NR }
    in_get && /user_slice_init\(act, kap->param, sizeof\(param\), sizeof\(param\),/ { saw_validate = NR }
    in_get && /USER_SLICE_WR, &param_slice\)/ { saw_write = NR }
    in_get && /return EFAULT;/ && NR == saw_write + 1 { saw_fault = NR }
    in_get && /user_slice_copyout\(&param_slice, &param\)/ && !saw_copy { saw_copy = NR }
    in_get && /lookup_pid\(kap->pid\)/ { saw_plain = NR }
    in_get && /^}/ { in_get = 0 }
    END {
        if (!saw_self) print "RCU detail: get self" > "/dev/stderr"
        if (!saw_prp_rcu) print "RCU detail: get lookup" > "/dev/stderr"
        if (!saw_denied) print "RCU detail: get denial" > "/dev/stderr"
        if (!saw_write) print "RCU detail: get validation" > "/dev/stderr"
        if (!saw_fault) print "RCU detail: get fault" > "/dev/stderr"
        if (!saw_copy) print "RCU detail: get copy" > "/dev/stderr"
        else if (saw_thp_unlock && saw_copy < saw_thp_unlock)
            print "RCU detail: get early copy" > "/dev/stderr"
        exit !(saw_validate && saw_write && saw_fault && saw_self && saw_prp_rcu && saw_prp_unlock &&
               saw_local && saw_denied && saw_thp_lock && saw_vec && saw_thp_unlock &&
               saw_copy &&
               saw_validate < saw_write && saw_write < saw_fault && saw_fault < saw_self &&
               saw_self < saw_prp_rcu &&
               saw_prp_rcu < saw_prp_unlock && saw_prp_unlock < saw_local &&
               saw_local < saw_denied && saw_denied < saw_thp_lock &&
               saw_thp_lock < saw_vec && saw_vec < saw_thp_unlock &&
               saw_thp_unlock < saw_copy &&
               !saw_plain)
    }
' kernel/sched/policy.c; then
    echo "RCU-sched: sched_get pid/tid lookup must snapshot under RCU and copy out after unlock" >&2
    exit 1
fi

if ! awk '
    /int kdecl ksys_sched_set\(thread_t \*act,/ {
        in_set = 1
    }
    in_set && /kap->pid == 0 \|\| kap->pid == act->process->pid/ { saw_self = NR }
    in_set && /lookup_pid_rcu\(kap->pid\)/ { saw_prp_rcu = NR }
    in_set && /rcu_sched_read_unlock\(\);/ { saw_prp_unlock = NR }
    in_set && /if \(prp != act->process\)/ { saw_local = NR }
    in_set && /return EPERM;/ && NR == saw_local + 1 { saw_denied = NR }
    in_set && /rcu_read_lock\(&tthread_rcu\);/ && !saw_thp_lock {
        saw_thp_lock = NR
    }
    in_set && /vector_lookup\(&prp->threads, \(unsigned\) \(kap->tid - 1\)\)/ && !saw_copyin {
        saw_initial_vec = NR
    }
    in_set && /user_slice_get\(act, &param, kap->param, sizeof\(param\)\)/ { saw_copyin = NR }
    in_set && /user_slice_get\(act, &param.sched_priority,/ { saw_priority_copy = NR }
    in_set && /&kap->param->sched_priority,/ { saw_priority_source = NR }
    in_set && /sizeof\(param.sched_priority\)\)/ { saw_priority_size = NR }
    in_set && /process_pin\(prp\)/ { saw_prp_pin = NR }
    in_set && saw_copyin &&
        /vector_lookup\(&prp->threads, \(unsigned\) \(kap->tid - 1\)\)/ {
        saw_pinned_vec = NR
    }
    in_set && /tthread_pin\(thp\)/ { saw_pin = NR }
    in_set && /rcu_read_unlock\(&tthread_rcu\);/ && !saw_copyin {
        saw_initial_unlock = NR
    }
    in_set && /rcu_read_unlock\(&tthread_rcu\);/ && saw_copyin {
        saw_pinned_unlock = NR
    }
    in_set && /sched_thread\(thp, kap->policy, &param\)/ { saw_sched = NR }
    in_set && /tthread_unpin\(thp\)/ { saw_unpin = NR }
    in_set && /process_unpin\(prp\)/ { saw_prp_unpin = NR }
    in_set && /lookup_pid\(kap->pid\)/ { saw_plain = NR }
    in_set && /^}/ { in_set = 0 }
    END {
        if (!saw_self) print "RCU detail: set self" > "/dev/stderr"
        if (!saw_prp_rcu) print "RCU detail: set lookup" > "/dev/stderr"
        if (!saw_denied) print "RCU detail: set denial" > "/dev/stderr"
        if (!saw_copyin) print "RCU detail: set sporadic copy" > "/dev/stderr"
        if (!saw_priority_copy) print "RCU detail: set priority copy" > "/dev/stderr"
        if (!saw_prp_pin) print "RCU detail: set process pin" > "/dev/stderr"
        exit !(saw_self && saw_prp_rcu && saw_prp_unlock && saw_local && saw_denied &&
               saw_thp_lock && saw_initial_vec &&
               saw_initial_unlock && saw_copyin && saw_priority_copy &&
               saw_priority_source && saw_priority_size && saw_prp_pin &&
               saw_pinned_vec && saw_pin && saw_pinned_unlock && saw_sched &&
               saw_unpin && saw_prp_unpin &&
               saw_self < saw_prp_rcu && saw_prp_rcu < saw_prp_unlock &&
               saw_prp_unlock < saw_local && saw_local < saw_denied &&
               saw_denied < saw_thp_lock && saw_thp_lock < saw_initial_vec &&
               saw_initial_vec < saw_initial_unlock &&
               saw_initial_unlock < saw_copyin && saw_copyin < saw_prp_pin &&
               saw_initial_unlock < saw_priority_copy && saw_priority_copy < saw_prp_pin &&
               saw_priority_copy < saw_priority_source && saw_priority_source < saw_priority_size &&
               saw_priority_size < saw_prp_pin &&
               saw_prp_pin < saw_pinned_vec && saw_pinned_vec < saw_pin &&
               saw_pin < saw_pinned_unlock && saw_pinned_unlock < saw_sched &&
               saw_sched < saw_unpin && saw_sched < saw_prp_unpin &&
               !saw_plain)
    }
' kernel/sched/policy.c; then
    echo "RCU-sched: sched_set pid/tid path must copy in outside RCU, then pin process/thread before sched_thread" >&2
    exit 1
fi

if ! awk '
    /static int sched_set_noop_status\(thread_t \*thp,/ {
        in_noop = 1
    }
    in_noop && /policy == RTMUX_SCHED_SETPRIO/ { setprio = NR }
    in_noop && setprio && /thp->policy == SCHED_SPORADIC/ {
        saw_setprio_sporadic = NR
    }
    in_noop && setprio && !policy_exclude &&
        /param->sched_priority == thp->real_priority &&/ {
        saw_setprio_base = NR
    }
    in_noop && setprio && !policy_exclude &&
        /thp->priority == thp->real_priority/ {
        saw_setprio_eff = NR
    }
    in_noop && /if \(policy == SCHED_SPORADIC \|\| policy == RTMUX_SCHED_ADJTOHEAD \|\|/ {
        policy_exclude = NR
    }
    in_noop && policy_exclude && /policy == RTMUX_SCHED_ADJTOTAIL/ {
        saw_policy_exclude_tail = NR
    }
    in_noop && policy_exclude && !saw_policy_exclude_return &&
        /return 0;/ {
        saw_policy_exclude_return = NR
    }
    in_noop && /policy == thp->policy &&/ { saw_policy = NR }
    in_noop && saw_policy &&
        /param->sched_priority == thp->real_priority &&/ {
        saw_policy_base = NR
    }
    in_noop && saw_policy && /thp->priority == thp->real_priority/ {
        saw_policy_eff = NR
    }
    in_noop && /^}/ { in_noop = 0 }
    END {
        exit !(setprio && saw_setprio_sporadic && saw_setprio_base &&
               saw_setprio_eff && policy_exclude &&
               saw_policy_exclude_tail && saw_policy_exclude_return &&
               saw_policy && saw_policy_base && saw_policy_eff &&
               setprio < saw_setprio_sporadic &&
               saw_setprio_sporadic < saw_setprio_base &&
               saw_setprio_eff == saw_setprio_base + 1 &&
               saw_setprio_eff < policy_exclude &&
               policy_exclude < saw_policy_exclude_tail &&
               saw_policy_exclude_tail < saw_policy_exclude_return &&
               saw_policy_exclude_return < saw_policy &&
               saw_policy < saw_policy_base &&
               saw_policy_eff == saw_policy_base + 1)
    }
' kernel/sched/policy.c; then
    echo "RCU-sched: sched_set no-op fast path must not skip boosted, sporadic, or queue-reposition states" >&2
    exit 1
fi

if ! awk '
    /int kdecl ksys_thread_create\(thread_t \*act,/ {
        in_create = 1
    }
    in_create && /user_slice_get\(act, &attr_copy, kap->attr, sizeof attr_copy\)/ { saw_probe = NR }
    in_create && /lookup_pid_rcu\(kap->pid\)/ { saw_prp_rcu = NR }
    in_create && /process_pin\(prp\)/ { saw_pin = NR }
    in_create && /prp_pinned = true;/ { saw_pin_flag = NR }
    in_create && /rcu_sched_read_unlock\(\);/ { saw_unlock = NR }
    in_create && /rcu_read_unlock\(&process_rcu\);/ { saw_old_unlock = NR }
    in_create && /thread_create\(act, prp, &event, THREAD_CREATE_BLOCK_FLAG,/ {
        saw_create = NR
    }
    in_create && /kap->attr \? &attr_copy : NULL\)/ { saw_attr_arg = NR }
    in_create && /^out:/ { saw_out = NR }
    in_create && /if \(prp_pinned\)/ { saw_unpin_guard = NR }
    in_create && /process_unpin\(prp\)/ { saw_unpin = NR }
    in_create && /RD_VERIFY_PTR\(act, kap->attr/ { saw_direct_verify = NR }
    in_create && /lookup_pid\(kap->pid\)/ { saw_plain = NR }
    in_create && /^}/ { in_create = 0 }
    END {
        if (!saw_attr_arg) print "RCU detail: create attrs" > "/dev/stderr"
        if (!saw_unpin_guard) print "RCU detail: create pin guard" > "/dev/stderr"
        exit !(saw_prp_rcu && saw_pin && saw_pin_flag && saw_unlock &&
               saw_probe && saw_create && saw_attr_arg && saw_out && saw_unpin_guard && saw_unpin &&
               saw_probe < saw_prp_rcu &&
               saw_prp_rcu < saw_pin && saw_pin < saw_pin_flag && saw_pin_flag < saw_unlock &&
               saw_unlock < saw_create &&
               saw_create < saw_attr_arg && saw_attr_arg < saw_out &&
               saw_out < saw_unpin_guard && saw_unpin_guard < saw_unpin &&
               !saw_direct_verify && !saw_plain && !saw_old_unlock)
    }
' kernel/sched/thread.c; then
    echo "RCU-sched: thread_create explicit pid lookup must probe attrs before pinning, then close RCU before thread_create" >&2
    exit 1
fi

if ! awk '
    /int kdecl ksys_thread_join\(thread_t \*act,/ {
        in_join = 1
    }
    in_join && /kspinlock_lock\(&prp->vec_slock\);/ { saw_lock = NR }
    in_join && /ITF_SET\(act, RTMUX_ITF_WAIT_PENDING\);/ { saw_wait = NR }
    in_join && saw_wait && /cpu_smp_mb\(\);/ && !saw_publish {
        saw_publish_mb = NR
    }
    in_join && /thp->join = act;/ { saw_publish = NR }
    in_join && /kspinlock_unlock\(&prp->vec_slock\);/ && saw_publish {
        saw_unlock = NR
    }
    in_join && /unready\(act, STATE_JOIN\);/ { saw_unready = NR }
    in_join && /act->blocked_on = thp;/ { saw_blocked = NR }
    in_join && /ITF_CLR\(act, RTMUX_ITF_WAIT_PENDING\);/ { saw_clear = NR }
    in_join && /^}/ { in_join = 0 }
    END {
        exit !(saw_lock && saw_wait && saw_publish_mb && saw_publish &&
               saw_unlock && saw_unready && saw_blocked && saw_clear &&
               saw_lock < saw_wait && saw_wait < saw_publish_mb &&
               saw_publish_mb < saw_publish &&
               saw_publish < saw_unlock && saw_unlock < saw_unready &&
               saw_unready < saw_blocked && saw_blocked < saw_clear)
    }
' kernel/sched/thread.c; then
    echo "RCU-sched: live thread_join must publish joiner under vec_slock and bridge block transition with WAIT_PENDING" >&2
    exit 1
fi

if ! grep -q 'thread_join_wait_ready(joiner)' kernel/sched/thread.c ||
   ! grep -q 'thread_join_wait_ready(jthp)' kernel/thread/lifecycle.c ||
	   ! awk '
	       /void rdecl thread_destroy\(thread_t \*thp\)/ { in_destroy = 1 }
	       # Capture the lock protecting the join snapshot, not an earlier lock.
	       in_destroy && /kspinlock_lock\(&prp->vec_slock\);/ {
	           if (!saw_snapshot)
	               saw_lock = NR
	       }
	       in_destroy && /jthp = thp->join;/ { saw_snapshot = NR }
	       in_destroy && /tthread_pin\(jthp\)/ { saw_pin = NR }
	       in_destroy && /thread_join_wait_ready\(jthp\)/ { saw_wait = NR }
	       in_destroy && /if \(thp->join == jthp\)/ { saw_guard = NR }
	       in_destroy && /thp->join = NULL;/ { saw_clear = NR }
	       in_destroy && /^[[:space:]]*join_owned = 1;/ { saw_owned = NR }
	       in_destroy && /if \(join_owned\)/ { saw_owned_wake = NR }
	       in_destroy && /thread_join_wait_ready\(jthp\)/ { saw_wait = NR }
	       in_destroy && /^[[:space:]]*ready\(jthp\);/ { saw_ready = NR }
	       in_destroy && /tthread_unpin\(jthp\)/ && saw_ready && !saw_unpin { saw_unpin = NR }
	       in_destroy && /^}/ { in_destroy = 0 }
	       END {
	           if (!saw_snapshot) print "RCU detail: join snapshot" > "/dev/stderr"
	           exit !(saw_lock && saw_snapshot && saw_pin && saw_wait &&
	                  saw_guard && saw_clear && saw_owned && saw_owned_wake &&
	                  saw_ready && saw_unpin &&
	                  saw_lock < saw_snapshot && saw_snapshot < saw_pin &&
	                  saw_pin < saw_wait && saw_wait < saw_guard &&
	                  saw_guard < saw_clear && saw_clear < saw_owned &&
	                  saw_owned < saw_owned_wake && saw_owned_wake < saw_ready &&
	                  saw_ready < saw_unpin)
	       }
	   ' kernel/thread/lifecycle.c ||
   ! awk '
       /void rdecl thread_specret\(thread_t \*thp\)/ { in_specret = 1 }
       in_specret && /thp->join && thp->join->state == STATE_WAITTHREAD/ {
           saw_waitthread_only = NR
       }
       in_specret && /thp->join = NULL;/ { saw_clear = NR }
       in_specret && /^}/ { in_specret = 0 }
       END { exit !(saw_waitthread_only && saw_clear &&
                    saw_waitthread_only < saw_clear) }
   ' kernel/thread/lifecycle.c ||
   ! awk '
       /case STATE_JOIN:/ { in_join = 1 }
       in_join && /RTMUX_ITF_WAIT_PENDING/ { saw_wait = NR }
       in_join && /cpu_smp_mb\(\);/ { saw_mb = NR }
       in_join && /kspinlock_lock\(&prp->vec_slock\);/ { saw_lock = NR }
       in_join && /if \(target->join == thp\)/ { saw_guard = NR }
       in_join && /target->join = NULL;/ {
           saw_clear = NR
       }
       in_join && /kspinlock_unlock\(&prp->vec_slock\);/ { saw_unlock = NR }
       in_join && /break;/ { in_join = 0 }
       END { exit !(saw_wait && saw_mb && saw_lock && saw_guard &&
                    saw_clear && saw_unlock &&
                    saw_wait < saw_mb && saw_mb < saw_lock &&
                    saw_lock < saw_guard && saw_guard < saw_clear &&
                    saw_clear < saw_unlock) }
   ' kernel/sched/dispatch.c; then
    echo "RCU-sched: join wake paths must wait for WAIT_PENDING before consuming blocked_on" >&2
    exit 1
fi

if ! awk '
    /int kdecl ksys_timer_info\(thread_t \*act,/ {
        in_timer = 1
    }
    in_timer && /lookup_pid_rcu\(kap->pid\)/ && !saw_prp_rcu {
        saw_prp_rcu = NR
    }
    in_timer && /if \(kap->flags & RTMUX_TIMER_SEARCH\)/ { saw_timer_if = NR }
    in_timer && /vector_search\(&prp->timers, id, &id\)/ { saw_timer_search = NR }
    in_timer && /} else {/ && saw_timer_search && !saw_timer_else {
        saw_timer_else = NR
    }
    in_timer && /vector_lookup2\(&prp->timers, id\)/ { saw_timer_vec = NR }
    in_timer && /xfer_probe\(inp, sizeof\(\*inp\)/ { saw_probe = NR }
    in_timer && /info.overruns = tip->overruns;/ { saw_snapshot = NR }
    in_timer && /xfer_memcpy\(inp, &info, sizeof\(info\)\)/ { saw_copy = NR }
    in_timer && /tip->overruns = 0;/ { saw_reset = NR }
    in_timer && /rcu_read_unlock\(&process_rcu\);/ && !saw_unlock {
        saw_unlock = NR
    }
    in_timer && /\*inp = info;/ { saw_raw_store = NR }
    in_timer && /lookup_pid\(kap->pid\)/ { saw_plain = NR }
    in_timer && /reset_prp|reset_tip/ { saw_second_lookup = NR }
    in_timer && /^}/ { in_timer = 0 }
    END {
        exit !(saw_prp_rcu && saw_timer_if && saw_timer_search &&
               saw_timer_else && saw_timer_vec && saw_probe &&
               saw_snapshot && saw_copy && saw_reset && saw_unlock &&
               saw_prp_rcu < saw_timer_if &&
               saw_timer_if < saw_timer_search &&
               saw_timer_search < saw_timer_else &&
               saw_timer_else < saw_timer_vec &&
               saw_timer_vec < saw_probe &&
               saw_probe < saw_snapshot &&
               saw_snapshot < saw_copy &&
               saw_copy < saw_reset &&
               saw_reset < saw_unlock &&
               !saw_plain && !saw_raw_store && !saw_second_lookup)
    }
' kernel/time/timer.c; then
    echo "RCU-sched: timer_info must snapshot tip under process_rcu and copy out with fault-safe xfer_memcpy before clearing overruns" >&2
    exit 1
fi

if ! awk '
    /int kdecl ksys_clock_id\(thread_t \*act,/ {
        in_clock = 1
    }
    in_clock && /kap->pid == 0 \|\| kap->pid == act->process->pid/ {
        saw_self_pid = NR
    }
    in_clock && /tid == 0 \|\| tid == \(int\) \(act->tid \+ 1\)/ {
        saw_self_tid = NR
    }
    in_clock && /KSTATUS\(act\) = MAKE_CLOCKID\(act->process->pid, tid\);/ {
        saw_self_status = NR
    }
    in_clock && /lookup_pid_rcu\(kap->pid\)/ { saw_prp_rcu = NR }
    in_clock && /prp_rcu = 1;/ { saw_prp_guard = NR }
    in_clock && /rcu_read_lock\(&tthread_rcu\);/ { saw_thp_lock = NR }
    in_clock && /thp_rcu = 1;/ { saw_thp_guard = NR }
    in_clock && /thp = vector_lookup\(&prp->threads, \(unsigned\) \(tid - 1\)\)/ {
        saw_thread_vec = NR
    }
    in_clock && /thp->tid != \(uint32_t\) \(tid - 1\)/ {
        saw_tid_check = NR
    }
    in_clock && /id = MAKE_CLOCKID\(prp->pid, tid\);/ { saw_snapshot = NR }
    in_clock && /rcu_read_unlock\(&tthread_rcu\);/ && saw_snapshot {
        saw_thp_unlock = NR
    }
    in_clock && /rcu_read_unlock\(&process_rcu\);/ && saw_snapshot {
        saw_unlock = NR
    }
    in_clock && /KSTATUS\(act\) = id;/ { saw_status = NR }
    in_clock && /lookup_pid\(kap->pid\)/ { saw_plain = NR }
    in_clock && /^}/ { in_clock = 0 }
    END {
        exit !(saw_self_pid && saw_self_tid && saw_self_status &&
               saw_prp_rcu && saw_prp_guard && saw_thp_lock && saw_thp_guard &&
               saw_thread_vec &&
               saw_tid_check && saw_snapshot && saw_thp_unlock && saw_unlock &&
               saw_status &&
               saw_self_pid < saw_self_tid &&
               saw_self_tid < saw_self_status &&
               saw_self_status < saw_prp_rcu &&
               saw_prp_rcu < saw_prp_guard &&
               saw_prp_guard < saw_thp_lock &&
               saw_thp_lock < saw_thp_guard &&
               saw_thp_guard < saw_thread_vec &&
               saw_thread_vec < saw_tid_check &&
               saw_tid_check < saw_snapshot &&
               saw_snapshot < saw_thp_unlock &&
               saw_snapshot < saw_unlock &&
               saw_thp_unlock < saw_status &&
               saw_unlock < saw_status &&
               !saw_plain)
    }
' kernel/time/clock.c; then
    echo "RCU-sched: clock_id must fast-path self and snapshot pid/tid under process/tthread RCU" >&2
    exit 1
fi

if ! awk '
    /void kext_query_object_done\(void \*data\)/ { in_done = 1 }
    in_done && /lookup_pid_rcu\(pid\)/ { saw_rcu = NR }
    in_done && /prp->querying = false;/ { saw_store = NR }
    in_done && /rcu_read_unlock\(&process_rcu\);/ { saw_unlock = NR }
    in_done && /lookup_pid\(pid\)/ { saw_plain = NR }
    in_done && /^}/ { in_done = 0 }
    END {
        exit !(saw_rcu && saw_store && saw_unlock &&
               saw_rcu < saw_store && saw_store < saw_unlock && !saw_plain)
    }
' kernel/ext/query.c; then
    echo "RCU-sched: query_object_done must clear querying under process_rcu" >&2
    exit 1
fi

if ! awk '
    /void kext_cred_dirty\(void \*data\)/ { in_dirty = 1 }
    in_dirty && /lookup_pid_rcu\(kap->pid\)/ { saw_rcu = NR }
    in_dirty && /kspinlock_lock\(&cred_slock\);/ { saw_lock = NR }
    in_dirty && /cred_dirty\(prp\);/ { saw_dirty = NR }
    in_dirty && /kspinlock_unlock\(&cred_slock\);/ { saw_unlock_lock = NR }
    in_dirty && /rcu_read_unlock\(&process_rcu\);/ && !saw_dirty {
        saw_early_unlock = NR
    }
    in_dirty && /rcu_read_unlock\(&process_rcu\);/ && saw_dirty {
        saw_unlock = NR
    }
    in_dirty && /SETKSTATUS\(get_active_thread\(get_cpunum\(\)\), 0\);/ {
        saw_status = NR
    }
    in_dirty && /lookup_pid\(kap->pid\)/ { saw_plain = NR }
    in_dirty && /^}/ { in_dirty = 0 }
    END {
        exit !(saw_rcu && saw_lock && saw_dirty && saw_unlock_lock &&
               saw_early_unlock && saw_unlock && saw_status &&
               saw_rcu < saw_lock && saw_lock < saw_dirty &&
               saw_dirty < saw_unlock_lock && saw_unlock_lock < saw_unlock &&
               saw_unlock < saw_status && !saw_plain)
    }
' kernel/ext/cred.c; then
    echo "RCU-sched: cred_dirty must hold process_rcu through credential update" >&2
    exit 1
fi

if ! awk '
    /void kext_cred_get\(void \*data\)/ { in_get = 1 }
    in_get && /lookup_pid_rcu\(kap->pid\)/ { saw_rcu = NR }
    in_get && /kspinlock_lock\(&cred_slock\);/ { saw_lock = NR }
    in_get && /\*kap->cip = prp->cred->info;/ { saw_snapshot = NR }
    in_get && /kspinlock_unlock\(&cred_slock\);/ { saw_unlock_lock = NR }
    in_get && /rcu_read_unlock\(&process_rcu\);/ && !saw_snapshot {
        saw_early_unlock = NR
    }
    in_get && /rcu_read_unlock\(&process_rcu\);/ && saw_snapshot {
        saw_unlock = NR
    }
    in_get && /SETKSTATUS\(get_active_thread\(get_cpunum\(\)\), 0\);/ {
        saw_status = NR
    }
    in_get && /lookup_pid\(kap->pid\)/ { saw_plain = NR }
    in_get && /^}/ { in_get = 0 }
    END {
        exit !(saw_rcu && saw_lock && saw_snapshot && saw_unlock_lock &&
               saw_early_unlock && saw_unlock && saw_status &&
               saw_rcu < saw_lock && saw_lock < saw_snapshot &&
               saw_snapshot < saw_unlock_lock && saw_unlock_lock < saw_unlock &&
               saw_unlock < saw_status && !saw_plain)
    }
' kernel/ext/cred.c; then
    echo "RCU-sched: cred_get must snapshot credentials under process_rcu" >&2
    exit 1
fi

if ! awk '
    /void kext_proc_query\(void \*data\)/ { in_query = 1 }
    in_query && /lookup_pid_rcu\(kap->pid\)/ { saw_rcu = NR }
    in_query && /out->pid = prp->pid;/ { saw_snapshot = NR }
    in_query && /rcu_read_unlock\(&process_rcu\);/ && !saw_snapshot {
        saw_early_unlock = NR
    }
    in_query && /rcu_read_unlock\(&process_rcu\);/ && saw_snapshot {
        saw_unlock = NR
    }
    in_query && /SETKSTATUS\(act, EOK\);/ { saw_status = NR }
    in_query && /lookup_pid\(kap->pid\)/ { saw_plain = NR }
    in_query && /^}/ { in_query = 0 }
    END {
        exit !(saw_rcu && saw_snapshot && saw_early_unlock && saw_unlock &&
               saw_status && saw_rcu < saw_snapshot &&
               saw_snapshot < saw_unlock && saw_unlock < saw_status &&
               !saw_plain)
    }
' kernel/ext/query.c; then
    echo "RCU-sched: proc_query must snapshot process fields under process_rcu" >&2
    exit 1
fi

if ! awk '
    /void kext_proc_cap_stats\(void \*data\)/ { in_stats = 1 }
    in_stats && /lookup_pid_rcu\(kap->pid\)/ { saw_rcu = NR }
    in_stats && /rtmux_cap_stats\(&prp->endpoint_caps, kap->stats\)/ {
        saw_stats = NR
    }
    in_stats && /rcu_read_unlock\(&process_rcu\);/ && !saw_stats {
        saw_early_unlock = NR
    }
    in_stats && /rcu_read_unlock\(&process_rcu\);/ && saw_stats {
        saw_unlock = NR
    }
    in_stats && /SETKSTATUS\(act, kap->ret\);/ { saw_status = NR }
    in_stats && /lookup_pid\(kap->pid\)/ { saw_plain = NR }
    in_stats && /^}/ { in_stats = 0 }
    END {
        exit !(saw_rcu && saw_stats && saw_early_unlock && saw_unlock &&
               saw_status && saw_rcu < saw_stats &&
               saw_stats < saw_unlock && saw_unlock < saw_status &&
               !saw_plain)
    }
' kernel/ext/query.c; then
    echo "RCU-sched: proc cap stats must hold process_rcu through cap-table snapshot" >&2
    exit 1
fi

if ! awk '
    /void kext_proc_endpoint_cap\(void \*data\)/ { in_cap = 1 }
    in_cap && /lookup_pid_rcu\(kap->pid\)/ { saw_prp_rcu = NR }
    in_cap && /rcu_read_lock\(&connobj_rcu\);/ { saw_conn_rcu = NR }
    in_cap && /kspinlock_lock\(&prp->vec_slock\);/ { saw_vec_lock = NR }
    in_cap && /vector_lookup\(vec, coid\)/ { saw_vec = NR }
    in_cap && /rtmux_cap_get_or_alloc_context\(&prp->endpoint_caps,/ {
        saw_cap = NR
    }
    in_cap && /kap->rights = cop->rights;/ { saw_snapshot = NR }
    in_cap && /^out_unlock:/ { saw_out = NR }
    in_cap && /kspinlock_unlock\(&prp->vec_slock\);/ && saw_out {
        saw_vec_unlock = NR
    }
    in_cap && /rcu_read_unlock\(&connobj_rcu\);/ && saw_vec_unlock {
        saw_conn_unlock = NR
    }
    in_cap && /rcu_read_unlock\(&process_rcu\);/ && saw_conn_unlock {
        saw_prp_unlock = NR
    }
    in_cap && /SETKSTATUS\(act, status\);/ { saw_status = NR }
    in_cap && /lookup_pid\(kap->pid\)/ { saw_plain = NR }
    in_cap && /^}/ { in_cap = 0 }
    END {
        exit !(saw_prp_rcu && saw_conn_rcu && saw_vec_lock && saw_vec &&
               saw_cap && saw_snapshot && saw_out && saw_vec_unlock &&
               saw_conn_unlock && saw_prp_unlock && saw_status &&
               saw_prp_rcu < saw_conn_rcu && saw_conn_rcu < saw_vec_lock &&
               saw_vec_lock < saw_vec && saw_vec < saw_cap &&
               saw_cap < saw_snapshot && saw_snapshot < saw_out &&
               saw_out < saw_vec_unlock && saw_vec_unlock < saw_conn_unlock &&
               saw_conn_unlock < saw_prp_unlock && saw_prp_unlock < saw_status &&
               !saw_plain)
    }
' kernel/ext/query.c; then
    echo "RCU-sched: proc endpoint cap must hold process/conn RCU through vector/cap snapshot" >&2
    exit 1
fi

if ! awk '
    /void kext_proc_index_to_pid\(void \*data\)/ { in_index = 1 }
    in_index && /rcu_read_lock\(&process_rcu\);/ { saw_lock = NR }
    in_index && /vector_lookup\(&process_vector, kap->index\)/ { saw_vec = NR }
    in_index && /kap->pid_out = prp \? prp->pid : 0;/ { saw_snapshot = NR }
    in_index && /rcu_read_unlock\(&process_rcu\);/ { saw_unlock = NR }
    in_index && /SETKSTATUS\(act, EOK\);/ { saw_status = NR }
    in_index && /^}/ { in_index = 0 }
    END {
        exit !(saw_lock && saw_vec && saw_snapshot && saw_unlock &&
               saw_status && saw_lock < saw_vec && saw_vec < saw_snapshot &&
               saw_snapshot < saw_unlock && saw_unlock < saw_status)
    }
' kernel/ext/query.c; then
    echo "RCU-sched: proc index-to-pid must snapshot process_vector under process_rcu" >&2
    exit 1
fi

if ! awk '
    /void kext_proc_wait_finalize\(void \*data\)/ { in_wait = 1 }
    in_wait && /lookup_pid_rcu\(kap->child_pid\)/ { saw_child = NR }
    in_wait && /lookup_pid_rcu\(kap->parent_pid\)/ { saw_parent = NR }
    in_wait && /kspinlock_lock\(&prp->vec_slock\);/ { saw_prp_lock = NR }
    in_wait && /kspinlock_lock\(&parent->vec_slock\);/ { saw_parent_lock = NR }
    in_wait && /parent->kids_running_time \+=/ { saw_parent_update = NR }
    in_wait && /prp->flags &= ~RTMUX_FLG_PROC_WAITINFO;/ { saw_child_update = NR }
    in_wait && /kspinlock_unlock\(&parent->vec_slock\);/ { saw_parent_unlock = NR }
    in_wait && /kspinlock_unlock\(&prp->vec_slock\);/ { saw_prp_unlock = NR }
    in_wait && /rcu_read_unlock\(&process_rcu\);/ { unlocks++ }
    in_wait && /SETKSTATUS\(act, EOK\);/ { saw_status = NR }
    in_wait && /lookup_pid\(kap->child_pid\)|lookup_pid\(kap->parent_pid\)/ {
        saw_plain = NR
    }
    in_wait && /^}/ { in_wait = 0 }
    END {
        exit !(saw_child && saw_parent && saw_prp_lock && saw_parent_lock &&
               saw_parent_update && saw_child_update && saw_parent_unlock &&
               saw_prp_unlock && unlocks >= 2 && saw_status &&
               saw_child < saw_parent && saw_parent < saw_prp_lock &&
               saw_prp_lock < saw_parent_update &&
               saw_parent_update < saw_child_update &&
               saw_child_update < saw_prp_unlock &&
               saw_prp_unlock < saw_status && !saw_plain)
    }
' kernel/ext/query.c; then
    echo "RCU-sched: proc wait_finalize must hold process_rcu and ordered vec_slocks through scalar updates" >&2
    exit 1
fi

if ! awk '
    /void kext_proc_set_spawn\(void \*data\)/ { in_spawn = 1 }
    in_spawn && /lookup_pid_rcu\(kap->pid\)/ { saw_rcu = NR }
    in_spawn && /kspinlock_lock\(&prp->vec_slock\);/ { saw_lock = NR }
    in_spawn && /prp->initial_ip = kap->initial_ip;/ { saw_ip = NR }
    in_spawn && /prp->initial_sp = kap->initial_sp;/ { saw_sp = NR }
    in_spawn && /kap->spawn_flags_out = flags;/ { saw_out = NR }
    in_spawn && /kspinlock_unlock\(&prp->vec_slock\);/ { saw_unlock_lock = NR }
    in_spawn && /rcu_read_unlock\(&process_rcu\);/ && !saw_ip {
        saw_early_unlock = NR
    }
    in_spawn && /rcu_read_unlock\(&process_rcu\);/ && saw_ip {
        saw_unlock = NR
    }
    in_spawn && /SETKSTATUS\(act, EOK\);/ { saw_status = NR }
    in_spawn && /lookup_pid\(kap->pid\)/ { saw_plain = NR }
    in_spawn && /^}/ { in_spawn = 0 }
    END {
        exit !(saw_rcu && saw_lock && saw_ip && saw_sp && saw_out &&
               saw_unlock_lock && saw_early_unlock && saw_unlock &&
               saw_status && saw_rcu < saw_lock && saw_lock < saw_ip &&
               saw_ip < saw_sp && saw_sp < saw_out &&
               saw_out < saw_unlock_lock && saw_unlock_lock < saw_unlock &&
               saw_unlock < saw_status && !saw_plain)
    }
' kernel/ext/process.c; then
    echo "RCU-sched: proc set_spawn must update scalar spawn state under process_rcu and vec_slock" >&2
    exit 1
fi

if ! awk '
    /void kext_proc_clear_lcp\(void \*data\)/ { in_lcp = 1 }
    in_lcp && /lookup_pid_rcu\(pid\)/ { saw_rcu = NR }
    in_lcp && /prp->lcp = NULL;/ { saw_store = NR }
    in_lcp && /rcu_read_unlock\(&process_rcu\);/ && !saw_store {
        saw_early_unlock = NR
    }
    in_lcp && /rcu_read_unlock\(&process_rcu\);/ && saw_store {
        saw_unlock = NR
    }
    in_lcp && /SETKSTATUS\(act, EOK\);/ { saw_status = NR }
    in_lcp && /lookup_pid\(pid\)/ { saw_plain = NR }
    in_lcp && /^}/ { in_lcp = 0 }
    END {
        exit !(saw_rcu && saw_store && saw_early_unlock && saw_unlock &&
               saw_status && saw_rcu < saw_store && saw_store < saw_unlock &&
               saw_unlock < saw_status && !saw_plain)
    }
' kernel/ext/query.c; then
    echo "RCU-sched: proc clear_lcp must update under process_rcu" >&2
    exit 1
fi

if ! awk '
    /void kext_proc_set_ocb_list\(void \*data\)/ { in_ocb = 1 }
    in_ocb && /lookup_pid_rcu\(kap->pid\)/ { saw_rcu = NR }
    in_ocb && /kspinlock_lock\(&prp->vec_slock\);/ { saw_lock = NR }
    in_ocb && /prp->ocb_list = kap->head;/ { saw_store = NR }
    in_ocb && /kspinlock_unlock\(&prp->vec_slock\);/ { saw_unlock_lock = NR }
    in_ocb && /rcu_read_unlock\(&process_rcu\);/ && !saw_store {
        saw_early_unlock = NR
    }
    in_ocb && /rcu_read_unlock\(&process_rcu\);/ && saw_store {
        saw_unlock = NR
    }
    in_ocb && /SETKSTATUS\(act, EOK\);/ { saw_status = NR }
    in_ocb && /lookup_pid\(kap->pid\)/ { saw_plain = NR }
    in_ocb && /^}/ { in_ocb = 0 }
    END {
        exit !(saw_rcu && saw_lock && saw_store && saw_unlock_lock &&
               saw_early_unlock && saw_unlock && saw_status &&
               saw_rcu < saw_lock && saw_lock < saw_store &&
               saw_store < saw_unlock_lock &&
               saw_unlock_lock < saw_unlock &&
               saw_unlock < saw_status && !saw_plain)
    }
' kernel/ext/query.c; then
    echo "RCU-sched: proc set_ocb_list must update under process_rcu and vec_slock" >&2
    exit 1
fi

if ! awk '
    /void kext_proc_clear_pls\(void \*data\)/ { in_pls = 1 }
    in_pls && /lookup_pid_rcu\(pid\)/ { saw_rcu = NR }
    in_pls && /prp->pls = NULL;/ { saw_store = NR }
    in_pls && /rcu_read_unlock\(&process_rcu\);/ && !saw_store {
        saw_early_unlock = NR
    }
    in_pls && /rcu_read_unlock\(&process_rcu\);/ && saw_store {
        saw_unlock = NR
    }
    in_pls && /SETKSTATUS\(act, EOK\);/ { saw_status = NR }
    in_pls && /lookup_pid\(pid\)/ { saw_plain = NR }
    in_pls && /^}/ { in_pls = 0 }
    END {
        exit !(saw_rcu && saw_store && saw_early_unlock && saw_unlock &&
               saw_status && saw_rcu < saw_store && saw_store < saw_unlock &&
               saw_unlock < saw_status && !saw_plain)
    }
' kernel/ext/query.c; then
    echo "RCU-sched: proc clear_pls must update under process_rcu" >&2
    exit 1
fi

for fn in kext_proc_nozombie_mark kext_proc_exit_mark \
          kext_proc_exit_finalize kext_proc_flags_op; do
    if ! awk -v fn="$fn" '
        $0 ~ "void " fn "\\(void \\*data\\)" { in_fn = 1 }
        in_fn && /lookup_pid_rcu\(kap->pid\)/ { saw_rcu = NR }
        in_fn && /kspinlock_lock\(&prp->vec_slock\);/ { saw_lock = NR }
        in_fn && /prp->flags/ { saw_flags = NR }
        in_fn && /kspinlock_unlock\(&prp->vec_slock\);/ { saw_unlock_lock = NR }
        in_fn && /rcu_read_unlock\(&process_rcu\);/ && !saw_flags {
            saw_early_unlock = NR
        }
        in_fn && /rcu_read_unlock\(&process_rcu\);/ && saw_flags {
            saw_unlock = NR
        }
        in_fn && /SETKSTATUS\(act, EOK\);/ { saw_status = NR }
        in_fn && /lookup_pid\(kap->pid\)/ { saw_plain = NR }
        in_fn && /^}/ { in_fn = 0 }
        END {
            exit !(saw_rcu && saw_lock && saw_flags && saw_unlock_lock &&
                   saw_early_unlock && saw_unlock && saw_status &&
                   saw_rcu < saw_lock && saw_lock < saw_flags &&
                   saw_flags < saw_unlock_lock &&
                   saw_unlock_lock < saw_unlock &&
                   saw_unlock < saw_status && !saw_plain)
        }
    ' kernel/ext/query.c; then
        echo "RCU-sched: $fn must update process scalar state under process_rcu and vec_slock" >&2
        exit 1
    fi
done

if ! awk '
    /void kext_channel_has_receivers\(void \*data\)/ { in_chan = 1 }
    in_chan && /lookup_pid_rcu\(kap->pid\)/ { saw_prp_rcu = NR }
    in_chan && /rcu_read_lock\(&connobj_rcu\);/ { saw_conn_rcu = NR }
    in_chan && /kspinlock_lock\(&prp->vec_slock\);/ { saw_vec_lock = NR }
    in_chan && /vector_lookup\(&prp->chancons, \(unsigned\) kap->chid\)/ {
        saw_vec = NR
    }
    in_chan && /has_receivers = chp->receive_queue \? 1 : 0;/ {
        saw_snapshot = NR
    }
    in_chan && /kspinlock_unlock\(&prp->vec_slock\);/ && saw_snapshot {
        saw_vec_unlock = NR
    }
    in_chan && /rcu_read_unlock\(&connobj_rcu\);/ && saw_vec_unlock {
        saw_conn_unlock = NR
    }
    in_chan && /rcu_read_unlock\(&process_rcu\);/ && saw_conn_unlock {
        saw_prp_unlock = NR
    }
    in_chan && /\*kap->out = has_receivers;/ { saw_copy = NR }
    in_chan && /lookup_pid\(kap->pid\)/ { saw_plain = NR }
    in_chan && /^}/ { in_chan = 0 }
    END {
        exit !(saw_prp_rcu && saw_conn_rcu && saw_vec_lock && saw_vec &&
               saw_snapshot && saw_vec_unlock && saw_conn_unlock &&
               saw_prp_unlock && saw_copy &&
               saw_prp_rcu < saw_conn_rcu && saw_conn_rcu < saw_vec_lock &&
               saw_vec_lock < saw_vec && saw_vec < saw_snapshot &&
               saw_snapshot < saw_vec_unlock &&
               saw_vec_unlock < saw_conn_unlock &&
               saw_conn_unlock < saw_prp_unlock &&
               saw_prp_unlock < saw_copy && !saw_plain)
    }
' kernel/ext/query.c; then
    echo "RCU-sched: channel_has_receivers must snapshot under process/conn RCU and vec_slock" >&2
    exit 1
fi

if ! awk '
    /int kdecl ksys_connect_detach\(thread_t \*act,/ {
        in_detach = 1
    }
    in_detach && /handle = kap->coid;/ { saw_handle = NR }
    in_detach && /rcu_read_lock\(&connobj_rcu\);/ { saw_conn_lock = NR }
    in_detach && /endpoint_cap_pin_context\(prp, handle,/ { saw_cap = NR }
    in_detach && /kspinlock_lock\(&prp->vec_slock\);/ && !saw_vec_lock {
        saw_vec_lock = NR
    }
    in_detach && /vector_lookup2\(vec, coid\)/ { saw_vec = NR }
    in_detach && /cop->type != TYPE_CONNECTION/ { saw_type = NR }
    in_detach && /cap_cop.*cap_cop != cop/ { saw_cap_check = NR }
    in_detach && /vector_rem\(vec, \(unsigned\) coid\);/ && saw_cap_check &&
        !saw_vec_rem {
        saw_vec_rem = NR
    }
    in_detach && /kspinlock_unlock\(&prp->vec_slock\);/ && saw_vec_rem &&
        !saw_vec_unlock {
        saw_vec_unlock = NR
    }
    in_detach && /rtmux_cap_unpin\(&prp->endpoint_caps, \(rtmux_cap_t\) handle\)/ {
        saw_unpin = NR
    }
    in_detach && /rcu_read_unlock\(&connobj_rcu\);/ && saw_unpin {
        saw_conn_unlock = NR
    }
    in_detach && /if \(cap_pinned\)/ {
        saw_cap_pinned_branch = NR
    }
    in_detach && /rtmux_cap_revoke\(&prp->endpoint_caps, \(rtmux_cap_t\) handle,/ {
        saw_revoke_handle = NR
    }
    in_detach && /else/ && saw_revoke_handle && !saw_revoke_else {
        saw_revoke_else = NR
    }
    in_detach && /endpoint_cap_revoke_coid\(cop, kap->coid\);/ {
        saw_revoke_context = NR
    }
    in_detach && /^}/ { in_detach = 0 }
    END {
        exit !(saw_handle && saw_conn_lock && saw_cap && saw_vec_lock &&
               saw_vec && saw_type && saw_cap_check && saw_vec_rem &&
               saw_vec_unlock && saw_unpin && saw_conn_unlock &&
               saw_cap_pinned_branch && saw_revoke_handle &&
               saw_revoke_else && saw_revoke_context &&
               saw_handle < saw_conn_lock && saw_conn_lock < saw_cap &&
               saw_cap < saw_vec_lock && saw_vec_lock < saw_vec &&
               saw_vec <= saw_type && saw_type < saw_cap_check &&
               saw_cap_check < saw_vec_rem && saw_vec_rem < saw_vec_unlock &&
               saw_vec_unlock < saw_unpin && saw_unpin < saw_conn_unlock &&
               saw_conn_unlock < saw_cap_pinned_branch &&
               saw_cap_pinned_branch < saw_revoke_handle &&
               saw_revoke_handle < saw_revoke_else &&
               saw_revoke_else < saw_revoke_context)
    }
' kernel/ipc/connect.c; then
    echo "RCU-sched: connect_detach must keep generated endpoint cap/object pinned through vector validation" >&2
    exit 1
fi

if ! awk '
    /int kdecl ksys_connect_server_info\(thread_t \*act,/ {
        in_info = 1
    }
    in_info && /lookup_pid_rcu\(kap->pid\)/ { saw_prp_rcu = NR }
    in_info && /rcu_read_lock\(&connobj_rcu\);/ { saw_conn_rcu = NR }
    in_info && /endpoint_cap_pin_context\(prp, kap->coid,/ { saw_cap = NR }
    in_info && /kspinlock_lock\(&prp->vec_slock\);/ { saw_lock = NR }
    in_info && /if \(exact_context\)/ && !saw_exact { saw_exact = NR }
    in_info && /cop = vector_lookup2\(vec, coid\);/ { saw_exact_lookup = NR }
    in_info && /cop->type != TYPE_CONNECTION/ && saw_exact_lookup &&
        !saw_legacy_search {
        saw_exact_type_check = NR
    }
    in_info && /chp = cop->channel;/ && saw_exact_lookup && !saw_legacy_search {
        saw_exact_channel = NR
    }
    in_info && /goto found;/ && saw_exact_lookup && !saw_legacy_search {
        saw_exact_goto = NR
    }
    in_info && /while \(\(cop = vector_search\(vec, coid, &coid\)\)\)/ {
        saw_legacy_search = NR
    }
    in_info && /rtmux_cap_get_or_alloc_context\(&prp->endpoint_caps,/ {
        saw_endpoint = NR
    }
    in_info && /info.endpoint_rights = cop->rights;/ { saw_snapshot = NR }
    in_info && /kspinlock_unlock\(&prp->vec_slock\);/ && saw_endpoint {
        saw_unlock_vec = NR
    }
    in_info && /rtmux_cap_unpin\(&prp->endpoint_caps,/ && saw_unlock_vec {
        saw_unpin = NR
    }
    in_info && /rcu_read_unlock\(&connobj_rcu\);/ && saw_unpin {
        saw_conn_unlock = NR
    }
    in_info && /rcu_read_unlock\(&process_rcu\);/ && saw_conn_unlock {
        saw_unlock_rcu = NR
    }
    in_info && /user_slice_put\(act, kap->info, &info, sizeof\(info\)\)/ && !saw_user_copy { saw_user_copy = NR }
    in_info && /lookup_pid\(kap->pid\)/ { saw_plain = NR }
    in_info && /^}/ { in_info = 0 }
    END {
        if (!saw_user_copy) print "RCU detail: server copy" > "/dev/stderr"
        else if (saw_unlock_rcu && saw_user_copy < saw_unlock_rcu)
            print "RCU detail: server early copy" > "/dev/stderr"
        exit !(saw_prp_rcu && saw_conn_rcu && saw_cap && saw_lock &&
               saw_exact && saw_exact_lookup && saw_exact_type_check &&
               saw_exact_channel && saw_exact_goto && saw_legacy_search &&
               saw_endpoint && saw_snapshot && saw_unlock_vec && saw_unpin &&
               saw_conn_unlock && saw_unlock_rcu && saw_user_copy &&
               saw_prp_rcu < saw_conn_rcu && saw_conn_rcu < saw_cap &&
               saw_cap < saw_lock && saw_lock < saw_exact &&
               saw_exact < saw_exact_lookup &&
               saw_exact_lookup < saw_exact_type_check &&
               saw_exact_type_check < saw_exact_channel &&
               saw_exact_channel < saw_exact_goto &&
               saw_exact_goto < saw_legacy_search &&
               saw_legacy_search < saw_endpoint &&
               saw_endpoint < saw_snapshot &&
               saw_snapshot < saw_unlock_vec && saw_unlock_vec < saw_unlock_rcu &&
               saw_unpin < saw_conn_unlock && saw_conn_unlock < saw_unlock_rcu &&
               saw_unlock_rcu < saw_user_copy && !saw_plain)
    }
' kernel/ipc/connect.c; then
    echo "RCU-sched: connect_server_info pid lookup must keep endpoint cap/object stable through snapshot and copy out after unlock" >&2
    exit 1
fi

if ! awk '
    /int kdecl ksys_connect_flags\(thread_t \*act,/ {
        in_flags = 1
    }
    in_flags && /lookup_pid_rcu\(kap->pid\)/ { saw_prp_rcu = NR }
    in_flags && /rcu_read_lock\(&connobj_rcu\);/ { saw_conn_rcu = NR }
    in_flags && /endpoint_cap_pin_context\(prp, kap->coid,/ { saw_cap = NR }
    in_flags && /kspinlock_lock\(&prp->vec_slock\);/ { saw_lock = NR }
    in_flags && /cop->type != TYPE_CONNECTION/ { saw_type_check = NR }
    in_flags && /old = VECAND\(ptr, 2\)/ { saw_snapshot = NR }
    in_flags && /kspinlock_unlock\(&prp->vec_slock\);/ && saw_snapshot {
        saw_unlock_vec = NR
    }
    in_flags && /rtmux_cap_unpin\(&prp->endpoint_caps,/ && saw_unlock_vec {
        saw_unpin = NR
    }
    in_flags && /rcu_read_unlock\(&connobj_rcu\);/ && saw_unpin {
        saw_conn_unlock = NR
    }
    in_flags && /rcu_read_unlock\(&process_rcu\);/ && saw_unlock_vec {
        saw_unlock_rcu = NR
    }
    in_flags && /SETKSTATUS\(act, old\);/ { saw_status = NR }
    in_flags && /lookup_pid\(kap->pid\)/ { saw_plain = NR }
    in_flags && /^}/ { in_flags = 0 }
    END {
        exit !(saw_prp_rcu && saw_conn_rcu && saw_cap && saw_lock &&
               saw_type_check && saw_snapshot && saw_unpin && saw_conn_unlock &&
               saw_unlock_vec && saw_unlock_rcu && saw_status &&
               saw_prp_rcu < saw_conn_rcu && saw_conn_rcu < saw_cap &&
               saw_cap < saw_lock &&
               saw_lock < saw_type_check && saw_type_check < saw_snapshot &&
               saw_snapshot < saw_unlock_vec &&
               saw_unlock_vec < saw_unpin && saw_unpin < saw_conn_unlock &&
               saw_conn_unlock < saw_unlock_rcu &&
               saw_unlock_rcu < saw_status &&
               !saw_plain)
    }
' kernel/ipc/connect.c; then
    echo "RCU-sched: connect_flags pid lookup must keep endpoint cap/object stable through flag snapshot and publish after unlock" >&2
    exit 1
fi

if ! awk '
    /int kdecl ksys_connect_attach\(thread_t \*act,/ {
        in_attach = 1
    }
    in_attach && /#define CONNECT_ATTACH_RETURN/ { saw_macro = NR }
    in_attach && /process_unpin\(prps\)/ { saw_unpin = NR }
    in_attach && /RTMUX_NODE_CMP\(kap->nd, RTMUX_LOCAL_NODE\)/ && !saw_remote {
        saw_remote = NR
    }
    in_attach && /\(prps = remote_ipc.prp\)/ { saw_remote_prp = NR }
    in_attach && /\(chp = remote_ipc.chp\)/ { saw_remote_chp = NR }
    in_attach && /goto common;/ && saw_remote && !saw_remote_goto {
        saw_remote_goto = NR
    }
    in_attach && /lookup_pid_rcu\(kap->pid\)/ { saw_prp_rcu = NR }
    in_attach && /process_pin\(prps\)/ { saw_pin = NR }
    in_attach && /rcu_read_unlock\(&process_rcu\);/ { saw_unlock = NR }
    in_attach && /vec_slocks_lock\(prpc, prps\);/ { saw_vec_lock = NR }
    in_attach && /CONNECT_ATTACH_RETURN\(ENOERROR\);/ { saw_success = NR }
    in_attach && /lookup_pid\(kap->pid\)/ { saw_plain = NR }
    in_attach && /^[[:space:]]*prps_pinned = 0;/ { saw_pin_clear = NR }
    in_attach && /^[[:space:]]*return / && $0 !~ /\\/ { saw_raw_return = NR }
    in_attach && /^}/ { in_attach = 0 }
    END {
        exit !(saw_macro && saw_unpin && saw_remote && saw_remote_prp &&
               saw_remote_chp && saw_remote_goto && saw_prp_rcu && saw_pin &&
               saw_unlock && saw_vec_lock && saw_success &&
               saw_macro < saw_remote &&
               saw_remote < saw_remote_prp && saw_remote_prp <= saw_remote_chp &&
               saw_remote_chp < saw_remote_goto && saw_remote_goto < saw_prp_rcu &&
               saw_prp_rcu < saw_pin &&
               saw_pin < saw_unlock && saw_unlock < saw_vec_lock &&
               !saw_plain && !saw_pin_clear && !saw_raw_return)
    }
' kernel/ipc/connect.c; then
    echo "RCU-sched: local connect_attach explicit pid lookup must pin under process_rcu and use cleanup returns" >&2
    exit 1
fi

if ! grep -q 'uint8_t lock_debug_held_depth(void);' kernel/include/lock.h; then
    echo "RCU-sched: lock-debug must expose held-depth for RCU waits" >&2
    exit 1
fi

if ! awk '
    /void kext_process_destroy\(void \*data\)/ { in_destroy = 1 }
    in_destroy && /rcu_sched_synchronize\(\);/ { saw_sync = NR }
    in_destroy && /process_wait_unpinned\(prp\);/ { saw_wait = NR }
    in_destroy && /object_free\(NULL, &process_souls, prp\);/ { saw_free = NR }
    in_destroy && /^}/ { in_destroy = 0 }
    END { exit !(saw_sync && saw_wait && saw_free && saw_sync < saw_wait && saw_wait < saw_free) }
' kernel/ext/process.c; then
    echo "RCU-sched: process destroy must wait for process pins after process_rcu" >&2
    exit 1
fi

if ! awk '
    /int rcu_sched_synchronize_detail\(struct rcu_sched_busy \*busy\)/ { in_sync = 1 }
    in_sync && /#ifdef CONFIG_LOCK_DEBUG/ { saw_ifdef = NR }
    in_sync && saw_ifdef && /lock_debug_held_depth\(\) != 0/ { saw_guard = NR }
    in_sync && saw_guard && /#endif/ { saw_endif = NR }
    in_sync && /^}/ { in_sync = 0 }
    END { exit !(saw_ifdef && saw_guard && saw_endif && saw_ifdef < saw_guard && saw_guard < saw_endif) }
' kernel/sync/rcu.c; then
    echo "RCU-sched: rcu_sched_synchronize must reject waits under held spinlocks" >&2
    exit 1
fi

rcu_wait_callers=$(
    find kernel -type f \( -name '*.[ch]' -o -name '*.S' \) -print |
        LC_ALL=C sort |
        xargs grep -EnH '^[[:space:]].*(rcu_sched_synchronize\(\);|rcu_sched_synchronize_detail\(&)' |
        grep -v ':[0-9][0-9]*:[[:space:]]*[*\/]' |
        sed 's/:[0-9][0-9]*:/:/'
)
rcu_wait_callers_expected='kernel/ext/process.c:    rcu_sched_synchronize();
kernel/ipc/channel.c:    rcu_sched_synchronize();
kernel/ipc/channel.c:    rcu_sched_synchronize();
kernel/ipc/connect.c:    rcu_sched_synchronize();
kernel/sched/dispatch.c:    rcu_sched_synchronize();
kernel/sched/dispatch.c:            rcu_sched_synchronize();
kernel/thread/lifecycle.c:            rcu_sched_synchronize();
kernel/thread/lifecycle.c:        (void) rcu_sched_synchronize_detail(&stats_busy);
kernel/thread/lifecycle.c:        rcu_sched_synchronize();'
if [ "$rcu_wait_callers" != "$rcu_wait_callers_expected" ]; then
    echo "RCU-sched: rcu_sched_synchronize caller set changed; re-audit held locks" >&2
    exit 1
fi

if ! awk '
    /void in_kcall_inc\(void\)/ { in_inc = 1 }
    in_inc && /__atomic_store_n\(&in_kcall\[cpu\], cur \+ 1, __ATOMIC_RELEASE\);/ {
        saw_store = NR
    }
    in_inc && saw_store && /cpu_smp_mb\(\);/ && !saw_mb { saw_mb = NR }
    in_inc && /^}/ { in_inc = 0 }
    END { exit !(saw_store && saw_mb && saw_store < saw_mb) }
' kernel/core/globals.c; then
    echo "RCU-sched: in_kcall_inc must fence after publishing non-zero depth" >&2
    exit 1
fi

if ! awk '
    /void in_kcall_dec\(void\)/ { in_dec = 1 }
    in_dec && /if \(next == 0\)/ { saw_zero = NR }
    in_dec && saw_zero && /cpu_smp_mb\(\);/ && !saw_mb { saw_mb = NR }
    in_dec && /__atomic_store_n\(&in_kcall\[cpu\], next, __ATOMIC_RELEASE\);/ {
        saw_store = NR
    }
    in_dec && /^}/ { in_dec = 0 }
    END { exit !(saw_zero && saw_mb && saw_store && saw_zero < saw_mb && saw_mb < saw_store) }
' kernel/core/globals.c; then
    echo "RCU-sched: in_kcall_dec must fence before release-storing zero" >&2
    exit 1
fi

if ! grep -q 'depth = __atomic_load_n(&in_kcall\[cpu\], __ATOMIC_RELAXED);' \
    kernel/sync/rcu.c ||
   ! grep -q '__atomic_load_n(&in_kcall\[cpu\], __ATOMIC_ACQUIRE) == 0' \
    kernel/sync/rcu.c; then
    echo "RCU-sched: rcu_sched_synchronize must relaxed-poll then acquire-confirm in_kcall zero" >&2
    exit 1
fi

if ! awk '
    /static void rcu_sched_busy_add_peer\(struct rcu_sched_busy \*detail, unsigned cpu\)/ {
        in_idle_attr = 1
    }
    /pc_idle_thp/ && !in_idle_attr { bad = NR }
    in_idle_attr && /^}/ { in_idle_attr = 0 }
    END { exit bad != 0 }
' kernel/sync/rcu.c; then
    echo "RCU-sched: rcu_sched_synchronize must not use the idle-thread shortcut" >&2
    exit 1
fi

if ! awk '
    /void rcu_sched_wait\(rcu_epoch_t goal\)/ { in_wait = 1 }
    in_wait && /__atomic_load_n\(&pcpu_array\[cpu\]\.pc_rcu_waiting,/ {
        saw_waiter_qs = NR
    }
    in_wait && /^}/ { in_wait = 0 }
    END { exit !saw_waiter_qs }
' kernel/sync/rcu.c; then
    echo "RCU-sched: concurrent rcu_sched_synchronize waiters must be quiescent" >&2
    exit 1
fi

if ! awk '
    /static unsigned rcu_sched_busy_peers\(struct rcu_sched_busy \*detail\)/ { in_fast = 1 }
    in_fast && /if \(cpu == self\)/ { saw_self = NR }
    in_fast && /depth = __atomic_load_n\(&in_kcall\[cpu\], __ATOMIC_ACQUIRE\);/ {
        saw_depth_acquire = NR
        depth_loads++
    }
    in_fast && saw_depth_acquire && /if \(depth == 0\)/ {
        saw_depth_zero = NR
    }
    in_fast && saw_depth_zero && /continue;/ && !saw_depth_continue {
        saw_depth_continue = NR
    }
    in_fast && /__atomic_load_n\(&pcpu_array\[cpu\]\.pc_rcu_waiting,/ {
        in_waiting_load = 1
        waiting_loads++
    }
    in_fast && in_waiting_load && /__ATOMIC_ACQUIRE/ {
        saw_waiter_acquire = NR
        in_waiting_load = 0
    }
    in_fast && saw_waiter_acquire && /continue;/ && !saw_waiter_continue {
        saw_waiter_continue = NR
    }
    in_fast && /peers\+\+;/ { saw_busy = NR }
    in_fast && saw_busy && /rcu_sched_busy_add_peer\(detail, cpu\);/ {
        saw_busy_detail = NR
    }
    in_fast && /return peers;/ { saw_done = NR; in_fast = 0 }
    END {
        exit !(saw_self && saw_waiter_acquire && saw_waiter_continue &&
               saw_depth_acquire && saw_depth_zero && saw_depth_continue &&
               saw_busy && saw_busy_detail && saw_done &&
               waiting_loads == 1 && depth_loads == 1 &&
               saw_depth_acquire < saw_depth_zero &&
               saw_depth_zero < saw_depth_continue &&
               saw_depth_continue < saw_waiter_acquire &&
               saw_waiter_acquire < saw_waiter_continue &&
               saw_waiter_continue < saw_busy &&
               saw_busy < saw_busy_detail &&
               saw_busy_detail < saw_done)
    }
' kernel/sync/rcu.c; then
    echo "RCU-sched: fast synchronize scan must skip zero-depth peers before waiter/alive checks" >&2
    exit 1
fi

if ! awk '
    /static void rcu_sched_busy_add_peer\(struct rcu_sched_busy \*detail, unsigned cpu\)/ {
        in_add = 1
    }
    in_add && /__atomic_load_n\(&pcpu_array\[cpu\]\.pc_curthread, __ATOMIC_ACQUIRE\)/ {
        saw_curthread = NR
    }
    in_add && saw_curthread && /cur == pcpu_array\[cpu\]\.pc_idle_thp/ {
        saw_idle_check = NR
    }
    in_add && saw_idle_check && /detail->idle\+\+;/ {
        saw_idle = NR
    }
    in_add && saw_idle && /rcu_sched_busy_add\(/ {
        saw_syscall = NR
    }
    in_add && /^}/ { saw_done = NR; in_add = 0 }
    END {
        exit !(saw_curthread && saw_idle_check && saw_idle && saw_syscall &&
               saw_done && saw_curthread < saw_idle_check &&
               saw_idle_check < saw_idle && saw_idle < saw_syscall &&
               saw_syscall < saw_done)
    }
' kernel/sync/rcu.c; then
    echo "RCU-sched: busy-peer attribution must split idle peers before syscall buckets" >&2
    exit 1
fi

if ! awk '
    /void in_kcall_inc\(void\)/ { in_inc = 1 }
    in_inc && /cur == 0/ { saw_zero = NR }
    in_inc && saw_zero && /pcpu_array\[cpu\]\.pc_syscall, IN_KCALL_PHASE_NONE/ {
        saw_inc_reset = NR
    }
    in_inc && /^}/ { in_inc = 0 }
    /void in_kcall_dec\(void\)/ { in_dec = 1 }
    in_dec && /pcpu_array\[cpu\]\.pc_syscall, IN_KCALL_PHASE_NONE/ {
        saw_dec_reset = NR
    }
    in_dec && /^}/ { in_dec = 0 }
    END { exit !(saw_zero && saw_inc_reset && !saw_dec_reset) }
' kernel/core/globals.c; then
    echo "RCU-sched: in_kcall must reset syscall snapshots only on outermost entry" >&2
    exit 1
fi

if ! awk '
    /int priv_call_phase\(unsigned id\)/ { in_phase = 1; saw_phase_fn = NR }
    in_phase && /__KSYS_PRIV_HEARTBEAT/ { saw_heartbeat_case = NR }
    in_phase && /IN_KCALL_PHASE_HEARTBEAT/ { saw_heartbeat_stamp = NR }
    in_phase && /__KSYS_PRIV_PA_ALLOC_GIVEN/ { saw_pa_alloc_case = NR }
    in_phase && /__KSYS_PRIV_PA_ALLOC_FULL/ { saw_pa_alloc_full_case = NR }
    in_phase && /IN_KCALL_PHASE_PRIV_PA_ALLOC/ { saw_pa_alloc_stamp = NR }
    in_phase && /__KSYS_PRIV_PA_FREE/ { saw_pa_free_case = NR }
    in_phase && /__KSYS_PRIV_PA_FREE_INFO/ { saw_pa_free_info_case = NR }
    in_phase && /__KSYS_PRIV_PA_FREE_PADDR/ { saw_pa_free_paddr_case = NR }
    in_phase && /__KSYS_PRIV_PA_FREE_LIST/ { saw_pa_free_list_case = NR }
    in_phase && /IN_KCALL_PHASE_PRIV_PA_FREE/ { saw_pa_free_stamp = NR }
    in_phase && /__KSYS_PRIV_KPAGE_ZERO/ { saw_pa_zero_case = NR }
    in_phase && /IN_KCALL_PHASE_PRIV_PA_ZERO/ { saw_pa_zero_stamp = NR }
    in_phase && /__KSYS_PRIV_PA_INIT/ { saw_pa_init_case = NR }
    in_phase && /__KSYS_PRIV_PA_START_BACKGROUND/ { saw_pa_start_case = NR }
    in_phase && /__KSYS_PRIV_PA_SYSRAM_OVERLAP/ { saw_pa_sysram_case = NR }
    in_phase && /__KSYS_PRIV_PA_GET_SIZES/ { saw_pa_sizes_case = NR }
    in_phase && /IN_KCALL_PHASE_PRIV_PA_INFO/ { saw_pa_info_stamp = NR }
    in_phase && /__KSYS_PRIV_PA_QUANTUM_TO_PADDR/ { saw_pa_q2p_case = NR }
    in_phase && /__KSYS_PRIV_PA_PADDR_TO_QUANTUM/ { saw_pa_p2q_case = NR }
    in_phase && /IN_KCALL_PHASE_PRIV_PA_XLAT/ { saw_pa_xlat_stamp = NR }
    in_phase && /__KSYS_PRIV_PA_PQ_OP/ { saw_pa_pq_case = NR }
    in_phase && /IN_KCALL_PHASE_PRIV_PA_PQ/ { saw_pa_pq_stamp = NR }
    in_phase && /RTMUX_PRIV_CALL_LIST\(PRIV_PHASE_ROW\)/ { saw_priv_list = NR }
    in_phase && /PRIV_PHASE_PROCESS/ { saw_process_case = NR }
    in_phase && /IN_KCALL_PHASE_PRIV_PROCESS/ { saw_process_stamp = NR }
    in_phase && /^}/ { in_phase = 0 }
    /act->syscall = \(int32_t\) num;/ { saw_thread = NR }
    /if \(num == __KSYS_PRIV\) {/ && saw_thread { in_priv_branch = 1 }
    in_priv_branch &&
        /__atomic_store_n\(&pcpu_array\[get_cpunum\(\)\]\.pc_syscall,/ {
        saw_priv_store = NR
    }
    in_priv_branch && /priv_call_phase\(\(unsigned\) kap\[0\]\)/ &&
        saw_priv_store {
        saw_phase_call = NR
    }
    in_priv_branch && /__ATOMIC_RELEASE\);/ && saw_phase_call {
        saw_priv_store_release = NR
        in_priv_branch = 0
    }
    /pcpu_array\[get_cpunum\(\)\]\.pc_syscall, \(int\) num/ { saw_pcpu = NR }
    END {
        exit !(saw_phase_fn && saw_heartbeat_case && saw_heartbeat_stamp &&
               saw_pa_alloc_case && saw_pa_alloc_full_case &&
               saw_pa_alloc_stamp &&
               saw_pa_free_case && saw_pa_free_info_case &&
               saw_pa_free_paddr_case && saw_pa_free_list_case &&
               saw_pa_free_stamp &&
               saw_pa_zero_case && saw_pa_zero_stamp &&
               saw_pa_init_case && saw_pa_start_case && saw_pa_sysram_case &&
               saw_pa_sizes_case && saw_pa_info_stamp &&
               saw_pa_q2p_case && saw_pa_p2q_case && saw_pa_xlat_stamp &&
               saw_pa_pq_case && saw_pa_pq_stamp && saw_priv_list &&
               saw_pa_alloc_stamp < saw_priv_list &&
               saw_pa_free_stamp < saw_priv_list &&
               saw_pa_zero_stamp < saw_priv_list &&
               saw_pa_info_stamp < saw_priv_list &&
               saw_pa_xlat_stamp < saw_priv_list &&
               saw_pa_pq_stamp < saw_priv_list &&
               saw_process_case && saw_process_stamp && saw_thread &&
               saw_priv_store && saw_phase_call && saw_priv_store_release &&
               saw_pcpu && saw_phase_fn < saw_thread &&
               saw_thread < saw_priv_store && saw_priv_store < saw_phase_call &&
               saw_phase_call < saw_priv_store_release)
    }
' arch/riscv/trap-handler.c; then
    echo "RCU-sched: syscall entry must publish per-CPU syscall and priv-family snapshots" >&2
    exit 1
fi

if ! awk '
    /int kdecl ksys_thread_detach\(thread_t \*act,/ { in_detach = 1 }
    in_detach && /thp->tid != \(uint32_t\) \(kap->tid - 1\)/ { saw_tid = NR }
    in_detach && /target_dead = thp->state == STATE_DEAD/ { saw_dead = NR }
    in_detach && /target_dead && !thread_reserve_destroy\(thp\)/ {
        saw_reserve = NR
    }
	    in_detach && /atomic_or\(&thp->flags, RTMUX_FLG_THR_DETACHED\)/ { saw_detached = NR }
	    in_detach && /tthread_pin\(joiner\)/ { saw_pin = NR }
	    in_detach && /kspinlock_unlock\(&prp->vec_slock\)/ {
	        if (saw_detached && !saw_unlock)
	            saw_unlock = NR
	    }
	    in_detach && /thread_join_wait_ready\(joiner\)/ { saw_wait = NR }
	    in_detach && /if \(thp->join == joiner\)/ { saw_guard = NR }
	    in_detach && /thp->join = NULL;/ { saw_clear = NR }
	    in_detach && /wake_joiner = 1;/ { saw_owned = NR }
	    in_detach && /if \(wake_joiner\)/ { saw_wake_guard = NR }
	    in_detach && /force_ready\(joiner, EINVAL\)/ { saw_wake = NR }
	    in_detach && /tthread_unpin\(joiner\)/ { saw_unpin = NR }
	    in_detach && /thread_destroy\(thp\)/ { saw_destroy = NR }
	    in_detach && /^}/ { in_detach = 0 }
	    END {
	        exit !(saw_tid && saw_dead && saw_reserve && saw_detached &&
	               saw_pin && saw_unlock && saw_wait && saw_guard && saw_clear &&
	               saw_owned && saw_wake_guard && saw_wake && saw_unpin &&
	               saw_destroy &&
	               saw_tid < saw_dead && saw_dead < saw_reserve &&
	               saw_reserve < saw_detached && saw_detached < saw_unlock &&
	               saw_pin < saw_unlock && saw_unlock < saw_wait &&
	               saw_wait < saw_guard &&
	               saw_guard < saw_clear && saw_clear < saw_owned &&
	               saw_owned < saw_wake_guard && saw_wake_guard < saw_wake &&
	               saw_wake < saw_unpin && saw_unpin < saw_destroy)
	    }
' kernel/sched/thread.c; then
    echo "RCU-sched: thread_detach must reserve destroy and publish DETACHED under vec_slock" >&2
    exit 1
fi

if ! awk '
    /int kdecl ksys_thread_join\(thread_t \*act,/ { in_join = 1 }
    in_join && /thp->tid != \(uint32_t\) \(kap->tid - 1\)/ { saw_tid = NR }
    in_join && /target_dead = thp->state == STATE_DEAD/ { saw_dead = NR }
    in_join && /user_slice_init\(act, kap->status, kap->status \? sizeof status : 0,/ {
        saw_status_check = NR
    }
    in_join && /sizeof status, USER_SLICE_WR, &status_slice\)/ { saw_status_write = NR }
    in_join && /target_dead && !thread_reserve_destroy\(thp\)/ {
        saw_reserve = NR
    }
    in_join && /atomic_or\(&thp->flags, RTMUX_FLG_THR_DETACHED\)/ { saw_detached = NR }
    in_join && /kspinlock_unlock\(&prp->vec_slock\)/ {
        if (saw_detached && !saw_unlock)
            saw_unlock = NR
    }
    in_join && /if \(!target_dead\)/ { saw_block = NR }
    in_join && /thread_destroy\(thp\)/ { saw_destroy = NR }
    in_join && /^}/ { in_join = 0 }
    END {
        if (!saw_status_write) print "RCU detail: join validation" > "/dev/stderr"
        exit !(saw_tid && saw_dead && saw_reserve && saw_status_check && saw_status_write &&
               saw_detached && saw_unlock && saw_block && saw_destroy &&
               saw_tid < saw_dead && saw_dead < saw_status_check &&
               saw_status_check < saw_status_write && saw_status_write < saw_reserve &&
               saw_reserve < saw_detached && saw_detached < saw_unlock &&
               saw_unlock < saw_block && saw_block < saw_destroy)
    }
' kernel/sched/thread.c; then
    echo "RCU-sched: dead thread_join must reserve claim and validate status before detach/reap" >&2
    exit 1
fi

if ! awk '
    /void rdecl thread_destroy\(thread_t \*thp\)/ { in_destroy = 1 }
    in_destroy && /THREAD_DESTROY_CLAIM_RESERVED_BASE \+ get_cpunum\(\)/ {
        saw_reserved = NR
    }
    in_destroy && /__atomic_load_n\(&thp->destroy_claim, __ATOMIC_ACQUIRE\) !=/ {
        saw_reserved_check = NR
    }
    in_destroy && /while \(!__atomic_compare_exchange_n\(/ { saw_cas = NR }
    in_destroy && /THREAD_DESTROY_CLAIM_ACTIVE/ { saw_active = NR }
    in_destroy && /if \(!self_reap\) {/ { saw_bail = NR }
    in_destroy && /thread_destroy claim stuck/ { saw_bound = NR }
    in_destroy && /thread_cpu_relax\(\)/ { saw_relax = NR }
    in_destroy && /dead_reap/ { saw_dead_reap = NR }
    in_destroy && /Quiescence gate/ { in_destroy = 0 }
    END {
        exit !(!saw_dead_reap && saw_reserved && saw_reserved_check &&
               saw_cas && saw_active && saw_bail && saw_bound &&
               saw_relax && saw_reserved < saw_reserved_check &&
               saw_reserved_check < saw_cas && saw_cas < saw_active &&
               saw_active < saw_bail &&
               saw_bail < saw_bound && saw_bound < saw_relax)
    }
' kernel/thread/lifecycle.c; then
    echo "RCU-sched: thread_destroy must consume same-CPU reservation or only self-wait" >&2
    exit 1
fi

if ! awk '
    /static uint8_t thread_state_load_acquire\(thread_t \*thp\)/ {
        saw_helper = NR
    }
    /void rdecl thread_destroy\(thread_t \*thp\)/ { in_destroy = 1 }
    in_destroy && /Quiescence gate/ { in_gate = 1 }
    in_gate && /uint8_t state = thread_state_load_acquire\(thp\)/ {
        saw_initial = NR
    }
    in_gate && /while \(state == STATE_DEAD && active_remote\)/ {
        saw_dead_loop = NR
    }
    in_gate && /state = thread_state_load_acquire\(thp\)/ {
        if (saw_dead_loop)
            saw_reload = NR
    }
    in_gate && /state != STATE_DEAD/ {
        if (!saw_not_dead)
            saw_not_dead = NR
    }
    in_gate && /state == STATE_RUNNING/ { saw_running = NR }
    in_gate && /_TRACE_TH_EMIT_DESTROY\(thp\)/ { in_gate = 0 }
    in_destroy && /thread_state_load_acquire\(thp\) != STATE_DEAD/ {
        saw_final = NR
    }
    in_destroy && /FPU buffer should be freed already/ { in_destroy = 0 }
    END {
        exit !(saw_helper && saw_initial && saw_dead_loop && saw_reload &&
               saw_not_dead && saw_running && saw_final &&
               saw_helper < saw_initial && saw_initial < saw_dead_loop &&
               saw_dead_loop < saw_reload && saw_reload < saw_not_dead &&
               saw_not_dead < saw_running && saw_running < saw_final)
    }
' kernel/thread/lifecycle.c; then
    echo "RCU-sched: thread_destroy quiescence gate must use acquire state loads" >&2
    exit 1
fi

if ! awk '
    /void rdecl thread_specret\(thread_t \*thp\)/ { in_specret = 1 }
    in_specret &&
        /if \(thp->join && thp->join->state == STATE_WAITTHREAD\) {/ {
        saw_wait = NR
    }
    in_specret && /ready\(thp->join\);/ { saw_ready = NR }
    in_specret && /thp->join = NULL;/ { saw_clear = NR }
    in_specret && /^}/ { in_specret = 0 }
    END {
        exit !(saw_wait && saw_ready && saw_clear &&
               saw_wait < saw_ready && saw_ready < saw_clear)
    }
' kernel/thread/lifecycle.c; then
    echo "RCU-sched: thread birth must clear the temporary join handshake" >&2
    exit 1
fi

if ! awk '
    /int rcu_sched_synchronize_detail\(struct rcu_sched_busy \*busy\)/ { in_sync = 1 }
    in_sync && /__atomic_store_n\(&pcpu_array\[get_cpunum\(\)\]\.pc_rcu_waiting, 1,/ {
        in_set = 1
        saw_set = NR
    }
    in_sync && in_set && /__ATOMIC_RELEASE/ { saw_set_release = NR; in_set = 0 }
    in_sync && /cpu_smp_mb\(\);/ && saw_set && !saw_mb {
        saw_mb = NR
    }
    in_sync && /cpu_smp_mb\(\);/ && saw_wait && !saw_clear {
        saw_late_mb = NR
    }
    in_sync && /busy_peers = rcu_sched_busy_peers\(busy\);/ { saw_fast = NR }
    in_sync && /goal = rcu_sched_advance\(\);/ { saw_advance = NR }
    in_sync && /rcu_sched_wait\(goal\);/ { saw_wait = NR }
    in_sync && /__atomic_store_n\(&pcpu_array\[get_cpunum\(\)\]\.pc_rcu_waiting, 0,/ {
        saw_clear = NR
    }
    in_sync && /^}/ { in_sync = 0 }
    END {
        exit !(saw_set && saw_set_release && saw_mb && saw_fast && saw_advance &&
               saw_wait && saw_clear &&
               saw_set < saw_mb && saw_mb < saw_fast &&
               saw_fast < saw_advance && saw_advance < saw_wait &&
               saw_wait < saw_clear && !saw_late_mb)
    }
' kernel/sync/rcu.c; then
    echo "RCU-sched: rcu_sched_synchronize must release-publish waiter state before the Store-Load fence and avoid a redundant clear-path fence" >&2
    exit 1
fi

if ! awk '
    /static void thread_destroy_wait_inactive\(thread_t \*thp, process_t \*prp\)/ {
        in_wait = 1
    }
    in_wait && /__atomic_load_n\(&pcpu_array\[hart\]\.pc_curthread,/ {
        saw_cur = NR
        in_cur = 1
    }
    in_wait && in_cur && /__ATOMIC_/ && !/__ATOMIC_ACQUIRE/ {
        bad_cur = NR
        in_cur = 0
    }
    in_wait && in_cur && /__ATOMIC_ACQUIRE/ {
        saw_cur_acquire = NR
        in_cur = 0
    }
    in_wait && /__atomic_load_n\(&in_kcall\[hart\],/ {
        saw_depth = NR
        in_depth = 1
    }
    in_wait && in_depth && /__ATOMIC_/ && !/__ATOMIC_ACQUIRE/ {
        bad_depth = NR
        in_depth = 0
    }
    in_wait && in_depth && /__ATOMIC_ACQUIRE/ {
        saw_depth_acquire = NR
        in_depth = 0
    }
    in_wait && /SENDIPI\(\(unsigned\) active_hart, IPI_RESCHED\);/ {
        saw_ipi = NR
    }
    in_wait && /thread_cpu_relax\(\);/ { saw_relax = NR }
    in_wait && /^}/ { in_wait = 0 }
    END {
        exit !(saw_cur_acquire && saw_depth_acquire && saw_ipi && saw_relax &&
               saw_cur_acquire < saw_depth_acquire &&
               saw_depth_acquire < saw_ipi && saw_ipi < saw_relax &&
               !bad_cur && !bad_depth)
    }
' kernel/thread/lifecycle.c; then
    echo "RCU-sched: freed-active gate must acquire-load peer state and kick stuck harts" >&2
    exit 1
fi

if ! awk '
    /void rdecl thread_destroy\(thread_t \*thp\)/ { in_destroy = 1 }
    in_destroy && /stats_enabled =/ { saw_stats_assign = NR }
    in_destroy &&
        /__atomic_load_n\(&thread_destroy_stats_enabled, __ATOMIC_ACQUIRE\);/ &&
        saw_stats_assign {
        saw_stats_load = NR
    }
    in_destroy && /destroy_entry_cycles = cpu_clock_ticks\(\);/ &&
        saw_stats_load {
        saw_entry_cycles = NR
    }
    in_destroy && /\/\* Quiescence gate/ && saw_entry_cycles {
        saw_quiescence = NR
    }
    in_destroy && /if \(join_owned\) {/ && saw_quiescence {
        saw_join_owned = NR
    }
    in_destroy && /ready\(jthp\);/ && saw_join_owned {
        saw_ready = NR
    }
    in_destroy && /join_woke = 1;/ && saw_ready {
        saw_join_woke = NR
    }
    in_destroy && /join_wake_cycles = cpu_clock_ticks\(\);/ && saw_join_woke {
        saw_join_cycles = NR
    }
    in_destroy && /tthread_unpin\(jthp\);/ && saw_join_cycles {
        saw_unpin = NR
    }
    END {
        exit !(saw_stats_assign && saw_stats_load && saw_entry_cycles &&
               saw_quiescence && saw_join_owned && saw_ready &&
               saw_join_woke && saw_join_cycles && saw_unpin &&
               saw_stats_assign < saw_stats_load &&
               saw_stats_load < saw_entry_cycles &&
               saw_entry_cycles < saw_quiescence &&
               saw_join_owned < saw_ready &&
               saw_ready < saw_join_woke &&
               saw_join_woke < saw_join_cycles &&
               saw_join_cycles < saw_unpin)
    }
' kernel/thread/lifecycle.c; then
    echo "RCU-sched: thread destroy join-wake stats must pin timestamp sites" >&2
    exit 1
fi

if ! awk '
    /void rdecl thread_destroy\(thread_t \*thp\)/ { in_destroy = 1 }
    in_destroy && /stats_destroy_t2 = cpu_clock_ticks\(\);/ { saw_t2 = NR }
    in_destroy && /thread_destroy_wait_inactive\(thp, prp\);/ && saw_t2 &&
        !saw_wait_inactive {
        saw_wait_inactive = NR
    }
    in_destroy && /stats_destroy_t3 = cpu_clock_ticks\(\);/ && saw_wait_inactive {
        saw_t3 = NR
    }
    in_destroy &&
        /__atomic_load_n\(&thp->destroy_claim, __ATOMIC_RELAXED\) >=/ &&
        saw_t3 {
        saw_claim_load = NR
    }
    in_destroy && /THREAD_DESTROY_CLAIM_RESERVED_BASE;/ && saw_claim_load {
        saw_claim_threshold = NR
    }
    in_destroy &&
        /object_free_track_heap_detail\(prp, &thread_souls, thp,/ &&
        saw_claim_threshold {
        saw_free = NR
    }
    in_destroy && /stats_destroy_t4 = cpu_clock_ticks\(\);/ && saw_free {
        saw_t4 = NR
    }
    in_destroy && /stats_have = true;/ && saw_t4 {
        saw_stats_ready = NR
    }
    in_destroy && /if \(last_active\)/ && saw_stats_ready {
        saw_notify_block = NR
    }
    in_destroy && /uint64_t t0 = stats_enabled && join_woke \? cpu_clock_ticks\(\) : 0;/ &&
        saw_notify_block {
        saw_notify_t0 = NR
    }
    in_destroy && /proc_threads_destroyed\(prp, &ev\);/ && saw_notify_t0 {
        saw_notify_fill = NR
    }
    in_destroy && /sigevent_notification_roottask\(ev.sigev_code,/ &&
        saw_notify_fill {
        saw_notify_send = NR
    }
    in_destroy && /post_notify_cycles = cpu_clock_ticks\(\) - t0;/ &&
        saw_notify_send {
        saw_notify_done = NR
    }
    in_destroy && /if \(stats_have\)/ && saw_notify_done {
        saw_stats_have_check = NR
    }
    in_destroy && /thread_destroy_stats_add\(/ && saw_stats_have_check {
        in_add = 1
    }
    in_add &&
        /thread_destroy_cycles_to_ns\(stats_destroy_t1 - stats_destroy_t0\),/ {
        saw_rcu_arg = NR
    }
    in_add &&
        /thread_destroy_cycles_to_ns\(stats_destroy_t2 - stats_destroy_t1\),/ &&
        saw_rcu_arg {
        saw_pin_arg = NR
    }
    in_add &&
        /thread_destroy_cycles_to_ns\(stats_destroy_t3 - stats_destroy_t2\),/ &&
        saw_pin_arg {
        saw_active_arg = NR
    }
    in_add &&
        /thread_destroy_cycles_to_ns\(stats_destroy_t4 - stats_destroy_t3\),/ &&
        saw_active_arg {
        saw_free_arg = NR
    }
    in_add && /thread_destroy_cycles_to_ns\(stats_clear_cycles\),/ &&
        saw_free_arg {
        saw_clear_arg = NR
    }
    in_add && /thread_destroy_cycles_to_ns\(stats_link_cycles\), stats_pin_waited,/ &&
        saw_clear_arg {
        saw_link_arg = NR
    }
    in_add &&
        /stats_object_free_heap, stats_destroy_claim_reserved, join_woke,/ &&
        saw_link_arg {
        saw_join_flag_arg = NR
    }
    in_add &&
        /join_woke \? thread_destroy_cycles_to_ns\(join_wake_cycles -/ &&
        saw_join_flag_arg {
        saw_join_ns_arg = NR
    }
    in_add && /destroy_entry_cycles\)/ && saw_join_ns_arg {
        saw_join_ns_end_arg = NR
    }
    in_add &&
        /join_woke \? thread_destroy_cycles_to_ns\(stats_destroy_t4 -/ &&
        saw_join_ns_end_arg {
        saw_post_ready_arg = NR
    }
    in_add && /join_wake_cycles\)/ && saw_post_ready_arg {
        saw_post_ready_end_arg = NR
    }
    in_add && /join_woke \? thread_destroy_cycles_to_ns\(post_notify_cycles\) : 0,/ &&
        saw_post_ready_end_arg {
        saw_notify_arg = NR
    }
    in_add && /&stats_busy\);/ && saw_notify_arg {
        saw_end_arg = NR
        in_add = 0
    }
    END {
        exit !(saw_t2 && saw_wait_inactive && saw_t3 && saw_free && saw_t4 &&
               saw_t2 < saw_wait_inactive && saw_wait_inactive < saw_t3 &&
               saw_t3 < saw_claim_load &&
               saw_claim_load < saw_claim_threshold &&
               saw_claim_threshold < saw_free && saw_free < saw_t4 &&
               saw_stats_ready && saw_notify_block && saw_notify_t0 &&
               saw_notify_fill && saw_notify_send && saw_notify_done &&
               saw_stats_have_check && saw_rcu_arg && saw_free_arg &&
               saw_pin_arg && saw_active_arg && saw_clear_arg &&
               saw_end_arg && saw_t4 < saw_stats_ready &&
               saw_stats_ready < saw_notify_block &&
               saw_notify_block < saw_notify_t0 &&
               saw_notify_t0 < saw_notify_fill &&
               saw_notify_fill < saw_notify_send &&
               saw_notify_send < saw_notify_done &&
               saw_notify_done < saw_stats_have_check &&
               saw_stats_have_check < saw_rcu_arg &&
               saw_rcu_arg < saw_pin_arg &&
               saw_pin_arg < saw_active_arg &&
               saw_active_arg < saw_free_arg &&
               saw_free_arg < saw_clear_arg &&
               saw_clear_arg < saw_link_arg &&
               saw_free_arg < saw_join_flag_arg &&
               saw_join_flag_arg < saw_join_ns_arg &&
               saw_join_ns_arg < saw_join_ns_end_arg &&
               saw_join_ns_end_arg < saw_post_ready_arg &&
               saw_post_ready_arg < saw_post_ready_end_arg &&
               saw_post_ready_end_arg < saw_notify_arg &&
               saw_notify_arg < saw_end_arg)
    }
' kernel/thread/lifecycle.c; then
    echo "RCU-sched: thread destroy stats must split active scan before object free" >&2
    exit 1
fi

if ! awk '
    /void rcu_sched_quiescent\(void\)/ { in_qs = 1 }
    in_qs && /__atomic_load_n\(&rcu_sched_epoch, __ATOMIC_RELAXED\)/ {
        saw_epoch = NR
    }
    in_qs && /__atomic_load_n\(&pcpu_array\[cpu\]\.pc_rcu_qs, __ATOMIC_RELAXED\) == epoch/ {
        saw_qs_load = NR
    }
    in_qs && /return;/ && saw_qs_load && !saw_return { saw_return = NR }
    in_qs && /__atomic_store_n\(&pcpu_array\[cpu\]\.pc_rcu_qs, epoch,/ {
        in_store = 1
    }
    in_qs && in_store && /__ATOMIC_RELEASE/ {
        saw_store = NR
        in_store = 0
    }
    in_qs && /^}/ { in_qs = 0 }
    END {
        exit !(saw_epoch && saw_qs_load && saw_return && saw_store &&
               saw_epoch < saw_qs_load && saw_qs_load < saw_return &&
               saw_return < saw_store)
    }
' kernel/sync/rcu.c; then
    echo "RCU-sched: rcu_sched_quiescent must bypass redundant QS stores before release-publishing" >&2
    exit 1
fi

if ! awk '
    /static inline int rcu_cpu_alive\(unsigned cpu\)/ { in_alive = 1 }
    in_alive && /__atomic_load_n\(&pcpu_array\[cpu\]\.pc_alive, __ATOMIC_ACQUIRE\)/ {
        saw_atomic = NR
    }
    in_alive && /^}/ { in_alive = 0 }
    /pcpu_array\[[^]]+\]\.pc_alive/ {
        line = $0
        gsub(/__atomic_load_n\(&pcpu_array\[cpu\]\.pc_alive, __ATOMIC_ACQUIRE\)/,
             "", line)
        if (line ~ /pcpu_array\[[^]]+\]\.pc_alive/)
            saw_plain = NR
    }
    END { exit !(saw_atomic && !saw_plain) }
' kernel/sync/rcu.c; then
    echo "RCU-sched: pc_alive peer checks must use acquire atomic loads" >&2
    exit 1
fi

if ! awk '
    /rcu_epoch_t rcu_sched_advance\(void\)/ { in_adv = 1 }
    in_adv && /cpu_smp_mb\(\);/ { saw_mb = NR }
    in_adv && /old = \(rcu_epoch_t\) atomic_add_value/ { saw_add = NR }
    in_adv && /return old \+ RCU_EPOCH_INCR;/ { saw_return = NR }
    in_adv && /^}/ { in_adv = 0 }
    END { exit !(saw_add && saw_return && saw_add < saw_return && !saw_mb) }
' kernel/sync/rcu.c; then
    echo "RCU-sched: rcu_sched_advance must rely on the aqrl AMO and return the post-increment epoch" >&2
    exit 1
fi

if ! grep -q '#define RCU_EPOCH_INIT ((rcu_epoch_t) 0u)' kernel/include/rcu.h; then
    echo "RCU-sched: initial QS stamps must compare before the first grace period" >&2
    exit 1
fi

if grep -Rqw 'rcu_read_held' kernel; then
    echo "RCU-sched: do not reintroduce the always-false rcu_read_held helper" >&2
    exit 1
fi

if ! awk '
    /void rcu_sched_wait\(rcu_epoch_t goal\)/ { in_wait = 1 }
    in_wait && /rcu_cpu_relax\(\);/ { saw_relax = NR }
    in_wait && /cpu_smp_mb\(\);/ { saw_mb = NR }
    in_wait && /^}/ { saw_end = NR; in_wait = 0 }
    END { exit !(saw_relax && saw_mb && saw_end && saw_relax < saw_mb && saw_mb < saw_end) }
' kernel/sync/rcu.c; then
    echo "RCU-sched: rcu_sched_wait must fence before returning" >&2
    exit 1
fi

if ! awk '
    /void rcu_sched_wait/ { in_wait = 1 }
    in_wait && /unsigned self = get_cpunum\(\);/ { saw_self = 1 }
    in_wait && /if \(cpu == self\)/ { saw_skip = 1 }
    in_wait && /__atomic_load_n\(&in_kcall\[cpu\], __ATOMIC_RELAXED\)/ {
        saw_relaxed_counter = 1
    }
    in_wait && /__atomic_load_n\(&in_kcall\[cpu\], __ATOMIC_ACQUIRE\)/ {
        saw_acquire_counter = 1
    }
    in_wait && /pc_in_kcall/ { saw_stale_counter = 1 }
    in_wait && /^}/ { in_wait = 0 }
    END {
        exit !(saw_self && saw_skip && saw_relaxed_counter &&
               saw_acquire_counter && !saw_stale_counter)
    }
' kernel/sync/rcu.c; then
    echo "RCU-sched: rcu_sched_synchronize must skip self and use live in_kcall[]" >&2
    exit 1
fi

if ! awk '
    /void rcu_sched_wait\(rcu_epoch_t goal\)/ { in_wait = 1 }
    in_wait && /pcpu_array\[cpu\]\.pc_rcu_qs,/ { in_epoch = 1 }
    in_wait && in_epoch && /__ATOMIC_RELAXED/ {
        saw_epoch_relaxed = NR
        in_epoch = 0
    }
    in_wait && in_epoch && /__ATOMIC_ACQUIRE/ {
        saw_epoch_acquire = NR
        in_epoch = 0
    }
    in_wait && saw_epoch_relaxed && /pcpu_array\[cpu\]\.pc_rcu_qs,/ {
        in_epoch_confirm = 1
    }
    in_wait && in_epoch_confirm && /__ATOMIC_ACQUIRE/ {
        saw_epoch_acquire = NR
        in_epoch_confirm = 0
    }
    in_wait && /rcu_cpu_relax\(\);/ { saw_pause = NR }
    in_wait && /\.insn r 0x0F, 0, 0, x0, x0, x0/ { saw_bad = 1 }
    in_wait && /^}/ { in_wait = 0 }
    END {
        exit !(saw_epoch_relaxed && saw_epoch_acquire && saw_pause &&
               saw_epoch_relaxed < saw_epoch_acquire &&
               saw_epoch_acquire < saw_pause && !saw_bad)
    }
' kernel/sync/rcu.c; then
    echo "RCU-sched: rcu_sched_wait must acquire-confirm QS epochs and use the RISC-V pause helper" >&2
    exit 1
fi

if ! awk '
    /static inline void rcu_cpu_relax\(void\)/ { in_relax = 1 }
    in_relax && /\.word 0x0100000f/ { saw_pause = NR }
    in_relax && /\.insn r 0x0F, 0, 0, x0, x0, x0/ { saw_bad = 1 }
    in_relax && /^}/ { in_relax = 0 }
    END { exit !(saw_pause && !saw_bad) }
' kernel/sync/rcu.c; then
    echo "RCU-sched: rcu_cpu_relax must use the correct raw RISC-V pause encoding" >&2
    exit 1
fi

if ! awk '
    /#define rcu_assign_pointer\(ptr, val\)/ { in_assign = 1 }
    in_assign && /fence rw, w/ { saw_release = NR }
    in_assign && /(cpu_smp_mb\(\)|__cpu_membarrier|fence rw, rw)/ { saw_full = 1 }
    in_assign && /\(ptr\) = \(val\);/ { saw_store = NR }
    in_assign && /} while \(0\)/ { in_assign = 0 }
    END { exit !(saw_release && saw_store && saw_release < saw_store && !saw_full) }
' kernel/include/rcu.h; then
    echo "RCU-sched: rcu_assign_pointer must publish with a release fence" >&2
    exit 1
fi

if grep -Eq 'rcu_sched_(qs|waiting)' kernel/sync/rcu.c ||
   ! grep -Fq '__atomic_store_n(&pcpu_array[cpu].pc_rcu_qs' \
    kernel/sync/rcu.c ||
   ! grep -Fq 'pcpu_array[cpu].pc_rcu_qs' kernel/sync/rcu.c ||
   ! grep -Fq 'pcpu_array[cpu].pc_rcu_waiting' kernel/sync/rcu.c ||
   ! grep -Fq 'pcpu_array[get_cpunum()].pc_rcu_waiting' kernel/sync/rcu.c ||
   ! grep -Fq 'uint32_t pc_rcu_qs;' arch/riscv/include/pcpu.h ||
   ! grep -Fq 'unsigned pc_rcu_waiting;' arch/riscv/include/pcpu.h; then
    echo "RCU-sched: per-hart QS/waiting state must stay in pcpu to avoid false sharing" >&2
    exit 1
fi

if ! awk '
    /void kext_cred_set\(void \*data\)/ { in_cred = 1 }
    in_cred && /lookup_pid_rcu\(kap->pid\)/ { saw_lookup = NR }
    in_cred && /process_pin\(prp\)/ { saw_pin = NR }
    in_cred && /rcu_read_unlock\(&process_rcu\);/ { saw_unlock = NR }
    in_cred && /cred_set\(&prp->cred, kap->cip\)/ { saw_set = NR }
    in_cred && /process_unpin\(prp\)/ { saw_unpin = NR }
    in_cred && /lookup_pid\(kap->pid\)/ { saw_plain = NR }
    in_cred && /^}/ { in_cred = 0 }
    END {
        exit !(saw_lookup && saw_pin && saw_unlock && saw_set && saw_unpin &&
               saw_lookup < saw_pin && saw_pin < saw_unlock &&
               saw_unlock < saw_set && saw_set < saw_unpin && !saw_plain)
    }
' kernel/ext/cred.c; then
    echo "RCU-sched: cred_set must promote the process before closing process_rcu" >&2
    exit 1
fi

if ! awk '
    /void kext_proc_register_syschan\(void \*data\)/ { in_reg = 1 }
    in_reg && /lookup_pid_rcu\(kap->pid\)/ { saw_lookup = NR }
    in_reg && /rcu_read_lock\(&connobj_rcu\);/ { saw_conn = NR }
    in_reg && /kspinlock_lock\(&prp->vec_slock\);/ { saw_lock = NR }
    in_reg && /vector_lookup2\(&prp->chancons, \(unsigned\) kap->chid\)/ {
        saw_vec = NR
    }
    in_reg && /prp->sys_chp = chp;/ { saw_store = NR }
    in_reg && /kspinlock_unlock\(&prp->vec_slock\);/ { saw_unlock = NR }
    in_reg && /rcu_read_unlock\(&connobj_rcu\);/ { saw_conn_unlock = NR }
    in_reg && /rcu_read_unlock\(&process_rcu\);/ { saw_proc_unlock = NR }
    in_reg && /lookup_pid\(kap->pid\)/ { saw_plain = NR }
    in_reg && /^}/ { in_reg = 0 }
    END {
        exit !(saw_lookup && saw_conn && saw_lock && saw_vec && saw_store &&
               saw_unlock && saw_conn_unlock && saw_proc_unlock &&
               saw_lookup < saw_conn && saw_conn < saw_lock &&
               saw_lock < saw_vec && saw_vec < saw_store &&
               saw_store < saw_unlock && saw_unlock < saw_conn_unlock &&
               saw_conn_unlock < saw_proc_unlock && !saw_plain)
    }
' kernel/ext/query.c; then
    echo "RCU-sched: proc_register_syschan must store sys_chp under process/connobj RCU and vec_slock" >&2
    exit 1
fi

if [ "$(grep -Fc 'if (chp == prp->sys_chp)' kernel/ipc/channel.c)" -lt 2 ]; then
    echo "RCU-sched: channel destroy paths must clear retained sys_chp pointers" >&2
    exit 1
fi

if ! awk '
    /void notification_notify_proc_terminated\(process_t \*parent, process_t \*prp\)/ {
        in_notify = 1
    }
    in_notify && /channel_t \*chp;/ { saw_decl = NR }
    in_notify && /rcu_read_lock\(&connobj_rcu\);/ { saw_lock = NR }
    in_notify && /chp = parent->sys_chp;/ { saw_snapshot = NR }
    in_notify && /notification_deliver\(/ { saw_deliver = NR }
    in_notify && /rcu_read_unlock\(&connobj_rcu\);/ { saw_unlock = NR }
    in_notify && /notification_deliver\(.*parent->sys_chp/ { saw_direct = NR }
    in_notify && /^}/ { in_notify = 0 }
    END {
        exit !(saw_decl && saw_lock && saw_snapshot && saw_deliver &&
               saw_unlock && saw_lock < saw_snapshot &&
               saw_snapshot < saw_deliver && saw_deliver < saw_unlock &&
               !saw_direct)
    }
' kernel/ext/exit.c; then
    echo "RCU-sched: proc-terminated notification must hold connobj_rcu around sys_chp delivery" >&2
    exit 1
fi

if ! awk '
    /void kext_stack_cont\(void \*data\)/ { in_stack = 1 }
    in_stack && /lookup_pid_rcu\(kap->pid\)/ { saw_lookup = NR }
    in_stack && /rcu_read_lock\(&tthread_rcu\);/ { saw_tlock = NR }
    in_stack && /kspinlock_lock\(&prp->vec_slock\);/ { saw_vlock = NR }
    in_stack && /vector_lookup2\(&prp->threads, \(unsigned\) \(kap->tid - 1\)\)/ {
        saw_vec = NR
    }
    in_stack && /tthread_pin\(thp\)/ { saw_pin = NR }
    in_stack && /kspinlock_unlock\(&prp->vec_slock\);/ && saw_pin {
        saw_vunlock = NR
    }
    in_stack && /rcu_read_unlock\(&tthread_rcu\);/ && saw_pin {
        saw_tunlock = NR
    }
    in_stack && /rcu_read_unlock\(&process_rcu\);/ && saw_pin {
        saw_punlock = NR
    }
    in_stack && /ready\(thp\);/ { saw_ready = NR }
    in_stack && /tthread_unpin\(thp\);/ { saw_unpin = NR }
    in_stack && /lookup_pid\(kap->pid\)/ { saw_plain = NR }
    in_stack && /^}/ { in_stack = 0 }
    END {
        exit !(saw_lookup && saw_tlock && saw_vlock && saw_vec && saw_pin &&
               saw_vunlock && saw_tunlock && saw_punlock && saw_ready &&
               saw_unpin && saw_lookup < saw_tlock && saw_tlock < saw_vlock &&
               saw_vlock < saw_vec && saw_vec < saw_pin &&
               saw_pin < saw_vunlock && saw_vunlock < saw_tunlock &&
               saw_tunlock < saw_punlock && saw_punlock < saw_ready &&
               saw_ready < saw_unpin && !saw_plain)
    }
' kernel/ext/stack.c; then
    echo "RCU-sched: stack_cont must pin the target thread before ready() outside vec_slock" >&2
    exit 1
fi

if grep -Eq 'lookup_pid\(ROOTTASK_PID\)' kernel/ext/query.c kernel/mm/roottask-view.c ||
   [ "$(grep -Fc 'process_t *prp = roottask_prp;' kernel/ext/query.c)" -lt 2 ] ||
   ! grep -Fq 'root_prp = roottask_prp;' kernel/mm/roottask-view.c; then
    echo "RCU-sched: roottask helpers must use the stable roottask_prp cache" >&2
    exit 1
fi

if ! awk '
    /void kext_proc_destroy_chancons\(void \*data\)/ { in_destroy = 1 }
    in_destroy && /lookup_pid_rcu\(kap->pid\)/ { saw_lookup = NR }
    in_destroy && /process_pin\(prp\)/ { saw_pin = NR }
    in_destroy && /rcu_read_unlock\(&process_rcu\);/ { saw_unlock = NR }
    in_destroy && /for \(i = 0; i < prp->chancons.nentries; \+\+i\)/ {
        saw_loop = NR
    }
    in_destroy && /process_unpin\(prp\);/ { saw_unpin = NR }
    in_destroy && /lookup_pid\(kap->pid\)/ { saw_plain = NR }
    in_destroy && /^}/ { in_destroy = 0 }
    END {
        exit !(saw_lookup && saw_pin && saw_unlock && saw_loop && saw_unpin &&
               saw_lookup < saw_pin && saw_pin < saw_unlock &&
               saw_unlock < saw_loop && saw_loop < saw_unpin && !saw_plain)
    }
' kernel/ext/exit.c; then
    echo "RCU-sched: proc_destroy_chancons must pin the process before bulk teardown" >&2
    exit 1
fi

if ! awk '
    /void kext_process_destroyall\(void \*data\)/ { in_destroyall = 1 }
    in_destroyall && /lookup_pid_rcu\(pid\)/ { saw_lookup = NR }
    in_destroyall && /process_pin\(prp\)/ { saw_prp_pin = NR }
    in_destroyall && /rcu_read_unlock\(&process_rcu\);/ { saw_unlock = NR }
    in_destroyall && /kspinlock_lock\(&prp->vec_slock\);/ && !saw_lock {
        saw_lock = NR
    }
    in_destroyall && /\(thp = prp->valid_thp\)/ { saw_valid = NR }
    in_destroyall && /tthread_pin\(thp\)/ { saw_thp_pin = NR }
    in_destroyall && /thread_destroyall\(thp\)/ { saw_destroy = NR }
    in_destroyall && /ready\(thp\);/ { saw_ready = NR }
    in_destroyall && /tthread_unpin\(thp\);/ { saw_thp_unpin = NR }
    in_destroyall && /process_unpin\(prp\);/ && saw_thp_unpin {
        saw_prp_unpin = NR
    }
    in_destroyall && /lookup_pid\(pid\)/ { saw_plain = NR }
    in_destroyall && /^}/ { in_destroyall = 0 }
    END {
        exit !(saw_lookup && saw_prp_pin && saw_unlock && saw_lock &&
               saw_valid && saw_thp_pin && saw_destroy && saw_ready &&
               saw_thp_unpin && saw_prp_unpin &&
               saw_lookup < saw_prp_pin && saw_prp_pin < saw_unlock &&
               saw_unlock < saw_lock && saw_lock < saw_valid &&
               saw_valid < saw_thp_pin && saw_thp_pin < saw_destroy &&
               saw_destroy < saw_ready && saw_ready < saw_thp_unpin &&
               saw_thp_unpin < saw_prp_unpin && !saw_plain)
    }
' kernel/ext/process.c; then
    echo "RCU-sched: process_destroyall must pin process/thread before teardown and ready()" >&2
    exit 1
fi

if ! awk '
    /void kext_process_create\(void \*data\)/ { in_create = 1 }
    in_create && /lookup_pid_rcu\(kap->parent_pid\)/ { saw_lookup = NR }
    in_create && /process_pin\(parent\)/ { saw_pin = NR }
    in_create && /rcu_read_unlock\(&process_rcu\);/ { saw_unlock = NR }
    in_create && /vm_ops->mcreate\(prp\)/ { saw_mcreate = NR }
    in_create && /parent->cred->info/ { saw_parent_cred = NR }
    in_create && /kspinlock_lock\(&parent->vec_slock\);/ { saw_parent_lock = NR }
    in_create && /parent->child = prp;/ { saw_child_link = NR }
    in_create && /process_unpin\(parent\);/ && saw_child_link {
        saw_success_unpin = NR
    }
    in_create && /lookup_pid\(kap->parent_pid\)/ { saw_plain = NR }
    in_create && /^}/ { in_create = 0 }
    END {
        exit !(saw_lookup && saw_pin && saw_unlock && saw_mcreate &&
               saw_parent_cred && saw_parent_lock && saw_child_link &&
               saw_success_unpin && saw_lookup < saw_pin &&
               saw_pin < saw_unlock && saw_unlock < saw_mcreate &&
               saw_mcreate < saw_parent_cred &&
               saw_parent_cred < saw_parent_lock &&
               saw_parent_lock < saw_child_link &&
               saw_child_link < saw_success_unpin && !saw_plain)
    }
' kernel/ext/process.c; then
    echo "RCU-sched: process_create must pin parent after lookup_pid_rcu" >&2
    exit 1
fi

if ! awk '
    /void kext_process_startup\(void \*data\)/ { in_startup = 1 }
    in_startup && /lookup_pid_rcu\(kap->pid\)/ { saw_lookup = NR }
    in_startup && /process_pin\(prp\)/ { saw_prp_pin = NR }
    in_startup && /rcu_read_unlock\(&process_rcu\);/ { saw_unlock = NR }
    in_startup && /kspinlock_lock\(&prp->vec_slock\);/ { saw_lock = NR }
    in_startup && /thp = prp->valid_thp;/ { saw_valid = NR }
    in_startup && /tthread_pin\(thp\)/ { saw_thp_pin = NR }
    in_startup && /kspinlock_unlock\(&prp->vec_slock\);/ && saw_thp_pin {
        saw_vec_unlock = NR
    }
    in_startup && /cpu_thread_enter_umode\(thp\)/ { saw_enter = NR }
    in_startup && /tthread_unpin\(thp\);/ { saw_thp_unpin = NR }
    in_startup && /process_unpin\(prp\);/ && saw_thp_unpin {
        saw_prp_unpin = NR
    }
    in_startup && /lookup_pid\(kap->pid\)/ { saw_plain = NR }
    in_startup && /^}/ { in_startup = 0 }
    END {
        exit !(saw_lookup && saw_prp_pin && saw_unlock && saw_lock &&
               saw_valid && saw_thp_pin && saw_vec_unlock && saw_enter &&
               saw_thp_unpin && saw_prp_unpin &&
               saw_lookup < saw_prp_pin && saw_prp_pin < saw_unlock &&
               saw_unlock < saw_lock && saw_lock < saw_valid &&
               saw_valid < saw_thp_pin && saw_thp_pin < saw_vec_unlock &&
               saw_vec_unlock < saw_enter && saw_enter < saw_thp_unpin &&
               saw_thp_unpin < saw_prp_unpin && !saw_plain)
    }
' kernel/ext/process.c; then
    echo "RCU-sched: process_startup must pin process/thread across U-mode setup" >&2
    exit 1
fi

if ! awk '
    /void kext_as_memcpy\(void \*data\)/ { in_copy = 1 }
    in_copy && /lookup_pid_rcu\(target_pid_l\)/ { saw_lookup = NR }
    in_copy && /process_pin\(target_prp\)/ { saw_pin = NR }
    in_copy && /rcu_read_unlock\(&process_rcu\);/ { saw_unlock = NR }
    in_copy && /vm_ops->as\(target_prp, &pcpu_array\[cpu\]\.pc_as_prp\)/ {
        saw_switch = NR
    }
    in_copy && /xfer_memcpy\(/ { saw_copy = NR }
    in_copy && /vm_ops->as\(saved_as_prp, &pcpu_array\[cpu\]\.pc_as_prp\)/ {
        saw_restore = NR
    }
    in_copy && /process_unpin\(target_prp\);/ { saw_unpin = NR }
    in_copy && /lookup_pid\(target_pid_l\)/ { saw_plain = NR }
    in_copy && /^}/ { in_copy = 0 }
    END {
        exit !(saw_lookup && saw_pin && saw_unlock && saw_switch &&
               saw_copy && saw_restore && saw_unpin &&
               saw_lookup < saw_pin && saw_pin < saw_unlock &&
               saw_unlock < saw_switch && saw_switch < saw_copy &&
               saw_copy < saw_restore && saw_restore < saw_unpin &&
               !saw_plain)
    }
' kernel/ext/as-memcpy.c; then
    echo "RCU-sched: as_memcpy must pin target process across satp switch/copy" >&2
    exit 1
fi

if ! awk '
    /void kext_proc_list_children\(void \*data\)/ { in_list = 1 }
    in_list && /lookup_pid_rcu\(kap->parent_pid\)/ { saw_lookup = NR }
    in_list && /kspinlock_lock\(&parent->vec_slock\);/ { saw_lock = NR }
    in_list && /child = parent->child/ { saw_walk = NR }
    in_list && /kspinlock_unlock\(&parent->vec_slock\);/ { saw_unlock = NR }
    in_list && /rcu_read_unlock\(&process_rcu\);/ { saw_rcu_unlock = NR }
    in_list && /lookup_pid\(kap->parent_pid\)/ { saw_plain = NR }
    in_list && /^}/ { in_list = 0 }
    END {
        exit !(saw_lookup && saw_lock && saw_walk && saw_unlock &&
               saw_rcu_unlock && saw_lookup < saw_lock && saw_lock < saw_walk &&
               saw_walk < saw_unlock && saw_unlock < saw_rcu_unlock &&
               !saw_plain)
    }
' kernel/ext/query.c; then
    echo "RCU-sched: proc_list_children must walk child list under process_rcu + parent vec_slock" >&2
    exit 1
fi

if ! awk '
    /void kext_process_destroy\(void \*data\)/ { in_destroy = 1 }
    in_destroy && /process_child_locks_lock\(prp, proc\);/ { saw_lock = NR }
    in_destroy && /prp->child = child->sibling;/ { saw_unlink = NR }
    in_destroy && /proc->child = child;/ { saw_link = NR }
    in_destroy && /process_child_locks_unlock\(prp, proc\);/ { saw_unlock = NR }
    in_destroy && /kspinlock_lock\(&process_vector_slock\);/ && saw_unlock {
        saw_after = NR
        in_destroy = 0
    }
    END {
        exit !(saw_lock && saw_unlink && saw_link && saw_unlock && saw_after &&
               saw_lock < saw_unlink && saw_unlink < saw_link &&
               saw_link < saw_unlock && saw_unlock < saw_after)
    }
' kernel/ext/process.c; then
    echo "RCU-sched: process_destroy child retarget must hold both child-list locks" >&2
    exit 1
fi

if ! awk '
    /void kext_reparent\(void \*data\)/ { in_reparent = 1 }
    in_reparent && /process_child_locks_lock\(old_parent, new_parent\);/ {
        saw_lock = NR
    }
    in_reparent && /\*pp = prp->sibling;/ { saw_unlink = NR }
    in_reparent && /new_parent->child = prp;/ { saw_link = NR }
    in_reparent && /process_child_locks_unlock\(old_parent, new_parent\);/ {
        saw_unlock = NR
    }
    in_reparent && /^}/ { in_reparent = 0 }
    END {
        exit !(saw_lock && saw_unlink && saw_link && saw_unlock &&
               saw_lock < saw_unlink && saw_unlink < saw_link &&
               saw_link < saw_unlock)
    }
' kernel/ext/process.c; then
    echo "RCU-sched: process reparent must hold both child-list locks" >&2
    exit 1
fi

if ! awk '
    /void kext_debug_process\(void \*args\)/ { in_debug = 1 }
    in_debug && /lookup_pid_rcu\(kap->pid\)/ { saw_lookup = NR }
    in_debug && /process_pin\(prp\)/ { saw_pin = NR }
    in_debug && /rcu_read_unlock\(&process_rcu\);/ { saw_unlock = NR }
    in_debug && /kspinlock_lock\(&prp->vec_slock\);/ { saw_vec_lock = NR }
    in_debug && /vector_search\(&prp->threads/ { saw_vec = NR }
    in_debug && /tthread_pin\(thp\)/ { saw_thp_pin = NR }
    in_debug && /kspinlock_unlock\(&prp->vec_slock\);/ { saw_vec_unlock = NR }
    in_debug && /^out:/ { saw_out = NR }
    in_debug && /tthread_unpin\(thp\)/ { saw_thp_unpin = NR }
    in_debug && /process_unpin\(prp\)/ { saw_prp_unpin = NR }
    in_debug && /lookup_pid\(kap->pid\)/ { saw_plain = NR }
    in_debug && /^}/ { in_debug = 0 }
    END {
        exit !(saw_lookup && saw_pin && saw_unlock && saw_vec_lock &&
               saw_vec && saw_thp_pin && saw_vec_unlock && saw_out &&
               saw_thp_unpin && saw_prp_unpin &&
               saw_lookup < saw_pin && saw_pin < saw_unlock &&
               saw_unlock < saw_vec_lock && saw_vec_lock < saw_vec &&
               saw_vec < saw_thp_pin && saw_thp_pin < saw_vec_unlock &&
               saw_vec_unlock < saw_out && saw_out < saw_thp_unpin &&
               saw_thp_unpin < saw_prp_unpin && !saw_plain)
    }
' kernel/ext/debug.c; then
    echo "RCU-sched: debug_process must pin process/thread after lookup_pid_rcu" >&2
    exit 1
fi

if ! awk '
    /void kext_debug_attach\(void \*args\)/ { in_attach = 1 }
    in_attach && /lookup_pid_rcu\(kap->pid\)/ { saw_lookup = NR }
    in_attach && /process_pin\(prp\)/ { saw_pin = NR }
    in_attach && /rcu_read_unlock\(&process_rcu\);/ { saw_unlock = NR }
    in_attach && /object_alloc\(prp, &debug_souls\)/ { saw_alloc = NR }
    in_attach && /process_unpin\(prp\)/ && saw_alloc { saw_unpin = NR }
    in_attach && /lookup_pid\(kap->pid\)/ { saw_plain = NR }
    in_attach && /^}/ { in_attach = 0 }
    END {
        exit !(saw_lookup && saw_pin && saw_unlock && saw_alloc &&
               saw_unpin && saw_lookup < saw_pin && saw_pin < saw_unlock &&
               saw_unlock < saw_alloc && saw_alloc < saw_unpin && !saw_plain)
    }
' kernel/ext/debug.c; then
    echo "RCU-sched: debug_attach must pin process before debug object install" >&2
    exit 1
fi

if ! awk '
    /void kext_debug_detach\(void \*args\)/ { in_detach = 1 }
    in_detach && /lookup_pid_rcu\(kap->pid\)/ { saw_lookup = NR }
    in_detach && /process_pin\(prp\)/ { saw_pin = NR }
    in_detach && /rcu_read_unlock\(&process_rcu\);/ { saw_unlock = NR }
    in_detach && /prp->debugger = 0;/ { saw_clear = NR }
    in_detach && /object_free\(prp, &debug_souls, dep\)/ { saw_free = NR }
    in_detach && /process_unpin\(prp\)/ { saw_unpin = NR }
    in_detach && /lookup_pid\(kap->pid\)/ { saw_plain = NR }
    in_detach && /^}/ { in_detach = 0 }
    END {
        exit !(saw_lookup && saw_pin && saw_unlock && saw_clear &&
               saw_free && saw_unpin && saw_lookup < saw_pin &&
               saw_pin < saw_unlock && saw_unlock < saw_clear &&
               saw_clear < saw_free && saw_free < saw_unpin && !saw_plain)
    }
' kernel/ext/debug.c; then
    echo "RCU-sched: debug_detach must pin process through debugger teardown" >&2
    exit 1
fi

if ! awk '
    /int rdecl thread_set_as_prp\(thread_t \*thp, process_t \*prp\)/ {
        in_set = 1
    }
    in_set && /kspinlock_lock\(&thread_as_slock\);/ { saw_lock = NR }
    in_set && /process_pin\(prp\)/ { saw_pin = NR }
    in_set && /old = thp->as_prp;/ { saw_old = NR }
    in_set && /thp->as_prp = prp;/ { saw_assign = NR }
    in_set && /process_unpin\(old\)/ { saw_unpin = NR }
    in_set && /kspinlock_unlock\(&thread_as_slock\);/ { saw_unlock = NR }
    in_set && /^}/ { in_set = 0 }
    END {
        exit !(saw_lock && saw_pin && saw_old && saw_assign && saw_unpin &&
               saw_unlock && saw_lock < saw_pin && saw_pin < saw_old &&
               saw_old < saw_assign && saw_assign < saw_unpin &&
               saw_unpin < saw_unlock)
    }
' kernel/thread/lifecycle.c; then
    echo "RCU-sched: thread_set_as_prp must serialize and pin new foreign process before retaining it" >&2
    exit 1
fi

if ! awk '
    /void rdecl thread_clear_as_prp_if\(thread_t \*thp, process_t \*prp\)/ {
        in_clear = 1
    }
    in_clear && /kspinlock_lock\(&thread_as_slock\);/ { saw_lock = NR }
    in_clear && /if \(thp->as_prp == prp\)/ { saw_if = NR }
    in_clear && /thp->as_prp = NULL;/ { saw_clear = NR }
    in_clear && /process_unpin\(prp\)/ { saw_unpin = NR }
    in_clear && /kspinlock_unlock\(&thread_as_slock\);/ { saw_unlock = NR }
    in_clear && /^}/ { in_clear = 0 }
    END {
        exit !(saw_lock && saw_if && saw_clear && saw_unpin && saw_unlock &&
               saw_lock < saw_if && saw_if < saw_clear &&
               saw_clear < saw_unpin && saw_unpin < saw_unlock)
    }
' kernel/thread/lifecycle.c; then
    echo "RCU-sched: thread_clear_as_prp_if must clear matching retained bindings under thread_as_slock" >&2
    exit 1
fi

if ! awk '
    /static kspinlock_t thread_as_slock;/ { saw_decl = NR }
    /kspinlock_init\(&thread_as_slock, LOCK_CLASS_THREAD/ { saw_init = NR }
    END { exit !(saw_decl && saw_init && saw_decl < saw_init) }
' kernel/thread/lifecycle.c; then
    echo "RCU-sched: thread_as_slock must be initialized before as_prp helper use" >&2
    exit 1
fi

if ! awk '
    /int rdecl thread_create\(thread_t \*act,/ { in_create = 1 }
    in_create && /thread_set_as_prp\(thp, as_prp\)/ { saw_set = NR }
    in_create && /cpu_thread_init\(act, thp, align\)/ { saw_init = NR }
    in_create && /^}/ { in_create = 0 }
    END {
        exit !(saw_set && saw_init && saw_set < saw_init)
    }
' kernel/thread/lifecycle.c; then
    echo "RCU-sched: thread_create must pin inherited foreign as_prp before publish" >&2
    exit 1
fi

if ! awk '
    /void rdecl thread_destroy\(thread_t \*thp\)/ { in_destroy = 1 }
    in_destroy && /thread_set_as_prp\(thp, NULL\)/ { saw_clear = NR }
    in_destroy && /\(prp->threads.nentries - 1 != prp->threads.nfree\)/ {
        saw_zombie = NR
    }
    in_destroy && /vector_rem\(&prp->threads/ { saw_rem = NR }
    in_destroy && /^}/ { in_destroy = 0 }
    END {
        exit !(saw_clear && saw_zombie && saw_rem &&
               saw_clear < saw_zombie && saw_zombie < saw_rem)
    }
' kernel/thread/lifecycle.c; then
    echo "RCU-sched: thread_destroy must release retained as_prp before zombie return or free" >&2
    exit 1
fi

if ! awk '
    /void kext_process_bind\(void \*data\)/ { in_bind = 1 }
    in_bind && /lookup_pid_rcu\(pid\)/ { saw_lookup = NR }
    in_bind && /prp->flags & RTMUX_FLG_PROC_TERMING/ { saw_terming = NR }
    in_bind && /thread_set_as_prp\(act, prp\)/ { saw_set = NR }
    in_bind && /rcu_read_unlock\(&process_rcu\);/ && saw_set {
        saw_unlock = NR
    }
    in_bind && /SETKSTATUS\(act, prp \? prp->pid : 0\)/ { saw_status = NR }
    in_bind && /lookup_pid\(pid\)/ { saw_plain = NR }
    in_bind && /^}/ { in_bind = 0 }
    END {
        exit !(saw_lookup && saw_terming && saw_set && saw_unlock && saw_status &&
               saw_lookup < saw_terming && saw_terming < saw_set &&
               saw_set < saw_unlock &&
               saw_unlock < saw_status && !saw_plain)
    }
' kernel/ext/bind.c; then
    echo "RCU-sched: process_bind must reject terming targets and use retained as_prp pin ownership" >&2
    exit 1
fi

if ! awk '
    /void kext_process_shutdown\(void \*data\)/ { in_shutdown = 1 }
    in_shutdown && /process_clear_as_bindings\(prp\)/ { saw_clear = NR }
    in_shutdown && /process_wait_unpinned\(prp\)/ { saw_wait = NR }
    in_shutdown && /vm_ops->mdestroy\(prp\)/ { saw_destroy = NR }
    in_shutdown && /^}/ { in_shutdown = 0 }
    END {
        exit !(saw_clear && saw_wait && saw_destroy &&
               saw_clear < saw_wait && saw_wait < saw_destroy)
    }
' kernel/ext/process.c; then
    echo "RCU-sched: process_shutdown must clear retained as_prp pins before mdestroy" >&2
    exit 1
fi

if ! awk '
    /static void process_clear_as_bindings\(process_t \*target\)/ {
        in_clear = 1
    }
    in_clear && /rcu_read_lock\(&process_rcu\);/ { saw_rcu = NR }
    in_clear && /vector_lookup\(&process_vector/ { saw_process = NR }
    in_clear && /kspinlock_lock\(&owner->vec_slock\);/ { saw_lock = NR }
    in_clear && /thread_clear_as_prp_if\(thp, target\)/ { saw_clear = NR }
    in_clear && /kspinlock_unlock\(&owner->vec_slock\);/ { saw_unlock = NR }
    in_clear && /rcu_read_unlock\(&process_rcu\);/ { saw_rcu_unlock = NR }
    in_clear && /^}/ { in_clear = 0 }
    END {
        exit !(saw_rcu && saw_process && saw_lock && saw_clear &&
               saw_unlock && saw_rcu_unlock &&
               saw_rcu < saw_process && saw_process < saw_lock &&
               saw_lock < saw_clear && saw_clear < saw_unlock &&
               saw_unlock < saw_rcu_unlock)
    }
' kernel/ext/process.c; then
    echo "RCU-sched: process_clear_as_bindings must scan all live thread vectors under RCU" >&2
    exit 1
fi

if ! awk '
    /thp->process && thp->process->pid == ROOTTASK_PID/ { saw_if = NR }
    /thread_set_as_prp\(thp, thp->process\)/ { saw_set = NR }
    /thp->as_prp = thp->process/ { saw_plain = NR }
    END { exit !(saw_if && saw_set && saw_if < saw_set && !saw_plain) }
' arch/riscv/exit.c; then
    echo "RCU-sched: roottask U-mode return must release foreign as_prp through helper" >&2
    exit 1
fi

if ! awk '
    /inline_lookup_connect\(process_t \*prp,/ {
        in_inline = 1
    }
    in_inline && /rtmux_cap_lookup_pin_context\(/ { saw_pin = NR }
    in_inline && /kspinlock_lock\(&prp->vec_slock\);/ && !saw_lock {
        saw_lock = NR
    }
    in_inline && /cop = vector_lookup2\(vec,/ && !saw_vec { saw_vec = NR }
    in_inline && /if \(cop != object\)/ && !saw_compare { saw_compare = NR }
    in_inline && /kspinlock_unlock\(&prp->vec_slock\);/ && saw_compare &&
        !saw_unlock {
        saw_unlock = NR
    }
    in_inline && /rtmux_cap_unpin\(&prp->endpoint_caps, \(rtmux_cap_t\) coid\)/ {
        saw_unpin = NR
    }
    in_inline && /^}/ { in_inline = 0 }
    END {
        exit !(saw_pin && saw_lock && saw_vec && saw_compare &&
               saw_unlock && saw_unpin &&
               saw_pin < saw_lock && saw_lock < saw_vec &&
               saw_vec < saw_compare && saw_compare < saw_unlock &&
               saw_unlock < saw_unpin)
    }
' kernel/ipc/fastmsg.c; then
    echo "RCU-sched: inline_lookup_connect must validate generated caps against the vector slot before unpin" >&2
    exit 1
fi

if ! awk '
    /lookup_connect\(uint64_t coid\)/ { in_lookup = 1 }
    in_lookup && /rtmux_cap_lookup_pin_context\(/ { saw_pin = NR }
    in_lookup && /kspinlock_lock\(&prp->vec_slock\);/ && !saw_lock {
        saw_lock = NR
    }
    in_lookup && /cop = vector_lookup2\(vec,/ && !saw_vec { saw_vec = NR }
    in_lookup && /if \(cop != object\)/ && !saw_compare { saw_compare = NR }
    in_lookup && /kspinlock_unlock\(&prp->vec_slock\);/ && saw_compare &&
        !saw_unlock {
        saw_unlock = NR
    }
    in_lookup && /rtmux_cap_unpin\(&prp->endpoint_caps, \(rtmux_cap_t\) coid\)/ {
        saw_unpin = NR
    }
    in_lookup && /^}/ { in_lookup = 0 }
    END {
        exit !(saw_pin && saw_lock && saw_vec && saw_compare &&
               saw_unlock && saw_unpin &&
               saw_pin < saw_lock && saw_lock < saw_vec &&
               saw_vec < saw_compare && saw_compare < saw_unlock &&
               saw_unlock < saw_unpin)
    }
' kernel/object/lookup.c; then
    echo "RCU-sched: lookup_connect must validate generated caps against the vector slot before unpin" >&2
    exit 1
fi

if ! awk '
    /lookup_pid_rcu\(pid_t pid\)/ { in_pid = 1 }
    in_pid && /rcu_sched_read_lock\(\);/ { saw_pid_lock = NR }
    in_pid && /rcu_read_lock\(&process_rcu\);/ { saw_pid_old = NR }
    in_pid && /^}/ { in_pid = 0 }
    /volatile uint8_t rcvid_rcu_nesting\[PROCESSORS_MAX\]/ { saw_counter = NR }
    /void rdecl lookup_rcvid_open\(void\)/ { in_open = 1; saw_open_fn = NR }
    in_open && /__atomic_compare_exchange_n\(&rcvid_rcu_nesting\[cpu\]/ {
        saw_open_cas = NR
    }
    in_open && /rcu_sched_read_lock\(\);/ && !saw_open_conn {
        saw_open_conn = NR
    }
    in_open && /rcu_sched_read_lock\(\);/ && saw_open_conn &&
        NR != saw_open_conn {
        saw_open_thread = NR
    }
    in_open && /rcu_read_lock\(&connobj_rcu\);/ { saw_old_conn = NR }
    in_open && /rcu_read_lock\(&tthread_rcu\);/ { saw_old_thread = NR }
    in_open && /^}/ { in_open = 0 }
    /lookup_rcvid\(kcall_args_t \*kap,/ { in_lookup = 1 }
    in_lookup && /lookup_rcvid_open\(\);/ { saw_lookup_open = NR }
    in_lookup && /^}/ { in_lookup = 0 }
    /void rdecl lookup_rcvid_release\(void\)/ { in_release = 1 }
    in_release && /__atomic_compare_exchange_n\(&rcvid_rcu_nesting\[cpu\]/ {
        saw_release_cas = NR
    }
    in_release && /if \(old == 1\)/ { saw_outer = NR }
    in_release && /rcu_sched_read_unlock\(\);/ && !saw_thread_unlock {
        saw_thread_unlock = NR
    }
    in_release && /rcu_sched_read_unlock\(\);/ && saw_thread_unlock &&
        NR != saw_thread_unlock {
        saw_conn_unlock = NR
    }
    in_release && /rcu_read_unlock\(&tthread_rcu\);/ { saw_old_thread_unlock = NR }
    in_release && /rcu_read_unlock\(&connobj_rcu\);/ { saw_old_conn_unlock = NR }
    in_release && /^}/ { in_release = 0 }
    /rcvid_rcu_open/ { saw_old = NR }
    END {
        exit !(saw_pid_lock && !saw_pid_old &&
               saw_counter && saw_open_fn && saw_open_cas &&
               saw_open_conn && saw_open_thread && saw_lookup_open &&
               saw_release_cas && saw_outer && saw_thread_unlock &&
               saw_conn_unlock && !saw_old && !saw_old_conn &&
               !saw_old_thread && !saw_old_thread_unlock &&
               !saw_old_conn_unlock &&
               saw_counter < saw_open_fn && saw_open_cas < saw_open_conn &&
               saw_open_conn < saw_open_thread && saw_open_thread < saw_lookup_open &&
               saw_release_cas < saw_outer && saw_outer < saw_thread_unlock &&
               saw_thread_unlock < saw_conn_unlock)
    }
' kernel/object/lookup.c; then
    echo "RCU-sched: lookup_rcvid cleanup must use a nesting counter, not a boolean flag" >&2
    exit 1
fi

if ! awk '
    /static int sendv_body\(thread_t \*act,/ {
        in_send = 1
    }
    in_send && /lookup_rcvid_open\(\);/ { saw_open = NR }
    in_send && /inline_lookup_connect\(actprp, kap->coid, &cap_context\)/ {
        saw_lookup = NR
    }
    in_send && /chp = cop->channel/ { saw_channel = NR }
    in_send && /^}/ { in_send = 0 }
    /int kdecl ksys_msg_sendv\(thread_t \*act,/ { in_wrapper = 1 }
    in_wrapper && /sendv_body\(act, kap, &held\)/ { saw_call = NR }
    in_wrapper && /if \(held\)/ { saw_held = NR }
    in_wrapper && /connect_client_drop\(held, act->real_priority\)/ { saw_drop = NR }
    in_wrapper && /^}/ { in_wrapper = 0 }
    END {
        if (!saw_open) print "RCU detail: send lock" > "/dev/stderr"
        exit !(saw_open && saw_lookup && saw_channel && saw_call && saw_held && saw_drop &&
               saw_open < saw_lookup && saw_lookup < saw_channel &&
               saw_call < saw_held && saw_held < saw_drop)
    }
' kernel/ipc/fastmsg.c; then
    echo "RCU-sched: msg_sendv must open connobj_rcu before endpoint lookup/channel deref" >&2
    exit 1
fi

if ! awk '
    /int kdecl ksys_msg_sendnotification\(thread_t \*act,/ {
        in_note = 1
    }
    in_note && /lookup_rcvid_open\(\);/ { saw_open = NR }
    in_note && /cop = lookup_connect\(kap->coid\)/ { saw_lookup = NR }
    in_note && /chp = cop->channel/ { saw_channel = NR }
    in_note && /^}/ { in_note = 0 }
    END {
        exit !(saw_open && saw_lookup && saw_channel &&
               saw_open < saw_lookup && saw_lookup < saw_channel)
    }
' kernel/ipc/fastmsg.c; then
    echo "RCU-sched: msg_sendnotification must open connobj_rcu before endpoint lookup/channel deref" >&2
    exit 1
fi

if ! awk '
    /int kdecl ksys_msg_receivev\(thread_t \*act,/ {
        in_recv = 1
    }
    in_recv && /chvec = &act->process->chancons;/ { saw_vec = NR }
    in_recv && /lookup_rcvid_open\(\);/ { saw_open = NR }
    in_recv && /chp = vector_lookup/ { saw_lookup = NR }
    in_recv && /kspinlock_lock\(&chp->lock\)/ { saw_lock = NR }
    in_recv && /^}/ { in_recv = 0 }
    END {
        exit !(saw_vec && saw_open && saw_lookup && saw_lock &&
               saw_vec < saw_open && saw_open < saw_lookup &&
               saw_lookup < saw_lock)
    }
' kernel/ipc/fastmsg.c; then
    echo "RCU-sched: msg_receivev must open connobj_rcu before channel lookup/lock" >&2
    exit 1
fi

if ! awk '
    /int kdecl ksys_timer_create\(thread_t \*act,/ {
        in_timer = 1
    }
    in_timer && /lookup_rcvid_open\(\);/ { saw_open = NR }
    in_timer && /cop = lookup_connect/ { saw_lookup = NR }
    in_timer && /chp = cop->channel/ { saw_channel = NR }
    in_timer && /^}/ { in_timer = 0 }
    END {
        exit !(saw_open && saw_lookup && saw_channel &&
               saw_open < saw_lookup && saw_lookup < saw_channel)
    }
' kernel/time/timer.c; then
    echo "RCU-sched: timer_create notification setup must open connobj_rcu before endpoint lookup/channel deref" >&2
    exit 1
fi

if ! awk '
    /long syscall_priv\(int id, void \*arg\)/ { in_priv = 1 }
    in_priv && /in_kcall_inc\(\);/ { saw_inc = NR }
    in_priv && /in_kcall_phase\(priv_call_phase\(\(unsigned\) id\)\);/ {
        saw_phase = NR
    }
    in_priv && /priv_dispatch\(act->process, \(unsigned\) id, arg\)/ {
        saw_dispatch = NR
    }
    in_priv && /^}/ { in_priv = 0 }
    END {
        exit !(saw_inc && saw_phase && saw_dispatch &&
               saw_inc < saw_phase && saw_phase < saw_dispatch)
    }
' arch/riscv/calls.c; then
    echo "RCU-sched: syscall_priv must stamp precise privileged-call phase" >&2
    exit 1
fi

tmp_lookup=$(mktemp)
trap 'rm -f "$tmp_lookup"' EXIT

grep -R 'lookup_pid(' kernel/ext kernel/mm |
    awk -F: '{
        line = $0
        sub($1 ":", "", line)
        sub(/^[[:space:]]+/, "", line)
        print $1 ":" line
    }' |
    sort > "$tmp_lookup"

if ! diff -u - "$tmp_lookup" <<'EOF' >/dev/null; then
EOF
    echo "RCU-sched: unexpected plain lookup_pid() site; convert it or classify the residual" >&2
    exit 1
fi

echo "rcu-sched: ok"
