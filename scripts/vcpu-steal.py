#!/usr/bin/env python3
"""Host-side audit of guest wake-latency samples against vCPU scheduling.

Under multi-threaded TCG every hart is a host thread. When the host
deschedules one, the guest sees a hart that stopped for tens of milliseconds
with interrupts pending, which no guest code can prevent and which looks
exactly like a missed preemption. This tool separates the two from the host
side, where the difference is visible: Linux accounts each thread's time
spent runnable but not running in /proc/<pid>/task/<tid>/schedstat.

sample  polls schedstat of every thread of one QEMU process until it exits.
audit   reads the guest's over-bound samples from the serial log, places each
        on the host clock, and compares it with the proven run-queue wait of
        the QEMU threads inside the sample's window.

Each over-bound sample gets one of three verdicts:

  INVALID    the threads the wake depends on (the hart whose timer or device
             interrupt fires, the hart that runs the waiter, and, except for
             an IPC reply, QEMU's main loop, which delivers both interrupts)
             waited for a host CPU long enough to
             account for the excess over the bound. The sample is excluded.
             It is never counted as a pass, and each phase must keep a floor
             of valid samples.
  UNDECIDED  those threads did not, but some other QEMU thread did. A hart
             descheduled while it holds a kernel spinlock, or QEMU's own big
             lock, stalls every hart that needs the lock, and schedstat
             cannot say which thread held one. The host was too loaded to
             judge this sample, so the whole run is invalid: neither a pass
             nor a failure.
  FAIL       no QEMU thread's wait accounts for the excess. The harts were
             running and the wake was late.

Exit status: 0 nothing over the bound or every such sample INVALID with the
floor kept, 1 a FAIL, 3 an invalid run (UNDECIDED samples or a phase under
its floor). Without usable host data, or when the samples in the log do not
add up to what the guest says it left to the audit, the result is a FAIL.

Placing guest time on the host clock: after each wake the guest writes the
low 32 bits of rdtime to the goldfish-rtc CLEAR_ALARM register, twice, each
with a fresh reading (beacons; the register ignores the value), and QEMU's
goldfish_rtc_write trace event stamps each write with host time. A write
happens after its own reading and before the next one, so every beacon bounds
host minus guest time from above and every pair of neighbours from below. A
sample is credited only with wait that lies inside its window for every
offset between the two bounds and every instant at which the guest may have
taken the measurement.

SPDX-License-Identifier: Apache-2.0
"""

import argparse
import calendar
import collections
import os
import re
import signal
import sys
import time

from rt_trace_common import CYCLES_PER_SEC_RE, cycles_to_ns

# -msg timestamp=on stamps a trace line with an ISO time, or, in QEMU releases
# whose log trace backend formats its own, with "<tid>@<sec>.<usec>:".
BEACON_RE = re.compile(
    r"^(?:(\d+)-(\d+)-(\d+)T(\d+):(\d+):(\d+)\.(\d+)Z |\d+@(\d+)\.(\d{6}):)"
    r"goldfish_rtc_write addr 0x14 value 0x([0-9a-f]+)"
)
# Logical CPU 0 is the boot hart, which OpenSBI picks by lottery, and the other
# harts take the next slots in FDT order: a logical CPU number is not a hart ID.
# QEMU names its vCPU threads after the hart.
BOOT_HART_RE = re.compile(r"^Boot HART ID\s*:\s*(\d+)\s*$", re.M)
HART_CPU_RE = re.compile(r"cpu_start_others: hart (\d+) \(cpu (\d+)\) started")
OVER_RE = re.compile(
    r"^preempt-latency: (\S+) over-bound i=(\d+) late_ns=(\d+) "
    r"wake_ticks=(\d+)-(\d+) fire_cpu=(\d+) target_cpu=(\d+) "
    r"main_loop=([01])\s*$",
    re.M,
)
UNAUDITED_RE = re.compile(
    r"^preempt-latency: (\S+) UNAUDITED (\d+) of (\d+) wakes over the (\d+) ns "
    r"bound ",
    re.M,
)
REQUIRED_RE = re.compile(r"^preempt-latency: AUDIT-REQUIRED (\d+) wakes ", re.M)
# The probe writes these two values to the beacon register before its first
# beacon (BEACON_START_0 and _1 in preempt-latency.c).
BEACON_START = (0x52544D58, 0x42434E53)
SLOW_PERIOD_S = 0.05  # poll period before the probe starts


def find_qemu(token, wait_s, stopping):
    """Return the pid of the qemu-system process whose command line has token."""
    needle = token.encode()
    deadline = time.monotonic() + wait_s
    while time.monotonic() < deadline and not stopping:
        for entry in os.listdir("/proc"):
            if not entry.isdigit():
                continue
            try:
                with open(f"/proc/{entry}/cmdline", "rb") as f:
                    cmdline = f.read()
            except OSError:
                continue
            if b"qemu-system" in cmdline.split(b"\0")[0] and needle in cmdline:
                return int(entry)
        time.sleep(0.05)
    return None


def cmd_sample(args):
    stopping = []
    signal.signal(signal.SIGTERM, lambda signum, frame: stopping.append(signum))
    # QEMU is told to write its trace there, so the path is on its command
    # line and on no other's.
    pid = find_qemu(args.trace, args.wait, stopping)
    if pid is None:
        print("vcpu-steal: no QEMU process matched", file=sys.stderr)
        return 1
    period = SLOW_PERIOD_S
    names = {}
    stats = {}  # tid -> open schedstat, read in place on every poll
    next_scan = 0.0
    # Until the probe's first write to the RTC, which comes before its first
    # measured wake, poll slowly and keep only the last polls: a boot can take
    # minutes to get there. The probe then waits long enough for the fast
    # polls to start (AUDIT_SETTLE_MS in preempt-latency.c).
    held = collections.deque(maxlen=8)
    recording = False
    with open(args.out, "w") as out:
        out.write(f"# pid {pid}\n")
        while not stopping:
            now = time.monotonic()
            if now >= next_scan:
                next_scan = now + 0.25
                try:
                    tids = os.listdir(f"/proc/{pid}/task")
                except OSError:
                    break
                # QEMU names a vCPU thread after it starts it, so read a name
                # again until it is one.
                for tid in tids:
                    if names.get(tid, "").startswith("CPU"):
                        continue
                    try:
                        with open(f"/proc/{pid}/task/{tid}/comm") as f:
                            name = f.read().strip().replace(" ", "")
                        if tid not in stats:
                            stats[tid] = os.open(
                                f"/proc/{pid}/task/{tid}/schedstat", os.O_RDONLY
                            )
                    except OSError:
                        continue
                    if names.get(tid) != name:
                        names[tid] = name
                        out.write(f"# thread {tid} {name}\n")
            # On every slow poll, not every scan: wakes measured before the
            # fast polls start have no wait proven for them.
            if not recording:
                try:
                    recording = os.path.getsize(args.trace) > 0
                except OSError:
                    pass
                if recording:
                    period = args.period_ms / 1000.0
                    out.writelines(held)
                    held.clear()
            fields = []
            before = time.time_ns()
            for tid, fd in list(stats.items()):
                try:
                    wait = os.pread(fd, 128, 0).split()[1].decode()
                except (OSError, IndexError):
                    os.close(fd)  # the thread exited
                    del stats[tid]
                    continue
                fields.append(f"{tid}:{wait}")
            after = time.time_ns()
            if not fields:
                break  # QEMU is gone
            line = f"{before} {after} {' '.join(fields)}\n"
            if recording:
                out.write(line)
            else:
                held.append(line)
            time.sleep(period)
    return 0


def load_beacons(path, rate):
    """Return (offset_min, offset_max, first, count) from a QEMU trace log.

    The offsets bound host time minus guest time in ns, the guest tick counter
    being taken from first, the first beacon's 32-bit value, and unwrapped from
    there; rate is the counter's ticks per second. Beacon i carries a reading
    t[i] taken before it was written at host time h[i], and the reading of
    beacon i+1 was taken after that write: t[i] <= h[i] - offset <= t[i+1].
    Returns None for both bounds with fewer than two beacons.
    """
    writes = []
    with open(path, errors="replace") as f:
        for line in f:
            m = BEACON_RE.match(line)
            if not m:
                continue
            if m.group(1) is not None:
                y, mo, d, h, mi, s, us = (int(x) for x in m.groups()[:7])
                host_ns = calendar.timegm((y, mo, d, h, mi, s)) * 10**9
            else:
                host_ns = int(m.group(8)) * 10**9
                us = int(m.group(9))
            host_ns += us * 1000
            writes.append((host_ns, int(m.group(10), 16)))
    # Only what follows the probe's start marker is a beacon: the register
    # has other writers, the probe's own reset among them.
    start = None
    for i in range(len(writes) - 1):
        if (writes[i][1], writes[i + 1][1]) == BEACON_START:
            start = i + 2
    writes = writes[start:] if start is not None else []

    offset_min = None
    offset_max = None
    first = writes[0][1] if writes else None
    prev_host = None
    high = 0
    prev = 0
    for host_ns, low in writes:
        if low + 2**31 < prev:  # the 32-bit beacon wrapped
            high += 2**32
        prev = low
        guest_ns = cycles_to_ns(low + high, rate)
        if offset_max is None or host_ns - guest_ns < offset_max:
            offset_max = host_ns - guest_ns
        if prev_host is not None and (
            offset_min is None or prev_host - guest_ns > offset_min
        ):
            offset_min = prev_host - guest_ns
        prev_host = host_ns
    count = len(writes)
    if offset_min is None:
        return None, None, first, count
    # A reading is a whole tick, rounded down, and so is a stamp's microsecond.
    return offset_min - cycles_to_ns(1, rate) - 1, offset_max + 1000, first, count


def load_stalls(path):
    """Return {thread name: [(start_ns, end_ns)]} of proven run-queue waits.

    schedstat charges a wait when it ends. Between two polls at p (taken no
    earlier than p.before) and c (no later than c.after) the counter grew by
    d. No more than c.after - p.before of that can lie between the polls, so
    when d is larger the thread was already waiting at p, and had been since
    c.after - d at the latest: [c.after - d, p.before] is proven wait. A
    shorter wait proves nothing and earns no credit.
    """
    names = {}
    main_tid = None
    polls = {}
    with open(path) as f:
        for line in f:
            if line.startswith("# pid "):
                main_tid = line.split()[2]
                continue
            if line.startswith("# thread "):
                _, _, tid, name = line.split(None, 3)
                names[tid] = name.strip()
                continue
            parts = line.split()
            if len(parts) < 2:
                continue
            before, after = int(parts[0]), int(parts[1])
            for field in parts[2:]:
                tid, wait = field.split(":")
                polls.setdefault(tid, []).append((before, after, int(wait)))
    stalls = {}
    for tid, series in polls.items():
        name = "main" if tid == main_tid else names.get(tid, tid)
        proven = stalls.setdefault(name, [])
        for prev, cur in zip(series, series[1:]):
            grew = cur[2] - prev[2]
            if grew > cur[1] - prev[0]:
                proven.append((cur[1] - grew, prev[0]))
    return stalls


def union_ns(intervals, lo, hi):
    """Length of the union of intervals clipped to [lo, hi]."""
    clipped = sorted(
        (max(a, lo), min(b, hi)) for a, b in intervals if b > lo and a < hi
    )
    total = 0
    end = lo
    for a, b in clipped:
        if b <= end:
            continue
        total += b - max(a, end)
        end = b
    return total


def hart_of_cpu(text):
    """Return {logical CPU: hart ID} as the guest's boot log gives it.

    The kernel names each secondary hart it starts. The boot hart, CPU 0, is
    in OpenSBI's banner; without one it is the hart the secondaries leave out
    of a contiguous range, when exactly one is left out.
    """
    harts = {int(cpu): int(hart) for hart, cpu in HART_CPU_RE.findall(text)}
    boot = BOOT_HART_RE.search(text)
    if boot:
        harts[0] = int(boot.group(1))
    elif 0 not in harts:
        missing = set(range(len(harts) + 1)) - set(harts.values())
        if len(missing) == 1:
            harts[0] = missing.pop()
    return harts


def cmd_audit(args):
    # The caller strips the kernel's spin markers, which land inside lines.
    text = sys.stdin.read().replace("\r", "")
    over = [
        (m.group(1),) + tuple(int(g) for g in m.groups()[1:])
        for m in OVER_RE.finditer(text)
    ]
    # Per phase: late wakes left to the audit, wakes in all, the bound in ns.
    phases = {
        m.group(1): tuple(int(g) for g in m.groups()[1:])
        for m in UNAUDITED_RE.finditer(text)
    }
    declared = {phase: counts[0] for phase, counts in phases.items()}
    required = [int(m.group(1)) for m in REQUIRED_RE.finditer(text)]
    if not declared and not required:
        if args.expect_samples:
            print(
                "vcpu-steal: FAIL the log names wakes left to the audit, but "
                "none of those lines is whole"
            )
            return 1
        print("vcpu-steal: the guest left nothing to audit")
        return 0

    # Every sample the guest left to the audit must be in hand: a record lost
    # to a truncated or interleaved line would otherwise go unexamined.
    over = [rec for rec in over if rec[0] in declared]
    found = collections.Counter(rec[0] for rec in over)
    if found != declared or required != [sum(declared.values())]:
        print(
            f"vcpu-steal: FAIL the log holds {dict(found)} over-bound samples, the "
            f"guest declared {declared} and a total of {required}"
        )
        return 1

    offset_min = None
    stalls = None
    rate = CYCLES_PER_SEC_RE.search(text)
    rate = int(rate.group(1)) if rate else 0
    try:
        if rate:
            offset_min, offset_max, first, beacons = load_beacons(
                args.trace, rate
            )
        stalls = load_stalls(args.sched)
    except OSError as err:
        print(f"vcpu-steal: host data unreadable: {err}")
    if offset_min is None or not stalls or "main" not in stalls:
        print(
            f"vcpu-steal: FAIL {len(over)} over-bound samples and no usable "
            "host scheduling data to audit them"
        )
        return 1
    if offset_min > offset_max:
        print("vcpu-steal: FAIL the beacons do not agree on a clock offset")
        return 1

    print(
        f"vcpu-steal: auditing {len(over)} over-bound samples ({beacons} "
        f"beacons, clock offset known to {(offset_max - offset_min) / 1e3:.0f} "
        f"us, {len(stalls)} QEMU threads)"
    )
    everything = [iv for proven in stalls.values() for iv in proven]
    harts = hart_of_cpu(text)
    failed = 0
    undecided = 0
    invalid = collections.Counter()
    for phase, idx, late, ticks_before, ticks_after, fire, target, main in over:
        bound = phases[phase][2]
        # The beacons carry 32 bits of the counter. The probe runs for seconds,
        # far less than the 32 bits span, so a reading's distance from the
        # first beacon, modulo 2^32, puts it on the beacons' unwrapped scale.
        # The wake was measured between the two readings, at an offset between
        # the two bounds: keep the part of the window common to all of those.
        before = first + ((ticks_before - first) & 0xFFFFFFFF)
        after = first + ((ticks_after - first) & 0xFFFFFFFF)
        start = offset_max + cycles_to_ns(after + 1, rate) - late
        end = offset_min + cycles_to_ns(before, rate)
        mapped = fire in harts and target in harts
        path = set()
        if mapped:
            path = {f"CPU{harts[fire]}/TCG", f"CPU{harts[target]}/TCG"}
        if main:
            path.add("main")
        on_path = [iv for name in path for iv in stalls.get(name, ())]
        path_wait = union_ns(on_path, start, end)
        any_wait = union_ns(everything, start, end)
        if not mapped:
            verdict = "FAIL the log does not name the hart of a wake-path cpu"
            failed += 1
        elif not path <= set(stalls):
            verdict = "FAIL no schedstat for a wake-path thread"
            failed += 1
        elif late - path_wait <= bound:
            verdict = "INVALID host wait on the wake path covers the excess"
            invalid[phase] += 1
        elif late - any_wait <= bound:
            verdict = "UNDECIDED only other QEMU threads' wait covers the excess"
            undecided += 1
        else:
            verdict = "FAIL not explained by host scheduling"
            failed += 1
        print(
            f"vcpu-steal: {phase} i={idx} late={late / 1e6:.1f}ms "
            f"path-wait={path_wait / 1e6:.1f}ms "
            f"any-wait={any_wait / 1e6:.1f}ms {verdict}"
        )

    under_floor = 0
    for phase, count in sorted(invalid.items()):
        wakes = phases[phase][1]
        valid = wakes - count
        if wakes == 0 or valid * 100 < wakes * args.floor_pct:
            under_floor += 1
            print(
                f"vcpu-steal: {phase} only {valid}/{wakes} samples valid, "
                f"under the {args.floor_pct}% floor"
            )
        else:
            print(
                f"vcpu-steal: {phase} {count}/{wakes} samples invalidated by "
                "host wait on the wake path"
            )
    if failed:
        print(f"vcpu-steal: FAIL {failed} samples over the bound")
        return 1
    if undecided or under_floor:
        print(
            f"vcpu-steal: INVALID RUN {undecided} undecided samples, "
            f"{under_floor} phases under the floor; the host was too loaded "
            "to judge this run"
        )
        return 3
    print("vcpu-steal: PASS every over-bound sample is invalidated, floors kept")
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = parser.add_subparsers(dest="cmd", required=True)
    sample = sub.add_parser("sample", help="poll a QEMU process's schedstat")
    sample.add_argument("--out", required=True)
    sample.add_argument("--period-ms", type=float, default=1.0)
    sample.add_argument("--trace", required=True,
                        help="the trace log QEMU was started with (-D); "
                             "recording starts with its first line")
    sample.add_argument("--wait", type=float, default=120.0,
                        help="seconds to wait for the process to appear")
    sample.set_defaults(func=cmd_sample)
    audit = sub.add_parser(
        "audit", help="audit the over-bound samples of a guest log on stdin"
    )
    audit.add_argument("--trace", required=True, help="QEMU trace log")
    audit.add_argument("--sched", required=True, help="output of sample")
    audit.add_argument("--floor-pct", type=int, default=75)
    audit.add_argument("--expect-samples", action="store_true",
                       help="fail when the log holds nothing to audit")
    audit.set_defaults(func=cmd_audit)
    args = parser.parse_args()
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
