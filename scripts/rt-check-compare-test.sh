#!/bin/sh
# Smoke test for rt-check baseline/instrumentation comparison.
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
tmp=${TMPDIR:-/tmp}/rtmux-rt-check-compare-test.$$
trap 'rm -rf "$tmp"' EXIT HUP INT TERM

mkdir -p "$tmp"
cat >"$tmp/base-1.log" <<'LOG'
rt-latency: policy=SCHED_FIFO priority=60 iterations=100 period_ns=1000000 min_ns=10 avg_ns=20 max_ns=100 p99_ns=80
timer-jitter: policy=SCHED_FIFO priority=60 interval_ns=100000 iterations=50 min_ns=10 avg_ns=20 max_ns=200 p99_ns=150
LOG
cat >"$tmp/base-2.log" <<'LOG'
rt-latency: policy=SCHED_FIFO priority=60 iterations=100 period_ns=1000000 min_ns=10 avg_ns=20 max_ns=120 p99_ns=90
timer-jitter: policy=SCHED_FIFO priority=60 interval_ns=100000 iterations=50 min_ns=10 avg_ns=20 max_ns=180 p99_ns=160
LOG
cat >"$tmp/instr-1.log" <<'LOG'
rt-latency: policy=SCHED_FIFO priority=60 iterations=100 period_ns=1000000 min_ns=10 avg_ns=20 max_ns=150 p99_ns=100
timer-jitter: policy=SCHED_FIFO priority=60 interval_ns=100000 iterations=50 min_ns=10 avg_ns=20 max_ns=100 p99_ns=90
LOG
cat >"$tmp/instr-2.log" <<'LOG'
rt-latency: policy=SCHED_FIFO priority=60 iterations=100 period_ns=1000000 min_ns=10 avg_ns=20 max_ns=1000 p99_ns=800
timer-jitter: policy=SCHED_FIFO priority=60 interval_ns=100000 iterations=50 min_ns=10 avg_ns=20 max_ns=110 p99_ns=95
LOG

python3 "$root/scripts/rt-check-compare.py" \
    --base "$tmp/base-*.log" --instr "$tmp/instr-*.log" >"$tmp/out"
grep -q '^probe,period_ns,base_runs,instr_runs,base_max_ns,instr_max_ns,max_ratio,max_delta_ns,base_p99_ns,instr_p99_ns,p99_delta_ns,p99_ratio$' "$tmp/out"
grep -q '^rt-latency,1000000,2,2,120,1000,8.333,880,90,800,710,8.889$' "$tmp/out"
grep -q '^timer-jitter,100000,2,2,200,110,0.550,-90,160,95,-65,0.594$' "$tmp/out"

python3 "$root/scripts/rt-check-compare.py" \
    --base "$tmp/base-*.log" --instr "$tmp/instr-*.log" \
    --aggregate median >"$tmp/median"
grep -q '^probe,period_ns,base_runs,instr_runs,base_median_max_ns,instr_median_max_ns,max_ratio,max_delta_ns,base_median_p99_ns,instr_median_p99_ns,p99_delta_ns,p99_ratio$' "$tmp/median"
grep -q '^rt-latency,1000000,2,2,110,575,5.227,465,85,450,365,5.294$' "$tmp/median"
grep -q '^timer-jitter,100000,2,2,190,105,0.553,-85,155,92.5,-62.5,0.597$' "$tmp/median"

python3 "$root/scripts/rt-check-compare.py" \
    --base "$tmp/base-*.log" --instr "$tmp/instr-1.log" \
    --max-max-ratio 1.3 --max-p99-ratio 1.2 >"$tmp/threshold-pass"

if python3 "$root/scripts/rt-check-compare.py" \
    --base "$tmp/base-*.log" --instr "$tmp/instr-1.log" \
    --max-max-ratio 1.1 >"$tmp/threshold-fail" 2>"$tmp/threshold-err"; then
    echo "rt-check-compare-test: threshold comparison passed unexpectedly" >&2
    exit 1
fi
grep -q 'max_ratio 1.250 exceeds 1.100' "$tmp/threshold-err"

if python3 "$root/scripts/rt-check-compare.py" \
    --base "$tmp/base-*.log" --instr "$tmp/instr-1.log" \
    --max-p99-delta-ns 5 >"$tmp/delta-fail" 2>"$tmp/delta-err"; then
    echo "rt-check-compare-test: delta comparison passed unexpectedly" >&2
    exit 1
fi
grep -q 'p99_delta_ns 10 exceeds 5' "$tmp/delta-err"

cat >"$tmp/other.log" <<'LOG'
rt-latency: policy=SCHED_FIFO priority=60 iterations=100 period_ns=2000000 min_ns=10 avg_ns=20 max_ns=100 p99_ns=80
LOG
if python3 "$root/scripts/rt-check-compare.py" \
    --base "$tmp/base-*.log" --instr "$tmp/other.log" >"$tmp/empty" 2>&1; then
    echo "rt-check-compare-test: no-overlap comparison passed unexpectedly" >&2
    exit 1
fi
grep -q '^no common rt-check probe rows found$' "$tmp/empty"

echo "rt-check-compare-test: PASS"
