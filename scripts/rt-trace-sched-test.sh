#!/bin/sh
# Smoke test for scheduler/IPC trace summarizer.
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
tmp=${TMPDIR:-/tmp}/rtmux-sched-trace-test.$$
trap 'rm -rf "$tmp"' EXIT HUP INT TERM

mkdir -p "$tmp"
cat >"$tmp/guest.log" <<'LOG'
tracelogger: buf 0 ev 0 header=0x1001 d=100,7,1
tracelogger: buf 0 ev 1 header=0x1004 d=130,7,1
tracelogger: buf 0 ev 2 header=0x1c00 d=135,0x10,42
tracelogger: buf 0 ev 3 header=0x1006 d=180,7,1
tracelogger: buf 0 ev 4 header=0x1001 d=260,7,1
tracelogger: buf 0 ev 5 header=0x1c0a d=300,0x10,7
tracelogger: buf 0 ev 6 header=0x1a00 d=310,0x3,0x1234
tracelogger: buf 0 ev 7 header=0x1a01 d=350,0x3,0x1234
tracelogger: buf 0 ev 8 header=0x1001 d=360,8,1
tracelogger: buf 0 ev 9 header=0x1002 d=370,7,1
tracelogger: buf 0 ev 10 header=0x1004 d=395,8,1
tracelogger: buf 0 ev 11 header=0x1001 d=390,7,1
tracelogger: buf 0 ev 12 header=0x1a02 d=400,0x2,9
tracelogger: buf 0 ev 13 header=0x1a05 d=401,0x02000009,390
tracelogger: buf 0 ev 14 header=0x1a06 d=402,0x02000009,0
tracelogger: buf 0 ev 15 header=0x1a04 d=405,0x2,9
tracelogger: buf 0 ev 16 header=0x1a05 d=406,0x12000009,450
tracelogger: buf 0 ev 17 header=0x1a06 d=407,0x12000009,0
tracelogger: buf 0 ev 18 header=0xc05 d=410,11,100
tracelogger: buf 0 ev 19 header=0xc06 d=415,100,1
tracelogger: buf 0 ev 20 header=0x1004 d=420,7,1
tracelogger: buf 0 ev 21 header=0x1c0a d=455,0x10,7
tracelogger: buf 0 ev 22 header=0x1006 d=456,7,1
tracelogger: buf 0 ev 23 header=0x1a05 d=458,0x22000009,450
tracelogger: buf 0 ev 24 header=0x1a06 d=459,0x22000009,0
tracelogger: buf 0 ev 25 header=0x1a03 d=460,0x2,9
tracelogger: buf 0 ev 26 header=0xc05 d=470,11,100
tracelogger: buf 0 ev 27 header=0xc06 d=480,100,1
tracelogger: buf 0 ev 28 header=0x140d d=490,0x70001,0xa0014
lkl-trace: mode=net sample=9 pid=7 tid=1 before_ns=400 after_ns=460 delta_ns=60
LOG

python3 "$root/scripts/rt-trace-sched.py" "$tmp/guest.log" --top 2 >"$tmp/out"
python3 - "$root" <<'PY'
import importlib.util
import pathlib
import sys

root = pathlib.Path(sys.argv[1])
# The script imports rt_trace_common, which it resolves because running it
# directly puts scripts/ on sys.path. Loading it by file path does not.
sys.path.insert(0, str(root / "scripts"))
spec = importlib.util.spec_from_file_location(
    "rt_trace_sched", root / "scripts" / "rt-trace-sched.py"
)
mod = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mod)
assert mod.decode_ipc_endpoint("comm_smsg", (7 << 16) | 3) == (7, 3)
assert mod.decode_ipc_endpoint("comm_reply", 16) == (15, 0)
assert mod.decode_ipc_endpoint("comm_rmsg", 16) == (0, 0)
PY
grep -q '^state_running,4$' "$tmp/out"
grep -q '^state_send,3$' "$tmp/out"
grep -q '^comm_smsg,1$' "$tmp/out"
grep -q '^comm_reply,2$' "$tmp/out"
grep -q '^irq_claim,2$' "$tmp/out"
grep -q '^ist_dispatch,2$' "$tmp/out"
grep -q '^pi_boost,1$' "$tmp/out"
grep -q '^lkl_host_enter,1$' "$tmp/out"
grep -q '^lkl_measure_begin,1$' "$tmp/out"
grep -q '^lkl_measure_wake,1$' "$tmp/out"
grep -q '^80,state_reply,state_running$' "$tmp/out"
grep -q '^state_running,4,30,32,110,110,7,1,0$' "$tmp/out"
grep -q '^state_send,2,36,43,50,50,7,1,0$' "$tmp/out"
grep -q '^state_reply,1,80,80,80,80,7,1,0$' "$tmp/out"
grep -q '^measure,net,9,60,7,1$' "$tmp/out"
grep -q '^measure_summary,net,1,60,60,60,60$' "$tmp/out"
grep -q '^measure_gap,net,9,35,state_send,comm_reply,7,1,16,7,0,0,0,15,0,7$' "$tmp/out"
grep -q '^measure_transition_summary,net,comm_reply,state_reply,1,1,1,1,1$' "$tmp/out"
grep -q '^measure_transition_summary,net,state_send,comm_reply,1,35,35,35,35$' "$tmp/out"
grep -q '^measure_state,net,9,state_running,20,7,1,0$' "$tmp/out"
grep -q '^measure_state,net,9,state_send,36,7,1,0$' "$tmp/out"
grep -q '^measure_state_summary,net,state_running,1,20,20,20,20$' "$tmp/out"
grep -q '^measure_state_summary,net,state_send,1,36,36,36,36$' "$tmp/out"
grep -q '^measure_client_state,net,9,state_running,20,1,0$' "$tmp/out"
grep -q '^measure_client_state,net,9,state_send,36,1,0$' "$tmp/out"
grep -q '^measure_client_state_summary,net,state_running,1,20,20,20,20$' "$tmp/out"
grep -q '^measure_client_state_summary,net,state_send,1,36,36,36,36$' "$tmp/out"
grep -q '^measure_event,net,9,comm_reply,1$' "$tmp/out"
grep -q '^measure_event,net,9,state_send,1$' "$tmp/out"
grep -q '^measure_event_summary,net,comm_reply,1$' "$tmp/out"
grep -q '^measure_event_summary,net,state_send,1$' "$tmp/out"
grep -q '^measure_irq,net,9,11,100,1,5$' "$tmp/out"
grep -q '^measure_irq_summary,net,11,100,1,5,5,5,5$' "$tmp/out"
grep -q '^measure_wake,net,9,5,55$' "$tmp/out"
grep -q '^measure_wake_summary,net,1,5,55,55$' "$tmp/out"
grep -q '^measure_clock,net,9,60,60,0$' "$tmp/out"
grep -q '^measure_mono,net,9,wake,407,450,-43$' "$tmp/out"
grep -q '^measure_mono_span,net,9,5,60,57,60$' "$tmp/out"
grep -q '^measure_deadline,net,9,-10,50,-10$' "$tmp/out"
grep -q '^measure_entry,net,9,20,10,0,state_running,20,8,1$' "$tmp/out"
grep -q '^measure_ipc,net,9,comm_reply,16,15,0,7,1$' "$tmp/out"
grep -q '^measure_ipc_summary,net,comm_reply,16,15,0,7,1$' "$tmp/out"
grep -q '^measure_critical,net,9,60,state_send,36,7,1,7,1,0,state_send,36,1,0,state_send,comm_reply,35,0,1,2$' "$tmp/out"
grep -q '^measure_critical_state_summary,net,state_send,1,36,36,36,36,7,1,0$' "$tmp/out"
grep -q '^measure_critical_transition_summary,net,state_send,comm_reply,1,35,35,35,35$' "$tmp/out"

cat >"$tmp/guest-wrap.log" <<'LOG'
tracelogger: buf 0 ev 0 header=0x1001 d=0xfffffff0,7,1
tracelogger: buf 0 ev 1 header=0x1004 d=0x10,7,1
tracelogger: buf 0 ev 2 header=0x1006 d=0x30,7,1
LOG

python3 "$root/scripts/rt-trace-sched.py" "$tmp/guest-wrap.log" >"$tmp/wrap.out"
grep -q '^32,state_running,state_send$' "$tmp/wrap.out"
grep -q '^state_running,1,32,32,32,32,7,1,0$' "$tmp/wrap.out"

cat >"$tmp/guest-control.log" <<'LOG'
tracelogger: buf 0 ev 0 header=0x401 d=1,0,0
LOG

python3 "$root/scripts/rt-trace-sched.py" "$tmp/guest-control.log" >"$tmp/control.out"
grep -q '^control_time,1$' "$tmp/control.out"

cat >"$tmp/guest-out-of-order.log" <<'LOG'
tracelogger: buf 0 ev 0 header=0x1004 d=120,7,1
tracelogger: buf 1 ev 0 header=0x1a02 d=100,0x2,9
tracelogger: buf 1 ev 1 header=0x1a03 d=160,0x2,9
lkl-trace: mode=net sample=9 pid=7 tid=1 before_ns=100 after_ns=160 delta_ns=60
LOG

python3 "$root/scripts/rt-trace-sched.py" "$tmp/guest-out-of-order.log" \
    >"$tmp/out-of-order.out"
grep -q '^measure,net,9,60,7,1$' "$tmp/out-of-order.out"
python3 - "$root" "$tmp/guest-out-of-order.log" <<'PY'
import importlib.util
import pathlib
import sys

root = pathlib.Path(sys.argv[1])
log = pathlib.Path(sys.argv[2])
sys.path.insert(0, str(root / "scripts"))
spec = importlib.util.spec_from_file_location(
    "rt_trace_sched", root / "scripts" / "rt-trace-sched.py"
)
mod = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mod)
events, _cycles_per_sec = mod.read_events(log)
timestamps = [ev[0] for ev in events]
if timestamps[:2] != [120, 100]:
    raise SystemExit(f"raw trace order was not preserved: {timestamps[:2]}")
PY

cat >"$tmp/guest-serial-only.log" <<'LOG'
tracelogger: buf 0 ev 0 header=0x1001 d=100,7,1
tracelogger: buf 0 ev 1 header=0x1002 d=160,7,1
lkl-trace: mode=net-rt sample=2 before_ns=90 after_ns=170 delta_ns=80
lkl-trace: mode=net-rt sample=3 pid=42 before_ns=100 after_ns=190 delta_ns=90
LOG

python3 "$root/scripts/rt-trace-sched.py" "$tmp/guest-serial-only.log" >"$tmp/serial-only.out"
grep -q '^measure,net-rt,2,80,0,0$' "$tmp/serial-only.out"
grep -q '^measure,net-rt,3,90,42,0$' "$tmp/serial-only.out"
grep -q '^measure_summary,net-rt,2,80,85,90,90$' "$tmp/serial-only.out"
if grep -q '^measure_gap,net-rt,' "$tmp/serial-only.out"; then
    echo "serial-only fallback must not emit raw-ring gap attribution" >&2
    exit 1
fi
if grep -q '^measure_event,net-rt,' "$tmp/serial-only.out"; then
    echo "serial-only fallback must not emit raw-ring event attribution" >&2
    exit 1
fi
if grep -q '^measure_ipc,net-rt,' "$tmp/serial-only.out"; then
    echo "serial-only fallback must not emit raw-ring IPC attribution" >&2
    exit 1
fi

cat >"$tmp/guest-mismatch.log" <<'LOG'
tracelogger: buf 0 ev 0 header=0x1a02 d=100,0x2,9
tracelogger: buf 0 ev 1 header=0x1004 d=200,7,1
tracelogger: buf 0 ev 2 header=0x1c0a d=500,0x10,7
tracelogger: buf 0 ev 3 header=0x1a03 d=100000000,0x2,9
lkl-trace: mode=net sample=9 pid=7 tid=1 before_ns=400 after_ns=460 delta_ns=60
LOG

python3 "$root/scripts/rt-trace-sched.py" "$tmp/guest-mismatch.log" \
    >"$tmp/mismatch.out"
grep -q '^measure,net,9,60,7,1$' "$tmp/mismatch.out"
if grep -q '^measure_gap,net,9,' "$tmp/mismatch.out"; then
    echo "mismatched raw windows must not emit raw-ring gap attribution" >&2
    exit 1
fi

cat >"$tmp/guest-scaled.log" <<'LOG'
init_qtime: cycles_per_sec=10000000 timer_rate=100 intr=5
tracelogger: buf 0 ev 0 header=0x1a02 d=400,0x2,9
tracelogger: buf 0 ev 1 header=0x1a04 d=405,0x2,9
tracelogger: buf 0 ev 2 header=0x1a03 d=460,0x2,9
LOG

python3 "$root/scripts/rt-trace-sched.py" "$tmp/guest-scaled.log" >"$tmp/scaled.out"
grep -q '^measure,net,9,6000,0,0$' "$tmp/scaled.out"
grep -q '^measure_wake,net,9,500,5500$' "$tmp/scaled.out"

# Priority-inheritance boost/restore pairing (header 0x140d = system class,
# pi_boost event; d1 = thread, d2 = (old_prio << 16) | new_prio). Covers a clean
# span, a nested boost that must collapse into one span, a partial restore that
# must not close the span, a stray restore with no open span, and a thread whose
# restore prints in an earlier buffer than its raise. The last case only pairs if
# each thread's events are sorted by timestamp before pairing, so a count of 3
# (not 2) proves the out-of-order case was paired.
cat >"$tmp/guest-pi-boost.log" <<'LOG'
tracelogger: buf 0 ev 0 header=0x140d d=100,0x30001,0xa0028
tracelogger: buf 0 ev 1 header=0x140d d=150,0x30001,0x28000a
tracelogger: buf 0 ev 2 header=0x140d d=200,0x30002,0xa0028
tracelogger: buf 0 ev 3 header=0x140d d=210,0x30002,0x280032
tracelogger: buf 0 ev 4 header=0x140d d=230,0x30002,0x320028
tracelogger: buf 0 ev 5 header=0x140d d=260,0x30002,0x28000a
tracelogger: buf 0 ev 6 header=0x140d d=300,0x30003,0x28000a
tracelogger: buf 0 ev 7 header=0x140d d=460,0x30004,0x28000a
tracelogger: buf 1 ev 0 header=0x140d d=400,0x30004,0xa0028
LOG

python3 "$root/scripts/rt-trace-sched.py" "$tmp/guest-pi-boost.log" \
    >"$tmp/pi-boost.out"
grep -q '^pi_boost,9$' "$tmp/pi-boost.out"
grep -q '^measure_summary,pi_boost,3,50,60,60,60$' "$tmp/pi-boost.out"

# Per-kernel-call cost rows. Class 2 (KER_CALL) with event >= 128 is an exit
# event whose d2 is the elapsed tick count the kernel measured; with no
# init_qtime line the parser passes ticks through as ns. Event 128+15 is
# msg_replyv. The enter event (15) and the dead-wrapper encoding (256+15) must
# both be ignored: neither carries a duration.
cat >"$tmp/guest-kcall.log" <<'LOG'
tracelogger: buf 0 ev 0 header=0x88f d=100,0x10001,100
tracelogger: buf 0 ev 1 header=0x88f d=200,0x10001,300
tracelogger: buf 0 ev 2 header=0x80f d=300,0x10001,999
tracelogger: buf 0 ev 3 header=0x90f d=400,0x10001,999
LOG

python3 "$root/scripts/rt-trace-sched.py" "$tmp/guest-kcall.log" >"$tmp/kcall.out"
grep -q '^measure_summary,kcall_msg_replyv,2,100,200,300,300$' "$tmp/kcall.out"
grep -q '^kcall_exit_msg_replyv,2$' "$tmp/kcall.out"
# Not `grep -vq`: that succeeds whenever any line fails to match, which on a
# multi-line CSV is always true. The assertion is that the enter event and
# the 2*_TRACE_MAX_KER_CALL_NUM encoding did not inflate the sample count.
! grep -q 'kcall_msg_replyv,4' "$tmp/kcall.out"

# The kernel-call name table and the exit-event offset are hand-mirrored from
# the kernel headers. Fail here rather than silently mislabelling a budget row
# if a call number is added, removed, or renumbered.
python3 - "$root" <<'PYEOF'
import importlib.util, re, sys

root = sys.argv[1]
# rt-trace-sched.py imports rt_trace_common, which it finds because running it
# directly puts scripts/ on sys.path. Loading it by file path here does not, so
# add it explicitly.
sys.path.insert(0, root + "/scripts")
spec = importlib.util.spec_from_file_location(
    "sched", root + "/scripts/rt-trace-sched.py")
mod = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mod)

trace_h = open(root + "/internal/include/sys/trace.h").read()
m = re.search(r"#define\s+_TRACE_MAX_KER_CALL_NUM\s+\((\d+)\)", trace_h)
if not m or int(m.group(1)) != mod.TRACE_MAX_KER_CALL_NUM:
    sys.exit("rt-trace-sched.py: TRACE_MAX_KER_CALL_NUM disagrees with "
             "_TRACE_MAX_KER_CALL_NUM in internal/include/sys/trace.h")

table = open(root + "/kernel/include/core/call-table.h").read()
handlers = re.findall(r"_KT\(pref##_([a-z0-9_]+)\)", table)
calls_h = open(root + "/internal/include/sys/calls.h").read()
syms = {}
for mm in re.finditer(
        r"^\s*(__KSYS_[A-Z0-9_]+)(?:\s*=\s*\d+)?\s*,?\s*/\*\s*(\d+)",
        calls_h, re.M):
    syms[int(mm.group(2))] = mm.group(1)

bad = []
for i, fn in enumerate(handlers):
    named = i in mod.KCALL_NAMES
    if fn == "bad":
        if named:
            bad.append("slot %d is unimplemented but named %r"
                       % (i, mod.KCALL_NAMES[i]))
    elif not named:
        bad.append("slot %d (%s) missing from KCALL_NAMES"
                   % (i, syms.get(i, "?")))
    elif syms.get(i) and mod.KCALL_NAMES[i] != syms[i][len("__KSYS_"):].lower():
        bad.append("slot %d named %r, ABI says %s"
                   % (i, mod.KCALL_NAMES[i], syms[i]))
for k in mod.KCALL_NAMES:
    if k >= len(handlers):
        bad.append("KCALL_NAMES has slot %d past the end of the call table" % k)
if bad:
    sys.exit("rt-trace-sched.py KCALL_NAMES drift:\n  " + "\n  ".join(bad))
PYEOF

echo "rt-trace-sched-test: PASS"
