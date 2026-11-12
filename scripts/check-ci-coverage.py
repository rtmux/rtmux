#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Fail when a test or gate target in the Makefile is not run by any workflow.

Every `check-*` and `host-*` target, plus the standalone gates listed in
EXTRA, must be run by .github/ (named in a workflow or the setup action, or
reached from a named target through Makefile prerequisites and recursive
$(MAKE) calls), or carry an entry in WAIVERS saying why CI does not run it.  A
waiver for a target that is now covered, or that no longer exists, is itself an
error, so the list cannot rot.
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

# Standalone gates that do not follow the check-/host- naming.
EXTRA = {
    "driver-gate",
    "posix-profile-boot-probe",
    "lkl-profile-smoke",
    "qemu-regress",
    "tty-smoke",
    "verify",
}

# Dependency probes, not tests.
HELPERS = {
    "check-qemu-deps",
    "check-lkl-differential-deps",
    "check-style-deps",
}

# Targets CI deliberately does not run, with the reason.
WAIVERS = {
    "check-indent": "needs the project's clang-format and commentflow versions; enable once the formatted tree is committed",
    "host-sanitize": "host sanitizer build does not compile yet (kernel/xfer/length.c)",
    "host-fuzz": "libFuzzer campaign, run on demand rather than per push",
    "host-tools": "build step for the host tests, which CI runs through host-tests",
    "verify": "needs an opam Frama-C switch; see formal/framac/README.md",
    "check-trace-overhead": "compares timer-jitter runs and needs a quiet host; shared runners flake",
    "check-lkl-iommu-live": "needs QEMU with riscv-iommu-sys; the Ubuntu 24.04 runner ships 8.2",
}


def is_test_target(name):
    return name not in HELPERS and (name.startswith(("check-", "host-")) or name in EXTRA)


def makefile_graph():
    """Return {target: set(targets it runs)} from prerequisites and $(MAKE)."""
    graph = {}
    current = []
    for line in (ROOT / "Makefile").read_text(encoding="utf-8").splitlines():
        m = re.match(r"([A-Za-z0-9_.% -]+?)\s*:(?![=])(.*)", line)
        if m and not line.startswith("\t") and "=" not in line.split(":")[0]:
            current = [t for t in m.group(1).split() if "%" not in t]
            deps = re.sub(r"\$\([^)]*\)", " ", m.group(2).split("#")[0]).split()
            for t in current:
                graph.setdefault(t, set()).update(deps)
        elif line.startswith("\t") and current:
            if "$(MAKE)" in line:
                sub = re.findall(r"(?<![\w-])((?:check|host)-[\w-]+)", line)
                for t in current:
                    graph.setdefault(t, set()).update(sub)
        elif line.strip() == "":
            current = []
    return graph


def reachable(graph, roots):
    seen = set()
    stack = list(roots)
    while stack:
        name = stack.pop()
        if name in seen:
            continue
        seen.add(name)
        stack.extend(graph.get(name, ()))
    return seen


def workflow_text():
    """Workflow text minus comments and step names, which only describe."""
    parts = []
    for path in sorted((ROOT / ".github").rglob("*.yml")):
        for line in path.read_text(encoding="utf-8").splitlines():
            stripped = line.lstrip(" -")
            if stripped.startswith(("#", "name:")):
                continue
            parts.append(line)
    return "\n".join(parts)


def main():
    graph = makefile_graph()
    targets = {name for name in graph if is_test_target(name)}
    covered = reachable(graph, graph.keys() & set(re.findall(r"[\w-]+", workflow_text())))
    errors = []
    for name in sorted(targets):
        if name in WAIVERS:
            if name in covered:
                errors.append(f"{name}: waived but now run by CI; drop the waiver")
        elif name not in covered:
            errors.append(f"{name}: not run by any workflow (add it or waive it)")
    for name in sorted(WAIVERS):
        if name not in targets:
            errors.append(f"{name}: waiver for a target that no longer exists")
    if errors:
        for err in errors:
            print(f"check-ci-coverage: {err}", file=sys.stderr)
        return 1
    print(f"check-ci-coverage: ok ({len(targets)} targets, {len(WAIVERS)} waived)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
