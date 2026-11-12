#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Run the Pi3 bootstrap in QEMU; timeout/partial banners are never success."""
import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import selectors
import shlex
import subprocess
import sys
import time

STAGES = (
    "boot-el1-uart", "rtmux-cpio-elf-aarch64", "mmu-cache-high-alias",
    "mmu-tlb-remap", "el1-unmapped-fault", "el1-readonly-fault",
    "timer-irq-5-ticks", "smp-4-cpus-el1", "mailbox-ipi-roundtrip",
    "el0-timer-preemption", "el0-svc-registers", "el0-memory-isolation",
)


class Transcript:
    def __init__(self, expect_failure=None):
        self.expect_failure = expect_failure
        self.stages = []
        self.complete = False

    def line(self, line):
        if self.complete and (line.startswith(("PASS ", "FAIL "))
                              or line == "PI3 BRINGUP PASS"):
            raise RuntimeError("evidence after terminal marker: " + line)
        if line.startswith("FAIL "):
            if (line == "FAIL " + str(self.expect_failure)
                    and self.stages == list(STAGES[:6])):
                self.complete = True
                return
            raise RuntimeError(line)
        if line.startswith("PASS "):
            stage = line[5:]
            if len(self.stages) >= len(STAGES) or stage != STAGES[len(self.stages)]:
                raise RuntimeError("unexpected or out-of-order stage: " + line)
            self.stages.append(stage)
        if line == "PI3 BRINGUP PASS":
            if self.expect_failure or self.stages != list(STAGES):
                raise RuntimeError("premature or unexpected success marker")
            self.complete = True


def run(args):
    image = args.image.resolve(strict=True)
    command = [args.qemu, "-M", "raspi3b", "-accel", "tcg", "-display", "none",
               "-monitor", "none", "-serial", "stdio", "-no-reboot",
               "-kernel", str(image)]
    version = subprocess.check_output([args.qemu, "--version"], text=True,
                                      timeout=5).splitlines()[0]
    print(shlex.join(command), flush=True)
    transcript = Transcript(args.expect_failure)
    output = bytearray()
    pending = b""
    started = time.monotonic()
    error = None
    proc = None

    def consume(chunk):
        nonlocal pending
        output.extend(chunk)
        if len(output) > 1024 * 1024:
            raise RuntimeError("excessive QEMU output")
        pending += chunk
        while b"\n" in pending:
            raw, pending = pending.split(b"\n", 1)
            line = raw.decode("utf-8", errors="replace").rstrip("\r")
            print(line, flush=True)
            transcript.line(line)

    try:
        proc = subprocess.Popen(command, stdin=subprocess.DEVNULL,
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        with selectors.DefaultSelector() as selector:
            selector.register(proc.stdout, selectors.EVENT_READ)
            while not transcript.complete:
                remaining = args.timeout - (time.monotonic() - started)
                if remaining <= 0:
                    raise RuntimeError("QEMU timeout without completed checks")
                if not selector.select(min(remaining, 0.2)):
                    continue
                chunk = os.read(proc.stdout.fileno(), 4096)
                if not chunk:
                    raise RuntimeError("QEMU exited before completed checks")
                consume(chunk)
            if proc.poll() not in (None, 0):
                raise RuntimeError("QEMU exited with an error")
    except (OSError, RuntimeError) as exc:
        error = str(exc)
    finally:
        if proc is not None:
            if proc.poll() is None:
                proc.terminate()
                try:
                    proc.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    proc.kill()
                    proc.wait(timeout=3)
            try:
                consume(proc.stdout.read())
            except RuntimeError as exc:
                error = error or str(exc)
            proc.stdout.close()
        args.log.parent.mkdir(parents=True, exist_ok=True)
        args.log.write_bytes(output)
        metadata = {
            "command": command, "qemu": version,
            "image_sha256": hashlib.sha256(image.read_bytes()).hexdigest(),
            "stages": transcript.stages,
            "expected_failure": args.expect_failure,
            "passed": error is None and transcript.complete,
            "error": error, "elapsed_seconds": time.monotonic() - started,
        }
        args.log.with_suffix(".json").write_text(
            json.dumps(metadata, indent=2) + "\n", encoding="utf-8")
    if error:
        print("check-pi3-bringup: " + error, file=sys.stderr)
        return 1
    print("check-pi3-bringup: " + (
        "expected timer failure detected" if args.expect_failure else "passed"))
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--qemu", default="qemu-system-aarch64")
    parser.add_argument("--image", type=Path, required=True)
    parser.add_argument("--log", type=Path, required=True)
    parser.add_argument("--timeout", type=float, default=20)
    parser.add_argument("--expect-failure", choices=["timer-irq-timeout"])
    args = parser.parse_args()
    if not math.isfinite(args.timeout) or args.timeout <= 0:
        parser.error("--timeout must be a finite positive number")
    try:
        return run(args)
    except (OSError, subprocess.SubprocessError) as exc:
        print(f"check-pi3-bringup: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
