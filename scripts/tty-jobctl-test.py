#!/usr/bin/env python3
"""
Interactive job-control smoke: boots RTMux under qemu, logs in over the serial
console, and drives the shell with the terminal's control bytes.

  - ^C ends a foreground `sleep 100` and the prompt returns promptly
  - ^Z stops it ("Stopped"), `jobs` lists it, `fg` resumes it, ^C then ends it
  - with `stty tostop`, a background writer is stopped by SIGTTOU
  - the shell itself survives ^C, ^\\ and ^Z typed at an idle prompt

Build first, then run:  make -j && python3 scripts/tty-jobctl-test.py
Exit status is nonzero if any check fails.
"""
import importlib.util
import os
import sys
import time

TOP = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
spec = importlib.util.spec_from_file_location(
    "vi_test", os.path.join(TOP, "scripts", "vi-test.py"))
vi = importlib.util.module_from_spec(spec)
spec.loader.exec_module(vi)

PROMPT = b"# "


def say(q, data):
    """Send data, and only look at output that comes after it."""
    q.pos = q.mark()
    q.send(data)


def timed_prompt(q, secs):
    t0 = time.time()
    ok = q.expect(PROMPT, secs)
    return ok, time.time() - t0


def main():
    q = vi.Qemu()
    c = vi.Checks()
    try:
        if not vi.login(q, c):
            return 1

        print("== ^C ends a foreground job ==")
        say(q, "sleep 100\r")
        time.sleep(1.0)
        say(q, b"\x03")
        ok, dt = timed_prompt(q, 10)
        c.ok(ok and dt < 2.5, "^C returns to the prompt promptly (%.1fs)" % dt)

        print("== ^Z stops, jobs lists, fg resumes ==")
        say(q, "sleep 100\r")
        time.sleep(1.0)
        say(q, b"\x1a")
        c.ok(q.expect("Stopped", 10), "^Z reports the job Stopped")
        c.ok(q.expect(PROMPT, 10), "prompt after ^Z")
        say(q, "jobs\r")
        c.ok(q.expect("Stopped", 10), "jobs lists the stopped job")
        c.ok(q.expect(PROMPT, 10), "prompt after jobs")
        say(q, "fg\r")
        time.sleep(1.0)
        say(q, b"\x03")
        ok, dt = timed_prompt(q, 10)
        c.ok(ok, "fg resumed the job and ^C ended it")

        print("== the shell survives the terminal's signal keys ==")
        say(q, b"\x03\x1c\x1a")
        time.sleep(0.5)
        say(q, "echo alive-$((1+1))\r")
        c.ok(q.expect("alive-", 10), "shell still answers after ^C ^\\ ^Z")
        q.expect(PROMPT, 10)

        print("== TOSTOP: a background writer stops with SIGTTOU ==")
        say(q, "stty tostop\r")
        q.expect(PROMPT, 10)
        say(q, "cat /etc/passwd &\r")
        q.expect(PROMPT, 10)
        time.sleep(1.5)
        say(q, "jobs\r")
        c.ok(q.expect("Stopped", 10), "background cat is Stopped under tostop")
        q.expect(PROMPT, 10)
        say(q, "stty -tostop\r")
        q.expect(PROMPT, 10)
    finally:
        if os.environ.get("TTY_DEBUG"):
            print(q.slice(0).decode(errors="replace")[-200000:])
        q.close()
    print("passed %d, failed %d" % (c.passed, c.failed))
    return 1 if c.failed else 0


if __name__ == "__main__":
    sys.exit(main())
