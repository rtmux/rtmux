#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
#
# A config change must rebuild the objects it affects, whatever the file times
# say (mk/build.mk, CONFIG_STAMP). In a scratch OUT, build one libc object that
# does not include autoconf.h (it sees CONFIG_* only as -D flags from
# mk/toolchain.mk), then require:
#   - nothing to do when nothing changed;
#   - a recompile when a CONFIG_* is given on the command line;
#   - a recompile again when the config goes back, since the OUT was last
#     built with the other one.
#
# Usage: scripts/check-config-rebuild.sh (needs a configured tree)
set -eu

ROOT="$(cd "$(dirname "$0")/.." && pwd -P)"
OBJ=lib/libc/strerror.o
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

fail() { echo "check-config-rebuild: FAIL: $*" >&2; exit 1; }

# The probe variable must reach make only where it is given on the command
# line: the config stamp records environment CONFIG_* values too, so one the
# caller exported would make the first build already carry it.
PROBE=CONFIG_RTMUX_CONFIG_REBUILD_TEST

# scratch_make ARGS...: make the object in the scratch OUT.
scratch_make() {
    env -u MAKEFLAGS -u "$PROBE" make -s -C "$ROOT/lib/libc" OUT="$tmp" "$tmp/$OBJ" "$@"
}

# plans [VAR=VALUE...]: how many compiles `make -n` plans for the object.
# A make that fails is a failure of the check, never a count of 0.
plans() {
    local out
    out=$(scratch_make -n "$@" 2>&1) ||
        fail "make -n${*:+ $*} failed:
$(tail -5 <<<"$out")"
    grep -c -- ' -c ' <<<"$out" || true
}

scratch_make >/dev/null || fail "cannot build $OBJ"
grep -q autoconf "$tmp/${OBJ%.o}.d" &&
    fail "$OBJ includes autoconf.h; pick an object that sees CONFIG_* only as flags"
n=$(plans) || exit 1
[ "$n" -eq 0 ] || fail "$OBJ is stale right after its build"
n=$(plans "$PROBE=y") || exit 1
[ "$n" -eq 1 ] || fail "a CONFIG_* on the command line does not rebuild $OBJ"
n=$(plans) || exit 1
[ "$n" -eq 1 ] ||
    fail "going back to the config does not rebuild $OBJ, last built with another"
echo "check-config-rebuild: ok"
