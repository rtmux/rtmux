#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
tmp=$(mktemp -d "${TMPDIR:-/tmp}/rtmux-fu740-smoke-test.XXXXXX")
trap 'rm -rf "$tmp"' EXIT INT TERM
cd "$root"

must_fail()
{
    label=$1
    shift

    if "$@" >/dev/null 2>&1; then
        echo "fu740-smoke-test: $label passed unexpectedly" >&2
        exit 1
    fi
}

cat >"$tmp/fsd.log" <<'EOF'
lkl-fsd-board: HiFive Unmatched FU740
lkl-fsd-timer: cycle counter 1GHz monotonic-ns
RTMux lkl-fsd-tree: PASS
lkl_block_read_bytes 4096
lkl_block_write_bytes 4096
EOF

scripts/fu740-fsd-smoke.sh --skip-preflight --log "$tmp/fsd.out" \
    --runner "cat '$tmp/fsd.log'" >/dev/null
scripts/fu740-fsd-smoke.sh --skip-preflight \
    --log "$tmp/fsd-functional-no-timer.out" \
    --runner "sed '/lkl-fsd-timer:/d' '$tmp/fsd.log'" >/dev/null
scripts/fu740-fsd-smoke.sh --skip-preflight --require-timing-metadata \
    --log "$tmp/fsd-timing.out" --runner "cat '$tmp/fsd.log'" >/dev/null
must_fail "fsd missing log arg" scripts/fu740-fsd-smoke.sh --log
must_fail "fsd option-valued log arg" scripts/fu740-fsd-smoke.sh --log --runner
must_fail "fsd missing runner arg" scripts/fu740-fsd-smoke.sh --runner
must_fail "fsd bad timeout" scripts/fu740-fsd-smoke.sh --timeout nope
must_fail "fsd missing timer metadata" scripts/fu740-fsd-smoke.sh \
    --skip-preflight --require-timing-metadata --log "$tmp/fsd-no-timer.out" \
    --runner "sed '/lkl-fsd-timer:/d' '$tmp/fsd.log'"

cat >"$tmp/bad-fsd.log" <<'EOF'
lkl-fsd-board: HiFive Unmatched FU740
RTMux lkl-fsd-tree: PASS
lkl_block_read_bytes 4096
lkl_block_write_bytes 0
EOF

must_fail "bad fsd log" scripts/fu740-fsd-smoke.sh --skip-preflight \
    --log "$tmp/bad-fsd.out" --runner "cat '$tmp/bad-fsd.log'"

cat >"$tmp/fsd-no-board.log" <<'EOF'
RTMux lkl-fsd-tree: PASS
lkl_block_read_bytes 4096
lkl_block_write_bytes 4096
EOF

must_fail "fsd missing board metadata" scripts/fu740-fsd-smoke.sh \
    --skip-preflight --log "$tmp/fsd-no-board.out" \
    --runner "cat '$tmp/fsd-no-board.log'"

cat >"$tmp/netd.log" <<'EOF'
lkl-netd-board: HiFive Unmatched FU740
lkl-netd-provider: fu740-gmac
lkl-netd-timer: cycle counter 1GHz monotonic-ns
RTMux lkl-netd-http: PASS
lkl-netd: lkl_start_kernel returned
lkl-netd: serving /net
verify-lkl-net: ping
verify-lkl-net: httpget body length ok
verify-lkl-net: wget body length ok
verify-lkl-net: nc http body length ok
verify-lkl-net: udp payload ok
lkl-backpressure-test: PASS
lkl-measure: mode=net-blocked samples=3
net_bounce_rx_bytes 64
net_bounce_tx_bytes 128
RTMux lkl-netd-revoke: PASS
lkl: virtio-net crash after bounced tx
lkl-gate: absent-domain roundtrip_ns=1234
EOF

scripts/fu740-netd-smoke.sh --skip-preflight --log "$tmp/netd.out" \
    --runner "cat '$tmp/netd.log'" >/dev/null
scripts/fu740-netd-smoke.sh --skip-preflight \
    --log "$tmp/netd-functional-no-timer.out" \
    --runner "sed '/lkl-netd-timer:/d' '$tmp/netd.log'" >/dev/null
scripts/fu740-netd-smoke.sh --skip-preflight --require-timing-metadata \
    --log "$tmp/netd-timing.out" --runner "cat '$tmp/netd.log'" >/dev/null
must_fail "netd missing log arg" scripts/fu740-netd-smoke.sh --log
must_fail "netd option-valued log arg" scripts/fu740-netd-smoke.sh --log --runner
must_fail "netd missing runner arg" scripts/fu740-netd-smoke.sh --runner
must_fail "netd zero timeout" scripts/fu740-netd-smoke.sh --timeout 0
must_fail "netd missing timer metadata" scripts/fu740-netd-smoke.sh \
    --skip-preflight --require-timing-metadata \
    --log "$tmp/netd-no-timer.out" \
    --runner "sed '/lkl-netd-timer:/d' '$tmp/netd.log'"

cat >"$tmp/gpio.log" <<'EOF'
RTMux gpio-latency: PASS
gpio-latency: delegated sifive-gpio MMIO 0x10060000 irq 7
gpio-latency-capture: samples=1000000 max_ns=3000 p99_ns=2000
gpio-latency-board: HiFive Unmatched FU740
gpio-latency-timer: logic-analyzer 100MHz monotonic-ns
EOF

scripts/fu740-gpio-latency.sh --skip-preflight --log "$tmp/gpio.out" \
    --runner "cat '$tmp/gpio.log'" >/dev/null
must_fail "gpio missing log arg" scripts/fu740-gpio-latency.sh --log
must_fail "gpio option-valued log arg" scripts/fu740-gpio-latency.sh --log --runner
must_fail "gpio missing samples arg" scripts/fu740-gpio-latency.sh --samples
must_fail "gpio bad timeout" scripts/fu740-gpio-latency.sh --timeout nope
scripts/fu740-gpio-latency.sh --skip-preflight --samples 1000000 \
    --log "$tmp/gpio-samples.out" --runner "cat '$tmp/gpio.log'" >/dev/null

cat >"$tmp/gpio-small.log" <<'EOF'
RTMux gpio-latency: PASS
gpio-latency: delegated sifive-gpio MMIO 0x10060000 irq 7
gpio-latency-capture: samples=42 max_ns=3000 p99_ns=2000
gpio-latency-board: HiFive Unmatched FU740
gpio-latency-timer: logic-analyzer 100MHz monotonic-ns
EOF

scripts/fu740-gpio-latency.sh --skip-preflight --samples 42 \
    --log "$tmp/gpio-small.out" --runner "cat '$tmp/gpio-small.log'" \
    >/dev/null

cat >"$tmp/bad-gpio.log" <<'EOF'
RTMux gpio-latency: PASS
gpio-latency: delegated sifive-gpio MMIO 0x10060000 irq 7
gpio-latency-capture: samples=999999 max_ns=3000 p99_ns=2000
gpio-latency-board: HiFive Unmatched FU740
gpio-latency-timer: logic-analyzer 100MHz monotonic-ns
EOF

must_fail "bad gpio log" scripts/fu740-gpio-latency.sh --skip-preflight \
    --log "$tmp/bad-gpio.out" --runner "cat '$tmp/bad-gpio.log'"
must_fail "bad gpio samples" scripts/fu740-gpio-latency.sh --skip-preflight \
    --samples 0 --log "$tmp/gpio-zero.out" --runner "cat '$tmp/gpio.log'"
must_fail "short gpio samples" scripts/fu740-gpio-latency.sh --skip-preflight \
    --samples 43 --log "$tmp/gpio-short.out" \
    --runner "cat '$tmp/gpio-small.log'"

cat >"$tmp/gpio-no-timer.log" <<'EOF'
RTMux gpio-latency: PASS
gpio-latency: delegated sifive-gpio MMIO 0x10060000 irq 7
gpio-latency-capture: samples=1000000 max_ns=3000 p99_ns=2000
gpio-latency-board: HiFive Unmatched FU740
EOF

must_fail "gpio missing timer metadata" scripts/fu740-gpio-latency.sh \
    --skip-preflight --log "$tmp/gpio-no-timer.out" \
    --runner "cat '$tmp/gpio-no-timer.log'"

mempart_class=$(
    awk '
        /^enum lock_class/ { in_enum = 1; next }
        in_enum && /^}/ { exit }
        in_enum && /^[[:space:]]*LOCK_CLASS_/ {
            line = $0
            sub(/=.*/, "", line)
            sub(/,.*/, "", line)
            gsub(/^[[:space:]]+|[[:space:]]+$/, "", line)
            if (line == "LOCK_CLASS_MEMPART") {
                print n
                exit
            }
            n++
        }
    ' kernel/include/lock.h
)
test -n "$mempart_class"

cat >"$tmp/irqoff.log" <<EOF
RTMux rt-check: PASS
irqoff-board: HiFive Unmatched FU740
irqoff-timer: cycle counter 1GHz monotonic-ns
irqoff: count=10 max_ns=3000 max_class=$mempart_class p99_ns=2000
EOF

scripts/fu740-irqoff-budget.sh --skip-preflight --budget-ns 4000 \
    --log "$tmp/irqoff.out" --runner "cat '$tmp/irqoff.log'" >/dev/null
must_fail "irqoff missing budget arg" scripts/fu740-irqoff-budget.sh \
    --budget-ns
must_fail "irqoff option-valued log arg" scripts/fu740-irqoff-budget.sh \
    --log --runner
must_fail "irqoff missing runner arg" scripts/fu740-irqoff-budget.sh \
    --runner
must_fail "irqoff zero timeout" scripts/fu740-irqoff-budget.sh --timeout 0

cat >"$tmp/bad-irqoff.log" <<EOF
RTMux rt-check: PASS
irqoff-board: HiFive Unmatched FU740
irqoff-timer: cycle counter 1GHz monotonic-ns
irqoff: count=10 max_ns=5000 max_class=$mempart_class p99_ns=2000
EOF

must_fail "bad irqoff log" scripts/fu740-irqoff-budget.sh --skip-preflight \
    --budget-ns 4000 --log "$tmp/bad-irqoff.out" \
    --runner "cat '$tmp/bad-irqoff.log'"

cat >"$tmp/irqoff-no-timer.log" <<EOF
RTMux rt-check: PASS
irqoff-board: HiFive Unmatched FU740
irqoff: count=10 max_ns=3000 max_class=$mempart_class p99_ns=2000
EOF

must_fail "irqoff missing timer metadata" scripts/fu740-irqoff-budget.sh \
    --skip-preflight --budget-ns 4000 --log "$tmp/irqoff-no-timer.out" \
    --runner "cat '$tmp/irqoff-no-timer.log'"

cat >"$tmp/bad-netd.log" <<'EOF'
lkl-netd-board: HiFive Unmatched FU740
lkl-netd-provider: fu740-gmac
RTMux lkl-netd-http: PASS
lkl-netd: lkl_start_kernel returned
lkl-netd: serving /net
verify-lkl-net: ping
verify-lkl-net: httpget body length ok
verify-lkl-net: wget body length ok
verify-lkl-net: nc http body length ok
verify-lkl-net: udp payload ok
lkl-backpressure-test: PASS
lkl-measure: mode=net-blocked samples=3
net_bounce_rx_bytes 64
net_bounce_tx_bytes 0
RTMux lkl-netd-revoke: PASS
lkl: virtio-net crash after bounced tx
lkl-gate: absent-domain roundtrip_ns=1234
EOF

must_fail "bad netd log" scripts/fu740-netd-smoke.sh --skip-preflight \
    --log "$tmp/bad-netd.out" --runner "cat '$tmp/bad-netd.log'"

cat >"$tmp/netd-no-provider.log" <<'EOF'
lkl-netd-board: HiFive Unmatched FU740
RTMux lkl-netd-http: PASS
lkl-netd: lkl_start_kernel returned
lkl-netd: serving /net
verify-lkl-net: ping
verify-lkl-net: httpget body length ok
verify-lkl-net: wget body length ok
verify-lkl-net: nc http body length ok
verify-lkl-net: udp payload ok
lkl-backpressure-test: PASS
lkl-measure: mode=net-blocked samples=3
net_bounce_rx_bytes 64
net_bounce_tx_bytes 128
RTMux lkl-netd-revoke: PASS
lkl: virtio-net crash after bounced tx
lkl-gate: absent-domain roundtrip_ns=1234
EOF

must_fail "netd missing provider metadata" scripts/fu740-netd-smoke.sh \
    --skip-preflight --log "$tmp/netd-no-provider.out" \
    --runner "cat '$tmp/netd-no-provider.log'"

echo "fu740-smoke-test: PASS"
