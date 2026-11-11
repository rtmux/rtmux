#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
# Diagnostic only: reproduce two open teardown races in an abstract model.
set -eu

command -v spin >/dev/null 2>&1 || { echo 'channel-teardown-race: spin not found' >&2; exit 1; }
CC=${CC:-cc}
command -v "$CC" >/dev/null 2>&1 || { echo "channel-teardown-race: '$CC' not found" >&2; exit 1; }
root=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

run() { # name, expected assertion fragment (empty for control), options
    name=$1 expected=$2
    shift 2
    mkdir "$work/$name"
    cp "$root/tests/spin/channel-teardown-drop.pml" "$work/$name/m.pml"
    if ! (cd "$work/$name" && spin -a "$@" m.pml) >"$work/$name.spin" 2>&1; then
        cat "$work/$name.spin" >&2
        echo "channel-teardown-race: Spin failed for $name" >&2
        exit 1
    fi
    if ! (cd "$work/$name" && "$CC" -O2 -o pan pan.c) >"$work/$name.cc" 2>&1; then
        cat "$work/$name.cc" >&2
        echo "channel-teardown-race: compiler failed for $name" >&2
        exit 1
    fi
    if ! (cd "$work/$name" && ./pan -m10000 -c0) >"$work/$name.out" 2>&1; then
        cat "$work/$name.out" >&2
        echo "channel-teardown-race: pan failed for $name" >&2
        exit 1
    fi
    if grep -Ei 'search depth too small|out of memory' "$work/$name.out" >/dev/null ||
       { [ -z "$expected" ] && grep -i 'search not completed' "$work/$name.out" >/dev/null; }; then
        echo "channel-teardown-race: incomplete search for $name" >&2
        exit 1
    fi
    errors=$(sed -n 's/.*errors: \([0-9][0-9]*\).*/\1/p' "$work/$name.out" | head -n 1)
    states=$(sed -n 's/^ *\([0-9][0-9]*\) states, stored.*/\1/p' "$work/$name.out" | head -n 1)
    if [ -z "$states" ] || [ "$states" -eq 0 ]; then
        echo "channel-teardown-race: no states explored for $name" >&2
        exit 1
    fi
    if [ -n "$expected" ]; then
        if [ -z "$errors" ] || [ "$errors" = 0 ] ||
           ! grep -F "assertion violated $expected" "$work/$name.out" >/dev/null; then
            cat "$work/$name.out" >&2
            echo "channel-teardown-race: expected violation missing for $name" >&2
            exit 1
        fi
    elif [ "$errors" != 0 ]; then
        cat "$work/$name.out" >&2
        echo "channel-teardown-race: control failed for $name" >&2
        exit 1
    fi
}

run orphan '(((freed==1)||slot)||(links!=0))' -DWITH_C -DWITH_D
run serial '' -DWITH_C -DWITH_D -DSERIAL_CTRL
run double '(freed<=1)' -DWITH_D -DWITH_S -DCLIENT_GONE
run slot '' -DWITH_D -DWITH_S -DCLIENT_GONE -DSLOT_CTRL
run client '' -DWITH_C
run destroy '' -DWITH_D
run marker '' -DWITH_D -DCLIENT_GONE
echo 'channel-teardown-race: orphan and double-free counterexamples; abstract controls pass'
