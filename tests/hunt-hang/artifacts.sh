#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
# Exercise artifact selection without booting QEMU or touching the real tree.
set -eu
root=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
mkdir -p "$work/scripts" "$work/build/lkl-logs"
cp "$root/scripts/hunt-hang.sh" "$work/scripts/hunt-hang.sh"
printf '%s\n' stale > "$work/build/lkl-logs/sample-1.dump"
cat > "$work/scripts/qemu-regress.sh" <<'EOF'
#!/bin/sh
root=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
count_file="$root/count${HUNT_TEST_TAG:-}"
count=0
test ! -f "$count_file" || count=$(cat "$count_file")
count=$((count + 1))
printf '%s\n' "$count" > "$count_file"
if [ "$count" -eq 3 ]; then
    exit 1  # Fail before the emulator creates a log or identity.
fi
printf 'kernel sha256: kernel-%s\n' "$count" > "$QEMU_REGRESS_LOG_DIR/sample-1.identity"
printf 'log %s%s\n' "$count" "${HUNT_TEST_TAG:-}" > "$QEMU_REGRESS_LOG_DIR/sample-1.log"
if [ "$count" -eq 1 ]; then
    printf '%s\n' fresh > "$QEMU_REGRESS_LOG_DIR/sample-1.dump"
    printf 'HANG: dumping harts to %s\n' "$QEMU_REGRESS_LOG_DIR/sample-1.dump"
fi
exit 1
EOF
chmod +x "$work/scripts/qemu-regress.sh"
if "$work/scripts/hunt-hang.sh" --mode sample --loops 3 --keep-going > "$work/output" 2>&1; then
    echo 'hunt unexpectedly succeeded' >&2
    exit 1
fi
set -- "$work"/build/hang/sample.*
test "$#" -eq 1 && test -d "$1"
test "$(cat "$1/sample-1.dump")" = fresh
test "$(cat "$1/sample-1.log")" = 'log 1'
test "$(cat "$1/sample-2.log")" = 'log 2'
test ! -e "$1/sample-2.dump"
test "$(cat "$1/sample-1.identity")" = 'kernel sha256: kernel-1'
test "$(cat "$1/sample-2.identity")" = 'kernel sha256: kernel-2'
test ! -e "$1/sample-3.log"
test ! -e "$1/sample-3.dump"
test "$(cat "$1/sample-3.identity")" = 'identity: unavailable (emulator did not start)'
# Two hunts of the same mode must never read each other's source artifacts.
HUNT_TEST_TAG=A "$work/scripts/hunt-hang.sh" --mode sample --loops 1 >"$work/a" 2>&1 &
a=$!
HUNT_TEST_TAG=B "$work/scripts/hunt-hang.sh" --mode sample --loops 1 >"$work/b" 2>&1 &
b=$!
if wait "$a"; then echo 'hunt A unexpectedly succeeded' >&2; exit 1; fi
if wait "$b"; then echo 'hunt B unexpectedly succeeded' >&2; exit 1; fi
dir_a=$(sed -n 's/^hunt-hang: artifacts in //p' "$work/a")
dir_b=$(sed -n 's/^hunt-hang: artifacts in //p' "$work/b")
test "$dir_a" != "$dir_b"
test "$(cat "$dir_a/sample-1.log")" = 'log 1A'
test "$(cat "$dir_b/sample-1.log")" = 'log 1B'
echo 'hunt-hang artifact selection: ok'
