#!/usr/bin/env python3
"""Write and verify per-gate provenance certificates (build/certs/<gate>.json).

  gate_cert.py write GATE --tool TOOL [--assertions-file FILE] [--result TEXT]
                          [--assertion TEXT ...] [--needs GATE ...]
  gate_cert.py verify [--allow-dirty] [--certs DIR]

A certificate records the tool and its version, the HEAD commit, whether the
tree was dirty, a UTC timestamp, the assertions the gate passed, and the
result.  The assertions come from the gate's own tool output: a gate script
appends one line per fact it observed to the file named by $GATE_ASSERTIONS
(see emit(), and scripts/gate-cert-lib.sh for shell), and `write` reads that
file, so the list cannot drift from the script.  A line "result: TEXT" sets
the result.  `--needs GATE` adds an assertion, and requires, that GATE's
certificate exists for the current HEAD (a sub-gate whose certificate is current).
`write` runs only after a gate has passed.  `verify` requires one certificate
for every gate in GATES whose commit equals the current HEAD and whose tree was
clean.  Certificates live under the gitignored build/certs/ (override with
$GATE_CERT_DIR or --certs); scripts/build-evidence-kit.sh regenerates them on a
clean tree and copies them into its output.
"""

import argparse
import json
import os
import subprocess
import sys
from datetime import datetime, timezone
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
CERTS = Path(os.environ.get("GATE_CERT_DIR") or ROOT / "build/certs")
GATES = ("check-assumptions", "check-evidence-claims", "check-formal-claims",
         "check-capability-law", "check-ipc-cbmc", "check-cap-smt",
         "check-srr-pi-smt", "check-temporal", "check-user-slice",
         "check-lifecycle-smt", "check-ipc-deadlock")
VERSION_CMD = {"python3": ["python3", "--version"], "z3": ["z3", "--version"],
               "cbmc": ["cbmc", "--version"], "spin": ["spin", "-V"]}
FIELDS = ("gate", "tool", "tool_version", "commit", "dirty", "timestamp_utc",
          "assertions", "result")


def out(argv):
    r = subprocess.run(argv, cwd=ROOT, text=True, capture_output=True)
    if r.returncode:
        raise RuntimeError(f"{' '.join(argv)}: {r.stderr.strip()}")
    return r.stdout.strip()


def tool_version(tool):
    if tool not in VERSION_CMD:
        raise RuntimeError(f"unknown tool '{tool}'")
    text = out(VERSION_CMD[tool])
    return text.splitlines()[0] if text else "unknown"


def tree_dirty():
    return bool(out(["git", "status", "--porcelain", "--", "."]))


def emit(text, result=False):
    """Record one observed fact (or the result) for the running gate."""
    path = os.environ.get("GATE_ASSERTIONS")
    if path:
        with open(path, "a") as f:
            f.write(("result: " if result else "") + text + "\n")


def read_assertions(path):
    assertions, result = [], None
    for line in Path(path).read_text().splitlines():
        if line.startswith("result: "):
            result = line[len("result: "):]
        elif line.strip():
            assertions.append(line)
    return assertions, result


def needs_assertion(gate, head):
    try:
        cert = json.loads((CERTS / f"{gate}.json").read_text())
    except (OSError, ValueError) as err:
        raise RuntimeError(f"needed gate {gate}: no certificate ({err})")
    if cert.get("commit") != head:
        raise RuntimeError(f"needed gate {gate}: certificate is not for HEAD")
    return f"sub-gate {gate} certificate is for HEAD ({cert.get('tool_version')}): {cert.get('result')}"


def write(gate, tool, result, assertions, needs=()):
    if gate not in GATES:
        raise RuntimeError(f"unknown gate '{gate}'")
    head = out(["git", "rev-parse", "HEAD"])
    assertions = list(assertions) + [needs_assertion(g, head) for g in needs]
    if not assertions:
        raise RuntimeError("a certificate needs at least one assertion")
    if not result:
        raise RuntimeError("a certificate needs a result")
    CERTS.mkdir(parents=True, exist_ok=True)
    cert = {
        "gate": gate,
        "tool": tool,
        "tool_version": tool_version(tool),
        "commit": head,
        "dirty": tree_dirty(),
        "timestamp_utc": datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
        "assertions": assertions,
        "result": result,
    }
    (CERTS / f"{gate}.json").write_text(json.dumps(cert, indent=2) + "\n")


def verify(allow_dirty):
    head = out(["git", "rev-parse", "HEAD"])
    errors = []
    for gate in GATES:
        try:
            cert = json.loads((CERTS / f"{gate}.json").read_text())
        except (OSError, ValueError) as err:
            errors.append(f"{gate}: no readable certificate ({err})")
            continue
        for key in FIELDS:
            if key not in cert:
                errors.append(f"{gate}: missing field '{key}'")
        if cert.get("gate") != gate:
            errors.append(f"{gate}: certificate names gate '{cert.get('gate')}'")
        asserts = cert.get("assertions")
        if (not isinstance(asserts, list) or not asserts
                or not all(isinstance(a, str) and a for a in asserts)):
            errors.append(f"{gate}: empty or malformed assertion list")
        if not cert.get("result"):
            errors.append(f"{gate}: empty result")
        if cert.get("commit") != head:
            errors.append(f"{gate}: commit {str(cert.get('commit'))[:12]} != "
                          f"HEAD {head[:12]}")
        if cert.get("dirty") is not False and not allow_dirty:
            errors.append(f"{gate}: certificate was produced on a dirty tree")
    return errors


def main(argv):
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    w = sub.add_parser("write")
    w.add_argument("gate")
    w.add_argument("--tool", required=True)
    w.add_argument("--result")
    w.add_argument("--assertion", action="append", default=[])
    w.add_argument("--assertions-file")
    w.add_argument("--needs", action="append", default=[])
    v = sub.add_parser("verify")
    v.add_argument("--allow-dirty", action="store_true")
    v.add_argument("--certs")
    args = ap.parse_args(argv)
    try:
        if args.cmd == "write":
            assertions, result = list(args.assertion), args.result
            if args.assertions_file:
                got, file_result = read_assertions(args.assertions_file)
                assertions += got
                result = result or file_result
            write(args.gate, args.tool, result, assertions, args.needs)
            return 0
        if args.certs:
            global CERTS
            CERTS = Path(args.certs)
        errors = verify(args.allow_dirty)
    except RuntimeError as err:
        print(f"gate_cert: {err}", file=sys.stderr)
        return 1
    for e in errors:
        print(f"gate_cert: {e}", file=sys.stderr)
    if not errors:
        print("gate_cert: ok")
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
