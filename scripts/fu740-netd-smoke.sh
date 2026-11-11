#!/bin/sh
set -eu

log=build/fu740-logs/lkl-netd.log
runner=${FU740_RUNNER:-}
skip_preflight=0
require_timing=0
timeout=420

usage()
{
    cat <<'EOF'
usage: scripts/fu740-netd-smoke.sh [--log path] [--runner command]
                                   [--require-timing-metadata]
                                   [--skip-preflight] [--timeout seconds]

Runs the QEMU lkl-netd-http and lkl-netd-revoke preflights, then runs the
supplied FU740 board command. The board command must boot RTMux net smoke modes
and stream the serial log to stdout. The log must include:
  lkl-netd-board: ... FU740 ...
  lkl-netd-provider: <native packet provider>
With --require-timing-metadata, the log must also include:
  lkl-netd-timer: <timer configuration>
EOF
}

require()
{
    pattern=$1
    name=$2

    grep -Eq "$pattern" "$log" || {
        echo "fu740-netd-smoke: missing $name" >&2
        exit 1
    }
}

reject_bad_log()
{
    bad_re='FAULT|FATAL|CRASH|Kernel panic|panic:|lkl-host: panic|FREED-ACTIVE|__kernel_restore bad ptr|/usr unavailable|Undefined symbol|No such file or directory|Bad file descriptor'

    if grep -Eq 'RTMux lkl-netd-http: FAIL|RTMux lkl-netd-revoke: FAIL' "$log"; then
        echo "fu740-netd-smoke: board log contains lkl-netd FAIL marker" >&2
        exit 1
    fi
    if grep -Eq "$bad_re" "$log"; then
        echo "fu740-netd-smoke: board log contains crash/fault signature" >&2
        exit 1
    fi
}

need_arg()
{
    opt=$1

    if [ "$#" -lt 2 ] || [ "${2#--}" != "$2" ]; then
        echo "fu740-netd-smoke: $opt requires an argument" >&2
        usage >&2
        exit 2
    fi
}

need_uint()
{
    opt=$1
    value=$2

    case "$value" in
        ''|*[!0-9]*|0)
            echo "fu740-netd-smoke: $opt must be a positive integer" >&2
            exit 2
            ;;
    esac
}

while [ "$#" -gt 0 ]; do
    case "$1" in
        --log)
            need_arg "$@"
            log=$2
            shift 2
            ;;
        --runner)
            need_arg "$@"
            runner=$2
            shift 2
            ;;
        --skip-preflight)
            skip_preflight=1
            shift
            ;;
        --require-timing-metadata)
            require_timing=1
            shift
            ;;
        --timeout)
            need_arg "$@"
            timeout=$2
            shift 2
            ;;
        --help|-h)
            usage
            exit 0
            ;;
        *)
            echo "fu740-netd-smoke: unknown argument: $1" >&2
            usage >&2
            exit 2
            ;;
    esac
done

need_uint --timeout "$timeout"

if [ "$skip_preflight" -eq 0 ]; then
    CPUS=8 scripts/qemu-regress.sh --mode lkl-netd-http --timeout 360 \
        --keep-logs
    CPUS=8 scripts/qemu-regress.sh --mode lkl-netd-revoke --timeout 180 \
        --keep-logs
fi

if [ -z "$runner" ]; then
    echo "fu740-netd-smoke: set FU740_RUNNER or pass --runner" >&2
    exit 2
fi

mkdir -p "$(dirname "$log")"
echo "fu740-netd-smoke: running FU740 lkl-netd smoke"
echo "fu740-netd-smoke: log $log"

set +e
if command -v timeout >/dev/null 2>&1; then
    timeout "$timeout" sh -c "$runner" >"$log" 2>&1
    rc=$?
elif command -v gtimeout >/dev/null 2>&1; then
    gtimeout "$timeout" sh -c "$runner" >"$log" 2>&1
    rc=$?
else
    sh -c "$runner" >"$log" 2>&1
    rc=$?
fi
set -e
cat "$log"
if [ "$rc" -ne 0 ]; then
    echo "fu740-netd-smoke: board runner failed with status $rc" >&2
    exit "$rc"
fi

reject_bad_log
require 'lkl-netd-board: .*FU740' 'FU740 board metadata'
require 'lkl-netd-provider: .+' 'native packet provider metadata'
if [ "$require_timing" -eq 1 ]; then
    require 'lkl-netd-timer: .+' 'timer metadata'
fi
require 'RTMux lkl-netd-http: PASS' 'lkl-netd-http PASS marker'
require 'lkl-netd: lkl_start_kernel returned' 'lkl-netd kernel start'
require 'lkl-netd: serving /net' 'lkl-netd /net service'
require 'verify-lkl-net: ping' 'ICMP smoke'
require 'verify-lkl-net: httpget body length ok' 'httpget smoke'
require 'verify-lkl-net: wget body length ok' 'wget smoke'
require 'verify-lkl-net: nc http body length ok' 'nc TCP smoke'
require 'verify-lkl-net: udp payload ok' 'UDP/DNS smoke'
require 'lkl-backpressure-test: PASS' 'backpressure smoke'
require 'lkl-measure: mode=net-blocked samples=[1-9][0-9]*' \
    'blocked-poll measurement'
require '^net_bounce_rx_bytes [1-9][0-9]*' 'nonzero net_bounce_rx_bytes'
require '^net_bounce_tx_bytes [1-9][0-9]*' 'nonzero net_bounce_tx_bytes'
require 'RTMux lkl-netd-revoke: PASS' 'lkl-netd-revoke PASS marker'
require 'lkl: virtio-net crash after bounced tx' 'crash-after-bounced-tx proof'
require 'lkl-gate: absent-domain roundtrip_ns=[0-9][0-9]*' \
    'namespace cleanup proof'

echo "fu740-netd-smoke: PASS"
