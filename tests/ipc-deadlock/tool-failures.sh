#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
# Check that the gate exposes tool failures instead of reporting model failures.
set -eu
root=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

expect_failure() {
    expected=$1
    shift
    # Mutations must not overwrite the real gate's certificate assertions.
    if env GATE_ASSERTIONS= "$@" >"$work/out" 2>&1; then
        echo "unexpected gate success: $expected" >&2
        exit 1
    fi
    if ! grep -F "$expected" "$work/out" >/dev/null; then
        cat "$work/out" >&2
        echo "missing diagnostic: $expected" >&2
        exit 1
    fi
}

mkdir "$work/bin"
cat >"$work/bin/spin" <<'EOF'
#!/bin/sh
echo injected-spin-failure >&2
exit 1
EOF
chmod +x "$work/bin/spin"
expect_failure injected-spin-failure env PATH="$work/bin:$PATH" "$root/scripts/check-ipc-deadlock.sh"
expect_failure 'pan compilation failed' env CC=false "$root/scripts/check-ipc-deadlock.sh"
cat >"$work/fake-cc" <<'EOF'
#!/bin/sh
cp "$PAN_STUB" pan
chmod +x pan
EOF
chmod +x "$work/fake-cc"
cat >"$work/pan-stub" <<'EOF'
#!/bin/sh
echo injected-pan-failure >&2
exit 1
EOF
expect_failure 'pan failed for client mask' env CC="$work/fake-cc" PAN_STUB="$work/pan-stub" "$root/scripts/check-ipc-deadlock.sh"
cat >"$work/pan-stub" <<'EOF'
#!/bin/sh
echo 'search not completed'
exit 0
EOF
expect_failure 'pan search incomplete for client mask' env CC="$work/fake-cc" PAN_STUB="$work/pan-stub" "$root/scripts/check-ipc-deadlock.sh"
printf '#!/bin/sh\necho no-count\n' >"$work/pan-stub"
expect_failure 'pan produced no error count' env CC="$work/fake-cc" PAN_STUB="$work/pan-stub" "$root/scripts/check-ipc-deadlock.sh"
echo 'ipc-deadlock tool failure diagnostics: ok'
