#!/bin/sh
# Lay the zone files RTMux ships into a directory (the image's /usr/share/zoneinfo).
#
# usage: gen-zoneinfo.sh OUTDIR
#
# The files are compiled by zic from scripts/zoneinfo-2025b.zi, which holds the
# rules and zones of the pinned tzdata release 2025b for the zones listed there
# (the compact zic input format of tzdata.zi, trimmed to those zones and the
# rules they use; the tz database is in the public domain). The input is a
# fixed file, so every build ships the same transitions whatever tzdata the
# host has. The host needs only zic. If it is missing, or fails, so does this
# script: an image without zone files would silently turn TZ=Europe/Berlin into
# UTC.
#
# To move to another release, regenerate the input from that release's
# tzdata.zi, keeping the zones and links of the old one, and change the name.
#
#   ZIC   the compiler, default zic

zi=$(dirname "$0")/zoneinfo-2025b.zi
zic=${ZIC:-zic}
out=$1

[ -n "$out" ] || { echo "usage: $0 OUTDIR" >&2; exit 2; }
command -v "$zic" >/dev/null 2>&1 || { echo "gen-zoneinfo: no $zic" >&2; exit 1; }

tmp=$(mktemp -d) || exit 1
trap 'rm -rf "$tmp" "$tmp.err"' EXIT
# Old zic releases warn about the compact month and weekday abbreviations.
"$zic" -d "$tmp" "$zi" 2>"$tmp.err" || { cat "$tmp.err" >&2; exit 1; }

mkdir -p "$out" && cp -R "$tmp/." "$out/"
