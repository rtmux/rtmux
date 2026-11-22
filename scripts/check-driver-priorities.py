#!/usr/bin/env python3
"""Reject numeric scheduler and notification priorities in device services."""
from pathlib import Path
import re
import sys

# Comments and string literals are not code: blank them, keeping line
# positions, so a message that quotes an assignment is not one. A character
# constant, prefixed (L, u, U, u8) or not, is a number, so it becomes 0 and a
# priority given as 'A' is still reported; the newlines of a spliced one are
# kept. One alternation, so "//" inside a string and a quote inside a comment
# stay where they are.
comments = re.compile(r"/\*.*?\*/|//[^\n]*|\"(?:\\.|[^\"\\\n])*\"|"
                      r"(?<!\w)(?:u8|[LuU])?'(?:\\.|[^'\\\n])*'", re.S)
# A numeric priority operand: a literal behind any opening parentheses, casts
# and unary signs, so "(21)", "(int) 21" and "-1" are numbers too.
cast = r"\(\s*[A-Za-z_]\w*(?:\s+[A-Za-z_]\w*)*\s*\**\s*\)\s*"
literal = r"(?:0[xX][0-9A-Fa-f]+|[0-9]+)[uUlL]*\b"
operand = r"(?P<op>(?:" + cast + r"|\(\s*|[-+]\s*)*" + literal + ")"
sched = re.compile(r"\bsched_priority\s*=\s*" + operand)
# -1 selects the sender's priority in the notification APIs, and is not a
# scheduler priority: only there is it allowed.
notify = re.compile(r"(?:\bmsg_send_notification(?:_cap)?(?:_r)?\s*\([^,;]+,\s*|"
                    r"\bSIGEV_NOTIFICATION_INIT\s*\([^,;]+,[^,;]+,\s*)" + operand)


def sender_priority(op):
    return re.sub(cast + r"|[()\s]", "", op) == "-1"


def scan(source):
    source = comments.sub(lambda m: ("0" if m.group().endswith("'") else " ") +
                          "\n" * m.group().count("\n"), source)
    starts = [m.start() for m in sched.finditer(source)]
    starts += [m.start() for m in notify.finditer(source)
               if not sender_priority(m.group("op"))]
    return sorted(source.count("\n", 0, s) + 1 for s in starts)


def main():
    fixture = ("sp.sched_priority = 21;\n"
               "msg_send_notification(fd,\n 10, code, 0);\n"
               "SIGEV_NOTIFICATION_INIT(&ev, fd, 0x10, code, 0);\n"
               "msg_send_notification(fd, -1, code, 0);\n"
               "printf(\"sched_priority = 21\\n\"); /* \" */\n"
               "puts(\"// \"); sp.sched_priority = 22;\n"
               "c = '\"'; sp.sched_priority = 23; // \"\n"
               "msg_send_notification_r(fd, 10, code, 0);\n"
               "msg_send_notification_cap(cap, 0x10, code, 0);\n"
               "msg_send_notification_cap_r(cap, 10u, code, 0);\n"
               "msg_send_notification_cap_r(cap, -1, code, 0);\n"
               "msg_send_notification_r(fd, prio, code, 0);\n"
               "sp.sched_priority = (21);\n"
               "sp.sched_priority = (int) 21;\n"
               "sp.sched_priority = -1;\n"
               "sp.sched_priority = (BASE + 1);\n"
               "msg_send_notification(fd, (int) 10, code, 0);\n"
               "msg_send_notification(fd, (-1), code, 0);\n"
               "msg_send_notification(fd, -2, code, 0);\n"
               "sp.sched_priority = 'A';\n"
               "sp.sched_priority = L'A';\n"
               "sp.sched_priority = u8'A';\n"
               "c = '\\\n';\n"
               "sp.sched_priority = 24;\n")
    if scan(fixture) != [1, 2, 4, 7, 8, 9, 10, 11, 14, 15, 16, 18, 20, 21, 22,
                         23, 26]:
        sys.exit("priority fixture failed")
    root = Path(__file__).resolve().parents[1]
    dirs = [root / "servers", *sorted((root / "userland").glob("dev-*"))]
    paths = [Path(p) for p in sys.argv[1:]] or [
        p for d in dirs for p in d.rglob("*") if p.suffix in (".c", ".h")]
    bad = False
    for path in paths:
        for line in scan(path.read_text()):
            print(f"{path}:{line}: use a named priority", file=sys.stderr)
            bad = True
    return int(bad)


if __name__ == "__main__":
    sys.exit(main())
