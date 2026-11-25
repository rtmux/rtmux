#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# Check the run-ending log signatures of scripts/crash-signatures.sh: no
# strerror() message matches one, and each line a signature exists for does.

set -eu
top="$(cd "$(dirname "$0")/.." && pwd -P)"
. "$top/scripts/crash-signatures.sh"
table="$top/lib/libc/stdio/strerror.c"
fail=0

# Every message of the strerror() table, and the unknown-error format, inside
# the kind of line a test prints.
msgs="$(sed -n 's/^ *\[[A-Z0-9]*\] = "\(.*\)",$/\1/p' "$table")"
n="$(grep -c . <<<"$msgs")"
if [ "$n" -lt 30 ]; then
    echo "check-crash-signatures: only $n messages read from $table" >&2
    exit 1
fi
while IFS= read -r m; do
    if grep -Eq -- "$RTMUX_BAD_RE" <<<"  FAIL  open: $m"; then
        echo "check-crash-signatures: strerror text matches a signature: $m" >&2
        fail=1
    fi
done <<<"$msgs
Unknown error 999"

# The lines each signature exists for still match, each by exactly one
# alternative of RTMUX_CRASH_RE, and every alternative has a line of its own:
# dropping or breaking any one alternative fails here.
IFS='|' read -ra alts <<<"$RTMUX_CRASH_RE"
used=" " # alternatives with a fixture line, space-separated (Bash 3.2 has no maps)
while IFS= read -r line; do
    hits=()
    for a in "${alts[@]}"; do
        grep -Eq -- "$a" <<<"$line" && hits+=("$a")
    done
    if [ "${#hits[@]}" -ne 1 ]; then
        echo "check-crash-signatures: crash line matched by ${#hits[@]} signatures, want 1: $line" >&2
        fail=1
    fi
    for a in "${hits[@]}"; do used="$used$a "; done
done <<'EOF'
=== CRASH DUMP ===
FATAL: crash requested by debug-inject on hart 1
FATAL ksys_exit: S-mode thread with user tp
RT:CRASH servers/roottask/mem/vm-map.c:120 (roottask halting)
Kernel panic - not syncing: Attempted to kill init!
lkl-host: panic
panic: freeing an object that is still linked
kmem: FREED-ACTIVE object 0x80201000
__kernel_restore bad ptr! a0=0x1000 (expected kernel address)
EOF
for a in "${alts[@]}"; do
    case "$used" in
    *" $a "*) ;;
    *)
        echo "check-crash-signatures: no fixture line for signature: $a" >&2
        fail=1
        ;;
    esac
done
for line in '*** /usr unavailable — dropping to rescue shell ***' \
            'Error relocating /bin/x: Undefined symbol "foo"'; do
    if ! grep -Eq -- "$RTMUX_BAD_RE" <<<"$line"; then
        echo "check-crash-signatures: setup failure not matched: $line" >&2
        fail=1
    fi
done

# crash_signature_report names the first matching line.
log="$(mktemp)"
trap 'rm -f "$log"' EXIT
printf 'boot\nopen: Bad file descriptor\nFATAL: x\nRT:CRASH y\n' >"$log"
report="$(crash_signature_report "$log" "$RTMUX_BAD_RE")"
if ! grep -qx '  first signature, log line 3: FATAL: x' <<<"$report"; then
    echo "check-crash-signatures: report does not name line 3:" >&2
    echo "$report" >&2
    fail=1
fi

[ "$fail" -eq 0 ] && echo "check-crash-signatures: ok ($n strerror messages)"
exit "$fail"
