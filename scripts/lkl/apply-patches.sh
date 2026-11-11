#!/bin/sh
set -eu

src=${1:?usage: apply-patches.sh LKL_SRC [PATCH_DIR]}
patch_dir=${2:-scripts/lkl/patches}

test -d "$patch_dir" || exit 0
patch_dir=$(CDPATH= cd "$patch_dir" && pwd -P)

set -- "$patch_dir"/*.patch
test -e "$1" || exit 0

for p in "$@"; do
    if (cd "$src" && git apply --check "$p" >/dev/null 2>&1); then
        (cd "$src" && git apply "$p")
    elif (cd "$src" && git apply -R --check "$p" >/dev/null 2>&1); then
        echo "LKL patch already applied: $p"
    else
        echo "LKL patch failed: $p" >&2
        exit 1
    fi
done
