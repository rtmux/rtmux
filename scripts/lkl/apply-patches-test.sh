#!/bin/sh
set -eu

root=$(CDPATH= cd "$(dirname "$0")/../.." && pwd -P)
tmp=${TMPDIR:-/tmp}/rtmux-lkl-patch-test.$$
trap 'rm -rf "$tmp"' EXIT INT TERM HUP

mkdir -p "$tmp/src" "$tmp/patches" "$tmp/empty"
cd "$tmp/src"
git init -q
printf 'old\n' > a.txt
git add a.txt
git -c user.name=t -c user.email=t@example.invalid commit -q -m init

"$root/scripts/lkl/apply-patches.sh" "$tmp/src" "$tmp/empty"

cat > "$tmp/patches/0001-a.patch" <<'PATCH'
diff --git a/a.txt b/a.txt
index 3367afd..3e75765 100644
--- a/a.txt
+++ b/a.txt
@@ -1 +1 @@
-old
+new
PATCH

"$root/scripts/lkl/apply-patches.sh" "$tmp/src" "$tmp/patches"
grep -qx 'new' "$tmp/src/a.txt"
"$root/scripts/lkl/apply-patches.sh" "$tmp/src" "$tmp/patches"
grep -qx 'new' "$tmp/src/a.txt"

cat > "$tmp/patches/0002-bad.patch" <<'PATCH'
diff --git a/missing.txt b/missing.txt
index 3367afd..3e75765 100644
--- a/missing.txt
+++ b/missing.txt
@@ -1 +1 @@
-old
+new
PATCH

if "$root/scripts/lkl/apply-patches.sh" "$tmp/src" "$tmp/patches" >/dev/null 2>&1; then
    echo "apply-patches-test: bad patch passed" >&2
    exit 1
fi

echo "apply-patches-test: PASS"
