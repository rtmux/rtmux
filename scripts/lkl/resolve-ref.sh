#!/bin/sh
# Resolve an lkl/linux ref (branch, tag, 40-hex commit; default HEAD) to a commit.
# Usage: scripts/lkl/resolve-ref.sh [REF]      Env: LKL_UPSTREAM overrides the repo.
# SPDX-License-Identifier: Apache-2.0
set -eu

upstream=${LKL_UPSTREAM:-https://github.com/lkl/linux.git}
ref=${1:-}

if printf '%s' "$ref" | grep -Eq '^[0-9a-f]{40}$'; then
    echo "$ref"
    exit 0
fi
commit=$(git ls-remote "$upstream" "${ref:-HEAD}" | awk 'NR==1 { print $1 }')
test -n "$commit" || {
    echo "resolve-ref: cannot resolve '${ref:-HEAD}' in $upstream" >&2
    exit 1
}
echo "$commit"
