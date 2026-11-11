#!/usr/bin/env python3
"""Negative mutations for the preemption-checkpoint inventory gate.

Each case edits a scratch copy of the tree or inventory and requires
scripts/preempt-inventory.sh to fail with a specific message, so a gate that
silently accepts a bad classification is caught.
"""

import shutil
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def run(root, *args):
    return subprocess.run(["sh", "scripts/preempt-inventory.sh", *args], cwd=root,
                          text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)


def expect_failure(root, message, what):
    result = run(root)
    if result.returncode == 0 or message not in result.stdout:
        raise SystemExit(f"mutation not rejected ({what}); wanted {message!r}:\n{result.stdout}")


def main():
    with tempfile.TemporaryDirectory(prefix="rtmux-preempt-mutations-") as tmp:
        work = Path(tmp)
        for directory in ("kernel/ipc", "kernel/sync", "kernel/xfer", "kernel/thread",
                          "kernel/ext", "kernel/sched", "kernel/time", "scripts"):
            (work / directory).parent.mkdir(parents=True, exist_ok=True)
            shutil.copytree(ROOT / directory, work / directory, symlinks=True)
        baseline = run(work)
        if baseline.returncode:
            raise SystemExit("baseline inventory failed:\n" + baseline.stdout)

        inventory = work / "scripts/preempt-inventory.txt"
        original = inventory.read_text()

        def restore():
            inventory.write_text(original)

        def entries():
            return original.splitlines(keepends=True)

        def mutate_first(cls, needle, fn):
            """Rewrite the first inventory line of class `cls` containing `needle`."""
            lines = entries()
            i = next(n for n, line in enumerate(lines)
                     if line.startswith(cls + "\t") and needle in line)
            lines[i] = fn(lines[i])
            inventory.write_text("".join(lines))

        def set_reason(line, reason):
            return line.rstrip("\n").rsplit("\t", 1)[0] + "\t" + reason + "\n"

        # 1. A new unclassified loop is rejected.
        source = work / "kernel/ipc/q83-mutation.c"
        source.write_text("void q83_mutation(void)\n{\n    for (;;) {\n    }\n}\n")
        expect_failure(work, "unclassified long loop", "new for (;;) loop")
        source.unlink()

        # 2. Function-name parsing of int(xfer_memchk)(...) style definitions.
        source = work / "kernel/xfer/q83-mutation.c"
        source.write_text("int(q83_xfer)(void)\n{\n    for (;;) {\n    }\n}\n")
        expect_failure(work, "unclassified long loop", "parenthesized function name")
        dump = run(work, "dump").stdout
        if "\tq83_xfer\t" not in dump:
            raise SystemExit("parenthesized function name was not identified:\n" + dump)
        source.unlink()

        # 3. A multiline spin header is still a WAIT_PENDING spin: it must be
        # detected and may not be filed as a bounded class.
        source = work / "kernel/ipc/q83-mutation.c"
        source.write_text("void q83_spin(thread_t *thp)\n{\n    while (\n"
                          "        thp->internal_flags & RTMUX_ITF_WAIT_PENDING)\n"
                          "        ;\n}\n")
        expect_failure(work, "unclassified long loop", "multiline spin header")
        line = next(entry for entry in run(work, "dump").stdout.splitlines()
                    if "\tq83_spin\t" in entry)
        key = line.split("\t", 3)
        inventory.write_text(original + "bounded\t%s\t%s\t%s\tbound: none, a lie\n"
                             % (key[0], key[2], key[3]))
        expect_failure(work, "WAIT_PENDING/CAS spin must be peer_dependent", "multiline spin as bounded")
        restore()
        source.unlink()

        # 4. A WAIT_PENDING spin filed as any non-peer class is rejected.
        mutate_first("peer_dependent", "ITF_WAIT_PENDING",
                     lambda l: l.replace("peer_dependent\t", "bounded\t", 1))
        expect_failure(work, "WAIT_PENDING/CAS spin must be peer_dependent", "spin filed as bounded")
        restore()

        # 5. Reasons that call a lock context unaudited, or carry TODO, are rejected.
        for text in ("holds: unaudited; waits-for: x", "TODO"):
            mutate_first("peer_dependent", "ITF_WAIT_PENDING", lambda l: set_reason(l, text))
            expect_failure(work, "malformed inventory line", f"reason {text!r}")
            restore()

        # 6. peer_dependent must state what it waits for and what it holds.
        mutate_first("peer_dependent", "ITF_WAIT_PENDING",
                     lambda l: set_reason(l, "waits-for: a peer"))
        expect_failure(work, "peer_dependent needs waits-for: and holds:", "no holds:")
        restore()

        # 7. A generic irqoff entry without a named lock is rejected.
        mutate_first("irqoff", "", lambda l: set_reason(l, "window under a kernel spinlock"))
        expect_failure(work, "irqoff needs a named lock", "generic irqoff")
        restore()

        # 8. input_dependent must name the input and a tracked review.
        mutate_first("input_dependent", "", lambda l: set_reason(l, "input: iovec count"))
        expect_failure(work, "input_dependent needs input: and review: Q<n>", "no review")
        restore()

        # 9. bounded and checkpointed need a stated bound or checkpoint.
        mutate_first("bounded", "", lambda l: set_reason(l, "walks a finite kernel list"))
        expect_failure(work, "bounded needs bound: or cap:", "generic bounded")
        restore()
        mutate_first("checkpointed", "", lambda l: set_reason(l, "restarts when needed"))
        expect_failure(work, "checkpointed needs checkpoint:", "generic checkpointed")
        restore()

        # 10. The stated scope must equal the scanned scope.
        inventory.write_text(original.replace("# scope: kernel/ipc ", "# scope: ", 1))
        expect_failure(work, "scan scope differs from the stated scope", "scope drift")
        restore()

        # 11. A removed inventory entry (unclassified loop) and a stale entry are rejected.
        lines = entries()
        i = next(n for n, line in enumerate(lines) if line.startswith("irqoff\t"))
        inventory.write_text("".join(lines[:i] + lines[i + 1:]))
        expect_failure(work, "unclassified long loop", "dropped inventory entry")
        inventory.write_text(original + "bounded\tkernel/ipc/none.c\tnone\twhile (x)\tbound: n\n")
        expect_failure(work, "stale inventory", "stale entry")
        restore()

        if run(work).returncode:
            raise SystemExit("inventory did not return to passing after restore")
        print("preempt-inventory mutations: ok")


if __name__ == "__main__":
    main()
