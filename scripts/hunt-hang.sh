#!/bin/sh
# Boot a regress mode in a loop with the hang post-mortem enabled, and stop at
# the first failure. Each failing loop leaves artifacts in its hunt directory
# and an identity file naming the commit, dirty state, and kernel hash.
#
# Usage: hunt-hang.sh [--loops N] [--mode NAME] [--timeout SEC] [--keep-going]
# CPUS and the other qemu-regress.sh environment variables pass through.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
LOOPS=10
MODE=syscall-testing
TIMEOUT=180
KEEP_GOING=0

while [ $# -gt 0 ]; do
    case "$1" in
        --loops) LOOPS=$2; shift 2 ;;
        --mode) MODE=$2; shift 2 ;;
        --timeout) TIMEOUT=$2; shift 2 ;;
        --keep-going) KEEP_GOING=1; shift ;;
        *) echo "hunt-hang: unknown option $1" >&2; exit 2 ;;
    esac
done

case "$MODE" in
    ''|*[!A-Za-z0-9_-]*) echo "hunt-hang: invalid mode: $MODE" >&2; exit 2 ;;
esac

mkdir -p "$ROOT/build/hang"
# Each hunt owns its artifacts; a later hunt must not inherit an old dump.
OUT=$(mktemp -d "$ROOT/build/hang/$MODE.XXXXXXXX")
echo "hunt-hang: artifacts in $OUT"
failed=0
i=1
while [ "$i" -le "$LOOPS" ]; do
    log_dir="$OUT/run-$i"
    mkdir -p "$log_dir"
    if QEMU_REGRESS_LOG_DIR="$log_dir" \
        "$ROOT/scripts/qemu-regress.sh" --mode "$MODE" --timeout "$TIMEOUT" \
        --gdb-on-timeout >"$OUT/$MODE-$i.run" 2>&1; then
        echo "hunt-hang: loop $i/$LOOPS ok"
        rm -f "$OUT/$MODE-$i.run"
        rm -rf "$log_dir"
    else
        failed=$((failed + 1))
        cp "$log_dir/$MODE-1.log" "$OUT/$MODE-$i.log" 2>/dev/null || true
        cp "$log_dir/$MODE-1.dump" "$OUT/$MODE-$i.dump" 2>/dev/null || true
        if ! cp "$log_dir/$MODE-1.identity" "$OUT/$MODE-$i.identity" 2>/dev/null; then
            echo 'identity: unavailable (emulator did not start)' >"$OUT/$MODE-$i.identity"
        fi
        echo "hunt-hang: loop $i/$LOOPS FAILED; artifacts in $OUT/$MODE-$i.* and $log_dir"
        [ "$KEEP_GOING" -eq 1 ] || exit 1
    fi
    i=$((i + 1))
done
[ "$failed" -eq 0 ] || exit 1
echo "hunt-hang: $LOOPS/$LOOPS ok"
