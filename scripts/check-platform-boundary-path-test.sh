#!/bin/sh
# Path validation in check-platform-boundary-contracts.sh is structural, and it
# must stay safe: a checkout root full of shell metacharacters is data, never a
# command, and a documented path that reaches outside the tree through a
# symbolic link does not count.  The checker lives in a copy of the tree whose
# root directory name carries `$(...)`, quotes, a space, a semicolon and a
# backslash (awk -v would interpret backslash escapes; the root reaches awk
# through ENVIRON).  A directory is not a documented file.
set -eu

src=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

root="$tmp/r o;ot \\n\\t\$(touch $tmp/PWNED) '\"x"
mkdir -p "$root/scripts" "$root/docs/adir" "$tmp/outside"
cp "$src/scripts/check-platform-boundary-contracts.sh" "$src/scripts/platform-field.ere" \
    "$root/scripts/"
echo run >"$root/docs/hw.md"
echo run >"$tmp/outside/hw.md"
ln -s "$tmp/outside" "$root/docs/dirlink"
ln -s "$tmp/outside/hw.md" "$root/docs/filelink.md"
chk="$root/scripts/check-platform-boundary-contracts.sh"

cat >"$tmp/claims.md" <<'MD'
platform: production PB-HW
MD
# contract EVIDENCE GATE: write a one-row contracts file
contract()
{
    printf '| PB-HW | Assumption. | production | %s | %s |\n' "$1" "$2" >"$tmp/contracts.md"
}

contract '`docs/hw.md` board run on the Unmatched' '`docs/hw.md` board procedure'
if ! "$chk" --contracts "$tmp/contracts.md" "$tmp/claims.md" >"$tmp/out" 2>&1; then
    echo "path-test: valid in-tree paths rejected under a hostile root" >&2
    cat "$tmp/out" >&2
    exit 1
fi
if [ -e "$tmp/PWNED" ]; then
    echo "path-test: the checkout root was executed as shell" >&2
    exit 1
fi

for case in \
    '`docs/dirlink/hw.md` board run on the Unmatched|`docs/hw.md` board procedure' \
    '`docs/filelink.md` board run on the Unmatched|`docs/hw.md` board procedure' \
    '`docs/hw.md` board run on the Unmatched|`docs/dirlink/hw.md` board procedure' \
    '`docs/hw.md` board run on the Unmatched|`docs/filelink.md` board procedure' \
    '`docs/adir` board run on the Unmatched|`docs/hw.md` board procedure' \
    '`docs/hw.md` board run on the Unmatched|`docs/adir` board procedure'
do
    contract "${case%%|*}" "${case#*|}"
    if "$chk" --contracts "$tmp/contracts.md" "$tmp/claims.md" >"$tmp/out" 2>&1; then
        echo "path-test: symlink escape accepted: $case" >&2
        exit 1
    fi
done
if [ -e "$tmp/PWNED" ]; then
    echo "path-test: the checkout root was executed as shell" >&2
    exit 1
fi

# emulator words inside a backticked path are not evidence prose; a version
# token is not a file; a missing path still fails beside a valid one
echo run >"$root/docs/qemu-notes.md"
ok_cases='`docs/qemu-notes.md` board run on the Unmatched|`docs/hw.md` board procedure
`docs/hw.md` board run on the Unmatched, fw `v0.42`|`docs/hw.md` board procedure at `v0.42`'
bad_cases='`docs/hw.md` `docs/nope.md` board run on the Unmatched|`docs/hw.md` board procedure
`docs/hw.md` board run on the Unmatched|`docs/hw.md` `docs/nope.md` board procedure
`docs/hw.md` `v0.42` `docs/nope.md` board run on the Unmatched|`docs/hw.md` board procedure
`docs/hw.md` board run on QEMU|`docs/hw.md` board procedure
`docs/hw.md` board run on the Unmatched, emulated|`docs/hw.md` board procedure'
nl='
'
IFS=$nl
for case in $ok_cases; do
    contract "${case%%|*}" "${case#*|}"
    if ! "$chk" --contracts "$tmp/contracts.md" "$tmp/claims.md" >"$tmp/out" 2>&1; then
        echo "path-test: valid evidence rejected: $case" >&2
        cat "$tmp/out" >&2
        exit 1
    fi
done
for case in $bad_cases; do
    contract "${case%%|*}" "${case#*|}"
    if "$chk" --contracts "$tmp/contracts.md" "$tmp/claims.md" >"$tmp/out" 2>&1; then
        echo "path-test: invalid evidence accepted: $case" >&2
        exit 1
    fi
done
unset IFS

# an empty contracts file is rejected explicitly
: >"$tmp/empty.md"
if "$chk" --contracts "$tmp/empty.md" "$tmp/claims.md" >"$tmp/out" 2>&1 ||
   ! grep -q 'empty or missing' "$tmp/out"; then
    echo "path-test: empty contracts file not rejected explicitly" >&2
    cat "$tmp/out" >&2
    exit 1
fi
echo "check-platform-boundary-path-test: ok"
