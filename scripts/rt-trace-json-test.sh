#!/bin/sh
# Self-contained check for the RT-benchmark Chrome-trace exporter: host-compile a
# driver that includes tests/userland/rt-latency/chrome-trace.h, emit spans, and
# confirm scripts/rt-trace-json.py accepts the real emitter output and rejects a
# malformed event.  Needs a host C compiler and python3; no qemu/target build.
#
# SPDX-License-Identifier: Apache-2.0
set -eu

here=$(CDPATH= cd "$(dirname "$0")" && pwd -P)
root=$(cd "$here/.." && pwd -P)
cc=${CC_HOST:-cc}
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

cat > "$tmp/driver.c" <<'EOF'
#include "chrome-trace.h"
int main(void)
{
    /* a sub-microsecond sample and a ~14.8 ms tail, like real lkl-measure output */
    chrome_emit_span("fs", chrome_tid("fs"), 2632738000ULL, 0ULL);
    chrome_emit_span("fs", chrome_tid("fs"), 2635011100ULL, 1037700ULL);
    chrome_emit_span("fs-rt", chrome_tid("fs-rt"), 2700000000ULL, 14799300ULL);
    return 0;
}
EOF

"$cc" -Wall -Wextra -I"$root/tests/userland/rt-latency" -o "$tmp/driver" "$tmp/driver.c"
"$tmp/driver" > "$tmp/guest.log"

# Distinct span classes must land on distinct Perfetto track ids.
if [ "$(grep -c '"tid"' "$tmp/guest.log")" -ne 3 ]; then
    echo "rt-trace-json-test: expected 3 emitted events" >&2
    exit 1
fi

python3 "$root/scripts/rt-trace-json.py" "$tmp/guest.log" -o "$tmp/trace.json"
python3 - "$tmp/trace.json" <<'EOF'
import json, sys
d = json.load(open(sys.argv[1]))
evs = d["traceEvents"]
assert len(evs) == 3, evs
assert {e["name"] for e in evs} == {"fs", "fs-rt"}, evs
assert len({e["tid"] for e in evs}) == 2, "fs and fs-rt must differ in tid"
assert all(e["ph"] == "X" for e in evs)
assert d["displayTimeUnit"] == "ns"
EOF

# Malformed events must each be rejected (nonzero exit): wrong phase, a negative
# duration, and a non-finite timestamp all fail the schema.
for bad in \
    '{"ph":"B","pid":1,"tid":1,"name":"x","ts":1.0,"dur":2.0}' \
    '{"ph":"X","pid":1,"tid":1,"name":"x","ts":1.0,"dur":-2.0}' \
    '{"ph":"X","pid":1,"tid":1,"name":"x","ts":NaN,"dur":2.0}'; do
    printf 'chrome-event: %s\n' "$bad" > "$tmp/bad.log"
    if python3 "$root/scripts/rt-trace-json.py" "$tmp/bad.log" >/dev/null 2>&1; then
        echo "rt-trace-json-test: validator accepted malformed event: $bad" >&2
        exit 1
    fi
done

echo "rt-trace-json-test: PASS"
