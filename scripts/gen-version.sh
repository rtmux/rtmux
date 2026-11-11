#!/bin/sh
# Generate internal/include/rtmux-version.h.
#
# usage: gen-version.sh OUT VERSION MAJOR MINOR PATCH [BUILD_ID]
#
# The header is written to OUT.tmp and moved over OUT only when its content
# differs, so an unchanged build identity keeps OUT's mtime and does not
# retrigger compilation.  The build date is carried over from the previous
# file and refreshed only when something else changed.  BUILD_ID defaults to a
# 12-hex git short hash with a "-d" suffix on a dirty tree ("unknown" outside
# a git checkout); it is sanitised to [0-9A-Za-z._-] and at most 14 characters
# so it fits the syspage build_id[16] field.

out=$1 version=$2 major=$3 minor=$4 patch=$5 id=$6

if [ -z "$id" ]; then
    id=$(git rev-parse --short=12 HEAD 2>/dev/null) || id=unknown
    git update-index -q --refresh 2>/dev/null
    if [ "$id" != unknown ] && ! git diff-index --quiet HEAD -- 2>/dev/null; then
        id="$id-d"
    fi
fi
id=$(printf '%s' "$id" | tr -c 'A-Za-z0-9._-' '_' | cut -c1-14)

gen()
{
    printf '/* Automatically generated, do not edit. */\n'
    printf '#define RTMUX_VERSION_STRING "%s"\n' "$version"
    printf '#define RTMUX_VERSION_MAJOR %s\n' "$major"
    printf '#define RTMUX_VERSION_MINOR %s\n' "$minor"
    printf '#define RTMUX_VERSION_PATCH %s\n' "$patch"
    printf '#define RTMUX_VERSION \\\n'
    printf '    ((RTMUX_VERSION_MAJOR << 16) | (RTMUX_VERSION_MINOR << 8) | \\\n'
    printf '     RTMUX_VERSION_PATCH)\n'
    printf '#define RTMUX_BUILD_ID "%s"\n' "$id"
    printf '#define RTMUX_BUILD_DATE "%s"\n' "$1"
}

today=$(date +%Y-%m-%d)
old=$(sed -n 's/^#define RTMUX_BUILD_DATE "\(.*\)"$/\1/p' "$out" 2>/dev/null)
mkdir -p "$(dirname "$out")"
gen "${old:-$today}" >"$out.tmp"
if cmp -s "$out.tmp" "$out"; then
    rm -f "$out.tmp"
else
    gen "$today" >"$out.tmp" && mv "$out.tmp" "$out"
    echo "  GEN      $out (build id $id)"
fi
