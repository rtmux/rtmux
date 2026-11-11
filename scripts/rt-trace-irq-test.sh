#!/bin/sh
# Smoke test for the IRQ claim -> IST dispatch trace summarizer.
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
tmp=${TMPDIR:-/tmp}/rtmux-irq-trace-test.$$
trap 'rm -rf "$tmp"' EXIT HUP INT TERM

mkdir -p "$tmp"
cat >"$tmp/guest.log" <<'LOG'
tracelogger: buf 0 ev 0 header=0xc05 d=100,11,42
tracelogger: buf 0 ev 1 header=0x1001 d=120,7,1
tracelogger: buf 0 ev 2 header=0xc06 d=180,42,3
tracelogger: buf 0 ev 3 header=0xc05 d=200,11,42
tracelogger: buf 0 ev 4 header=0xc06 d=260,42,3
tracelogger: buf 0 ev 5 header=0xc05 d=300,12,77
tracelogger: buf 0 ev 6 header=0xc06 d=330,77,4
tracelogger: buf 1 ev 6 header=0xc06 d=330,77,4
LOG

python3 "$root/scripts/rt-trace-irq.py" "$tmp/guest.log" >"$tmp/out"
grep -q '^irq,pid,count,min_ns,p50_ns,p99_ns,max_ns$' "$tmp/out"
grep -q '^11,42,2,60,70,80,80$' "$tmp/out"
grep -q '^12,77,1,30,30,30,30$' "$tmp/out"

cat >"$tmp/wrap.log" <<'LOG'
tracelogger: buf 0 ev 0 header=0xc05 d=0xfffffff0,11,42
tracelogger: buf 0 ev 1 header=0xc06 d=0x20,42,3
LOG

python3 "$root/scripts/rt-trace-irq.py" "$tmp/wrap.log" >"$tmp/wrap.out"
grep -q '^11,42,1,48,48,48,48$' "$tmp/wrap.out"

echo "rt-trace-irq-test: PASS"
