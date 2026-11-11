#!/bin/sh
set -eu

budget_ns=${FU740_IRQOFF_BUDGET_NS:-}
log=build/fu740-logs/irqoff-budget.log
runner=${FU740_RUNNER:-}
skip_preflight=0
timeout=420

usage()
{
    cat <<'EOF'
usage: scripts/fu740-irqoff-budget.sh --budget-ns ns [--log path]
                                      [--runner command] [--skip-preflight]
                                      [--timeout seconds]

Runs the QEMU rt-check-instr preflight, then runs the supplied FU740
instrumented-board command. The command must boot the instrumented rt-check mode
and stream the RTMux serial log to stdout. The log must include:
  irqoff-board: ... FU740 ...
  irqoff-timer: <timer configuration>
EOF
}

require()
{
    pattern=$1
    name=$2

    grep -Eq "$pattern" "$log" || {
        echo "fu740-irqoff-budget: missing $name" >&2
        exit 1
    }
}

lock_class_id()
{
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
}

reject_bad_log()
{
    bad_re='FAULT|FATAL|CRASH|Kernel panic|panic:|FREED-ACTIVE|__kernel_restore bad ptr|/usr unavailable|Undefined symbol|No such file or directory|Bad file descriptor'

    if grep -Eq 'RTMux rt-check: FAIL' "$log"; then
        echo "fu740-irqoff-budget: board log contains rt-check FAIL marker" >&2
        exit 1
    fi
    if grep -Eq "$bad_re" "$log"; then
        echo "fu740-irqoff-budget: board log contains crash/fault signature" >&2
        exit 1
    fi
}

require_mempart_budget()
{
    class_id=$1

    awk -v cls="$class_id" -v budget="$budget_ns" '
        /^irqoff:/ {
            count = max = class = p99 = ""
            for (i = 1; i <= NF; i++) {
                if ($i ~ /^count=/) {
                    count = $i
                    sub(/^count=/, "", count)
                } else if ($i ~ /^max_ns=/) {
                    max = $i
                    sub(/^max_ns=/, "", max)
                } else if ($i ~ /^max_class=/) {
                    class = $i
                    sub(/^max_class=/, "", class)
                } else if ($i ~ /^p99_ns=/) {
                    p99 = $i
                    sub(/^p99_ns=/, "", p99)
                }
            }
            if (class == cls) {
                if (count !~ /^[0-9]+$/ || max !~ /^[0-9]+$/ ||
                    p99 !~ /^[0-9]+$/ || count + 0 == 0 ||
                    max + 0 == 0 || p99 + 0 == 0 || p99 + 0 > max + 0 ||
                    max + 0 > budget + 0)
                    bad = 1
                found = 1
            }
        }
        END { exit found && !bad ? 0 : 1 }
    ' "$log" || {
        echo "fu740-irqoff-budget: missing in-budget MEMPART irqoff line" >&2
        exit 1
    }
}

need_arg()
{
    opt=$1

    if [ "$#" -lt 2 ] || [ "${2#--}" != "$2" ]; then
        echo "fu740-irqoff-budget: $opt requires an argument" >&2
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
            echo "fu740-irqoff-budget: $opt must be a positive integer" >&2
            exit 2
            ;;
    esac
}

while [ "$#" -gt 0 ]; do
    case "$1" in
        --budget-ns)
            need_arg "$@"
            budget_ns=$2
            shift 2
            ;;
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
            echo "fu740-irqoff-budget: unknown argument: $1" >&2
            usage >&2
            exit 2
            ;;
    esac
done

need_uint --timeout "$timeout"

case "$budget_ns" in
    ''|*[!0-9]*|0)
        echo "fu740-irqoff-budget: pass --budget-ns or set FU740_IRQOFF_BUDGET_NS" >&2
        exit 2
        ;;
esac

if [ "$skip_preflight" -eq 0 ]; then
    CPUS=8 scripts/qemu-regress.sh --mode rt-check-instr --timeout 240 \
        --keep-logs
fi

if [ -z "$runner" ]; then
    echo "fu740-irqoff-budget: set FU740_RUNNER or pass --runner" >&2
    exit 2
fi

mempart_class=$(lock_class_id)
if [ -z "$mempart_class" ]; then
    echo "fu740-irqoff-budget: cannot resolve LOCK_CLASS_MEMPART" >&2
    exit 2
fi

mkdir -p "$(dirname "$log")"
echo "fu740-irqoff-budget: running FU740 irqoff budget capture"
echo "fu740-irqoff-budget: budget_ns $budget_ns"
echo "fu740-irqoff-budget: log $log"

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
    echo "fu740-irqoff-budget: board runner failed with status $rc" >&2
    exit "$rc"
fi

reject_bad_log
require 'RTMux rt-check: PASS' 'rt-check PASS marker'
require 'irqoff-board: .*FU740' 'FU740 board metadata'
require 'irqoff-timer: .+' 'timer metadata'
require_mempart_budget "$mempart_class"

echo "fu740-irqoff-budget: PASS"
