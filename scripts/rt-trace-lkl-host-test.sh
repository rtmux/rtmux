#!/bin/sh
# Smoke test for the LKL host-operation trace summarizer.
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
tmp=${TMPDIR:-/tmp}/rtmux-lkl-host-trace-test.$$
trap 'rm -rf "$tmp"' EXIT HUP INT TERM

mkdir -p "$tmp"
cat >"$tmp/guest.log" <<'LOG'
tracelogger: buf 0 ev 0 header=0x1a00 d=100,0x3,0x1111
tracelogger: buf 0 ev 1 header=0x1a01 d=150,0x3,0x1111
tracelogger: buf 0 ev 2 header=0x1a00 d=200,0x4,0x2222
tracelogger: buf 0 ev 3 header=0x1a01 d=260,0x4,0x2222
tracelogger: buf 0 ev 4 header=0x1a00 d=300,0x4,0x2222
tracelogger: buf 0 ev 5 header=0x1a01 d=390,0x4,0x2222
LOG

python3 "$root/scripts/rt-trace-lkl-host.py" "$tmp/guest.log" >"$tmp/out"
grep -q '^op,count,min_ns,p50_ns,p99_ns,max_ns$' "$tmp/out"
grep -q '^sem_down,1,50,50,50,50$' "$tmp/out"
grep -q '^mutex_lock,2,60,75,90,90$' "$tmp/out"

# Overlapping waits on two different locks of the same op must pair by object
# identity, not nesting order: enter A, enter B, exit A, exit B gives one A span
# and one B span, not a cross-paired A-with-B. Old op-only LIFO pairing would
# report 190 and 220 here; identity pairing reports 200 and 210.
cat >"$tmp/overlap.log" <<'LOG'
tracelogger: buf 0 ev 0 header=0x1a00 d=100,0x4,0xaaa
tracelogger: buf 0 ev 1 header=0x1a00 d=110,0x4,0xbbb
tracelogger: buf 0 ev 2 header=0x1a01 d=300,0x4,0xaaa
tracelogger: buf 0 ev 3 header=0x1a01 d=320,0x4,0xbbb
LOG
python3 "$root/scripts/rt-trace-lkl-host.py" "$tmp/overlap.log" >"$tmp/overlap.out"
grep -q '^mutex_lock,2,200,205,210,210$' "$tmp/overlap.out"

# With a cycles_per_sec line the raw counter ticks are scaled to nanoseconds; a
# 10 MHz timebase makes one tick 100 ns, so a 50-tick sem_down span is 5000 ns.
cat >"$tmp/scaled.log" <<'LOG'
init_qtime: cycles_per_sec=10000000 timer_rate=100 intr=5
tracelogger: buf 0 ev 0 header=0x1a00 d=100,0x3,0x1111
tracelogger: buf 0 ev 1 header=0x1a01 d=150,0x3,0x1111
LOG
python3 "$root/scripts/rt-trace-lkl-host.py" "$tmp/scaled.log" >"$tmp/scaled.out"
grep -q '^sem_down,1,5000,5000,5000,5000$' "$tmp/scaled.out"

cat >"$tmp/empty.log" <<'LOG'
tracelogger: buf 0 ev 0 header=0x1001 d=100,7,1
LOG
python3 "$root/scripts/rt-trace-lkl-host.py" "$tmp/empty.log" >"$tmp/empty.out"
grep -q '^op,count,min_ns,p50_ns,p99_ns,max_ns$' "$tmp/empty.out"
test "$(wc -l < "$tmp/empty.out" | tr -d ' ')" = 1

# The host-op CSV feeds rt-trace-budget.py, which turns each op row into an
# rt-budget block with a host wait-span class and no budget-ns ceiling.
python3 "$root/scripts/rt-trace-budget.py" --lkl-host "$tmp/out" \
    --command cmd --config-hash sha256:x --platform-fs "qemu-only PB-TIMER-RDTIME, PB-DMA-COHERENT" \
    --platform-host "qemu-only PB-TIMER-RDTIME" \
    --target tcg --opensbi def \
    --kernel-git-sha abc --date 2026-07-14 /dev/null >"$tmp/budget.out"
# a host wait span cites only the timer, not the guest DMA contract
grep -q '^platform: qemu-only PB-TIMER-RDTIME$' "$tmp/budget.out"
if grep -q 'PB-DMA-COHERENT' "$tmp/budget.out"; then
    echo "host-op wait blocks must not cite the guest DMA contract" >&2
    exit 1
fi
# host rows without --platform-host are refused rather than borrowing another class
if python3 "$root/scripts/rt-trace-budget.py" --lkl-host "$tmp/out" \
    --command cmd --config-hash sha256:x --platform-fs "qemu-only PB-TIMER-RDTIME, PB-DMA-COHERENT" \
    --target tcg --opensbi def --kernel-git-sha abc --date 2026-07-14 \
    /dev/null >/dev/null 2>"$tmp/err"; then
    echo "--lkl-host without --platform-host was accepted" >&2
    exit 1
fi
grep -q -- 'needs --platform-host' "$tmp/err"
grep -q '^class: LKL host mutex_lock wait span$' "$tmp/budget.out"
grep -q '^samples: 2$' "$tmp/budget.out"
grep -q '^max-ns: 90$' "$tmp/budget.out"
grep -q '^p99-ns: 90$' "$tmp/budget.out"
if grep -q '^budget-ns:' "$tmp/budget.out"; then
    echo "host-op wait blocks are TCG-absolute and must not assert budget-ns" >&2
    exit 1
fi

echo "rt-trace-lkl-host-test: PASS"
