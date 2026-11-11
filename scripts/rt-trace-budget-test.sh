#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
tmp=${TMPDIR:-/tmp}/rtmux-trace-budget-test.$$
trap 'rm -rf "$tmp"' EXIT HUP INT TERM

mkdir -p "$tmp"
cat >"$tmp/sched.csv" <<'CSV'
event,count
state_running,1

measure_summary,mode,count,min_ns,p50_ns,p99_ns,max_ns
measure_summary,net,3,100,200,300,400
measure_summary,net-rt,5,10,20,30,40
measure_summary,kcall_msg_replyv,7,1000,1100,1200,1300
CSV
cat >"$tmp/guest.log" <<'LOG'
lkl-measure: mode=net samples=2 max_ns=500 p99_ns=450 p50_ns=200 min_ns=100
LOG

COMMON="--command c --config-hash sha256:abc --target t --opensbi o --kernel-git-sha s --date 2026-07-07"
FS="qemu-only PB-TIMER-RDTIME, PB-DMA-COHERENT"
NET="qemu-only PB-TIMER-RDTIME, PB-DMA-COHERENT, PB-IOMMU-UNDESCRIBED"

# Mixed run: LKL fs/net base rows plus a kcall row, a pi_boost row and the
# remaining known LKL modes in one CSV.  Each row cites by its own class, never by the run's mode.
cat >"$tmp/mixed.csv" <<'CSV'
measure_summary,mode,count,min_ns,p50_ns,p99_ns,max_ns
measure_summary,fs,4,1,2,3,4
measure_summary,net,3,100,200,300,400
measure_summary,net-rt,5,10,20,30,40
measure_summary,kcall_msg_replyv,7,1000,1100,1200,1300
measure_summary,pi_boost,2,5,6,7,8
measure_summary,fs-rt,6,9,10,11,12
measure_summary,net-blocked,8,1,2,3,4
measure_summary,net-revoke,9,5,6,7,8
CSV
cat >"$tmp/guest.log" <<'LOG'
lkl-measure: mode=net samples=2 max_ns=500 p99_ns=450 p50_ns=200 min_ns=100
LOG
# shellcheck disable=SC2086
python3 "$root/scripts/rt-trace-budget.py" "$tmp/mixed.csv" $COMMON \
    --platform-fs "$FS" --platform-net "$NET" \
    --platform-host "qemu-only PB-TIMER-RDTIME" \
    --guest-log "$tmp/guest.log" >"$tmp/budget.md"
python3 "$root/scripts/rt-budgets-check.py" "$tmp/budget.md"
# block_platform CLASS: the platform line of the block whose class is CLASS
block_platform()
{
    awk -v c="class: $1" '$0 == c { f = 1 } f && /^platform: / { print; exit }' "$tmp/budget.md"
}
test "$(block_platform 'LKL ext4 traced request round trip')" = "platform: $FS"
test "$(block_platform 'LKL network traced request round trip')" = "platform: $NET"
test "$(block_platform 'LKL network traced under-load native wake')" = "platform: $NET"
test "$(block_platform 'kcall-msg_replyv')" = "platform: qemu-only PB-TIMER-RDTIME"
test "$(block_platform 'priority-inheritance boost span')" = "platform: qemu-only PB-TIMER-RDTIME"
test "$(block_platform 'LKL ext4 traced under-load native wake')" = "platform: $FS"
test "$(block_platform 'LKL network traced blocked-poll timeout')" = "platform: $NET"
test "$(block_platform 'LKL network traced revoke')" = "platform: $NET"
# the kcall and pi rows must not pick up the LKL base (DMA) citation: the
# fs, fs-rt, net, net-rt, net-blocked and net-revoke blocks cite it, no more
test "$(grep -c 'PB-DMA-COHERENT' "$tmp/budget.md")" = 6
grep -q '^samples: 2$' "$tmp/budget.md"
grep -q '^max-ns: 500$' "$tmp/budget.md"
grep -q '^budget-ns: 2000000$' "$tmp/budget.md"
grep -Fq "histogram-artifact: $(cd "$tmp" && pwd)/mixed.csv" "$tmp/budget.md"

# A kcall-only run needs no per-class option at all.
printf 'measure_summary,mode,count,min_ns,p50_ns,p99_ns,max_ns\nmeasure_summary,kcall_x,1,1,1,1,1\n' \
    >"$tmp/kcall.csv"
# shellcheck disable=SC2086
python3 "$root/scripts/rt-trace-budget.py" "$tmp/kcall.csv" $COMMON >"$tmp/kcall.md"
grep -q '^platform: qemu-only PB-TIMER-RDTIME$' "$tmp/kcall.md"

# fail closed: an fs/net/host row without its own option is refused, and
# another class's option is never borrowed.
expect_refused()
{
    why=$1; shift
    if python3 "$root/scripts/rt-trace-budget.py" "$@" >"$tmp/out" 2>"$tmp/err"; then
        echo "rt-trace-budget-test: $why was accepted" >&2
        exit 1
    fi
    if [ -s "$tmp/out" ]; then
        echo "rt-trace-budget-test: $why left partial output" >&2
        exit 1
    fi
}
# shellcheck disable=SC2086
{
expect_refused "mixed run missing --platform-net" "$tmp/mixed.csv" $COMMON \
    --platform-fs "$FS"
grep -q 'needs --platform-net' "$tmp/err"
expect_refused "mixed run missing --platform-fs" "$tmp/mixed.csv" $COMMON \
    --platform-net "$NET" --platform-host "qemu-only PB-TIMER-RDTIME"
grep -q 'needs --platform-fs' "$tmp/err"
# a net row never borrows the fs citation
expect_refused "net row borrowing --platform-fs" "$tmp/sched.csv" $COMMON \
    --platform-fs "$FS"
grep -q 'needs --platform-net' "$tmp/err"
# each known LKL mode alone is refused without its own class option
for m in fs-rt:fs net-blocked:net net-revoke:net; do
    printf 'measure_summary,mode,count,min_ns,p50_ns,p99_ns,max_ns\nmeasure_summary,%s,1,1,1,1,1\n' \
        "${m%%:*}" >"$tmp/one.csv"
    expect_refused "${m%%:*} without --platform-${m#*:}" "$tmp/one.csv" $COMMON
    grep -q "needs --platform-${m#*:}" "$tmp/err"
done
}
printf 'measure_summary,mode,count,min_ns,p50_ns,p99_ns,max_ns\nmeasure_summary,mystery,1,1,1,1,1\n' \
    >"$tmp/unknown.csv"
# shellcheck disable=SC2086
expect_refused "unclassified row class" "$tmp/unknown.csv" $COMMON \
    --platform-fs "$FS" --platform-net "$NET" \
    --platform-host "qemu-only PB-TIMER-RDTIME"
grep -q "mode 'mystery' has no platform contract" "$tmp/err"
# irq is not a measure_summary class, so an irq-named mode is just unknown
printf 'measure_summary,mode,count,min_ns,p50_ns,p99_ns,max_ns\nmeasure_summary,irq,1,1,1,1,1\n' \
    >"$tmp/irq.csv"
# shellcheck disable=SC2086
expect_refused "irq mode" "$tmp/irq.csv" $COMMON --platform-fs "$FS" --platform-net "$NET" \
    --platform-host "qemu-only PB-TIMER-RDTIME"
grep -q "mode 'irq' has no platform contract" "$tmp/err"
# the retired --platform-irq option is rejected, and abbreviations are not
# accepted for the real options
# shellcheck disable=SC2086
expect_refused "--platform-irq" "$tmp/kcall.csv" $COMMON --platform-irq "$FS"
# shellcheck disable=SC2086
expect_refused "abbreviated --platform-f" "$tmp/kcall.csv" $COMMON --platform-f "$FS"
# shellcheck disable=SC2086
expect_refused "abbreviated --comm" "$tmp/kcall.csv" --comm c --config-hash sha256:abc \
    --target t --opensbi o --kernel-git-sha s --date 2026-07-07
# an unclassified row among good ones still fails the whole run, with no output
printf 'measure_summary,mode,count,min_ns,p50_ns,p99_ns,max_ns\nmeasure_summary,kcall_x,1,1,1,1,1\nmeasure_summary,mystery,1,1,1,1,1\n' \
    >"$tmp/unknown2.csv"
# shellcheck disable=SC2086
expect_refused "unclassified row after a good one" "$tmp/unknown2.csv" $COMMON
# the retired --platform option is gone, not silently accepted
# shellcheck disable=SC2086
expect_refused "retired --platform" "$tmp/kcall.csv" $COMMON --platform "$FS"
# generated blocks cannot claim production through any class option
for cls in fs net host; do
    # shellcheck disable=SC2086
    expect_refused "production --platform-$cls" "$tmp/kcall.csv" $COMMON \
        --platform-$cls "production PB-ICACHE-FENCEI"
    grep -q -- "--platform-$cls: generated blocks" "$tmp/err"
done
# shellcheck disable=SC2086
expect_refused "malformed --platform-fs" "$tmp/kcall.csv" $COMMON --platform-fs "qemu-only"

printf 'event,count\nstate_running,1\n' >"$tmp/empty.csv"
# shellcheck disable=SC2086
expect_refused "empty input without --allow-empty" "$tmp/empty.csv" $COMMON
grep -q 'no measure_summary rows found' "$tmp/err"
# shellcheck disable=SC2086
python3 "$root/scripts/rt-trace-budget.py" "$tmp/empty.csv" --allow-empty $COMMON \
    >/dev/null 2>&1

cat >"$tmp/over.md" <<'MD'
```rt-budget
class: over budget fixture
command: fixture
config-hash: sha256:abc
target: qemu
platform: qemu-only PB-TIMER-RDTIME
opensbi: test
kernel-git-sha: testsha
date: 2026-07-07
samples: 1
max-ns: 500
p99-ns: 450
budget-ns: 400
histogram-artifact: fixture.csv
```
MD
if python3 "$root/scripts/rt-budgets-check.py" "$tmp/over.md" >/dev/null 2>&1; then
    echo "rt-trace-budget-test: over-budget block passed" >&2
    exit 1
fi
python3 "$root/scripts/rt-budgets-check.py" --schema-only "$tmp/over.md"

echo "rt-trace-budget-test: PASS"
