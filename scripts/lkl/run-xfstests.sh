#!/bin/sh
# Optional out-of-tree xfstests wrapper for LKL ext4 validation.
#
# No GPL test bits live in this tree.  Point XFS_TESTS_DIR at an xfstests
# checkout and provide TEST_DEV/SCRATCH_DEV loop/block devices when you want this
# wrapper to run a quick ext4 subset against Linux's own checker.
#
# SPDX-License-Identifier: Apache-2.0
set -eu

ROOT=$(CDPATH= cd "$(dirname "$0")/../.." && pwd -P)
OUT=${OUT:-"$ROOT/build/lkl-suite-reports"}
REPORT=${REPORT:-"$OUT/xfstests-summary.txt"}
GROUP=${GROUP:-quick}
EXCLUDE=${EXCLUDE:-"$ROOT/scripts/lkl/xfstests.exclude"}
XFS_TESTS_DIR=${XFS_TESTS_DIR:-}

mkdir -p "$OUT"

unsupported()
{
    {
        echo "suite xfstests"
        echo "status UNSUPPORTED"
        echo "reason $*"
    } >"$REPORT"
    cat "$REPORT"
    exit 0
}

if [ -z "$XFS_TESTS_DIR" ]; then
    for d in "$ROOT/externals/xfstests" "$HOME/xfstests" "$HOME/src/xfstests" \
             /usr/local/xfstests /opt/xfstests; do
        if [ -x "$d/check" ]; then
            XFS_TESTS_DIR=$d
            break
        fi
    done
fi

[ -n "$XFS_TESTS_DIR" ] || unsupported "xfstests checkout not found"
[ -x "$XFS_TESTS_DIR/check" ] || unsupported "xfstests check runner missing"
[ -n "${TEST_DEV:-}" ] || unsupported "TEST_DEV not set"
[ -n "${SCRATCH_DEV:-}" ] || unsupported "SCRATCH_DEV not set"

TMP=$(mktemp -d)
restore_config()
{
    if [ -f "$TMP/local.config.saved" ]; then
        cp "$TMP/local.config.saved" "$XFS_TESTS_DIR/local.config"
    else
        rm -f "$XFS_TESTS_DIR/local.config"
    fi
    rm -rf "$TMP"
}
trap restore_config EXIT
LOG="$OUT/xfstests.log"
TEST_DIR=${TEST_DIR:-"$TMP/test"}
SCRATCH_MNT=${SCRATCH_MNT:-"$TMP/scratch"}
mkdir -p "$TEST_DIR" "$SCRATCH_MNT"

cat >"$TMP/local.config" <<EOF
export FSTYP=ext4
export TEST_DEV=$TEST_DEV
export TEST_DIR=$TEST_DIR
export SCRATCH_DEV=$SCRATCH_DEV
export SCRATCH_MNT=$SCRATCH_MNT
EOF

set +e
(
    cd "$XFS_TESTS_DIR"
    [ ! -f local.config ] || cp local.config "$TMP/local.config.saved"
    cp "$TMP/local.config" local.config
    if [ -s "$EXCLUDE" ]; then
        ./check -g "$GROUP" -E "$EXCLUDE"
    else
        ./check -g "$GROUP"
    fi
) >"$LOG" 2>&1
rc=$?
set -e

notrun=$(grep -Ec '(^Not run:|^Not run )' "$LOG" || true)
failed=$(grep -E '^[a-z]+/[0-9]+[[:space:]]+.*(fail|FAILED)' "$LOG" || true)

{
    echo "suite xfstests"
    echo "group $GROUP"
    echo "log $LOG"
    echo "unsupported $notrun"
    if [ -n "$failed" ] || [ "$rc" -ne 0 ]; then
        echo "status FAIL"
        [ -z "$failed" ] || printf '%s\n' "$failed"
    else
        echo "status PASS"
    fi
} >"$REPORT"
cat "$REPORT"

[ "$rc" -eq 0 ] && [ -z "$failed" ]
