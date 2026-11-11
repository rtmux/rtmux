#!/bin/sh
set -eu

log=build/fu740-logs/lkl-fsd-tree.log
runner=${FU740_RUNNER:-}
skip_preflight=0
require_timing=0
timeout=360

usage()
{
    cat <<'EOF'
usage: scripts/fu740-fsd-smoke.sh [--log path] [--runner command]
                                  [--require-timing-metadata]
                                  [--skip-preflight] [--timeout seconds]

Runs the QEMU lkl-fsd-tree differential preflight, then runs the supplied FU740
board command. The board command must boot RTMux with rtmux.run=lkl-fsd-tree and
stream the serial log to stdout. The log must include:
  lkl-fsd-board: ... FU740 ...
With --require-timing-metadata, the log must also include:
  lkl-fsd-timer: <timer configuration>
EOF
}

require()
{
    pattern=$1
    name=$2

    grep -Eq "$pattern" "$log" || {
        echo "fu740-fsd-smoke: missing $name" >&2
        exit 1
    }
}

reject_bad_log()
{
    bad_re='FAULT|FATAL|CRASH|Kernel panic|panic:|lkl-host: panic|FREED-ACTIVE|__kernel_restore bad ptr|/usr unavailable|Undefined symbol|No such file or directory|Bad file descriptor'

    if grep -Eq 'RTMux lkl-fsd-tree: FAIL' "$log"; then
        echo "fu740-fsd-smoke: board log contains lkl-fsd-tree FAIL marker" >&2
        exit 1
    fi
    if grep -Eq "$bad_re" "$log"; then
        echo "fu740-fsd-smoke: board log contains crash/fault signature" >&2
        exit 1
    fi
}

need_arg()
{
    opt=$1

    if [ "$#" -lt 2 ] || [ "${2#--}" != "$2" ]; then
        echo "fu740-fsd-smoke: $opt requires an argument" >&2
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
            echo "fu740-fsd-smoke: $opt must be a positive integer" >&2
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
            echo "fu740-fsd-smoke: unknown argument: $1" >&2
            usage >&2
            exit 2
            ;;
    esac
done

need_uint --timeout "$timeout"

if [ "$skip_preflight" -eq 0 ]; then
    CPUS=8 scripts/qemu-regress.sh --mode lkl-fsd-tree --timeout 360 \
        --keep-logs --differential
fi

if [ -z "$runner" ]; then
    echo "fu740-fsd-smoke: set FU740_RUNNER or pass --runner" >&2
    exit 2
fi

mkdir -p "$(dirname "$log")"
echo "fu740-fsd-smoke: running FU740 lkl-fsd-tree smoke"
echo "fu740-fsd-smoke: log $log"

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
    echo "fu740-fsd-smoke: board runner failed with status $rc" >&2
    exit "$rc"
fi

reject_bad_log
require 'lkl-fsd-board: .*FU740' 'FU740 board metadata'
if [ "$require_timing" -eq 1 ]; then
    require 'lkl-fsd-timer: .+' 'timer metadata'
fi
require 'RTMux lkl-fsd-tree: PASS' 'lkl-fsd-tree PASS marker'
awk '/lkl_block_read_bytes/ { if ($2 + 0 > 0) found = 1 } END { exit !found }' \
    "$log" || {
    echo "fu740-fsd-smoke: missing nonzero lkl_block_read_bytes" >&2
    exit 1
}
awk '/lkl_block_write_bytes/ { if ($2 + 0 > 0) found = 1 } END { exit !found }' \
    "$log" || {
    echo "fu740-fsd-smoke: missing nonzero lkl_block_write_bytes" >&2
    exit 1
}

echo "fu740-fsd-smoke: PASS"
