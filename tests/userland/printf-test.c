/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * printf_test — exercise libc printf/snprintf for known formatting bugs.
 *
 * Targets:
 * - %#lx / %#x with ALT-flag prefix when followed by another conversion (the
 * second conversion's iter-top reset of "ox[1]" was clobbering pending iov
 * entries pointing at it, emitting NUL bytes mid-string).
 * - Negative %d when followed by another conversion (same class of bug with the
 * "sign" local).
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "test.h"

static void expect(const char *name, const char *got, const char *want)
{
    char detail[256];
    if (!strcmp(got, want)) {
        test_ok(name);
        return;
    }
    snprintf(detail, sizeof detail, "got=[%s] want=[%s]", got, want);
    test_fail(name, detail);
}

/* flockfile()/ftrylockfile()/funlockfile() recursion and the _unlocked
 * accessors. The lock is recursive, so a thread already holding the FILE must
 * be able to re-lock without deadlocking; ftrylockfile() must succeed on a free
 * stream and again while already owned. putc_unlocked/getc_unlocked move a byte
 * through a pipe to prove they are wired to the buffer machinery.
 */
static void test_flockfile(void)
{
    int fds[2];

    /* Balanced lock/unlock completes (no hang). */
    flockfile(stdout);
    funlockfile(stdout);
    test_ok("flockfile/funlockfile balanced");

    /* Recursive: lock three deep, unlock three deep. */
    flockfile(stdout);
    flockfile(stdout);
    flockfile(stdout);
    funlockfile(stdout);
    funlockfile(stdout);
    funlockfile(stdout);
    test_ok("flockfile recursive x3");

    /* ftrylockfile succeeds on a free stream, and again while already owned. */
    check(ftrylockfile(stdout) == 0, "ftrylockfile acquires free stream",
          "non-zero return");
    check(ftrylockfile(stdout) == 0, "ftrylockfile re-acquires owned stream",
          "non-zero return");
    funlockfile(stdout);
    funlockfile(stdout);

    /* After full release, ftrylockfile succeeds again. */
    check(ftrylockfile(stdout) == 0, "ftrylockfile after full release",
          "non-zero return");
    funlockfile(stdout);

    /* _unlocked accessors move a byte through a pipe. */
    if (pipe(fds) == 0) {
        FILE *w = fdopen(fds[1], "w");
        FILE *r = fdopen(fds[0], "r");

        if (w && r) {
            flockfile(w);
            putc_unlocked('Z', w);
            funlockfile(w);
            fflush(w);

            flockfile(r);
            int c = getc_unlocked(r);
            funlockfile(r);
            check(c == 'Z', "putc_unlocked/getc_unlocked pipe roundtrip",
                  "byte mismatch");
        } else {
            test_fail("flockfile pipe fdopen", "fdopen returned NULL");
        }
        if (w)
            fclose(w);
        if (r)
            fclose(r);
    } else {
        test_fail("flockfile pipe", "pipe() failed");
    }
}

/* getchar_unlocked()/putchar_unlocked() are the stdin/stdout-bound _unlocked
 * accessors (the pipe roundtrip above covers the arbitrary-FILE getc/putc
 * variants). Redirect stdout and stdin through a pipe -- saving and restoring
 * the real fds around each -- to prove each is wired to its standard stream.
 */
static void test_char_unlocked_stdio(void)
{
    int pfd[2];
    int saved;
    char c;

    /* putchar_unlocked -> stdout: capture the emitted byte through a pipe. */
    if (pipe(pfd) != 0) {
        test_fail("putchar_unlocked pipe", "pipe() failed");
    } else {
        fflush(stdout); /* drain any pending real output before the swap */
        saved = dup(STDOUT_FILENO);
        if (saved >= 0 && dup2(pfd[1], STDOUT_FILENO) >= 0) {
            flockfile(stdout);
            putchar_unlocked('Q');
            funlockfile(stdout);
            fflush(stdout);                    /* push the byte to the pipe */
            (void) dup2(saved, STDOUT_FILENO); /* restore before any check() */
        }
        (void) close(saved);
        (void) close(pfd[1]); /* drop the write end so read() cannot block */
        c = 0;
        if (read(pfd[0], &c, 1) != 1)
            c = 0;
        (void) close(pfd[0]);
        check(c == 'Q', "putchar_unlocked writes to stdout",
              "putchar_unlocked must emit through the stdout buffer");
    }

    /* getchar_unlocked -> stdin: feed a byte through a pipe. */
    if (pipe(pfd) != 0) {
        test_fail("getchar_unlocked pipe", "pipe() failed");
    } else {
        int got = EOF;

        if (write(pfd[1], "R", 1) == 1) {
            saved = dup(STDIN_FILENO);
            if (saved >= 0 && dup2(pfd[0], STDIN_FILENO) >= 0) {
                /* Unbuffered + purged: setvbuf resets stdin->_r to 0, so
                 * getchar_unlocked reads the redirected fd directly instead of
                 * any byte stdin may have buffered from earlier input.
                 */
                (void) setvbuf(stdin, NULL, _IONBF, 0);
                flockfile(stdin);
                got = getchar_unlocked();
                funlockfile(stdin);
                (void) dup2(saved, STDIN_FILENO);
            }
            (void) close(saved);
        } else {
            test_fail("getchar_unlocked seed", "pipe write failed");
        }
        (void) close(pfd[0]);
        (void) close(pfd[1]);
        check(got == 'R', "getchar_unlocked reads from stdin",
              "getchar_unlocked must read through the stdin buffer");
    }
}

/* The plain stdio ops now take the per-FILE lock internally. Round-trip the
 * newly-bracketed block ops (fputs/fgets, fwrite/fread) through a pipe to
 * confirm they still move data correctly with the lock in the path.
 */
static void test_stdio_locked_ops(void)
{
    int fds[2];
    FILE *w, *r;
    char line[32];
    char out[16] = "0123456789ABCDEF";
    char in[16];

    if (pipe(fds) != 0) {
        test_fail("stdio-locked pipe", "pipe() failed");
        return;
    }
    w = fdopen(fds[1], "w");
    r = fdopen(fds[0], "r");
    if (!w || !r) {
        test_fail("stdio-locked fdopen", "fdopen returned NULL");
        if (w)
            fclose(w);
        if (r)
            fclose(r);
        return;
    }

    fputs("locked line\n", w);
    fflush(w);
    check(fgets(line, sizeof line, r) != NULL &&
              !strcmp(line, "locked line\n"),
          "fputs/fgets roundtrip under lock", "line mismatch");

    check(fwrite(out, 1, sizeof out, w) == sizeof out,
          "fwrite returns full count under lock", "short write");
    fflush(w);
    check(fread(in, 1, sizeof in, r) == sizeof in &&
              memcmp(in, out, sizeof out) == 0,
          "fwrite/fread block roundtrip under lock", "block mismatch");

    /* fprintf now locks the stream too. */
    fprintf(w, "n=%d\n", 42);
    fflush(w);
    check(fgets(line, sizeof line, r) != NULL && !strcmp(line, "n=42\n"),
          "fprintf/fgets roundtrip under lock", "line mismatch");

    /* The flag accessors and fileno lock too. */
    check(fileno(w) >= 0 && fileno(r) >= 0,
          "fileno under lock returns valid fds", "bad fd");
    check(feof(r) == 0 && ferror(r) == 0,
          "feof/ferror under lock (mid-stream, no error)", "unexpected flag");
    clearerr(r);
    check(ferror(r) == 0, "clearerr under lock leaves ferror clear",
          "error flag set");

    fclose(w);
    fclose(r);
}

void test_printf(void)
{
    char buf[128];

    /* Single %#lx with no following conversion — was already correct. */
    snprintf(buf, sizeof buf, "%#lx", 0xe00000000UL);
    expect("single %#lx", buf, "0xe00000000");

    /* Two %#lx separated by literal — second one used to lose its 'x'. */
    snprintf(buf, sizeof buf, "%#lx/%#lx", 0xe00000000UL, 0x80000000UL);
    expect("two %#lx separated", buf, "0xe00000000/0x80000000");

    /* %#lx followed by %u — the %u iter resets ox[1] before flush. */
    snprintf(buf, sizeof buf, "DBI %#lx PLIC IRQ %u", 0x80000000UL, 56u);
    expect("%#lx then %u", buf, "DBI 0x80000000 PLIC IRQ 56");

    /* %#x with int arg, then trailing literal text. */
    snprintf(buf, sizeof buf, "X%#xY", 0x80000000u);
    expect("%#x then literal", buf, "X0x80000000Y");

    /* %#x followed by %u (the dropped-x case from /tmp/u740_5.log). */
    snprintf(buf, sizeof buf, "len=%#x irq=%u", 0x80000000u, 56u);
    expect("%#x then %u", buf, "len=0x80000000 irq=56");

    /* %X uppercase with same pattern. */
    snprintf(buf, sizeof buf, "v=%#X next=%d", 0xCAFEu, 7);
    expect("%#X then %d", buf, "v=0XCAFE next=7");

    /* %p followed by %u — pointer also uses the ox prefix. Default precision
     * for %p is 2*sizeof(void*) = 16 hex digits.
     */
    snprintf(buf, sizeof buf, "p=%p n=%u", (void *) 0xdeadbeefUL, 1u);
    expect("%p then %u", buf, "p=0x00000000deadbeef n=1");

    /* Negative %d followed by %d — sign-byte aliasing. */
    snprintf(buf, sizeof buf, "a=%d b=%d", -5, 100);
    expect("%d(neg) then %d", buf, "a=-5 b=100");

    /* Three conversions, varying types. */
    snprintf(buf, sizeof buf, "%#lx-%d-%s", 0xff00UL, -1, "end");
    expect("%#lx-%d-%s", buf, "0xff00--1-end");

    /* Width + ZEROPAD + ALT — must still get "0x" + zeroes + digits. */
    snprintf(buf, sizeof buf, "%#08x", 0xffu);
    expect("%#08x zeropad", buf, "0x0000ff");

    /* Width without ZEROPAD: spaces before the "0x" prefix. */
    snprintf(buf, sizeof buf, "[%8x]", 0xffu);
    expect("%8x space pad", buf, "[      ff]");

    /* Left-justify with ALT. */
    snprintf(buf, sizeof buf, "[%-#8x]", 0xffu);
    expect("%-#8x ladjust", buf, "[0xff    ]");

    /* Precision + ALT (zero pad between "0x" and digits). */
    snprintf(buf, sizeof buf, "%#.6x", 0xffu);
    expect("%#.6x precision", buf, "0x0000ff");

    test_flockfile();
    test_char_unlocked_stdio();
    test_stdio_locked_ops();
}
