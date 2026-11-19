#!/bin/sh
# RTMux is freely redistributable under the Apache License. See the file
# "LICENSE" for information on usage and redistribution of this file.
#
# SPDX-License-Identifier: Apache-2.0
#
# Build the romfs image that /sbin/init mounts on /usr from mainfs=.
#
# Usage: mkusr.sh [-s <MiB>] [-a <src>:<dst>]... <mkromfs> <out.romfs>
#   -s  image size (default 16)
#   -a  also copy <src> to <dst>, relative to /usr
set -eu

usage()
{
    echo "usage: $0 [-s <MiB>] [-a <src>:<dst>]... <mkromfs> <out.romfs>" >&2
    exit 2
}

top=$(cd "$(dirname "$0")/.." && pwd)
root=$(mktemp -d)
trap 'rm -rf "$root"' EXIT

mkdir -p "$root/bin" "$root/sbin/sysinit" "$root/conf/sysinit" \
    "$root/home/rtmuxuser" "$root/share"
cp "$top/userland/utils/sysinit/level1.sh" "$root/sbin/sysinit/level1.sh"
chmod +x "$root/sbin/sysinit/level1.sh"
echo "RTMux /usr starter tree." > "$root/share/README"

size=16
while getopts s:a: opt; do
    case $opt in
    s) size=$OPTARG ;;
    a)
        src=${OPTARG%%:*}
        dst=${OPTARG#*:}
        [ "$src" != "$OPTARG" ] && [ -n "$dst" ] || usage
        mkdir -p "$root/$(dirname "$dst")"
        cp "$src" "$root/$dst"
        ;;
    *) usage ;;
    esac
done
shift $((OPTIND - 1))
[ $# -eq 2 ] || usage

cat > "$root/home/rtmuxuser/.profile" <<'EOF'
# rtmuxuser default login profile.
echo
echo "Welcome to RTMux!"
echo
EOF

# /usr/conf/{passwd,shadow,group} — credentials shipped with the dev image so
# toolbox login can authenticate against them. Hashes produced on the host
# with `mkpasswd -m sha-512 -S <salt> <pw>`, bit-compatible with RTMux libc's
# crypt(3) output.
#   root    -> RTMux
#   rtmuxuser -> rtmux
cat > "$root/conf/passwd" <<'EOF'
root:x:0:0:root:/root:/bin/sh
rtmuxuser:x:1000:1000:RTMux default user:/home/rtmuxuser:/bin/sh
EOF
cat > "$root/conf/shadow" <<'EOF'
root:$6$rtmuxtest1$Y9rQ.8S6PpteL5Hq1H2pCsyW5HcfMZB7bhQUG4wBa7rikBXOvQhY5gAUOBgC4Qs0AzS9nGxW2B8Mk0wxjQs7E.:::::::
rtmuxuser:$6$rtmuxtest2$v0STB0rbO2isO/Se.Beh5t2V7vxpLykb6moB5vN8hwqYUWVevhXsaAGIVuf4agv69sbgVHi1XnppWNUlnrlff.:::::::
EOF
cat > "$root/conf/group" <<'EOF'
root:x:0:
rtmuxuser:x:1000:
EOF
chmod 600 "$root/conf/shadow"

"$1" -s "$size" "$2" "$root" >/dev/null
