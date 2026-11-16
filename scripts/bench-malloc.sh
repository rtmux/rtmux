#!/bin/sh
# Compare the allocator built with and without its counters.
#
# usage: bench-malloc.sh WITH_COUNTERS WITHOUT_COUNTERS
#
# Each argument is a malloc-stats test binary that takes "bench". The two are
# run alternately, ROUNDS times each (default 7), so a drift in host load hits
# both alike. Each run is already the best of several passes. The report gives
# the median of each build and the spread of its samples, (max - min) / median.
# The noise threshold is the larger of the two spreads: a difference of the
# medians at or below it is reported as within noise, not as a cost. Compare
# builds on a quiet host; a load that shifts mid-run widens the spread.
#
#   ROUNDS   samples per build

with=$1
without=$2
rounds=${ROUNDS:-7}

[ -x "$with" ] && [ -x "$without" ] || {
    echo "usage: $0 WITH_COUNTERS WITHOUT_COUNTERS" >&2
    exit 2
}

tmp=$(mktemp) || exit 1
trap 'rm -f "$tmp"' EXIT

i=0
while [ "$i" -lt "$rounds" ]; do
    for b in with without; do
        if [ "$b" = with ]; then bin=$with; else bin=$without; fi
        out=$("$bin" bench) || { echo "bench-malloc: $bin failed" >&2; exit 1; }
        printf '%s\n' "$out" | awk -v b="$b" 'NF >= 8 { print b, $2, $8 }' >>"$tmp"
    done
    i=$((i + 1))
done

[ "$(wc -l <"$tmp")" -eq $((2 * rounds)) ] || {
    echo "bench-malloc: missing samples" >&2
    exit 1
}

awk '
{ who[NR] = $1; small[NR] = $2; mapped[NR] = $3; cnt = NR }
function med_spread(name, v,    n, i, j, t, a, m)
{
    n = 0
    for (i = 1; i <= cnt; i++)
        if (who[i] == name)
            a[++n] = v[i]
    for (i = 2; i <= n; i++) {
        t = a[i]
        for (j = i - 1; j >= 1 && a[j] > t; j--)
            a[j + 1] = a[j]
        a[j + 1] = t
    }
    m = (n % 2) ? a[(n + 1) / 2] : (a[n / 2] + a[n / 2 + 1]) / 2
    MED = m
    SPREAD = (a[n] - a[1]) / m
}
function report(label, v,    mw, sw, mo, so, d, th)
{
    med_spread("with", v); mw = MED; sw = SPREAD
    med_spread("without", v); mo = MED; so = SPREAD
    d = (mw - mo) / mo
    th = sw > so ? sw : so
    printf "%s: with %.2f ns (spread %.1f%%), without %.2f ns (spread %.1f%%)\n",
           label, mw, sw * 100, mo, so * 100
    printf "  difference %+.1f%%, noise threshold %.1f%%: %s\n", d * 100,
           th * 100, (d < 0 ? -d : d) <= th ? "within noise" : (d > 0 ? "counters cost" : "counters faster")
}
END {
    report("small malloc+free pair", small)
    report("mapped pair", mapped)
}' "$tmp"
