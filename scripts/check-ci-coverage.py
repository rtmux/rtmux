#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Fail when a test or gate target in the Makefile is not run by any workflow.

Every `check-*` and `host-*` target, plus the standalone gates listed in
EXTRA, must be run by .github/ (named in a workflow or the setup action, or
reached from a named target through Makefile prerequisites and recursive
$(MAKE) calls), or carry an entry in WAIVERS saying why CI does not run it.  A
waiver for a target that is now covered, or that no longer exists, is itself an
error, so the list cannot rot.

The same holds for every QEMU mode in scripts/qemu-modes.txt: a covered target
must run it (`--mode X` in its recipe, a mode list it loops over, or a script it
calls), or the line carries "# waived: <reason>".
"""
import re
import sys
from pathlib import Path

MODE_ARG = re.compile(r"--mode[ =]([a-z][a-z0-9-]*)")
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


def makefile_modes():
    """Return {mode: set(targets whose recipe runs it)}.

    A recipe runs a mode by `--mode X`, by referencing a `*_MODES` word list
    (words are `mode` or `mode:timeout`), directly or through a `define` macro it
    expands, or by calling a shell script under scripts/ that has a line running
    qemu-regress.sh with `--mode X` (comments and other file types do not count).
    """
    text = (ROOT / "Makefile").read_text(encoding="utf-8").replace("\\\n", " ")
    defs = {}

    def take_define(m):
        defs[m.group(1)] = m.group(2)
        return ""

    text = re.sub(r"^define (\S+)\n(.*?)^endef\n", take_define, text, flags=re.M | re.S)
    variables = {}
    for line in text.splitlines():
        m = re.match(r"([A-Za-z_][A-Za-z0-9_]*)\s*[:?+]?=\s*(.*)", line)
        if m and not line.startswith("\t"):
            variables[m.group(1)] = m.group(2).split("#")[0].split()

    def runs_qemu(line):
        return "qemu-regress" in line or "run-qemu-modes" in line

    def refs_modes(line, depth=0):
        """Modes named by the *_MODES lists a QEMU-running recipe line expands."""
        found = set()
        for name in re.findall(r"\$\(([A-Za-z_][\w-]*)\)", line):
            if name.endswith("_MODES") and runs_qemu(line):
                found.update(w.split(":")[0] for w in variables.get(name, ()))
            elif name in defs and depth < 3:
                found.update(refs_modes(defs[name], depth + 1))
        return found

    def strip_comments(text):
        return "\n".join(l for l in text.splitlines() if not l.lstrip().startswith("#"))

    modes = {}
    current = []
    for line in text.splitlines():
        m = re.match(r"([A-Za-z0-9_.% -]+?)\s*:(?![=])", line)
        if m and not line.startswith("\t") and "=" not in line.split(":")[0]:
            current = [t for t in m.group(1).split() if "%" not in t]
        elif line.startswith("\t") and current:
            if line.lstrip().startswith(("#", "\t#")) or re.match(r"\t\s*@?\s*(echo|printf)\b", line):
                continue
            found = set(MODE_ARG.findall(line))
            found |= refs_modes(line)
            for script in re.findall(r"scripts/([\w./-]+\.sh)", line):
                path = ROOT / "scripts" / script
                if path.exists() and script != "qemu-regress.sh":
                    body = strip_comments(path.read_text(encoding="utf-8")).replace("\\\n", " ")
                    found |= modes_in_calls(body.splitlines())
            for mode in found:
                modes.setdefault(mode, set()).update(current)
        elif line.strip() == "":
            current = []
    return modes


def modes_in_calls(lines):
    """--mode values on the lines that invoke qemu-regress."""
    return {m for line in lines if "qemu-regress" in line for m in MODE_ARG.findall(line)}


def registry():
    """Return {mode: waiver reason or None} from scripts/qemu-modes.txt."""
    result = {}
    for line in (ROOT / "scripts/qemu-modes.txt").read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        name, _, comment = line.partition("#")
        m = re.match(r"\s*waived:(.*)", comment)
        reason = m.group(1).strip() if m else None
        if reason == "":
            raise SystemExit(f"check-ci-coverage: mode {name.strip()}: empty waiver reason")
        result[name.strip()] = reason
    return result


def qemu_regress_modes():
    """Mode names qemu-regress.sh dispatches on: the arms of its `case "$MODE" in`
    blocks (nested case/esac pairs are skipped over). A `"$MODE" = "x"` test is not
    counted: it can survive, or sit in a comment, after the mode's arm is removed."""
    text = (ROOT / "scripts/qemu-regress.sh").read_text(encoding="utf-8")
    found = set()
    depth = 0
    for line in text.splitlines():
        if depth == 0:
            if re.match(r'\s*case "\$MODE" in\s*$', line):
                depth = 1
            continue
        if re.match(r"\s*case\b.*\bin\s*$", line):
            depth += 1
        elif re.match(r"\s*esac\b", line):
            depth -= 1
        elif depth == 1:
            arm = re.match(r"\s*([a-z][a-z0-9|*_-]*)\)", line)
            if arm:
                found.update(m for m in arm.group(1).split("|") if re.fullmatch(r"[a-z][a-z0-9-]*", m))
    return found


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
    """Workflow text minus comments (whole-line and trailing) and step names, which
    only describe."""
    parts = []
    for path in sorted((ROOT / ".github").rglob("*.yml")):
        for line in path.read_text(encoding="utf-8").splitlines():
            stripped = line.lstrip(" -")
            if stripped.startswith(("#", "name:")):
                continue
            parts.append(line.split(" #")[0])
    return "\n".join(parts)


def main():
    graph = makefile_graph()
    targets = {name for name in graph if is_test_target(name)}
    wf = workflow_text()
    covered = reachable(graph, graph.keys() & set(re.findall(r"[\w-]+", wf)))
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
    run_by = makefile_modes()
    from_workflows = modes_in_calls(wf.splitlines())
    reg = registry()
    for mode, reason in sorted(reg.items()):
        is_covered = bool(run_by.get(mode, set()) & covered) or mode in from_workflows
        if reason is not None and is_covered:
            errors.append(f"mode {mode}: waived but now run by CI; drop the waiver")
        elif reason is None and not is_covered:
            errors.append(f"mode {mode}: not run by any covered target (wire it or waive it in "
                          "scripts/qemu-modes.txt)")
    dispatched = qemu_regress_modes()
    for mode in sorted(dispatched - reg.keys()):
        errors.append(f"mode {mode}: handled by qemu-regress.sh but missing from "
                      "scripts/qemu-modes.txt")
    for mode in sorted(reg.keys() - dispatched):
        errors.append(f"mode {mode}: listed in scripts/qemu-modes.txt but qemu-regress.sh has "
                      "no case arm for it")
    if errors:
        for err in errors:
            print(f"check-ci-coverage: {err}", file=sys.stderr)
        return 1
    waived_modes = sum(1 for r in reg.values() if r is not None)
    print(f"check-ci-coverage: ok ({len(targets)} targets, {len(WAIVERS)} waived; "
          f"{len(reg)} modes, {waived_modes} waived)")
    return 0


def self_test():
    """Prove the mode checks on a synthetic tree: each case must give its verdict."""
    import contextlib
    import io
    import tempfile

    global ROOT, WAIVERS
    real_root, real_waivers = ROOT, WAIVERS
    WAIVERS = {}
    makefile = (
        "all: x\n"
        "check-run: check-qemu-deps\n\t./scripts/qemu-regress.sh --mode wired --timeout 1\n"
        "check-grp: check-qemu-deps\n\t$(call run-qemu-modes,$(GRP_MODES),1)\n"
        "GRP_MODES = looped other:60\n"
    )
    workflow = "jobs:\n  t:\n    steps:\n      - run: make check-run check-grp\n"
    qemu = ('case "$MODE" in\n    wired|looped|other|loose|waived-ok) ;;\nesac\n')

    def verdict(registry_text, make=makefile, script=qemu):
        with tempfile.TemporaryDirectory() as tmp:
            ROOT = Path(tmp)
            globals()["ROOT"] = ROOT
            (ROOT / "scripts").mkdir()
            (ROOT / ".github").mkdir()
            (ROOT / "Makefile").write_text(make)
            (ROOT / ".github/w.yml").write_text(workflow)
            (ROOT / "scripts/qemu-regress.sh").write_text(script)
            (ROOT / "scripts/qemu-modes.txt").write_text(registry_text)
            err = io.StringIO()
            try:
                with contextlib.redirect_stderr(err), contextlib.redirect_stdout(io.StringIO()):
                    rc = main()
            except SystemExit as exc:  # a malformed registry stops the check
                rc = 1 if exc.code else 0
                err.write(str(exc.code))
            return rc, err.getvalue()

    base = "wired\nlooped\nother\n"
    try:
        cases = [
            (base + "loose # waived: not run here\nwaived-ok # waived: reason\n", 0,
             "a wired mode, a looped mode and waived modes pass"),
            (base + "loose\nwaived-ok # waived: reason\n", 1, "an unwired mode fails"),
            ("wired # waived: but CI runs it\nlooped\nother\nloose # waived: r\n"
             "waived-ok # waived: r\n", 1, "a waiver on a covered mode fails"),
            (base + "loose # waived: r\n", 1, "a mode the script handles but the registry lacks fails"),
            (base + "loose # waived: r\nwaived-ok # waived: r\nghost # waived: r\n", 1,
             "a registry mode the script does not handle fails"),
        ]
        listed = base + "loose # waived: r\nwaived-ok # waived: r\n"
        only_echo = makefile.replace(
            "$(call run-qemu-modes,$(GRP_MODES),1)", "echo $(GRP_MODES)")
        commented = makefile.replace(
            "./scripts/qemu-regress.sh --mode wired --timeout 1",
            "# ./scripts/qemu-regress.sh --mode wired --timeout 1")
        stale_arm = qemu.replace("wired|looped|other|loose|waived-ok",
                                 "looped|other|loose|waived-ok") + "# wired is handled elsewhere\n"
        one_char = qemu.replace("wired|", "wired|x|")
        cmp_only = qemu.replace("wired|looped", "looped") + 'if [ "$MODE" = "wired" ]; then :; fi\n'
        cmp_commented = qemu.replace("wired|looped", "looped") + '# [ "$MODE" = "wired" ]\n'
        cases += [
            (listed, 1, "a mode list that is only echoed is not coverage", only_echo, qemu),
            (listed, 1, "a commented-out invocation is not coverage", commented, qemu),
            (listed, 1, "a removed dispatch arm is caught despite a comment naming the mode",
             makefile, stale_arm),
            (listed, 1, "a one-letter mode missing from the registry is caught",
             makefile, one_char),
            (listed, 1, "a comparison alone is not a dispatch arm", makefile, cmp_only),
            (listed, 1, "a commented-out comparison is not a dispatch arm", makefile, cmp_commented),
            (base + "loose # not waived: r\nwaived-ok # waived: r\n", 1,
             "a comment that only mentions waived: is not a waiver", makefile, qemu),
            (base + "loose # waived:\nwaived-ok # waived: r\n", 1, "an empty waiver reason fails",
             makefile, qemu),
        ]
        for case in cases:
            registry_text, want, what, *rest = case
            rc, err = verdict(registry_text, *rest) if rest else verdict(registry_text)
            if (rc != 0) != bool(want):
                print(f"check-ci-coverage: self-test failed: {what}\n{err}", file=sys.stderr)
                return 1
    finally:
        globals()["ROOT"] = real_root
        WAIVERS = real_waivers
    print("check-ci-coverage: self-test ok")
    return 0


if __name__ == "__main__":
    sys.exit(self_test() if sys.argv[1:] == ["--self-test"] else main())
