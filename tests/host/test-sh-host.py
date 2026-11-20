#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Run the toolbox shell as a host program (see sh-host-shim.c) and check it.

    test-sh-host.py SH GAPS

SH is the shell built by tools/host, GAPS the tests/userland/sh-posix/gaps.sh
suite. Two checks:

  trap-read-race  a signal that arrives after a read has found nothing pending
                  and before it waits for input must still run its trap action
                  at once. The shell is built to send itself TERM at exactly
                  that point, and the test gives it no further input until the
                  action has run, so a read that lost the signal fails on the
                  deadline.
  gaps            the suite whose expected values dash made, run by the shell.
"""
import os
import select
import subprocess
import sys
import tempfile
import time

DEADLINE = 20


def trap_read_race(sh):
    env = dict(os.environ, SH_TEST_RAISE="1")
    p = subprocess.Popen([sh], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                         stderr=subprocess.DEVNULL, env=env)
    out = b""
    try:
        p.stdin.write(b"trap 'echo trapped' TERM\n")
        p.stdin.flush()
        end = time.monotonic() + DEADLINE
        while b"trapped" not in out and time.monotonic() < end:
            if select.select([p.stdout], [], [], 0.2)[0]:
                chunk = os.read(p.stdout.fileno(), 4096)
                if not chunk:
                    break
                out += chunk
        ok = b"trapped" in out
        p.stdin.write(b"exit\n")
        p.stdin.close()
    finally:
        try:
            p.wait(timeout=DEADLINE)
        except subprocess.TimeoutExpired:
            p.kill()
            p.wait()
    return ok


def gaps(sh, suite):
    with tempfile.TemporaryDirectory() as tmp:
        env = dict(os.environ, SH=sh, GAPS_DIR=tmp)
        r = subprocess.run([sh, suite], env=env, stdout=subprocess.PIPE,
                           stderr=subprocess.STDOUT, text=True, timeout=600)
    if r.returncode != 0:
        print("".join(l + "\n" for l in r.stdout.splitlines()
                      if not l.startswith("  ok")))
    return r.returncode == 0


def main():
    sh, suite = sys.argv[1:3]
    failed = []
    for name, ok in (("trap-read-race", lambda: trap_read_race(sh)),
                     ("gaps", lambda: gaps(sh, suite))):
        good = ok()
        print(f"sh-host {name}: {'PASS' if good else 'FAIL'}")
        if not good:
            failed.append(name)
    return 1 if failed else 0


sys.exit(main())
