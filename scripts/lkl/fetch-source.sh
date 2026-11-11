#!/bin/sh
# Fetch one lkl/linux commit into DIR and apply scripts/lkl/patches.
# Usage: scripts/lkl/fetch-source.sh DIR COMMIT   Env: LKL_UPSTREAM overrides the repo.
# SPDX-License-Identifier: Apache-2.0
set -eu

dir=${1:?usage: fetch-source.sh DIR COMMIT}
commit=${2:?usage: fetch-source.sh DIR COMMIT}
upstream=${LKL_UPSTREAM:-https://github.com/lkl/linux.git}
here=$(CDPATH='' cd "$(dirname "$0")" && pwd -P)

git init -q "$dir"
git -C "$dir" fetch -q --depth=1 "$upstream" "$commit"
git -C "$dir" checkout -q FETCH_HEAD
"$here/apply-patches.sh" "$(CDPATH='' cd "$dir" && pwd -P)" "$here/patches"
