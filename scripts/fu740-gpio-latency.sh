#!/bin/sh
set -eu

log=build/fu740-logs/gpio-latency.log
runner=${FU740_RUNNER:-}
skip_preflight=0
timeout=420
samples=1000000

usage()
{
    cat <<'EOF'
usage: scripts/fu740-gpio-latency.sh [--log path] [--runner command]
                                     [--samples count] [--skip-preflight]
                                     [--timeout seconds]

Runs the QEMU gpio-latency preflight, then runs the supplied FU740 board and
logic-analyzer command. The command must boot rtmux.run=gpio-latency and stream
both the RTMux serial log and capture summary to stdout. The log must include:
  gpio-latency-board: ... FU740 ...
  gpio-latency-timer: <timer and analyzer configuration>
EOF
}

require()
{
    pattern=$1
    name=$2

    grep -Eq "$pattern" "$log" || {
        echo "fu740-gpio-latency: missing $name" >&2
        exit 1
    }
}

reject_bad_log()
{
    bad_re='FAULT|FATAL|CRASH|Kernel panic|panic:|FREED-ACTIVE|__kernel_restore bad ptr|/usr unavailable|Undefined symbol|No such file or directory|Bad file descriptor'

    if grep -Eq 'RTMux gpio-latency: FAIL|gpio-latency: UNSUPPORTED' "$log"; then
        echo "fu740-gpio-latency: board log did not run supported GPIO path" >&2
        exit 1
    fi
    if grep -Eq "$bad_re" "$log"; then
        echo "fu740-gpio-latency: board log contains crash/fault signature" >&2
        exit 1
    fi
}

require_capture()
{
    awk -v want="$samples" '
        /^gpio-latency-capture:/ {
            samples = $2
            max = $3
            p99 = $4
            sub(/^samples=/, "", samples)
            sub(/^max_ns=/, "", max)
            sub(/^p99_ns=/, "", p99)
            if (samples ~ /^[0-9]+$/ && max ~ /^[0-9]+$/ &&
                p99 ~ /^[0-9]+$/ &&
                samples + 0 >= want + 0 && max + 0 > 0 &&
                p99 + 0 > 0 && p99 + 0 <= max + 0)
                found = 1
        }
        END { exit !found }
    ' "$log" || {
        echo "fu740-gpio-latency: missing coherent ${samples}-sample capture" >&2
        exit 1
    }
}

need_arg()
{
    opt=$1

    if [ "$#" -lt 2 ] || [ "${2#--}" != "$2" ]; then
        echo "fu740-gpio-latency: $opt requires an argument" >&2
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
            echo "fu740-gpio-latency: $opt must be a positive integer" >&2
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
        --samples)
            need_arg "$@"
            samples=$2
            shift 2
            ;;
        --skip-preflight)
            skip_preflight=1
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
            echo "fu740-gpio-latency: unknown argument: $1" >&2
            usage >&2
            exit 2
            ;;
    esac
done

need_uint --timeout "$timeout"

case "$samples" in
    ''|*[!0-9]*)
        echo "fu740-gpio-latency: --samples must be a positive integer" >&2
        exit 2
        ;;
    0)
        echo "fu740-gpio-latency: --samples must be a positive integer" >&2
        exit 2
        ;;
esac

if [ "$skip_preflight" -eq 0 ]; then
    CPUS=8 scripts/qemu-regress.sh --mode gpio-latency --timeout 180 --keep-logs
fi

if [ -z "$runner" ]; then
    echo "fu740-gpio-latency: set FU740_RUNNER or pass --runner" >&2
    exit 2
fi

mkdir -p "$(dirname "$log")"
echo "fu740-gpio-latency: running FU740 GPIO latency capture"
echo "fu740-gpio-latency: log $log"

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
    echo "fu740-gpio-latency: board runner failed with status $rc" >&2
    exit "$rc"
fi

reject_bad_log
require 'RTMux gpio-latency: PASS' 'gpio-latency PASS marker'
require 'gpio-latency: delegated sifive-gpio MMIO 0x[0-9a-fA-F]+ irq [0-9]+' \
    'delegated SiFive GPIO'
require_capture
require 'gpio-latency-board: .*FU740' 'FU740 board metadata'
require 'gpio-latency-timer: .+' 'timer/analyzer metadata'

echo "fu740-gpio-latency: PASS"
