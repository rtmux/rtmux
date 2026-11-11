#!/usr/bin/env python3
"""
Interactive TTY smoke: boots RTMux under qemu and drives login, the shell, and
the vi applet through expect-style serial I/O, asserting on the raw output byte
stream. This is the "sh plus vi over serial" terminal regression.

Login + shell:
  - login prompt, credential entry, shell prompt (root/RTMux)
  - login rejects a wrong password and an unknown user, gives up after the
    retry limit, and the terminal recovers to a usable shell
  - the interactive shell runs a command over serial

vi rendering regressions (userland/utils/vi.c, arch/riscv/calls.c):
  - no kernel heartbeat spinner scribbling on the console (\x1b[1;76H)
  - no DECSC/DECRC leak from the CPR probe (\x1b7 / \x1b8 -> literal '7'/'8')
  - terminal height honored end to end (full body draw, not capped at ~24 rows)
  - no per-refresh cursor hide/show churn (\x1b[?25l)
  - clean alt-screen enter/exit
  - '/' search moves the cursor to the match
  - insert mode edits and status flags

vi editing + failure handling:
  - backspace/DEL deletes a character in insert mode
  - saving to a read-only file fails cleanly (error status, no data loss, the
    editor stays responsive) rather than crashing

Build first (kernel + initramfs), then run:
    make -j && python3 scripts/vi-test.py     # or: make tty-smoke

Exit status is nonzero if any check fails.
"""
import os
import re
import subprocess
import sys
import threading
import time

TOP = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EMU = os.path.join(TOP, "scripts", "emu.sh")

# vi is told the terminal is this big via the CPR reply we inject.
ROWS, COLS = 40, 100
CPR = b"\x1b[%d;%dR" % (ROWS, COLS)

ESC_ALT_ENTER = b"\x1b[?1049h"
ESC_ALT_LEAVE = b"\x1b[?1049l"
ESC_PROBE = b"\x1b[6n"
ESC_SPINNER = b"\x1b[1;76H"   # kernel heartbeat corner
ESC_DECSC = b"\x1b7"          # DECSC (save cursor) — used to leak '7'
ESC_HIDE = b"\x1b[?25l"       # per-refresh cursor hide — removed
STATUS_MSG = b"toolbox vi"    # vi's startup status line — marks the first frame


class Qemu:
    def __init__(self):
        self.proc = subprocess.Popen(
            ["bash", EMU],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
        )
        self.buf = bytearray()
        self.pos = 0
        self.lock = threading.Lock()
        threading.Thread(target=self._reader, daemon=True).start()

    def _reader(self):
        while True:
            b = self.proc.stdout.read(1)
            if not b:
                break
            with self.lock:
                self.buf += b

    def expect(self, needle, timeout=30):
        """Wait until `needle` appears after the current cursor; advance past it."""
        if isinstance(needle, str):
            needle = needle.encode()
        end = time.time() + timeout
        while time.time() < end:
            with self.lock:
                idx = self.buf.find(needle, self.pos)
                if idx != -1:
                    self.pos = idx + len(needle)
                    return True
            time.sleep(0.03)
        return False

    def send(self, data):
        if isinstance(data, str):
            data = data.encode()
        self.proc.stdin.write(data)
        self.proc.stdin.flush()

    def mark(self):
        with self.lock:
            return len(self.buf)

    def slice(self, start, end=None):
        with self.lock:
            return bytes(self.buf[start:end])

    def close(self):
        try:
            self.proc.terminate()
            self.proc.wait(timeout=5)
        except Exception:
            self.proc.kill()


class Checks:
    def __init__(self):
        self.failed = 0
        self.passed = 0

    def ok(self, cond, msg):
        tag = "PASS" if cond else "FAIL"
        print(f"  [{tag}] {msg}")
        if cond:
            self.passed += 1
        else:
            self.failed += 1
        return cond


def cup_rows(data):
    """All row numbers targeted by CUP (ESC[<row>;<col>H) in order."""
    return [int(m.group(1)) for m in re.finditer(rb"\x1b\[(\d+);\d+H", data)]


def malformed_cups(data):
    """CSI cursor-position sequences that aren't a well-formed ESC[<r>;<c>H or
    ESC[H — evidence of a dropped byte mid-sequence (e.g. ESC[16;H)."""
    return [m.group(0) for m in re.finditer(rb"\x1b\[[0-9;]*H", data)
            if not re.fullmatch(rb"\x1b\[\d+;\d+H", m.group(0))
            and m.group(0) != b"\x1b[H"]


def login(q, c):
    print("== boot + login ==")
    if not c.ok(q.expect("login:", 90), "reached login prompt"):
        return False
    q.send("\n")
    q.expect("login:", 5)
    q.send("root\n")
    if not c.ok(q.expect("Password:", 20), "got password prompt"):
        return False
    q.send("RTMux\n")
    return c.ok(q.expect(b"# ", 20), "shell prompt after login")


def run_vi(q, cmd):
    """Launch vi via `cmd`, answer its CPR probe, wait for the first frame,
    and return the buffer mark at entry."""
    m = q.mark()
    q.send(cmd + "\n")
    if not q.expect(ESC_PROBE, 15):        # vi asked for the cursor position
        return None
    q.send(CPR)                            # act as the terminal and reply
    if not q.expect(STATUS_MSG, 10):       # first full frame must render
        return None
    return m


def quit_vi(q):
    q.send(":q!\r")
    return q.expect(ESC_ALT_LEAVE, 15)


def scenario_render(q, c):
    print("== render / height / hygiene (empty buffer, %dx%d) ==" % (ROWS, COLS))
    m = run_vi(q, "vi")
    if m is None:
        c.ok(False, "vi launched and probed")
        return
    c.ok(True, "vi launched and probed")
    if not c.ok(quit_vi(q), "vi exited cleanly (alt-screen leave)"):
        return
    data = q.slice(m, q.pos)
    rows = set(cup_rows(data))
    c.ok(ESC_ALT_ENTER in data, "entered alt-screen")
    c.ok(ESC_SPINNER not in data, "no kernel heartbeat spinner (ESC[1;76H)")
    c.ok(ESC_DECSC not in data, "no DECSC leak (ESC 7)")
    c.ok(ESC_HIDE not in data, "no per-refresh cursor hide (ESC[?25l)")
    # Full height: body rows go to ROWS-2, status bar at ROWS-1, message at ROWS.
    c.ok(rows.issuperset({ROWS - 2, ROWS - 1, ROWS}),
         "body fills to row %d and status at %d/%d" % (ROWS - 2, ROWS - 1, ROWS))
    # The probe parks the cursor at row 999 (ESC[999;999H) on purpose; nothing
    # else should be drawn past the real terminal height.
    over = [r for r in rows if ROWS < r < 999]
    c.ok(not over, "no draw past terminal height (stray rows: %s)" % over)


def scenario_search(q, c):
    print("== '/' search moves cursor to match ==")
    m = run_vi(q, "vi /etc/passwd")
    if m is None:
        c.ok(False, "vi opened /etc/passwd")
        return
    c.ok(True, "vi opened /etc/passwd")
    # The initial render is >255 bytes; a dropped byte at a console-chunk
    # boundary shows up as a malformed cursor-position sequence (e.g. ESC[16;H).
    render = q.slice(m, q.pos)
    bad = malformed_cups(render)
    c.ok(not bad, "no dropped bytes in render (malformed CUP: %s)" % bad[:3])
    s = q.mark()
    q.send("/default\r")                   # 'default' is on line 2 of passwd
    # After the match the status reports the new position as "<row>,<col>";
    # 'default' is on line 2, so wait for that status update as the sync point.
    found = q.expect(b" 2,", 8)
    c.ok(found, "status shows row 2 after search")
    if found:
        # expect() stops right after " 2,"; the column digits follow, so slice
        # to the current buffer end to capture the whole "<row>,<col>" field.
        seg = q.slice(s)
        col = re.search(rb" 2,(\d+) ", seg)
        c.ok(col is not None and int(col.group(1)) > 1,
             "cursor column advanced into the match (col %s)"
             % (col.group(1).decode() if col else "?"))
    quit_vi(q)


def scenario_help(q, c):
    print("== :help full-screen overlay ==")
    m = run_vi(q, "vi")
    if m is None:
        c.ok(False, "vi launched for help")
        return
    c.ok(True, "vi launched for help")
    s = q.mark()
    q.send(":help\r")
    # Sync on the LAST overlay line so the whole screen is in the buffer before
    # we inspect it (the flush arrives as a stream, top line first).
    shown = q.expect(b"Press any key to return", 8)
    c.ok(shown, "help overlay rendered")
    if shown:
        seg = q.slice(s)
        sections = [b"key reference", b"Movement", b"Insert", b"Edit",
                    b"Search", b"Files"]
        missing = [x.decode() for x in sections if x not in seg]
        c.ok(not missing, "help lists all sections (missing: %s)" % missing)
    q.send(" ")                            # any key dismisses the overlay
    c.ok(q.expect(b"[No Name]", 8), "editor repainted after dismiss")
    quit_vi(q)


def scenario_shell(q, c):
    print("== interactive shell over serial ==")
    # A typed `ls /` must execute and stream its output back: `usr` (the block-fs
    # mount point) appears in the listing but not in the typed command line, so
    # matching it proves the shell ran the command rather than the terminal merely
    # echoing input. The returning prompt confirms the shell is ready for more.
    # (Pipelines are not exercised here: the interactive login session runs no
    # /bin/pipe service, so `cmd | cmd` returns ENOSYS — that path is covered by
    # the rtmux.run=interactive-smoke mode, which starts the pipe service first.)
    q.send("ls /\r")
    c.ok(q.expect(b"usr", 8), "ls / lists the mounted /usr")
    c.ok(q.expect(b"# ", 5), "shell prompt returns, ready for the next command")


def scenario_login_failure(q, c):
    print("== login rejects bad credentials and recovers ==")
    # Drive the login applet from the shell through its rejection paths. The name
    # is read from stdin and read_password() reads /dev/tty — both are this serial
    # line, so the harness supplies them. CR line endings are accepted (the tty
    # maps CR->NL). Three failed attempts (LOGIN_MAX_TRIES) force login to give up
    # and return to the shell; each failure sleeps LOGIN_BACKOFF_SECONDS (3s), so
    # the expects below use generous timeouts.
    # Every expect is a checked sync point: on a miss, return early rather than
    # send the next credential into an unknown state (which could desync the
    # session and let the final "# " recovery pass spuriously).
    q.send("login\r")
    if not c.ok(q.expect(b"login: ", 10), "login prompts for a name"):
        return
    # Attempt 1: a real user with the wrong password.
    q.send("root\r")
    if not c.ok(q.expect(b"Password: ", 10), "login prompts for a password"):
        return
    q.send("wrongpw\r")
    if not c.ok(q.expect(b"Login incorrect", 12), "wrong password is rejected"):
        return
    # Attempt 2: an unknown user (getpwnam fails) is still rejected, not crashed,
    # and the prompt comes back — the terminal recovered mid-flow.
    if not c.ok(q.expect(b"login: ", 12), "login re-prompts after a failed attempt"):
        return
    q.send("ghostuser\r")
    if not c.ok(q.expect(b"Password: ", 10), "login prompts the unknown user"):
        return
    q.send("x\r")
    if not c.ok(q.expect(b"Login incorrect", 12), "unknown user is rejected"):
        return
    # Attempt 3: exhaust the retry budget so login gives up.
    if not c.ok(q.expect(b"login: ", 12), "login re-prompts for the final attempt"):
        return
    q.send("root\r")
    if not c.ok(q.expect(b"Password: ", 10), "login prompts for the final password"):
        return
    q.send("nope\r")
    c.ok(q.expect(b"Maximum number of tries exceeded", 12),
         "login gives up after the retry limit")
    c.ok(q.expect(b"# ", 10), "shell prompt returns — terminal recovered")


def scenario_edit(q, c):
    print("== insert + backspace editing ==")
    m = run_vi(q, "vi")
    if m is None:
        c.ok(False, "vi launched for edit")
        return
    c.ok(True, "vi launched for edit")
    q.send("iXY")                           # insert mode, type X then Y
    c.ok(q.expect(b"XY", 5), "typed XY before backspace")
    q.send("\x7f")                          # DEL/backspace removes the Y
    q.send("Z")                             # type Z -> buffer is "XZ", not "XYZ"
    got = q.expect(b"XZ", 5)
    # "XZ" as adjacent bytes can only render if backspace deleted Y; a no-op
    # backspace would leave "XYZ" and never produce the "XZ" pair. This relies on
    # vi repainting the edited line contiguously (full line redraw) so the two
    # surviving chars land adjacent in the output stream.
    c.ok(got, "backspace deleted a char (XY <bs> Z -> XZ)")
    q.send("\x1bll")                        # leave insert mode
    c.ok(quit_vi(q), "vi exited after edit")


def scenario_readonly_save(q, c):
    print("== clean failure saving to a read-only file ==")
    # /etc -> usr/conf on the read-only romfs, so :w must fail, not crash.
    m = run_vi(q, "vi /etc/passwd")
    if m is None:
        c.ok(False, "vi opened /etc/passwd")
        return
    c.ok(True, "vi opened /etc/passwd")
    q.send("iX\x1bll")                      # dirty the buffer so :w actually writes
    s = q.mark()
    q.send(":w\r")                          # attempt the save
    # vi_save()'s fopen(path,"w") fails on the read-only fs and sets the status to
    # "<path>: <errno string>"; a successful save would instead say "lines
    # written". Assert the failure is reported and success is not.
    c.ok(q.expect(b"passwd: ", 8),
         "save reports a read-only error in the status line")
    seg = q.slice(s)
    c.ok(b"lines written" not in seg, "no false 'lines written' on a failed save")
    # vi must still be alive and responsive after the failed write.
    c.ok(quit_vi(q), "vi still responsive after the failed save")


def body_reverse_video(data):
    """True if a reverse-video SGR (ESC[7m) is emitted inside a body row rather
    than the status bar. The status bar always emits ESC[7m after parking the
    cursor on the status row (ESC[<ROWS-1>;1H), so classify each ESC[7m by the
    row of its nearest preceding CUP: a body row (< ROWS-1) is a selection
    highlight in the text area, the status row is just the status bar."""
    for m in re.finditer(rb"\x1b\[7m", data):
        prev = None
        for cup in re.finditer(rb"\x1b\[(\d+);\d+H", data[: m.start()]):
            prev = cup
        if prev and int(prev.group(1)) < ROWS - 1:
            return True
    return False


def scenario_visual(q, c):
    print("== visual selection: v/V + motion + y/d + p ==")
    m = run_vi(q, "vi")
    if m is None:
        c.ok(False, "vi launched for visual")
        return
    c.ok(True, "vi launched for visual")
    # Build a known single line: "ABCDEF".
    q.send("iABCDEF\x1bll")
    if not c.ok(q.expect(b"ABCDEF", 5), "typed ABCDEF"):
        return
    # Charwise: from col 0 select the whole line with a count containing zero.
    q.send("0v")
    s = q.mark()
    q.send("10l")
    # Sync on a frame after the selection extended, then inspect that frame.
    c.ok(q.expect(b"VISUAL", 5), "VISUAL mode shown in status")
    seg = q.slice(s)
    c.ok(body_reverse_video(seg),
         "selection reverse-video highlight in body")
    q.send("y")                              # yank "ABCDEF", back to normal + col 0
    q.send("$p")                             # paste after last char
    c.ok(q.expect(b"ABCDEFABCDEF", 6),
         "charwise yank+paste round-trips the selected span")
    # Linewise: V selects the whole line, d cuts it (register holds the line),
    # p pastes it back.
    q.send("0Vdp")
    c.ok(q.expect(b"ABCDEFABCDEF", 6), "linewise cut+paste restores the line")
    q.send("\x1bll")
    c.ok(quit_vi(q), "vi exited after visual")


def scenario_insert(q, c):
    print("== insert mode edits + status flags ==")
    m = run_vi(q, "vi")
    if m is None:
        c.ok(False, "vi launched for insert")
        return
    c.ok(True, "vi launched for insert")
    s = q.mark()
    q.send("iHELLO")                       # enter insert, type HELLO
    typed = q.expect(b"HELLO", 5)          # sync on the fully-typed content
    seg = q.slice(s, q.pos)
    c.ok(typed and b"HELLO" in seg, "typed text rendered")
    c.ok(b"INSERT" in seg, "INSERT mode shown in status")
    c.ok(b"[+]" in seg, "dirty flag shown after edit")
    # Exit insert: vi_read_key consumes two bytes after ESC, so pad with two
    # harmless 'l' before the ex command lands in normal mode.
    q.send("\x1bll")
    c.ok(quit_vi(q), "vi exited after insert")


def main():
    if not os.path.exists(os.path.join(TOP, "build", "initramfs.cpio")):
        print("initramfs.cpio missing — run `make -j` first", file=sys.stderr)
        return 2
    q = Qemu()
    c = Checks()
    try:
        if not login(q, c):
            print("login failed; aborting", file=sys.stderr)
            return 1
        scenario_shell(q, c)
        scenario_render(q, c)
        scenario_search(q, c)
        scenario_help(q, c)
        scenario_insert(q, c)
        scenario_visual(q, c)
        scenario_edit(q, c)
        scenario_readonly_save(q, c)
        # Runs last: on a mid-flow failure it may leave the login applet waiting
        # for input, so keep it after the scenarios that need a clean shell.
        scenario_login_failure(q, c)
    finally:
        q.close()
    print("\n== summary: %d passed, %d failed ==" % (c.passed, c.failed))
    return 1 if c.failed else 0


if __name__ == "__main__":
    sys.exit(main())
