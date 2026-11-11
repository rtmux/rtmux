#!/bin/sh
# A termination signal must stop check-platform-boundary-contracts.sh with the
# conventional status and leave no temporary file behind.  The contracts file is
# a FIFO so the checker is parked mid-run while the signal arrives.
#
# The test itself holds the FIFO open read-write (fd 3) while the checker runs,
# so the checker's open never blocks and its read parks until fd 3 closes; this
# does not depend on whether the checker has reached the FIFO yet.  Every wait
# is bounded, and the harness proves it is sensitive: stub checkers that ignore
# the signal, exit with the wrong status or leak a temporary file must fail it.
#
# usage: check-platform-boundary-trap-test.sh
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
tmp=$(mktemp -d)
pid=
cleanup()
{
    exec 3>&- 2>/dev/null || true
    if [ -n "$pid" ]; then
        kill -s KILL "$pid" 2>/dev/null || true
    fi
    rm -rf "$tmp"
}
trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM
mkdir "$tmp/tmpdir"
mkfifo "$tmp/contracts"

# drive CHECKER SIGNAL WANT: returns 0 when CHECKER exits WANT after SIGNAL and
# leaves tmpdir empty; otherwise prints why and returns 1.  Never waits unboundedly.
drive()
{
    checker=$1 sig=$2 want=$3
    rm -rf "$tmp/tmpdir"; mkdir "$tmp/tmpdir"
    exec 3<>"$tmp/contracts"
    # A non-interactive shell starts background jobs with SIGINT ignored, and an
    # ignored signal cannot be trapped by the checker; restore the default
    # disposition in a small launcher.  fd 3 is closed in the child so only
    # this shell holds the FIFO's write end.
    TMPDIR=$tmp/tmpdir python3 -I -c '
import os, signal, sys
signal.signal(signal.SIGINT, signal.SIG_DFL)
os.execv(sys.argv[1], sys.argv[1:])
' "$checker" --contracts "$tmp/contracts" "$root/tests/platform-boundary/valid-claims.md" \
        >"$tmp/out" 2>&1 3>&- &
    pid=$!
    n=0
    while [ -z "$(ls "$tmp/tmpdir")" ]; do
        n=$((n + 1))
        if [ "$n" -gt 100 ]; then
            echo "trap-test: checker never started" >&2
            exec 3>&-; kill -s KILL "$pid" 2>/dev/null || true
            wait "$pid" 2>/dev/null || true; pid=
            return 1
        fi
        sleep 0.1
    done
    kill -s "$sig" "$pid"
    exec 3>&- # EOF for the parked reader; the pending trap then runs
    n=0
    while kill -0 "$pid" 2>/dev/null; do
        n=$((n + 1))
        if [ "$n" -gt 100 ]; then
            echo "trap-test: $sig did not stop the checker" >&2
            kill -s KILL "$pid" 2>/dev/null || true
            wait "$pid" 2>/dev/null || true; pid=
            return 1
        fi
        sleep 0.1
    done
    rc=0
    wait "$pid" || rc=$?
    pid=
    if [ "$rc" -ne "$want" ]; then
        echo "trap-test: $sig gave status $rc, want $want" >&2
        cat "$tmp/out" >&2
        return 1
    fi
    if [ -n "$(ls "$tmp/tmpdir")" ]; then
        echo "trap-test: $sig left a temporary file" >&2
        return 1
    fi
    return 0
}

for sig in TERM HUP INT; do
    case $sig in TERM) want=143 ;; HUP) want=129 ;; INT) want=130 ;; esac
    drive "$root/scripts/check-platform-boundary-contracts.sh" "$sig" "$want" || exit 1
done

# Sensitivity: each broken checker must make drive fail.
stub()
{
    printf '#!/bin/sh\n%s\n' "$2" >"$tmp/$1"
    chmod +x "$tmp/$1"
}
stub ignore 'f=$(mktemp); trap "" TERM; while :; do sleep 1; done'
stub badrc 'f=$(mktemp); trap "rm -f $f; exit 1" TERM; while :; do sleep 0.1; done'
stub leak 'f=$(mktemp); trap "exit 143" TERM; while :; do sleep 0.1; done'
for s in ignore badrc leak; do
    if drive "$tmp/$s" TERM 143 2>/dev/null; then
        echo "trap-test: broken checker '$s' passed the harness" >&2
        exit 1
    fi
done
echo "check-platform-boundary-trap-test: ok"
