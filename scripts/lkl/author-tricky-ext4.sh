#!/bin/bash
#
# Author an ext4 image with e2fsprogs only, so the reverse lkl-fsd readback
# test runs on Linux and macOS without loop mounts or sudo.  The staged tree
# still carries the shapes the guest asserts: a hardlink (nlink 2, shared
# inode), a sparse file (logical size >> allocated blocks), a directory with
# many entries, and non-trivial permission bits.  hello.txt is included so
# callers that reuse the lkl-fsd-tree chain still find it.  This is the reverse
# of the --differential FS oracle: there RTMux writes and host e2fsprogs checks;
# here host e2fsprogs writes and RTMux reads.
#
# Needs e2fsprogs mkfs.ext4.  Args: <out-image> <size-bytes>.
#
# SPDX-License-Identifier: Apache-2.0
set -eu

if [ "$#" -ne 2 ]; then
    echo "usage: author-tricky-ext4.sh <out-image> <size-bytes>" >&2
    exit 2
fi
out="$1"
size="$2"

mkfs="$(command -v mkfs.ext4 || true)"
if [ -z "$mkfs" ]; then
    for f in /opt/homebrew/opt/e2fsprogs/sbin/mkfs.ext4 \
             /usr/local/opt/e2fsprogs/sbin/mkfs.ext4; do
        if [ -x "$f" ]; then
            mkfs="$f"
            break
        fi
    done
    if [ -z "$mkfs" ]; then
        echo "author-tricky-ext4: mkfs.ext4 not found; install e2fsprogs" >&2
        exit 2
    fi
fi

root="$(mktemp -d /tmp/rtmux-tricky-root.XXXXXX)"
cleanup() {
    rm -rf "$root"
}
trap cleanup EXIT INT TERM

rm -f "$out"
truncate -s "$size" "$out"

printf 'hello from lkl-fsd\n' >"$root/hello.txt"
printf 'original\n' >"$root/orig.txt"
ln "$root/orig.txt" "$root/hardlink.txt"
truncate -s 1048576 "$root/sparse.bin"
printf X | dd of="$root/sparse.bin" bs=1 seek=1000000 conv=notrunc status=none
mkdir "$root/bigdir"
i=1
while [ "$i" -le 200 ]; do
    echo "$i" >"$root/bigdir/f$i"
    i=$((i + 1))
done
chmod 0741 "$root/orig.txt"

"$mkfs" -q -F -O metadata_csum -d "$root" "$out"

echo "author-tricky-ext4: wrote $out ($size bytes)"
