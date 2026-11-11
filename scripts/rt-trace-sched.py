#!/usr/bin/env python3
"""Summarize scheduler/IPC timing from tracelogger serial output."""

import argparse
import re
import statistics
import sys
from collections import Counter, defaultdict, deque

from rt_trace_common import (  # shared with the other rt-trace parsers
    cycles_to_ns,
    parse_u32,
    percentile,
    read_cycles_per_sec,
    trace_class,
    trace_event,
)

EVENT_RE = re.compile(
    r"tracelogger:\s+buf\s+(\d+)\s+ev\s+(\d+)\s+"
    r"header=(0x[0-9a-fA-F]+|\d+)\s+"
    r"d=(0x[0-9a-fA-F]+|\d+),(0x[0-9a-fA-F]+|\d+),(0x[0-9a-fA-F]+|\d+)"
)
LKL_TRACE_RE = re.compile(
    r"lkl-trace:\s+mode=([A-Za-z0-9_-]+)\s+sample=(\d+)\s+"
    r"(?:pid=(\d+)\s+)?"
    r"(?:tid=(\d+)\s+)?"
    r"before_ns=(\d+)\s+after_ns=(\d+)\s+delta_ns=(\d+)"
)

TRACE_PR_TH_C = 4
TRACE_CONTROL_C = 1
TRACE_KER_CALL_C = 2
TRACE_SYSTEM_C = 5
TRACE_USER_C = 6
TRACE_COMM_C = 7
TRACE_INT_C = 3

USER_LKL_HOST_ENTER = 0x200
USER_LKL_HOST_EXIT = 0x201
USER_LKL_MEASURE_BEGIN = 0x202
USER_LKL_MEASURE_END = 0x203
USER_LKL_MEASURE_WAKE = 0x204
USER_LKL_MEASURE_MONO_LOW = 0x205
USER_LKL_MEASURE_MONO_HIGH = 0x206

STATE_NAMES = {
    0: "dead",
    1: "running",
    2: "ready",
    3: "stopped",
    4: "send",
    5: "receive",
    6: "reply",
    7: "stack",
    8: "waitthread",
    9: "waitpage",
    10: "sigsuspend",
    11: "sigwaitinfo",
    12: "nanosleep",
    13: "mutex",
    14: "condvar",
    15: "join",
    16: "intr",
    17: "sem",
    18: "waitctx",
    19: "net_send",
    20: "net_reply",
    24: "thread_create",
    25: "thread_destroy",
}

COMM_NAMES = {
    0: "smsg",
    1: "snotification",
    2: "rmsg",
    3: "rnotification",
    4: "snotification_exe",
    5: "snotification_dis",
    6: "snotification_dea",
    7: "snotification_un",
    8: "snotification_qun",
    9: "signal",
    10: "reply",
    11: "error",
}

USER_NAMES = {
    USER_LKL_HOST_ENTER: "lkl_host_enter",
    USER_LKL_HOST_EXIT: "lkl_host_exit",
    USER_LKL_MEASURE_BEGIN: "lkl_measure_begin",
    USER_LKL_MEASURE_END: "lkl_measure_end",
    USER_LKL_MEASURE_WAKE: "lkl_measure_wake",
    USER_LKL_MEASURE_MONO_LOW: "lkl_measure_mono_low",
    USER_LKL_MEASURE_MONO_HIGH: "lkl_measure_mono_high",
}
MEASURE_MARKER_NAMES = (
    "lkl_measure_begin",
    "lkl_measure_end",
    "lkl_measure_wake",
    "lkl_measure_mono_low",
    "lkl_measure_mono_high",
)
MEASURE_PHASE_NAMES = {
    0: "begin",
    1: "wake",
    2: "end",
}

INT_NAMES = {
    5: "irq_claim",
    6: "ist_dispatch",
}
INT_IRQ_CLAIM = 5
INT_IST_DISPATCH = 6
SYS_PI_BOOST = 13

CONTROL_NAMES = {
    1: "control_time",
    2: "control_buffer",
}

SYSTEM_NAMES = {
    2: "sys_path",
    13: "pi_boost",
}

# Kernel-call numbers, mirroring the __KSYS_* enum in
# internal/include/sys/calls.h. Only the implemented slots are named; spare and
# removed slots fall back to kcall_<num> so a renumbering shows up as an unnamed
# row rather than a silently mislabelled one. _TRACE_MAX_KER_CALL_NUM is the
# offset the kernel adds to the call number for the exit event.
TRACE_MAX_KER_CALL_NUM = 128

KCALL_NAMES = {
    0: "nop",
    1: "trace_event",
    2: "priv",
    3: "dbgprint",
    7: "sys_cpupage_get",
    8: "sys_cpupage_set",
    10: "msg_current",
    11: "msg_sendv",
    12: "msg_sendvnc",
    13: "msg_error",
    14: "msg_receivev",
    15: "msg_replyv",
    16: "msg_readv",
    17: "msg_writev",
    19: "msg_info",
    20: "msg_send_notification",
    21: "msg_deliver_event",
    22: "msg_keydata",
    23: "msg_readiov",
    24: "msg_receivenotificationv",
    25: "msg_verify_event",
    35: "channel_create",
    36: "channel_destroy",
    39: "connect_attach",
    40: "connect_detach",
    41: "connect_server_info",
    42: "connect_client_info",
    43: "connect_flags",
    46: "thread_create",
    47: "thread_destroy",
    48: "thread_destroyall",
    49: "thread_detach",
    50: "thread_join",
    51: "thread_cancel",
    52: "thread_ctl",
    55: "interrupt_attach_thread",
    56: "interrupt_attach_event",
    57: "interrupt_detach",
    58: "interrupt_wait",
    59: "interrupt_mask",
    60: "interrupt_unmask",
    65: "clock_time",
    67: "clock_period",
    68: "clock_id",
    70: "timer_create",
    71: "timer_destroy",
    72: "timer_settime",
    73: "timer_info",
    75: "timer_timeout",
    78: "sync_create",
    79: "sync_destroy",
    80: "sync_mutex_lock",
    81: "sync_mutex_unlock",
    82: "sync_condvar_wait",
    83: "sync_condvar_signal",
    84: "sync_sem_post",
    85: "sync_sem_wait",
    86: "sync_ctl",
    87: "sync_mutex_revive",
    88: "sched_get",
    89: "sched_set",
    90: "sched_yield",
    91: "sched_info",
}

MEASURE_MODES = {
    1: "fs",
    2: "net",
    3: "net-blocked",
    4: "net-revoke",
    5: "fs-rt",
    6: "net-rt",
}
MEASURE_MODE_IDS = {name: mode for mode, name in MEASURE_MODES.items()}


def raw_measure_window_valid(raw_duration, serial_duration):
    if serial_duration is None:
        return True
    return raw_duration <= max(serial_duration * 10, serial_duration + 10000000)


def trace_cpu(header):
    return (header >> 24) & 0xFF


def kcall_name(num):
    return KCALL_NAMES.get(num, f"kcall_{num}")


def event_name(cls, ev):
    if cls == TRACE_KER_CALL_C:
        if ev < TRACE_MAX_KER_CALL_NUM:
            return f"kcall_enter_{kcall_name(ev)}"
        return f"kcall_exit_{kcall_name(ev - TRACE_MAX_KER_CALL_NUM)}"
    if cls == TRACE_CONTROL_C:
        return CONTROL_NAMES.get(ev, f"control_{ev}")
    if cls == TRACE_PR_TH_C:
        return f"state_{STATE_NAMES.get(ev, f'thread_{ev}')}"
    if cls == TRACE_COMM_C:
        return f"comm_{COMM_NAMES.get(ev, str(ev))}"
    if cls == TRACE_INT_C:
        return INT_NAMES.get(ev, f"int_{ev}")
    if cls == TRACE_SYSTEM_C:
        return SYSTEM_NAMES.get(ev, f"system_{ev}")
    if cls == TRACE_USER_C:
        return USER_NAMES.get(ev, f"user_{ev}")
    return f"class_{cls}_event_{ev}"


def read_events(path):
    out = []
    seen = set()
    last_raw_ts = None
    wrap_base = 0
    cycles_per_sec = read_cycles_per_sec(path)
    with open(path, encoding="utf-8", errors="replace") as f:
        for lineno, line in enumerate(f, 1):
            match = EVENT_RE.search(line)
            if not match:
                continue
            try:
                buf, ev = (int(v) for v in match.groups()[:2])
                header, d0, d1, d2 = (parse_u32(v) for v in match.groups()[2:])
            except ValueError as e:
                raise SystemExit(f"{path}:{lineno}: {e}") from e
            key = (buf, ev, header, d0, d1, d2)
            if key in seen:
                continue
            seen.add(key)
            if last_raw_ts is not None and d0 < last_raw_ts and \
               last_raw_ts - d0 > 0x80000000:
                wrap_base += 0x100000000
            last_raw_ts = d0
            out.append((cycles_to_ns(wrap_base + d0, cycles_per_sec),
                        header, d1, d2))
    if not out:
        raise SystemExit(f"{path}: no tracelogger raw events found")
    # Hand the timebase back rather than making the caller re-read the log; these
    # serial captures run to tens of megabytes.
    return out, cycles_per_sec


def read_lkl_trace_samples(path):
    samples = []
    pids = {}
    times = {}
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            match = LKL_TRACE_RE.search(line)
            if not match:
                continue
            mode, sample, _pid, _tid, start, end, duration = match.groups()
            mode_id = MEASURE_MODE_IDS.get(mode, mode)
            sample_id = int(sample)
            pid = int(_pid) if _pid is not None else 0
            tid = int(_tid) if _tid is not None else 0
            start_ns = int(start)
            end_ns = int(end)
            if pid:
                pids[(mode_id, sample_id)] = (pid, tid)
            times[(mode_id, sample_id)] = (start_ns, end_ns)
            samples.append(
                (
                    mode_id,
                    sample_id,
                    int(duration),
                    start_ns,
                    end_ns,
                    False,
                    pid,
                    tid,
                )
            )
    return samples, pids, times


def decode_ipc_endpoint(name, srcvid):
    if name == "comm_smsg":
        return srcvid >> 16, srcvid & 0xFFFF
    if name in ("comm_reply", "comm_error") and srcvid:
        return srcvid - 1, 0
    return 0, 0


def decode_gap_ipc(name, d1, d2):
    if not name.startswith("comm_"):
        return 0, 0, 0
    src_tid, src_scoid = decode_ipc_endpoint(name, d1)
    return src_tid, src_scoid, d2


def decode_measure_mono_key(key):
    return (key >> 24) & 0xf, key & 0x00ffffff, (key >> 28) & 0xf


def summarize(path):
    events, cycles_per_sec = read_events(path)
    counts = Counter()
    gaps = []
    state_samples = defaultdict(list)
    state_spans = []
    named_events = []
    ipc_events = []
    irq_samples = []
    pending_irqs = defaultdict(deque)
    measure_starts = {}
    measure_wakes = {}
    measure_mono_parts = {}
    measure_samples = []
    thread_last = {}
    pi_boost_by_thread = {}
    kcall_samples = defaultdict(list)

    prev = None
    for raw_ts, header, d1, d2 in events:
        ts = raw_ts
        cls = trace_class(header)
        ev = trace_event(header)
        name = event_name(cls, ev)
        counts[name] += 1
        named_events.append((ts, name))
        if cls == TRACE_COMM_C:
            ipc_events.append((ts, name, d1, d2))
        if cls == TRACE_INT_C:
            if ev == INT_IRQ_CLAIM:
                pending_irqs[d2].append((ts, d1))
            elif ev == INT_IST_DISPATCH and pending_irqs.get(d1):
                start, irq = pending_irqs[d1].popleft()
                irq_samples.append((start, ts, irq, d1, d2, ts - start))

        if prev is not None:
            delta = ts - prev[0]
            gaps.append(
                (delta, prev[1], name, prev[0], ts, prev[2], prev[3], d1, d2)
            )
        prev = (ts, name, d1, d2)

        cpu = trace_cpu(header)
        if cls != TRACE_PR_TH_C:
            if cls == TRACE_USER_C and ev == USER_LKL_MEASURE_BEGIN:
                measure_starts[(d1, d2)] = ts
            elif cls == TRACE_USER_C and ev == USER_LKL_MEASURE_WAKE:
                measure_wakes[(d1, d2)] = ts
            elif cls == TRACE_USER_C and ev in (
                USER_LKL_MEASURE_MONO_LOW,
                USER_LKL_MEASURE_MONO_HIGH,
            ):
                mode, sample, phase = decode_measure_mono_key(d1)
                part = measure_mono_parts.setdefault((mode, sample, phase), {})
                part["trace_ns"] = ts
                if ev == USER_LKL_MEASURE_MONO_LOW:
                    part["low"] = d2
                else:
                    part["high"] = d2
            elif cls == TRACE_USER_C and ev == USER_LKL_MEASURE_END:
                start = measure_starts.pop((d1, d2), None)
                if start is not None:
                    measure_samples.append(
                        (
                            d1,
                            d2,
                            ts - start,
                            start,
                            ts,
                            True,
                            0,
                            0,
                        )
                    )
            elif cls == TRACE_SYSTEM_C and ev == SYS_PI_BOOST:
                # Collect raw boost/restore events per thread; the trace stream is
                # only per-buffer ordered, so a migrated thread can print its
                # restore before its raise. Pair them after sorting each thread's
                # events by timestamp (below), not in print order.
                old_prio = (d2 >> 16) & 0xFFFF
                new_prio = d2 & 0xFFFF
                pi_boost_by_thread.setdefault(d1, []).append(
                    (ts, old_prio, new_prio)
                )
            elif cls == TRACE_KER_CALL_C and \
                    TRACE_MAX_KER_CALL_NUM <= ev < 2 * TRACE_MAX_KER_CALL_NUM:
                # The kernel measures the span itself and ships it as d2 (cycles)
                # on the exit event, so there is nothing to pair: each event is
                # one complete sample. d1 packs pid and tid+1, unused here.
                kcall_samples[ev - TRACE_MAX_KER_CALL_NUM].append(
                    cycles_to_ns(d2, cycles_per_sec)
                )
            continue
        key = (d1, d2)
        old = thread_last.get(key)
        if old is not None:
            old_state, old_ts, old_cpu = old
            duration = ts - old_ts
            state_samples[old_state].append((duration, d1, d2, old_cpu))
            state_spans.append((old_ts, ts, old_state, d1, d2, old_cpu))
        thread_last[key] = (name, ts, cpu)

    serial_samples, serial_pids, serial_times = read_lkl_trace_samples(path)
    serial_durations = {
        (mode, sample): duration
        for mode, sample, duration, _start, _end, _raw_window, _pid, _tid in
        serial_samples
    }
    if measure_samples:
        measure_samples = [
            (
                mode,
                sample,
                duration if raw_measure_window_valid(
                    duration, serial_durations.get((mode, sample))
                ) else serial_durations[(mode, sample)],
                start,
                end,
                raw_window and raw_measure_window_valid(
                    duration, serial_durations.get((mode, sample))
                ),
                serial_pids.get((mode, sample), (0, 0))[0],
                serial_pids.get((mode, sample), (0, 0))[1],
            )
            for mode, sample, duration, start, end, raw_window, _pid, _tid in
            measure_samples
        ]
    else:
        measure_samples = serial_samples

    pi_boost_span_list = pi_boost_spans(pi_boost_by_thread)

    return (
        counts,
        gaps,
        state_samples,
        state_spans,
        named_events,
        irq_samples,
        ipc_events,
        measure_wakes,
        measure_mono_parts,
        serial_durations,
        serial_times,
        measure_samples,
        pi_boost_span_list,
        kcall_samples,
    )


def print_kcalls(samples):
    """Per-kernel-call cost rows.

    One sample is one ecall execution, not one logical kernel call: a call that
    hits KCALL_RESTART returns to user space and re-enters as a fresh trap, so a
    logical call that restarts N times contributes N samples. These rows are
    therefore a per-ecall-execution cost, not a WCET.
    """
    if not samples:
        return
    print()
    print("measure_summary,mode,count,min_ns,p50_ns,p99_ns,max_ns")
    for num in sorted(samples):
        values = samples[num]
        print(
            f"measure_summary,kcall_{kcall_name(num)},{len(values)},"
            f"{min(values)},{int(statistics.median(values))},"
            f"{percentile(values, 99)},{max(values)}"
        )


def pi_boost_spans(by_thread):
    """Pair priority-inheritance raise/restore events into boost spans.

    Each thread's events are sorted by timestamp first, so cross-buffer print
    order cannot reverse a raise and its restore. A raise from baseline opens a
    span; nested raises collapse into it; the first restore back to or below the
    baseline closes it. Stray restores with no open span, and spans still open at
    end of trace, are dropped.
    """
    spans = []
    for events in by_thread.values():
        active = None
        for ts, old_prio, new_prio in sorted(events):
            if new_prio > old_prio:
                if active is None:
                    active = (old_prio, ts)
            elif active is not None and new_prio <= active[0]:
                spans.append(ts - active[1])
                active = None
    return spans


def print_pi_boost(spans):
    print()
    print("measure_summary,mode,count,min_ns,p50_ns,p99_ns,max_ns")
    if spans:
        print(
            f"measure_summary,pi_boost,{len(spans)},{min(spans)},"
            f"{int(statistics.median(spans))},{percentile(spans, 99)},"
            f"{max(spans)}"
        )


def print_counts(counts):
    print("event,count")
    for name, count in sorted(counts.items()):
        print(f"{name},{count}")


def print_gaps(gaps, limit):
    print()
    print("gap_ns,from_event,to_event")
    for delta, src, dst, _ts0, _ts1, *_payload in sorted(gaps, reverse=True)[:limit]:
        print(f"{delta},{src},{dst}")


def print_states(state_samples):
    print()
    print("state,count,min_ns,p50_ns,p99_ns,max_ns,top_pid,top_tid,top_cpu")
    for state in sorted(state_samples):
        samples = state_samples[state]
        durations = [s[0] for s in samples]
        top = max(samples)
        print(
            f"{state},{len(durations)},{min(durations)},"
            f"{int(statistics.median(durations))},{percentile(durations, 99)},"
            f"{max(durations)},{top[1]},{top[2]},{top[3]}"
        )


def print_measures(measure_samples, gaps):
    by_mode = defaultdict(list)
    by_mode_transition = defaultdict(list)

    print()
    print("measure,mode,sample,duration_ns,pid,tid")
    for mode, sample, duration, _start, _end, _raw_window, pid, tid in \
            measure_samples:
        name = MEASURE_MODES.get(mode, f"mode_{mode}")
        by_mode[name].append(duration)
        print(f"measure,{name},{sample},{duration},{pid},{tid}")

    print()
    print("measure_summary,mode,count,min_ns,p50_ns,p99_ns,max_ns")
    for mode in sorted(by_mode):
        values = by_mode[mode]
        print(
            f"measure_summary,{mode},{len(values)},{min(values)},"
            f"{int(statistics.median(values))},{percentile(values, 99)},"
            f"{max(values)}"
        )

    print()
    print(
        "measure_gap,mode,sample,max_gap_ns,from_event,to_event,"
        "from_d1,from_d2,to_d1,to_d2,"
        "from_src_tid,from_src_scoid,from_peer_pid,"
        "to_src_tid,to_src_scoid,to_peer_pid"
    )
    for mode, sample, _duration, start, end, raw_window, _pid, _tid in \
            measure_samples:
        if not raw_window:
            continue
        if end < start:
            continue
        in_window = [
            g
            for g in gaps
            if start <= g[3]
            and g[4] <= end
            and g[1] not in MEASURE_MARKER_NAMES
            and g[2] not in MEASURE_MARKER_NAMES
        ]
        if not in_window:
            continue
        name = MEASURE_MODES.get(mode, f"mode_{mode}")
        for gap, src_event, dst_event, _ts0, _ts1, *_payload in in_window:
            by_mode_transition[(name, src_event, dst_event)].append(gap)
        delta, src, dst, _ts0, _ts1, src_d1, src_d2, dst_d1, dst_d2 = max(in_window)
        src_tid, src_scoid, src_peer = decode_gap_ipc(src, src_d1, src_d2)
        dst_tid, dst_scoid, dst_peer = decode_gap_ipc(dst, dst_d1, dst_d2)
        print(
            f"measure_gap,{name},{sample},{delta},{src},{dst},"
            f"{src_d1},{src_d2},{dst_d1},{dst_d2},"
            f"{src_tid},{src_scoid},{src_peer},"
            f"{dst_tid},{dst_scoid},{dst_peer}"
        )

    print()
    print("measure_transition_summary,mode,from_event,to_event,count,min_ns,p50_ns,p99_ns,max_ns")
    for (mode, src, dst), values in sorted(by_mode_transition.items()):
        print(
            f"measure_transition_summary,{mode},{src},{dst},{len(values)},"
            f"{min(values)},{int(statistics.median(values))},"
            f"{percentile(values, 99)},{max(values)}"
        )


def print_measure_states(measure_samples, state_spans):
    by_mode_state = defaultdict(list)

    print()
    print("measure_state,mode,sample,state,duration_ns,top_pid,top_tid,top_cpu")
    for mode, sample, _duration, start, end, raw_window, _pid, _tid in \
            measure_samples:
        if not raw_window:
            continue
        if end < start:
            continue
        totals = defaultdict(int)
        top = {}
        for span_start, span_end, state, pid, tid, cpu in state_spans:
            overlap_start = max(start, span_start)
            overlap_end = min(end, span_end)
            if overlap_end <= overlap_start:
                continue
            duration = overlap_end - overlap_start
            totals[state] += duration
            if duration > top.get(state, (0, 0, 0, 0))[0]:
                top[state] = (duration, pid, tid, cpu)
        name = MEASURE_MODES.get(mode, f"mode_{mode}")
        for state, duration in sorted(totals.items()):
            by_mode_state[(name, state)].append(duration)
            _top_duration, pid, tid, cpu = top[state]
            print(
                f"measure_state,{name},{sample},{state},{duration},"
                f"{pid},{tid},{cpu}"
            )

    print()
    print("measure_state_summary,mode,state,count,min_ns,p50_ns,p99_ns,max_ns")
    for (mode, state), values in sorted(by_mode_state.items()):
        print(
            f"measure_state_summary,{mode},{state},{len(values)},"
            f"{min(values)},{int(statistics.median(values))},"
            f"{percentile(values, 99)},{max(values)}"
        )


def print_measure_client_states(measure_samples, state_spans):
    by_mode_state = defaultdict(list)

    print()
    print("measure_client_state,mode,sample,state,duration_ns,top_tid,top_cpu")
    for mode, sample, _duration, start, end, raw_window, sample_pid, \
            sample_tid in measure_samples:
        if not raw_window or not sample_pid:
            continue
        if end < start:
            continue
        totals = defaultdict(int)
        top = {}
        for span_start, span_end, state, pid, tid, cpu in state_spans:
            if pid != sample_pid or (sample_tid and tid != sample_tid):
                continue
            overlap_start = max(start, span_start)
            overlap_end = min(end, span_end)
            if overlap_end <= overlap_start:
                continue
            duration = overlap_end - overlap_start
            totals[state] += duration
            if duration > top.get(state, (0, 0, 0))[0]:
                top[state] = (duration, tid, cpu)
        name = MEASURE_MODES.get(mode, f"mode_{mode}")
        for state, duration in sorted(totals.items()):
            by_mode_state[(name, state)].append(duration)
            _top_duration, tid, cpu = top[state]
            print(
                f"measure_client_state,{name},{sample},{state},{duration},"
                f"{tid},{cpu}"
            )

    print()
    print("measure_client_state_summary,mode,state,count,min_ns,p50_ns,p99_ns,max_ns")
    for (mode, state), values in sorted(by_mode_state.items()):
        print(
            f"measure_client_state_summary,{mode},{state},{len(values)},"
            f"{min(values)},{int(statistics.median(values))},"
            f"{percentile(values, 99)},{max(values)}"
        )


def print_measure_events(measure_samples, named_events):
    by_mode_event = Counter()

    print()
    print("measure_event,mode,sample,event,count")
    for mode, sample, _duration, start, end, raw_window, _pid, _tid in \
            measure_samples:
        if not raw_window:
            continue
        if end < start:
            continue
        counts = Counter(
            name
            for ts, name in named_events
            if start <= ts <= end
            and name not in MEASURE_MARKER_NAMES
        )
        mode_name = MEASURE_MODES.get(mode, f"mode_{mode}")
        for name, count in sorted(counts.items()):
            by_mode_event[(mode_name, name)] += count
            print(f"measure_event,{mode_name},{sample},{name},{count}")

    print()
    print("measure_event_summary,mode,event,count")
    for (mode, name), count in sorted(by_mode_event.items()):
        print(f"measure_event_summary,{mode},{name},{count}")


def print_measure_irqs(measure_samples, irq_samples):
    by_mode_irq = defaultdict(list)

    print()
    print("measure_irq,mode,sample,irq,pid,tid,latency_ns")
    for mode, sample, _duration, start, end, raw_window, _pid, _tid in \
            measure_samples:
        if not raw_window:
            continue
        if end < start:
            continue
        mode_name = MEASURE_MODES.get(mode, f"mode_{mode}")
        for claim_ts, dispatch_ts, irq, pid, tid, latency in irq_samples:
            if claim_ts < start or dispatch_ts > end:
                continue
            by_mode_irq[(mode_name, irq, pid)].append(latency)
            print(
                f"measure_irq,{mode_name},{sample},{irq},{pid},{tid},"
                f"{latency}"
            )

    print()
    print("measure_irq_summary,mode,irq,pid,count,min_ns,p50_ns,p99_ns,max_ns")
    for (mode, irq, pid), values in sorted(by_mode_irq.items()):
        print(
            f"measure_irq_summary,{mode},{irq},{pid},{len(values)},"
            f"{min(values)},{int(statistics.median(values))},"
            f"{percentile(values, 99)},{max(values)}"
        )


def print_measure_wakes(measure_samples, measure_wakes):
    by_mode_begin = defaultdict(list)
    by_mode_post = defaultdict(list)

    print()
    print("measure_wake,mode,sample,begin_to_wake_ns,wake_to_end_ns")
    for mode, sample, _duration, start, end, raw_window, _pid, _tid in \
            measure_samples:
        if not raw_window:
            continue
        wake = measure_wakes.get((mode, sample))
        if wake is None or wake < start or wake > end:
            continue
        mode_name = MEASURE_MODES.get(mode, f"mode_{mode}")
        begin_to_wake = wake - start
        wake_to_end = end - wake
        by_mode_begin[mode_name].append(begin_to_wake)
        by_mode_post[mode_name].append(wake_to_end)
        print(
            f"measure_wake,{mode_name},{sample},"
            f"{begin_to_wake},{wake_to_end}"
        )

    print()
    print(
        "measure_wake_summary,mode,count,"
        "begin_max_ns,post_wake_max_ns,post_wake_p99_ns"
    )
    for mode in sorted(by_mode_begin):
        post = by_mode_post[mode]
        print(
            f"measure_wake_summary,{mode},{len(post)},"
            f"{max(by_mode_begin[mode])},{max(post)},{percentile(post, 99)}"
        )


def print_measure_clock_compare(measure_samples, serial_durations):
    print()
    print("measure_clock,mode,sample,trace_duration_ns,serial_duration_ns,delta_ns")
    for mode, sample, duration, _start, _end, raw_window, _pid, _tid in \
            measure_samples:
        if not raw_window:
            continue
        serial_duration = serial_durations.get((mode, sample))
        if serial_duration is None:
            continue
        mode_name = MEASURE_MODES.get(mode, f"mode_{mode}")
        delta = duration - serial_duration
        print(
            f"measure_clock,{mode_name},{sample},{duration},"
            f"{serial_duration},{delta}"
        )


def print_measure_mono_sync(measure_mono_parts):
    print()
    print("measure_mono,mode,sample,phase,trace_ns,mono_ns,delta_ns")
    for (mode, sample, phase), part in sorted(measure_mono_parts.items()):
        if "low" not in part or "high" not in part:
            continue
        mode_name = MEASURE_MODES.get(mode, f"mode_{mode}")
        phase_name = MEASURE_PHASE_NAMES.get(phase, f"phase_{phase}")
        trace_ns = part.get("trace_ns", 0)
        mono_ns = (part["high"] << 32) | part["low"]
        print(
            f"measure_mono,{mode_name},{sample},{phase_name},"
            f"{trace_ns},{mono_ns},{trace_ns - mono_ns}"
        )


def measure_mono_value(part):
    if "low" not in part or "high" not in part:
        return None
    return (part["high"] << 32) | part["low"]


def print_measure_mono_spans(measure_mono_parts):
    keys = sorted({(mode, sample) for mode, sample, _phase in measure_mono_parts})

    print()
    print(
        "measure_mono_span,mode,sample,"
        "begin_to_wake_trace_ns,begin_to_wake_mono_ns,"
        "begin_to_end_trace_ns,begin_to_end_mono_ns"
    )
    for mode, sample in keys:
        begin = measure_mono_parts.get((mode, sample, 0))
        wake = measure_mono_parts.get((mode, sample, 1))
        end = measure_mono_parts.get((mode, sample, 2))
        if not begin or not wake or not end:
            continue
        begin_mono = measure_mono_value(begin)
        wake_mono = measure_mono_value(wake)
        end_mono = measure_mono_value(end)
        if begin_mono is None or wake_mono is None or end_mono is None:
            continue
        mode_name = MEASURE_MODES.get(mode, f"mode_{mode}")
        print(
            f"measure_mono_span,{mode_name},{sample},"
            f"{wake.get('trace_ns', 0) - begin.get('trace_ns', 0)},"
            f"{wake_mono - begin_mono},"
            f"{end.get('trace_ns', 0) - begin.get('trace_ns', 0)},"
            f"{end_mono - begin_mono}"
        )


def print_measure_deadlines(measure_mono_parts, serial_times):
    keys = sorted(set(serial_times) & {
        (mode, sample) for mode, sample, _phase in measure_mono_parts
    })

    print()
    print(
        "measure_deadline,mode,sample,begin_late_ns,wake_late_ns,"
        "end_vs_serial_after_ns"
    )
    for mode, sample in keys:
        begin = measure_mono_parts.get((mode, sample, 0))
        wake = measure_mono_parts.get((mode, sample, 1))
        end = measure_mono_parts.get((mode, sample, 2))
        if not begin or not wake or not end:
            continue
        begin_mono = measure_mono_value(begin)
        wake_mono = measure_mono_value(wake)
        end_mono = measure_mono_value(end)
        if begin_mono is None or wake_mono is None or end_mono is None:
            continue
        deadline, serial_after = serial_times[(mode, sample)]
        mode_name = MEASURE_MODES.get(mode, f"mode_{mode}")
        print(
            f"measure_deadline,{mode_name},{sample},"
            f"{begin_mono - deadline},{wake_mono - deadline},"
            f"{end_mono - serial_after}"
        )


def print_measure_entry_latency(measure_samples, state_spans):
    print()
    print(
        "measure_entry,mode,sample,ready_span_ns,"
        "after_ready_to_begin_ns,cpu,"
        "cpu_top_state,cpu_state_ns,cpu_pid,cpu_tid"
    )
    for mode, sample, _duration, start, _end, raw_window, sample_pid, \
            sample_tid in measure_samples:
        if not raw_window or not sample_pid:
            continue
        ready_span = None
        for span_start, span_end, state, pid, tid, cpu in state_spans:
            if pid != sample_pid or (sample_tid and tid != sample_tid):
                continue
            if state != "state_ready" or span_end > start:
                continue
            if ready_span is None or span_end > ready_span[1]:
                ready_span = (span_start, span_end, cpu)
        if ready_span is None:
            continue
        cpu_best = (0, "", 0, 0)
        for span_start, span_end, state, pid, tid, cpu in state_spans:
            if cpu != ready_span[2] or \
               (pid == sample_pid and (not sample_tid or tid == sample_tid)):
                continue
            overlap_start = max(ready_span[0], span_start)
            overlap_end = min(ready_span[1], span_end)
            if overlap_end <= overlap_start:
                continue
            duration = overlap_end - overlap_start
            if duration > cpu_best[0]:
                cpu_best = (duration, state, pid, tid)
        mode_name = MEASURE_MODES.get(mode, f"mode_{mode}")
        print(
            f"measure_entry,{mode_name},{sample},"
            f"{ready_span[1] - ready_span[0]},{start - ready_span[1]},"
            f"{ready_span[2]},{cpu_best[1]},{cpu_best[0]},"
            f"{cpu_best[2]},{cpu_best[3]}"
        )


def print_measure_ipc(measure_samples, ipc_events):
    by_mode_ipc = Counter()

    print()
    print("measure_ipc,mode,sample,event,srcvid,src_tid,src_scoid,peer_pid,count")
    for mode, sample, _duration, start, end, raw_window, _pid, _tid in \
            measure_samples:
        if not raw_window:
            continue
        if end < start:
            continue
        mode_name = MEASURE_MODES.get(mode, f"mode_{mode}")
        counts = Counter(
            (name, srcvid, peer_pid)
            for ts, name, srcvid, peer_pid in ipc_events
            if start <= ts <= end
        )
        for (name, srcvid, peer_pid), count in sorted(counts.items()):
            src_tid, src_scoid = decode_ipc_endpoint(name, srcvid)
            by_mode_ipc[
                (mode_name, name, srcvid, src_tid, src_scoid, peer_pid)
            ] += count
            print(
                f"measure_ipc,{mode_name},{sample},{name},"
                f"{srcvid},{src_tid},{src_scoid},{peer_pid},{count}"
            )

    print()
    print(
        "measure_ipc_summary,mode,event,srcvid,src_tid,src_scoid,peer_pid,count"
    )
    for (mode, name, srcvid, src_tid, src_scoid, peer_pid), count in \
            sorted(by_mode_ipc.items()):
        print(
            f"measure_ipc_summary,{mode},{name},{srcvid},"
            f"{src_tid},{src_scoid},{peer_pid},{count}"
        )


def print_measure_critical(measure_samples, gaps, state_spans, named_events):
    by_mode_state = defaultdict(list)
    by_mode_state_top = {}
    by_mode_transition = defaultdict(list)

    print()
    print(
        "measure_critical,mode,sample,duration_ns,top_state,state_ns,"
        "sample_pid,sample_tid,top_pid,top_tid,top_cpu,"
        "client_top_state,client_state_ns,client_tid,client_cpu,"
        "transition_from,transition_to,transition_ns,"
        "lkl_host_events,ipc_events,irq_events"
    )
    for mode, sample, duration, start, end, raw_window, sample_pid, \
            sample_tid in measure_samples:
        if not raw_window:
            continue
        if end < start:
            continue

        state_best = (0, "", 0, 0, 0)
        client_best = (0, "", 0, 0)
        for span_start, span_end, state, pid, tid, cpu in state_spans:
            overlap_start = max(start, span_start)
            overlap_end = min(end, span_end)
            if overlap_end <= overlap_start:
                continue
            state_duration = overlap_end - overlap_start
            if state_duration > state_best[0]:
                state_best = (state_duration, state, pid, tid, cpu)
            if sample_pid and pid == sample_pid and \
               (not sample_tid or tid == sample_tid) and \
               state_duration > client_best[0]:
                client_best = (state_duration, state, tid, cpu)

        transition_best = (0, "", "")
        for gap, src, dst, ts0, ts1, *_payload in gaps:
            if ts0 < start or ts1 > end:
                continue
            if src in MEASURE_MARKER_NAMES:
                continue
            if dst in MEASURE_MARKER_NAMES:
                continue
            if gap > transition_best[0]:
                transition_best = (gap, src, dst)

        counts = Counter(
            name
            for ts, name in named_events
            if start <= ts <= end
            and name not in MEASURE_MARKER_NAMES
        )
        lkl_host_events = sum(
            count for name, count in counts.items() if name.startswith("lkl_host_")
        )
        ipc_events = sum(
            count for name, count in counts.items() if name.startswith("comm_")
        )
        irq_events = counts["irq_claim"] + counts["ist_dispatch"]
        mode_name = MEASURE_MODES.get(mode, f"mode_{mode}")
        if state_best[1]:
            by_mode_state[(mode_name, state_best[1])].append(state_best[0])
            key = (mode_name, state_best[1])
            if state_best[0] > by_mode_state_top.get(key, (0, 0, 0, 0))[0]:
                by_mode_state_top[key] = (
                    state_best[0],
                    state_best[2],
                    state_best[3],
                    state_best[4],
                )
        if transition_best[1] or transition_best[2]:
            by_mode_transition[
                (mode_name, transition_best[1], transition_best[2])
            ].append(transition_best[0])
        print(
            f"measure_critical,{mode_name},{sample},{duration},"
            f"{state_best[1]},{state_best[0]},{sample_pid},{sample_tid},"
            f"{state_best[2]},{state_best[3]},{state_best[4]},"
            f"{client_best[1]},{client_best[0]},"
            f"{client_best[2]},{client_best[3]},"
            f"{transition_best[1]},{transition_best[2]},{transition_best[0]},"
            f"{lkl_host_events},{ipc_events},{irq_events}"
        )

    print()
    print(
        "measure_critical_state_summary,mode,state,count,"
        "min_ns,p50_ns,p99_ns,max_ns,top_pid,top_tid,top_cpu"
    )
    for (mode, state), values in sorted(by_mode_state.items()):
        _duration, top_pid, top_tid, top_cpu = by_mode_state_top[(mode, state)]
        print(
            f"measure_critical_state_summary,{mode},{state},{len(values)},"
            f"{min(values)},{int(statistics.median(values))},"
            f"{percentile(values, 99)},{max(values)},"
            f"{top_pid},{top_tid},{top_cpu}"
        )

    print()
    print(
        "measure_critical_transition_summary,mode,from_event,to_event,"
        "count,min_ns,p50_ns,p99_ns,max_ns"
    )
    for (mode, src, dst), values in sorted(by_mode_transition.items()):
        print(
            f"measure_critical_transition_summary,{mode},{src},{dst},"
            f"{len(values)},{min(values)},{int(statistics.median(values))},"
            f"{percentile(values, 99)},{max(values)}"
        )


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("log", help="guest serial log containing tracelogger lines")
    ap.add_argument("--top", type=int, default=10, help="global gaps to print")
    args = ap.parse_args()

    (
        counts,
        gaps,
        state_samples,
        state_spans,
        named_events,
        irq_samples,
        ipc_events,
        measure_wakes,
        measure_mono_parts,
        serial_durations,
        serial_times,
        measure_samples,
        pi_boost_spans,
        kcall_samples,
    ) = summarize(args.log)
    print_counts(counts)
    print_gaps(gaps, args.top)
    print_states(state_samples)
    print_measures(measure_samples, gaps)
    print_pi_boost(pi_boost_spans)
    print_kcalls(kcall_samples)
    print_measure_states(measure_samples, state_spans)
    print_measure_client_states(measure_samples, state_spans)
    print_measure_events(measure_samples, named_events)
    print_measure_irqs(measure_samples, irq_samples)
    print_measure_wakes(measure_samples, measure_wakes)
    print_measure_clock_compare(measure_samples, serial_durations)
    print_measure_mono_sync(measure_mono_parts)
    print_measure_mono_spans(measure_mono_parts)
    print_measure_deadlines(measure_mono_parts, serial_times)
    print_measure_entry_latency(measure_samples, state_spans)
    print_measure_ipc(measure_samples, ipc_events)
    print_measure_critical(measure_samples, gaps, state_spans, named_events)
    return 0


if __name__ == "__main__":
    sys.exit(main())
