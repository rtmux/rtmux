#!/bin/sh
# Hermetic test for resolve-ref.sh and fetch-source.sh against a local repo.
# SPDX-License-Identifier: Apache-2.0
set -eu

here=$(CDPATH='' cd "$(dirname "$0")" && pwd -P)
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

fail() {
    echo "lkl fetch-test: FAIL: $*" >&2
    exit 1
}

git init -q "$tmp/up"
git -C "$tmp/up" -c user.name=t -c user.email=t@example.com commit -q --allow-empty -m one
echo hello >"$tmp/up/file"
git -C "$tmp/up" add file
git -C "$tmp/up" -c user.name=t -c user.email=t@example.com commit -q -m two
git -C "$tmp/up" tag v1
head=$(git -C "$tmp/up" rev-parse HEAD)
LKL_UPSTREAM="file://$tmp/up"
export LKL_UPSTREAM

# A 40-hex commit passes through untouched, without contacting the remote.
sha=1111111111111111111111111111111111111111
test "$(LKL_UPSTREAM=file:///nonexistent "$here/resolve-ref.sh" "$sha")" = "$sha" ||
    fail "40-hex ref was not passed through"

# Default, tag and branch refs resolve to the upstream commit.
test "$("$here/resolve-ref.sh")" = "$head" || fail "default ref"
test "$("$here/resolve-ref.sh" v1)" = "$head" || fail "tag ref"
branch=$(git -C "$tmp/up" branch --show-current)
test "$("$here/resolve-ref.sh" "$branch")" = "$head" || fail "branch ref"

# An unknown ref is an error, not an empty commit.
if "$here/resolve-ref.sh" no-such-ref >/dev/null 2>&1; then
    fail "unknown ref resolved"
fi

# fetch-source checks out exactly that commit.
"$here/fetch-source.sh" "$tmp/src" "$head"
test "$(git -C "$tmp/src" rev-parse HEAD)" = "$head" || fail "wrong commit checked out"
test "$(cat "$tmp/src/file")" = hello || fail "file content missing"

echo "lkl fetch-test: ok"
