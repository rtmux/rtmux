#!/bin/sh
# Self-contained smoke test for the raw tracelogger -> CTF exporter.
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
tmp=${TMPDIR:-/tmp}/rtmux-ctf-test.$$
trap 'rm -rf "$tmp"' EXIT HUP INT TERM

mkdir -p "$tmp"
cat >"$tmp/guest.log" <<'LOG'
boot noise
tracelogger: buf 0 ev 0 header=0x400 d=0xfffffff0,0x1,0x0
tracelogger: buf 0 ev 1 header=0x701 d=0x20,0x2,0x3
tracelogger: buf 0 ev 2 header=0x1c00 d=0x30,0x4,0x5
tracelogger: buf 0 ev 3 header=0x1c02 d=0x40,0x6,0x7
tracelogger: buf 0 ev 4 header=0x1c0a d=0x50,0x8,0x9
tracelogger: buf 0 ev 5 header=0x1a00 d=0x60,0x3,0x1234
tracelogger: buf 0 ev 6 header=0x1a01 d=0x70,0x3,0x1234
tracelogger: buf 0 ev 7 header=0x1a02 d=0x80,0x2,0x0
tracelogger: buf 0 ev 8 header=0x1a04 d=0x88,0x2,0x0
tracelogger: buf 0 ev 9 header=0x1a05 d=0x89,0x12000000,0x12345678
tracelogger: buf 0 ev 10 header=0x1a06 d=0x8a,0x12000000,0x0
tracelogger: buf 0 ev 11 header=0x1a03 d=0x90,0x2,0x0
tracelogger: buf 0 ev 12 header=0xc05 d=0xa0,0xb,0x2
tracelogger: buf 0 ev 13 header=0xc06 d=0xb0,0x2,0x7
tracelogger: buf 0 ev 14 header=0x140d d=0xc0,0x70001,0xa0014
tracelogger: buf 0 ev 15 header=0x1402 d=0xd0,0,0
tracelogger: PASS
LOG

python3 "$root/scripts/rt-trace-ctf.py" "$tmp/guest.log" -o "$tmp/trace"
test -s "$tmp/trace/metadata"
test -s "$tmp/trace/stream"
grep -q 'name = "comm_smsg";' "$tmp/trace/metadata"
grep -q 'name = "comm_rmsg";' "$tmp/trace/metadata"
grep -q 'name = "comm_reply";' "$tmp/trace/metadata"
grep -q 'name = "lkl_host_enter";' "$tmp/trace/metadata"
grep -q 'name = "lkl_host_exit";' "$tmp/trace/metadata"
grep -q 'name = "lkl_host_enter_sem_down";' "$tmp/trace/metadata"
grep -q 'name = "lkl_host_exit_sem_down";' "$tmp/trace/metadata"
grep -q 'name = "lkl_measure_begin";' "$tmp/trace/metadata"
grep -q 'name = "lkl_measure_end";' "$tmp/trace/metadata"
grep -q 'name = "lkl_measure_wake";' "$tmp/trace/metadata"
grep -q 'name = "lkl_measure_mono_low";' "$tmp/trace/metadata"
grep -q 'name = "lkl_measure_mono_high";' "$tmp/trace/metadata"
grep -q 'name = "irq_claim";' "$tmp/trace/metadata"
grep -q 'name = "ist_dispatch";' "$tmp/trace/metadata"
grep -q 'name = "pi_boost";' "$tmp/trace/metadata"
grep -q 'name = "sys_path";' "$tmp/trace/metadata"
grep -q 'uint32_t srcvid;' "$tmp/trace/metadata"
grep -q 'uint32_t peer_pid;' "$tmp/trace/metadata"
grep -q 'uint32_t op;' "$tmp/trace/metadata"
grep -q 'uint32_t value;' "$tmp/trace/metadata"
grep -q 'uint32_t mode;' "$tmp/trace/metadata"
grep -q 'uint32_t sample;' "$tmp/trace/metadata"
grep -q 'uint32_t phase_mode_sample;' "$tmp/trace/metadata"
grep -q 'uint32_t mono_low;' "$tmp/trace/metadata"
grep -q 'uint32_t mono_high;' "$tmp/trace/metadata"
grep -q 'uint32_t irq;' "$tmp/trace/metadata"
grep -q 'uint32_t target_pid;' "$tmp/trace/metadata"
grep -q 'uint32_t pid_tid;' "$tmp/trace/metadata"
grep -q 'uint32_t old_new_priority;' "$tmp/trace/metadata"

python3 - "$tmp/trace/stream" <<'PY'
import struct
import sys

data = open(sys.argv[1], "rb").read()
magic, stream_id = struct.unpack_from("<I16xI", data, 0)
assert magic == 0xC1FC1FC1
assert stream_id == 0
ts_begin, ts_end, content_bits, packet_bits, discarded = struct.unpack_from(
    "<QQQQI", data, 24)
assert ts_begin == 0xfffffff0
assert ts_end == 0x1000000d0
assert content_bits == packet_bits == len(data) * 8
assert discarded == 0
records = [
    struct.unpack_from("<IQIIIIIIII", data, 60 + i * 44)
    for i in range(16)
]
assert records == [
    (0, 0xfffffff0, 0x400, 0xfffffff0, 0x1, 0x0, 1, 0, 0, 0),
    (0, 0x100000020, 0x701, 0x20, 0x2, 0x3, 1, 0x301, 0, 0),
    (5, 0x100000030, 0x1c00, 0x30, 0x4, 0x5, 7, 0, 0, 0),
    (6, 0x100000040, 0x1c02, 0x40, 0x6, 0x7, 7, 2, 0, 0),
    (7, 0x100000050, 0x1c0a, 0x50, 0x8, 0x9, 7, 10, 0, 0),
    (38, 0x100000060, 0x1a00, 0x60, 0x3, 0x1234, 6, 0x200, 0, 0),
    (39, 0x100000070, 0x1a01, 0x70, 0x3, 0x1234, 6, 0x201, 0, 0),
    (10, 0x100000080, 0x1a02, 0x80, 0x2, 0x0, 6, 0x202, 0, 0),
    (16, 0x100000088, 0x1a04, 0x88, 0x2, 0x0, 6, 0x204, 0, 0),
    (17, 0x100000089, 0x1a05, 0x89, 0x12000000, 0x12345678, 6, 0x205, 0, 0),
    (18, 0x10000008a, 0x1a06, 0x8a, 0x12000000, 0x0, 6, 0x206, 0, 0),
    (11, 0x100000090, 0x1a03, 0x90, 0x2, 0x0, 6, 0x203, 0, 0),
    (12, 0x1000000a0, 0xc05, 0xa0, 0xb, 0x2, 3, 5, 0, 0),
    (13, 0x1000000b0, 0xc06, 0xb0, 0x2, 0x7, 3, 6, 0, 0),
    (14, 0x1000000c0, 0x140d, 0xc0, 0x70001, 0xa0014, 5, 13, 0, 0),
    (15, 0x1000000d0, 0x1402, 0xd0, 0x0, 0x0, 5, 2, 0, 0),
]
PY

cat >"$tmp/guest-scaled.log" <<'LOG'
init_qtime: cycles_per_sec=10000000 timer_rate=100 intr=5
tracelogger: buf 0 ev 0 header=0x1a02 d=0x80,0x2,0x0
tracelogger: buf 0 ev 1 header=0x1a03 d=0x90,0x2,0x0
LOG

python3 "$root/scripts/rt-trace-ctf.py" "$tmp/guest-scaled.log" -o "$tmp/scaled-trace"
python3 - "$tmp/scaled-trace/stream" <<'PY'
import struct
import sys

data = open(sys.argv[1], "rb").read()
ts_begin, ts_end = struct.unpack_from("<QQ", data, 24)
assert ts_begin == 0x80 * 100
assert ts_end == 0x90 * 100
records = [
    struct.unpack_from("<IQIIIIIIII", data, 60 + i * 44)
    for i in range(2)
]
assert records[0][1] == 0x80 * 100
assert records[0][3] == 0x80
assert records[1][1] == 0x90 * 100
assert records[1][3] == 0x90
PY

if command -v babeltrace2 >/dev/null 2>&1; then
    babeltrace2 "$tmp/trace" >"$tmp/babeltrace.out"
    grep -q 'comm_smsg' "$tmp/babeltrace.out"
    grep -q 'comm_rmsg' "$tmp/babeltrace.out"
    grep -q 'comm_reply' "$tmp/babeltrace.out"
    grep -q 'lkl_host_enter' "$tmp/babeltrace.out"
    grep -q 'lkl_host_exit' "$tmp/babeltrace.out"
    grep -q 'lkl_host_enter_sem_down' "$tmp/babeltrace.out"
    grep -q 'lkl_host_exit_sem_down' "$tmp/babeltrace.out"
    grep -q 'lkl_measure_begin' "$tmp/babeltrace.out"
    grep -q 'lkl_measure_end' "$tmp/babeltrace.out"
    grep -q 'lkl_measure_wake' "$tmp/babeltrace.out"
    grep -q 'lkl_measure_mono_low' "$tmp/babeltrace.out"
    grep -q 'lkl_measure_mono_high' "$tmp/babeltrace.out"
    grep -q 'irq_claim' "$tmp/babeltrace.out"
    grep -q 'ist_dispatch' "$tmp/babeltrace.out"
    grep -q 'pi_boost' "$tmp/babeltrace.out"
    grep -q 'sys_path' "$tmp/babeltrace.out"
fi

echo "rt-trace-ctf-test: PASS"
