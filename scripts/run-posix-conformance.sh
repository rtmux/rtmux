#!/bin/sh
# Run the adopted out-of-tree POSIX conformance suite and write a JSON summary.
#
# The GPL/Open POSIX Test Suite checkout is never vendored.  This wrapper calls
# the existing PSE51 subset runner when the external tree is available; otherwise
# it records an UNSUPPORTED summary so CI can still prove the handoff contract.
#
# SPDX-License-Identifier: Apache-2.0
set -eu

ROOT=$(CDPATH= cd "$(dirname "$0")/.." && pwd -P)
SUMMARY=
REPORT=${REPORT:-"$ROOT/build/opts-pse51/opts-pse51-summary.txt"}
COMMAND="make check-opts-pse51"

usage()
{
    echo "usage: $0 --summary build/posix-summary.json" >&2
}

while [ "$#" -gt 0 ]; do
    case "$1" in
    --summary)
        [ "$#" -ge 2 ] || {
            usage
            exit 2
        }
        SUMMARY=$2
        shift 2
        ;;
    --help|-h)
        usage
        exit 0
        ;;
    *)
        usage
        exit 2
        ;;
    esac
done

[ -n "$SUMMARY" ] || {
    usage
    exit 2
}

case "$SUMMARY" in
/*) ;;
*) SUMMARY="$ROOT/$SUMMARY" ;;
esac

mkdir -p "$(dirname "$SUMMARY")" "$(dirname "$REPORT")"
rm -f "$REPORT"

status=PASS
if ! REPORT="$REPORT" make -C "$ROOT" check-opts-pse51; then
    status=FAIL
fi

python3 - "$ROOT" "$SUMMARY" "$REPORT" "$COMMAND" "$status" <<'PY'
import json
import sys
from pathlib import Path

root = Path(sys.argv[1])
summary = Path(sys.argv[2])
report = Path(sys.argv[3])
command = sys.argv[4]
fallback_status = sys.argv[5]

fields = {}
results = []
if report.is_file():
    for line in report.read_text(encoding="utf-8").splitlines():
        if not line:
            continue
        if line.startswith("OPTS PSE51 RESULT "):
            _, _, _, name, result, rc = line.split(maxsplit=5)
            results.append({"name": name, "status": result, "rc": rc[3:]})
            continue
        parts = line.split(maxsplit=1)
        if len(parts) == 2:
            fields[parts[0]] = parts[1]

def count(name: str) -> int:
    try:
        return int(fields.get(name, "0"))
    except ValueError:
        return 0

exclusions = []
exclusions_path = root / "scripts/opts/pse51-exclusions.tsv"
if exclusions_path.is_file():
    for raw in exclusions_path.read_text(encoding="utf-8").splitlines():
        if not raw or raw.startswith("#"):
            continue
        symbol, tag, reason = raw.split(maxsplit=2)
        exclusions.append({"symbol": symbol, "tag": tag, "reason": reason})

external = fields.get("external", "externals/open-posix-test-suite")
external_path = root / external
version = "absent"
if external_path.is_dir():
    import subprocess
    try:
        version = subprocess.check_output(
            ["git", "-C", str(external_path), "rev-parse", "--short", "HEAD"],
            text=True,
            stderr=subprocess.DEVNULL,
        ).strip()
    except (OSError, subprocess.CalledProcessError):
        version = "present"

data = {
    "suite": fields.get("suite", "opts-pse51"),
    "version": fields.get("version", version),
    "status": fields.get("status", fallback_status),
    "command": command,
    "manifest": fields.get("manifest", "scripts/opts/pse51-subset.tsv"),
    "external": external,
    "log": fields.get("log", "build/lkl-logs/opts-pse51-1.log"),
    "counts": {
        "passed": count("passed"),
        "failed": count("failed"),
        "unresolved": count("unresolved"),
        "unsupported": count("unsupported"),
        "untested": count("untested"),
        "interrupted": count("interrupted"),
    },
    "unsupported_profile_exclusions": exclusions,
    "results": results,
}
if "reason" in fields:
    data["reason"] = fields["reason"]

summary.write_text(json.dumps(data, indent=2, sort_keys=True) + "\n",
                   encoding="utf-8")
try:
    print(summary.relative_to(root))
except ValueError:
    print(summary)
PY

if grep -q '^status FAIL$' "$REPORT" 2>/dev/null; then
    exit 1
fi
[ "$status" = PASS ] || exit 1
exit 0
