#!/bin/sh
# Build and run the adopted Open POSIX Test Suite PSE51 subset.
#
# GPL test source stays out of tree.  This script compiles selected external
# tests into build/ and injects only the resulting binaries plus a generated
# runner into the initramfs for one QEMU boot.
#
# SPDX-License-Identifier: Apache-2.0
set -eu

ROOT=$(CDPATH= cd "$(dirname "$0")/../.." && pwd -P)
OPTS_DIR=${OPTS_DIR:-"$ROOT/externals/open-posix-test-suite"}
MANIFEST=${OPTS_PSE51_MANIFEST:-"$ROOT/scripts/opts/pse51-subset.tsv"}
OUT=${OPTS_OUT:-"$ROOT/build/opts-pse51"}
REPORT=${REPORT:-"$OUT/opts-pse51-summary.txt"}
LOG=${LOG:-"$ROOT/build/lkl-logs/opts-pse51-1.log"}
INITRAMFS_DIR=${INITRAMFS_DIR:-"$ROOT/build/initramfs"}
INITRAMFS_CPIO=${INITRAMFS_CPIO:-"$ROOT/build/initramfs.cpio"}

mkdir -p "$OUT"

unsupported()
{
    {
        echo "suite opts-pse51"
        echo "status UNSUPPORTED"
        echo "reason $*"
    } >"$REPORT"
    cat "$REPORT"
    exit 0
}

[ -d "$OPTS_DIR/conformance/interfaces" ] ||
    unsupported "Open POSIX Test Suite checkout not found: $OPTS_DIR"
[ -f "$OPTS_DIR/include/posixtest.h" ] ||
    unsupported "OPTS posixtest.h not found: $OPTS_DIR/include/posixtest.h"

: "${CC:?CC not set}"
: "${LD:?LD not set}"
: "${CPPFLAGS:?CPPFLAGS not set}"
: "${CFLAGS:?CFLAGS not set}"
: "${CRT0_O:?CRT0_O not set}"
: "${LIBC_SO:?LIBC_SO not set}"

rm -rf "$OUT/bin" "$OUT/obj"
mkdir -p "$OUT/bin" "$OUT/obj" "$INITRAMFS_DIR/bin/opts-pse51"

tests=
while read -r iface case src rest; do
    case "${iface:-}" in ""|\#*) continue ;; esac
    [ -n "${src:-}" ] || {
        echo "opts-pse51: malformed manifest row for $iface $case" >&2
        exit 2
    }
    source="$OPTS_DIR/$src"
    [ -f "$source" ] || {
        echo "opts-pse51: source missing: $src" >&2
        exit 2
    }
    name="${iface}_${case}"
    obj="$OUT/obj/$name.o"
    bin="$OUT/bin/$name"
    echo "  OPTSCC   $src"
    "$CC" $CPPFLAGS -I"$OPTS_DIR/include" $CFLAGS -c "$source" -o "$obj"
    echo "  OPTSLD   $name"
    $LD -nostdlib -o "$bin" "$CRT0_O" "$obj" "$LIBC_SO"
    if [ -n "${STRIP:-}" ]; then
        "$STRIP" --strip-unneeded "$bin" 2>/dev/null || true
    fi
    cp "$bin" "$INITRAMFS_DIR/bin/opts-pse51/$name"
    tests="$tests $name"
done <"$MANIFEST"

[ -n "$tests" ] || unsupported "no adopted OPTS PSE51 cases in $MANIFEST"

cat >"$INITRAMFS_DIR/bin/opts-pse51-run" <<EOF
#!/bin/sh
pass=0
fail=0
unresolved=0
unsupported=0
untested=0
interrupted=0
for t in$tests; do
    echo "OPTS PSE51 BEGIN \$t"
    /bin/opts-pse51/\$t
    rc=\$?
    case "\$rc" in
        0) status=PASS; pass=\$((pass + 1)) ;;
        1) status=FAIL; fail=\$((fail + 1)) ;;
        2) status=UNRESOLVED; unresolved=\$((unresolved + 1)) ;;
        4) status=UNSUPPORTED; unsupported=\$((unsupported + 1)) ;;
        5) status=UNTESTED; untested=\$((untested + 1)) ;;
        *) status=INTERRUPTED; interrupted=\$((interrupted + 1)) ;;
    esac
    echo "OPTS PSE51 RESULT \$t \$status rc=\$rc"
done
echo "OPTS PSE51 SUMMARY pass=\$pass fail=\$fail unresolved=\$unresolved unsupported=\$unsupported untested=\$untested interrupted=\$interrupted"
test "\$fail" = 0 && test "\$unresolved" = 0 && test "\$untested" = 0 && test "\$interrupted" = 0
EOF
chmod +x "$INITRAMFS_DIR/bin/opts-pse51-run"

tmp="$INITRAMFS_CPIO.tmp.$$"
(cd "$INITRAMFS_DIR" && find . | sort | cpio -H newc -o) >"$tmp" 2>/dev/null
mv "$tmp" "$INITRAMFS_CPIO"

scripts/qemu-regress.sh --mode opts-pse51 --timeout "${OPTS_PSE51_TIMEOUT:-180}" --keep-logs

pass=$(grep -c 'OPTS PSE51 RESULT .* PASS ' "$LOG" || true)
fail=$(grep -c 'OPTS PSE51 RESULT .* FAIL ' "$LOG" || true)
unresolved=$(grep -c 'OPTS PSE51 RESULT .* UNRESOLVED ' "$LOG" || true)
unsupported_count=$(grep -c 'OPTS PSE51 RESULT .* UNSUPPORTED ' "$LOG" || true)
untested=$(grep -c 'OPTS PSE51 RESULT .* UNTESTED ' "$LOG" || true)
interrupted=$(grep -c 'OPTS PSE51 RESULT .* INTERRUPTED ' "$LOG" || true)

{
    echo "suite opts-pse51"
    echo "manifest ${MANIFEST#$ROOT/}"
    echo "external ${OPTS_DIR#$ROOT/}"
    echo "log ${LOG#$ROOT/}"
    echo "passed $pass"
    echo "failed $fail"
    echo "unresolved $unresolved"
    echo "unsupported $unsupported_count"
    echo "untested $untested"
    echo "interrupted $interrupted"
    if [ "$fail" -eq 0 ] && [ "$unresolved" -eq 0 ] &&
       [ "$untested" -eq 0 ] && [ "$interrupted" -eq 0 ]; then
        echo "status PASS"
    else
        echo "status FAIL"
    fi
    grep 'OPTS PSE51 RESULT ' "$LOG" || true
} >"$REPORT"
cat "$REPORT"

grep -q '^status PASS$' "$REPORT"
