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
 * - A table of every flag, width, and precision with each integer, string,
 * character, and pointer conversion, against glibc's output; integer extremes;
 * and the snprintf return value on truncation.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <limits.h>
#include <fcntl.h>
#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/mallocstats.h>
#include <sys/resource.h>
#include <unistd.h>
#include "test.h"

/* Deny the direct mapping for a large buffer to exercise a real malloc
 * failure without exhausting the guest. Warm the FILE lock before measuring
 * since the pool keeps its lock after fclose.
 */
static void test_setvbuf_failure(unsigned deny_alloc)
{
    const char *name = deny_alloc ? "setvbuf allocation failure" :
                                    "setvbuf invalid arguments";
    int fds[2];
    struct __rtmux_mallocstats before, after;
    struct rlimit saved, limit;
    char output[3] = {0};
    int ok = 0;

    if (pipe(fds) != 0) {
        test_fail(name, "pipe failed");
        return;
    }
    FILE *fp = fdopen(fds[1], "w");
    if (!fp) {
        close(fds[1]);
        close(fds[0]);
        test_fail(name, "fdopen failed");
        return;
    }
    flockfile(fp);
    funlockfile(fp);
    __rtmux_mallocstats(&before);
    if (setvbuf(fp, NULL, _IOFBF, 64) == 0 && fputs("a", fp) >= 0) {
        if (!deny_alloc) {
            ok = setvbuf(fp, NULL, 99, 0) != 0 &&
                 setvbuf(fp, NULL, _IOFBF, (size_t) INT_MAX + 1) != 0 &&
                 setvbuf(fp, NULL, _IONBF, (size_t) INT_MAX + 1) == 0;
        } else if (getrlimit(RLIMIT_AS, &saved) == 0) {
            limit = saved;
            limit.rlim_cur = 0;
            if (setrlimit(RLIMIT_AS, &limit) == 0) {
                ok = setvbuf(fp, NULL, _IOLBF, 256 * 1024) != 0 &&
                     errno == ENOMEM && fp->_bf._base == fp->_nbuf;
                if (setrlimit(RLIMIT_AS, &saved) != 0)
                    ok = 0;
            }
        }
    }
    if (fputs("b", fp) < 0)
        ok = 0;
    if (fclose(fp) != 0)
        ok = 0;
    __rtmux_mallocstats(&after);
    if (read(fds[0], output, sizeof output) != 2 || strcmp(output, "ab"))
        ok = 0;
    close(fds[0]);
    check(ok && after.failed == before.failed + deny_alloc &&
              after.bytes_in_use == before.bytes_in_use &&
              after.allocs - before.allocs == after.frees - before.frees,
          name, "stream output or allocator counts differed");
}

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

/* The conversion table below was generated on the host: each expected string is
 * what glibc's snprintf produces for that format and argument, checked in as a
 * literal. Every conversion meets every flag, a width, and a precision; flag
 * and conversion pairs whose behavior C leaves undefined or unspecified (# on a
 * decimal, string, or pointer conversion; 0 on a string or character; a
 * precision on %c) are left out. Floating-point output is not covered here.
 *
 * To add a row, run the format with the same argument through snprintf on a
 * glibc host and paste the result; pf_d through pf_p are the cross product of
 * the flags "", "-", "+", " ", "#", "0", "-0", "+0", " 0", "#0", "-#", "+ ",
 * "-+", the widths "" and "6", and the precisions "", ".0", ".3".
 */
struct pf_row {
    const char *fmt;
    const char *want[2]; /* for the group's two arguments, in order */
};

struct pf_case {
    const char *fmt;
    char type;
    long long v;
    const char *s;
    const char *want;
};

/* One argument of a given kind, selected by type: 'i' int, 'u' unsigned int,
 * 'l' long long, 'U' unsigned long long, 'L' long, 'j' intmax_t, 't'
 * ptrdiff_t, 'z' size_t, 's' string, 'c' char (as int), 'p' pointer. Each
 * length modifier gets an argument of exactly its type.
 */
static int pf_format(char *out, size_t n, const char *fmt, char type, long long v, const char *s)
{
    switch (type) {
    case 'i':
    case 'c':
        return snprintf(out, n, fmt, (int) v);
    case 'u':
        return snprintf(out, n, fmt, (unsigned int) v);
    case 'l':
        return snprintf(out, n, fmt, v);
    case 'U':
        return snprintf(out, n, fmt, (unsigned long long) v);
    case 's':
        return snprintf(out, n, fmt, s);
    case 'p':
        return snprintf(out, n, fmt, (void *) (uintptr_t) v);
    case 'L':
        return snprintf(out, n, fmt, (long) v);
    case 'j':
        return snprintf(out, n, fmt, (intmax_t) v);
    case 't':
        return snprintf(out, n, fmt, (ptrdiff_t) v);
    case 'z':
        return snprintf(out, n, fmt, (size_t) v);
    }
    return -1;
}

/* Check one formatted result: the string and the return value, which must be
 * the full length. Return non-zero on a mismatch, after reporting it.
 */
static int pf_check(const char *fmt, char type, long long v, const char *s, const char *want)
{
    char got[128];
    char name[64];
    char detail[256];
    int ret = pf_format(got, sizeof got, fmt, type, v, s);

    if (ret == (int) strlen(want) && !strcmp(got, want))
        return 0;
    snprintf(name, sizeof name, "printf table \"%s\"", fmt);
    snprintf(detail, sizeof detail, "got=[%s] ret=%d want=[%s] ret=%d", got, ret, want,
             (int) strlen(want));
    test_fail(name, detail);
    return 1;
}

static const struct pf_row pf_d[] = {
    {"%d", {"0", "-42"}},
    {"%.0d", {"", "-42"}},
    {"%.3d", {"000", "-042"}},
    {"%6d", {"     0", "   -42"}},
    {"%6.0d", {"      ", "   -42"}},
    {"%6.3d", {"   000", "  -042"}},
    {"%-d", {"0", "-42"}},
    {"%-.0d", {"", "-42"}},
    {"%-.3d", {"000", "-042"}},
    {"%-6d", {"0     ", "-42   "}},
    {"%-6.0d", {"      ", "-42   "}},
    {"%-6.3d", {"000   ", "-042  "}},
    {"%+d", {"+0", "-42"}},
    {"%+.0d", {"+", "-42"}},
    {"%+.3d", {"+000", "-042"}},
    {"%+6d", {"    +0", "   -42"}},
    {"%+6.0d", {"     +", "   -42"}},
    {"%+6.3d", {"  +000", "  -042"}},
    {"% d", {" 0", "-42"}},
    {"% .0d", {" ", "-42"}},
    {"% .3d", {" 000", "-042"}},
    {"% 6d", {"     0", "   -42"}},
    {"% 6.0d", {"      ", "   -42"}},
    {"% 6.3d", {"   000", "  -042"}},
    {"%0d", {"0", "-42"}},
    {"%0.0d", {"", "-42"}},
    {"%0.3d", {"000", "-042"}},
    {"%06d", {"000000", "-00042"}},
    {"%06.0d", {"      ", "   -42"}},
    {"%06.3d", {"   000", "  -042"}},
    {"%-0d", {"0", "-42"}},
    {"%-0.0d", {"", "-42"}},
    {"%-0.3d", {"000", "-042"}},
    {"%-06d", {"0     ", "-42   "}},
    {"%-06.0d", {"      ", "-42   "}},
    {"%-06.3d", {"000   ", "-042  "}},
    {"%+0d", {"+0", "-42"}},
    {"%+0.0d", {"+", "-42"}},
    {"%+0.3d", {"+000", "-042"}},
    {"%+06d", {"+00000", "-00042"}},
    {"%+06.0d", {"     +", "   -42"}},
    {"%+06.3d", {"  +000", "  -042"}},
    {"% 0d", {" 0", "-42"}},
    {"% 0.0d", {" ", "-42"}},
    {"% 0.3d", {" 000", "-042"}},
    {"% 06d", {" 00000", "-00042"}},
    {"% 06.0d", {"      ", "   -42"}},
    {"% 06.3d", {"   000", "  -042"}},
    {"%+ d", {"+0", "-42"}},
    {"%+ .0d", {"+", "-42"}},
    {"%+ .3d", {"+000", "-042"}},
    {"%+ 6d", {"    +0", "   -42"}},
    {"%+ 6.0d", {"     +", "   -42"}},
    {"%+ 6.3d", {"  +000", "  -042"}},
    {"%-+d", {"+0", "-42"}},
    {"%-+.0d", {"+", "-42"}},
    {"%-+.3d", {"+000", "-042"}},
    {"%-+6d", {"+0    ", "-42   "}},
    {"%-+6.0d", {"+     ", "-42   "}},
    {"%-+6.3d", {"+000  ", "-042  "}},
};
static const struct pf_row pf_i[] = {
    {"%i", {"7", "-1234"}},
    {"%.0i", {"7", "-1234"}},
    {"%.3i", {"007", "-1234"}},
    {"%6i", {"     7", " -1234"}},
    {"%6.0i", {"     7", " -1234"}},
    {"%6.3i", {"   007", " -1234"}},
    {"%-i", {"7", "-1234"}},
    {"%-.0i", {"7", "-1234"}},
    {"%-.3i", {"007", "-1234"}},
    {"%-6i", {"7     ", "-1234 "}},
    {"%-6.0i", {"7     ", "-1234 "}},
    {"%-6.3i", {"007   ", "-1234 "}},
    {"%+i", {"+7", "-1234"}},
    {"%+.0i", {"+7", "-1234"}},
    {"%+.3i", {"+007", "-1234"}},
    {"%+6i", {"    +7", " -1234"}},
    {"%+6.0i", {"    +7", " -1234"}},
    {"%+6.3i", {"  +007", " -1234"}},
    {"% i", {" 7", "-1234"}},
    {"% .0i", {" 7", "-1234"}},
    {"% .3i", {" 007", "-1234"}},
    {"% 6i", {"     7", " -1234"}},
    {"% 6.0i", {"     7", " -1234"}},
    {"% 6.3i", {"   007", " -1234"}},
    {"%0i", {"7", "-1234"}},
    {"%0.0i", {"7", "-1234"}},
    {"%0.3i", {"007", "-1234"}},
    {"%06i", {"000007", "-01234"}},
    {"%06.0i", {"     7", " -1234"}},
    {"%06.3i", {"   007", " -1234"}},
    {"%-0i", {"7", "-1234"}},
    {"%-0.0i", {"7", "-1234"}},
    {"%-0.3i", {"007", "-1234"}},
    {"%-06i", {"7     ", "-1234 "}},
    {"%-06.0i", {"7     ", "-1234 "}},
    {"%-06.3i", {"007   ", "-1234 "}},
    {"%+0i", {"+7", "-1234"}},
    {"%+0.0i", {"+7", "-1234"}},
    {"%+0.3i", {"+007", "-1234"}},
    {"%+06i", {"+00007", "-01234"}},
    {"%+06.0i", {"    +7", " -1234"}},
    {"%+06.3i", {"  +007", " -1234"}},
    {"% 0i", {" 7", "-1234"}},
    {"% 0.0i", {" 7", "-1234"}},
    {"% 0.3i", {" 007", "-1234"}},
    {"% 06i", {" 00007", "-01234"}},
    {"% 06.0i", {"     7", " -1234"}},
    {"% 06.3i", {"   007", " -1234"}},
    {"%+ i", {"+7", "-1234"}},
    {"%+ .0i", {"+7", "-1234"}},
    {"%+ .3i", {"+007", "-1234"}},
    {"%+ 6i", {"    +7", " -1234"}},
    {"%+ 6.0i", {"    +7", " -1234"}},
    {"%+ 6.3i", {"  +007", " -1234"}},
    {"%-+i", {"+7", "-1234"}},
    {"%-+.0i", {"+7", "-1234"}},
    {"%-+.3i", {"+007", "-1234"}},
    {"%-+6i", {"+7    ", "-1234 "}},
    {"%-+6.0i", {"+7    ", "-1234 "}},
    {"%-+6.3i", {"+007  ", "-1234 "}},
};
static const struct pf_row pf_u[] = {
    {"%u", {"0", "42"}},
    {"%.0u", {"", "42"}},
    {"%.3u", {"000", "042"}},
    {"%6u", {"     0", "    42"}},
    {"%6.0u", {"      ", "    42"}},
    {"%6.3u", {"   000", "   042"}},
    {"%-u", {"0", "42"}},
    {"%-.0u", {"", "42"}},
    {"%-.3u", {"000", "042"}},
    {"%-6u", {"0     ", "42    "}},
    {"%-6.0u", {"      ", "42    "}},
    {"%-6.3u", {"000   ", "042   "}},
    {"%+u", {"0", "42"}},
    {"%+.0u", {"", "42"}},
    {"%+.3u", {"000", "042"}},
    {"%+6u", {"     0", "    42"}},
    {"%+6.0u", {"      ", "    42"}},
    {"%+6.3u", {"   000", "   042"}},
    {"% u", {"0", "42"}},
    {"% .0u", {"", "42"}},
    {"% .3u", {"000", "042"}},
    {"% 6u", {"     0", "    42"}},
    {"% 6.0u", {"      ", "    42"}},
    {"% 6.3u", {"   000", "   042"}},
    {"%0u", {"0", "42"}},
    {"%0.0u", {"", "42"}},
    {"%0.3u", {"000", "042"}},
    {"%06u", {"000000", "000042"}},
    {"%06.0u", {"      ", "    42"}},
    {"%06.3u", {"   000", "   042"}},
    {"%-0u", {"0", "42"}},
    {"%-0.0u", {"", "42"}},
    {"%-0.3u", {"000", "042"}},
    {"%-06u", {"0     ", "42    "}},
    {"%-06.0u", {"      ", "42    "}},
    {"%-06.3u", {"000   ", "042   "}},
    {"%+0u", {"0", "42"}},
    {"%+0.0u", {"", "42"}},
    {"%+0.3u", {"000", "042"}},
    {"%+06u", {"000000", "000042"}},
    {"%+06.0u", {"      ", "    42"}},
    {"%+06.3u", {"   000", "   042"}},
    {"% 0u", {"0", "42"}},
    {"% 0.0u", {"", "42"}},
    {"% 0.3u", {"000", "042"}},
    {"% 06u", {"000000", "000042"}},
    {"% 06.0u", {"      ", "    42"}},
    {"% 06.3u", {"   000", "   042"}},
    {"%+ u", {"0", "42"}},
    {"%+ .0u", {"", "42"}},
    {"%+ .3u", {"000", "042"}},
    {"%+ 6u", {"     0", "    42"}},
    {"%+ 6.0u", {"      ", "    42"}},
    {"%+ 6.3u", {"   000", "   042"}},
    {"%-+u", {"0", "42"}},
    {"%-+.0u", {"", "42"}},
    {"%-+.3u", {"000", "042"}},
    {"%-+6u", {"0     ", "42    "}},
    {"%-+6.0u", {"      ", "42    "}},
    {"%-+6.3u", {"000   ", "042   "}},
};
static const struct pf_row pf_x[] = {
    {"%x", {"0", "beef"}},
    {"%.0x", {"", "beef"}},
    {"%.3x", {"000", "beef"}},
    {"%6x", {"     0", "  beef"}},
    {"%6.0x", {"      ", "  beef"}},
    {"%6.3x", {"   000", "  beef"}},
    {"%-x", {"0", "beef"}},
    {"%-.0x", {"", "beef"}},
    {"%-.3x", {"000", "beef"}},
    {"%-6x", {"0     ", "beef  "}},
    {"%-6.0x", {"      ", "beef  "}},
    {"%-6.3x", {"000   ", "beef  "}},
    {"%+x", {"0", "beef"}},
    {"%+.0x", {"", "beef"}},
    {"%+.3x", {"000", "beef"}},
    {"%+6x", {"     0", "  beef"}},
    {"%+6.0x", {"      ", "  beef"}},
    {"%+6.3x", {"   000", "  beef"}},
    {"% x", {"0", "beef"}},
    {"% .0x", {"", "beef"}},
    {"% .3x", {"000", "beef"}},
    {"% 6x", {"     0", "  beef"}},
    {"% 6.0x", {"      ", "  beef"}},
    {"% 6.3x", {"   000", "  beef"}},
    {"%#x", {"0", "0xbeef"}},
    {"%#.0x", {"", "0xbeef"}},
    {"%#.3x", {"000", "0xbeef"}},
    {"%#6x", {"     0", "0xbeef"}},
    {"%#6.0x", {"      ", "0xbeef"}},
    {"%#6.3x", {"   000", "0xbeef"}},
    {"%0x", {"0", "beef"}},
    {"%0.0x", {"", "beef"}},
    {"%0.3x", {"000", "beef"}},
    {"%06x", {"000000", "00beef"}},
    {"%06.0x", {"      ", "  beef"}},
    {"%06.3x", {"   000", "  beef"}},
    {"%-0x", {"0", "beef"}},
    {"%-0.0x", {"", "beef"}},
    {"%-0.3x", {"000", "beef"}},
    {"%-06x", {"0     ", "beef  "}},
    {"%-06.0x", {"      ", "beef  "}},
    {"%-06.3x", {"000   ", "beef  "}},
    {"%+0x", {"0", "beef"}},
    {"%+0.0x", {"", "beef"}},
    {"%+0.3x", {"000", "beef"}},
    {"%+06x", {"000000", "00beef"}},
    {"%+06.0x", {"      ", "  beef"}},
    {"%+06.3x", {"   000", "  beef"}},
    {"% 0x", {"0", "beef"}},
    {"% 0.0x", {"", "beef"}},
    {"% 0.3x", {"000", "beef"}},
    {"% 06x", {"000000", "00beef"}},
    {"% 06.0x", {"      ", "  beef"}},
    {"% 06.3x", {"   000", "  beef"}},
    {"%#0x", {"0", "0xbeef"}},
    {"%#0.0x", {"", "0xbeef"}},
    {"%#0.3x", {"000", "0xbeef"}},
    {"%#06x", {"000000", "0xbeef"}},
    {"%#06.0x", {"      ", "0xbeef"}},
    {"%#06.3x", {"   000", "0xbeef"}},
    {"%-#x", {"0", "0xbeef"}},
    {"%-#.0x", {"", "0xbeef"}},
    {"%-#.3x", {"000", "0xbeef"}},
    {"%-#6x", {"0     ", "0xbeef"}},
    {"%-#6.0x", {"      ", "0xbeef"}},
    {"%-#6.3x", {"000   ", "0xbeef"}},
    {"%+ x", {"0", "beef"}},
    {"%+ .0x", {"", "beef"}},
    {"%+ .3x", {"000", "beef"}},
    {"%+ 6x", {"     0", "  beef"}},
    {"%+ 6.0x", {"      ", "  beef"}},
    {"%+ 6.3x", {"   000", "  beef"}},
    {"%-+x", {"0", "beef"}},
    {"%-+.0x", {"", "beef"}},
    {"%-+.3x", {"000", "beef"}},
    {"%-+6x", {"0     ", "beef  "}},
    {"%-+6.0x", {"      ", "beef  "}},
    {"%-+6.3x", {"000   ", "beef  "}},
};
static const struct pf_row pf_xu[] = {
    {"%X", {"0", "BEEF"}},
    {"%.0X", {"", "BEEF"}},
    {"%.3X", {"000", "BEEF"}},
    {"%6X", {"     0", "  BEEF"}},
    {"%6.0X", {"      ", "  BEEF"}},
    {"%6.3X", {"   000", "  BEEF"}},
    {"%-X", {"0", "BEEF"}},
    {"%-.0X", {"", "BEEF"}},
    {"%-.3X", {"000", "BEEF"}},
    {"%-6X", {"0     ", "BEEF  "}},
    {"%-6.0X", {"      ", "BEEF  "}},
    {"%-6.3X", {"000   ", "BEEF  "}},
    {"%+X", {"0", "BEEF"}},
    {"%+.0X", {"", "BEEF"}},
    {"%+.3X", {"000", "BEEF"}},
    {"%+6X", {"     0", "  BEEF"}},
    {"%+6.0X", {"      ", "  BEEF"}},
    {"%+6.3X", {"   000", "  BEEF"}},
    {"% X", {"0", "BEEF"}},
    {"% .0X", {"", "BEEF"}},
    {"% .3X", {"000", "BEEF"}},
    {"% 6X", {"     0", "  BEEF"}},
    {"% 6.0X", {"      ", "  BEEF"}},
    {"% 6.3X", {"   000", "  BEEF"}},
    {"%#X", {"0", "0XBEEF"}},
    {"%#.0X", {"", "0XBEEF"}},
    {"%#.3X", {"000", "0XBEEF"}},
    {"%#6X", {"     0", "0XBEEF"}},
    {"%#6.0X", {"      ", "0XBEEF"}},
    {"%#6.3X", {"   000", "0XBEEF"}},
    {"%0X", {"0", "BEEF"}},
    {"%0.0X", {"", "BEEF"}},
    {"%0.3X", {"000", "BEEF"}},
    {"%06X", {"000000", "00BEEF"}},
    {"%06.0X", {"      ", "  BEEF"}},
    {"%06.3X", {"   000", "  BEEF"}},
    {"%-0X", {"0", "BEEF"}},
    {"%-0.0X", {"", "BEEF"}},
    {"%-0.3X", {"000", "BEEF"}},
    {"%-06X", {"0     ", "BEEF  "}},
    {"%-06.0X", {"      ", "BEEF  "}},
    {"%-06.3X", {"000   ", "BEEF  "}},
    {"%+0X", {"0", "BEEF"}},
    {"%+0.0X", {"", "BEEF"}},
    {"%+0.3X", {"000", "BEEF"}},
    {"%+06X", {"000000", "00BEEF"}},
    {"%+06.0X", {"      ", "  BEEF"}},
    {"%+06.3X", {"   000", "  BEEF"}},
    {"% 0X", {"0", "BEEF"}},
    {"% 0.0X", {"", "BEEF"}},
    {"% 0.3X", {"000", "BEEF"}},
    {"% 06X", {"000000", "00BEEF"}},
    {"% 06.0X", {"      ", "  BEEF"}},
    {"% 06.3X", {"   000", "  BEEF"}},
    {"%#0X", {"0", "0XBEEF"}},
    {"%#0.0X", {"", "0XBEEF"}},
    {"%#0.3X", {"000", "0XBEEF"}},
    {"%#06X", {"000000", "0XBEEF"}},
    {"%#06.0X", {"      ", "0XBEEF"}},
    {"%#06.3X", {"   000", "0XBEEF"}},
    {"%-#X", {"0", "0XBEEF"}},
    {"%-#.0X", {"", "0XBEEF"}},
    {"%-#.3X", {"000", "0XBEEF"}},
    {"%-#6X", {"0     ", "0XBEEF"}},
    {"%-#6.0X", {"      ", "0XBEEF"}},
    {"%-#6.3X", {"000   ", "0XBEEF"}},
    {"%+ X", {"0", "BEEF"}},
    {"%+ .0X", {"", "BEEF"}},
    {"%+ .3X", {"000", "BEEF"}},
    {"%+ 6X", {"     0", "  BEEF"}},
    {"%+ 6.0X", {"      ", "  BEEF"}},
    {"%+ 6.3X", {"   000", "  BEEF"}},
    {"%-+X", {"0", "BEEF"}},
    {"%-+.0X", {"", "BEEF"}},
    {"%-+.3X", {"000", "BEEF"}},
    {"%-+6X", {"0     ", "BEEF  "}},
    {"%-+6.0X", {"      ", "BEEF  "}},
    {"%-+6.3X", {"000   ", "BEEF  "}},
};
static const struct pf_row pf_o[] = {
    {"%o", {"0", "10"}},
    {"%.0o", {"", "10"}},
    {"%.3o", {"000", "010"}},
    {"%6o", {"     0", "    10"}},
    {"%6.0o", {"      ", "    10"}},
    {"%6.3o", {"   000", "   010"}},
    {"%-o", {"0", "10"}},
    {"%-.0o", {"", "10"}},
    {"%-.3o", {"000", "010"}},
    {"%-6o", {"0     ", "10    "}},
    {"%-6.0o", {"      ", "10    "}},
    {"%-6.3o", {"000   ", "010   "}},
    {"%+o", {"0", "10"}},
    {"%+.0o", {"", "10"}},
    {"%+.3o", {"000", "010"}},
    {"%+6o", {"     0", "    10"}},
    {"%+6.0o", {"      ", "    10"}},
    {"%+6.3o", {"   000", "   010"}},
    {"% o", {"0", "10"}},
    {"% .0o", {"", "10"}},
    {"% .3o", {"000", "010"}},
    {"% 6o", {"     0", "    10"}},
    {"% 6.0o", {"      ", "    10"}},
    {"% 6.3o", {"   000", "   010"}},
    {"%#o", {"0", "010"}},
    {"%#.0o", {"0", "010"}},
    {"%#.3o", {"000", "010"}},
    {"%#6o", {"     0", "   010"}},
    {"%#6.0o", {"     0", "   010"}},
    {"%#6.3o", {"   000", "   010"}},
    {"%0o", {"0", "10"}},
    {"%0.0o", {"", "10"}},
    {"%0.3o", {"000", "010"}},
    {"%06o", {"000000", "000010"}},
    {"%06.0o", {"      ", "    10"}},
    {"%06.3o", {"   000", "   010"}},
    {"%-0o", {"0", "10"}},
    {"%-0.0o", {"", "10"}},
    {"%-0.3o", {"000", "010"}},
    {"%-06o", {"0     ", "10    "}},
    {"%-06.0o", {"      ", "10    "}},
    {"%-06.3o", {"000   ", "010   "}},
    {"%+0o", {"0", "10"}},
    {"%+0.0o", {"", "10"}},
    {"%+0.3o", {"000", "010"}},
    {"%+06o", {"000000", "000010"}},
    {"%+06.0o", {"      ", "    10"}},
    {"%+06.3o", {"   000", "   010"}},
    {"% 0o", {"0", "10"}},
    {"% 0.0o", {"", "10"}},
    {"% 0.3o", {"000", "010"}},
    {"% 06o", {"000000", "000010"}},
    {"% 06.0o", {"      ", "    10"}},
    {"% 06.3o", {"   000", "   010"}},
    {"%#0o", {"0", "010"}},
    {"%#0.0o", {"0", "010"}},
    {"%#0.3o", {"000", "010"}},
    {"%#06o", {"000000", "000010"}},
    {"%#06.0o", {"     0", "   010"}},
    {"%#06.3o", {"   000", "   010"}},
    {"%-#o", {"0", "010"}},
    {"%-#.0o", {"0", "010"}},
    {"%-#.3o", {"000", "010"}},
    {"%-#6o", {"0     ", "010   "}},
    {"%-#6.0o", {"0     ", "010   "}},
    {"%-#6.3o", {"000   ", "010   "}},
    {"%+ o", {"0", "10"}},
    {"%+ .0o", {"", "10"}},
    {"%+ .3o", {"000", "010"}},
    {"%+ 6o", {"     0", "    10"}},
    {"%+ 6.0o", {"      ", "    10"}},
    {"%+ 6.3o", {"   000", "   010"}},
    {"%-+o", {"0", "10"}},
    {"%-+.0o", {"", "10"}},
    {"%-+.3o", {"000", "010"}},
    {"%-+6o", {"0     ", "10    "}},
    {"%-+6.0o", {"      ", "10    "}},
    {"%-+6.3o", {"000   ", "010   "}},
};
static const struct pf_row pf_s[] = {
    {"%s", {"", "abcde"}},
    {"%.0s", {"", ""}},
    {"%.3s", {"", "abc"}},
    {"%6s", {"      ", " abcde"}},
    {"%6.0s", {"      ", "      "}},
    {"%6.3s", {"      ", "   abc"}},
    {"%-s", {"", "abcde"}},
    {"%-.0s", {"", ""}},
    {"%-.3s", {"", "abc"}},
    {"%-6s", {"      ", "abcde "}},
    {"%-6.0s", {"      ", "      "}},
    {"%-6.3s", {"      ", "abc   "}},
    {"%+s", {"", "abcde"}},
    {"%+.0s", {"", ""}},
    {"%+.3s", {"", "abc"}},
    {"%+6s", {"      ", " abcde"}},
    {"%+6.0s", {"      ", "      "}},
    {"%+6.3s", {"      ", "   abc"}},
    {"% s", {"", "abcde"}},
    {"% .0s", {"", ""}},
    {"% .3s", {"", "abc"}},
    {"% 6s", {"      ", " abcde"}},
    {"% 6.0s", {"      ", "      "}},
    {"% 6.3s", {"      ", "   abc"}},
    {"%+ s", {"", "abcde"}},
    {"%+ .0s", {"", ""}},
    {"%+ .3s", {"", "abc"}},
    {"%+ 6s", {"      ", " abcde"}},
    {"%+ 6.0s", {"      ", "      "}},
    {"%+ 6.3s", {"      ", "   abc"}},
    {"%-+s", {"", "abcde"}},
    {"%-+.0s", {"", ""}},
    {"%-+.3s", {"", "abc"}},
    {"%-+6s", {"      ", "abcde "}},
    {"%-+6.0s", {"      ", "      "}},
    {"%-+6.3s", {"      ", "abc   "}},
};
static const struct pf_row pf_c[] = {
    {"%c", {"A", "z"}},
    {"%6c", {"     A", "     z"}},
    {"%-c", {"A", "z"}},
    {"%-6c", {"A     ", "z     "}},
    {"%+c", {"A", "z"}},
    {"%+6c", {"     A", "     z"}},
    {"% c", {"A", "z"}},
    {"% 6c", {"     A", "     z"}},
    {"%+ c", {"A", "z"}},
    {"%+ 6c", {"     A", "     z"}},
    {"%-+c", {"A", "z"}},
    {"%-+6c", {"A     ", "z     "}},
};
static const struct pf_row pf_p[] = {
    {"%p", {"(nil)", "0x1234"}},
    {"%.0p", {"(nil)", "0x1234"}},
    {"%.3p", {"(nil)", "0x1234"}},
    {"%6p", {" (nil)", "0x1234"}},
    {"%6.0p", {" (nil)", "0x1234"}},
    {"%6.3p", {" (nil)", "0x1234"}},
    {"%-p", {"(nil)", "0x1234"}},
    {"%-.0p", {"(nil)", "0x1234"}},
    {"%-.3p", {"(nil)", "0x1234"}},
    {"%-6p", {"(nil) ", "0x1234"}},
    {"%-6.0p", {"(nil) ", "0x1234"}},
    {"%-6.3p", {"(nil) ", "0x1234"}},
    {"%+p", {"(nil)", "+0x1234"}},
    {"%+.0p", {"(nil)", "+0x1234"}},
    {"%+.3p", {"(nil)", "+0x1234"}},
    {"%+6p", {" (nil)", "+0x1234"}},
    {"%+6.0p", {" (nil)", "+0x1234"}},
    {"%+6.3p", {" (nil)", "+0x1234"}},
    {"% p", {"(nil)", " 0x1234"}},
    {"% .0p", {"(nil)", " 0x1234"}},
    {"% .3p", {"(nil)", " 0x1234"}},
    {"% 6p", {" (nil)", " 0x1234"}},
    {"% 6.0p", {" (nil)", " 0x1234"}},
    {"% 6.3p", {" (nil)", " 0x1234"}},
    {"%0p", {"(nil)", "0x1234"}},
    {"%0.0p", {"(nil)", "0x1234"}},
    {"%0.3p", {"(nil)", "0x1234"}},
    {"%06p", {" (nil)", "0x1234"}},
    {"%06.0p", {" (nil)", "0x1234"}},
    {"%06.3p", {" (nil)", "0x1234"}},
    {"%-0p", {"(nil)", "0x1234"}},
    {"%-0.0p", {"(nil)", "0x1234"}},
    {"%-0.3p", {"(nil)", "0x1234"}},
    {"%-06p", {"(nil) ", "0x1234"}},
    {"%-06.0p", {"(nil) ", "0x1234"}},
    {"%-06.3p", {"(nil) ", "0x1234"}},
    {"%+0p", {"(nil)", "+0x1234"}},
    {"%+0.0p", {"(nil)", "+0x1234"}},
    {"%+0.3p", {"(nil)", "+0x1234"}},
    {"%+06p", {" (nil)", "+0x1234"}},
    {"%+06.0p", {" (nil)", "+0x1234"}},
    {"%+06.3p", {" (nil)", "+0x1234"}},
    {"% 0p", {"(nil)", " 0x1234"}},
    {"% 0.0p", {"(nil)", " 0x1234"}},
    {"% 0.3p", {"(nil)", " 0x1234"}},
    {"% 06p", {" (nil)", " 0x1234"}},
    {"% 06.0p", {" (nil)", " 0x1234"}},
    {"% 06.3p", {" (nil)", " 0x1234"}},
    {"%+ p", {"(nil)", "+0x1234"}},
    {"%+ .0p", {"(nil)", "+0x1234"}},
    {"%+ .3p", {"(nil)", "+0x1234"}},
    {"%+ 6p", {" (nil)", "+0x1234"}},
    {"%+ 6.0p", {" (nil)", "+0x1234"}},
    {"%+ 6.3p", {" (nil)", "+0x1234"}},
    {"%-+p", {"(nil)", "+0x1234"}},
    {"%-+.0p", {"(nil)", "+0x1234"}},
    {"%-+.3p", {"(nil)", "+0x1234"}},
    {"%-+6p", {"(nil) ", "+0x1234"}},
    {"%-+6.0p", {"(nil) ", "+0x1234"}},
    {"%-+6.3p", {"(nil) ", "+0x1234"}},
};
static const struct pf_case pf_edge[] = {
    {"%d", 'i', INT_MIN, NULL, "-2147483648"},
    {"%d", 'i', 0x7fffffff, NULL, "2147483647"},
    {"%+d", 'i', 0x7fffffff, NULL, "+2147483647"},
    {"%012d", 'i', INT_MIN, NULL, "-02147483648"},
    {"%u", 'u', 0xffffffff, NULL, "4294967295"},
    {"%#x", 'u', 0xffffffff, NULL, "0xffffffff"},
    {"%#o", 'u', 0xffffffff, NULL, "037777777777"},
    {"%lld", 'l', LLONG_MIN, NULL, "-9223372036854775808"},
    {"%lld", 'l', 0x7fffffffffffffff, NULL, "9223372036854775807"},
    {"%ld", 'L', LLONG_MIN, NULL, "-9223372036854775808"},
    {"%jd", 'j', LLONG_MIN, NULL, "-9223372036854775808"},
    {"%25lli", 'l', LLONG_MIN, NULL, "     -9223372036854775808"},
    {"%llu", 'U', (long long) ULLONG_MAX, NULL, "18446744073709551615"},
    {"%llx", 'U', (long long) ULLONG_MAX, NULL, "ffffffffffffffff"},
    {"%#llX", 'U', (long long) ULLONG_MAX, NULL, "0XFFFFFFFFFFFFFFFF"},
    {"%llo", 'U', (long long) ULLONG_MAX, NULL, "1777777777777777777777"},
    {"%#llo", 'U', (long long) ULLONG_MAX, NULL, "01777777777777777777777"},
    {"%zu", 'z', (long long) ULLONG_MAX, NULL, "18446744073709551615"},
    {"%zx", 'z', (long long) ULLONG_MAX, NULL, "ffffffffffffffff"},
    {"%td", 't', LLONG_MIN, NULL, "-9223372036854775808"},
    {"%hd", 'i', 0x11170, NULL, "4464"},
    {"%hu", 'i', -1, NULL, "65535"},
    {"%hhd", 'i', 0x12c, NULL, "44"},
    {"%hhu", 'i', -1, NULL, "255"},
    {"%hhx", 'i', 0x1ff, NULL, "ff"},
    {"%#hho", 'i', 0x1ff, NULL, "0377"},
    {"%.10d", 'i', -42, NULL, "-0000000042"},
    {"%-+8.4d|", 'i', 0x2a, NULL, "+0042   |"},
    {"%+.0d", 'i', 0, NULL, "+"},
    {"% .0d", 'i', 0, NULL, " "},
    {"%5.0d|", 'i', 0, NULL, "     |"},
    {"%#.0o", 'u', 0, NULL, "0"},
    {"%#5.0o|", 'u', 0, NULL, "    0|"},
    {"%#.3o", 'u', 8, NULL, "010"},
    {"%#.0x", 'u', 0, NULL, ""},
    {"%05.2d", 'i', 3, NULL, "   03"},
    {"%-05d|", 'i', 3, NULL, "3    |"},
    {"%+u", 'u', 5, NULL, "5"},
    {"% x", 'u', 5, NULL, "5"},
    {"%+o", 'u', 5, NULL, "5"},
    {"%#o", 'u', 8, NULL, "010"},
    {"%.10s|", 's', 0, "abc", "abc|"},
    {"%10.2s|", 's', 0, "abc", "        ab|"},
    {"%-10s|", 's', 0, "abc", "abc       |"},
    {"%s", 's', 0, "", ""},
    {"%3c|", 'c', 'q', NULL, "  q|"},
    {"%-3c|", 'c', 'q', NULL, "q  |"},
    {"%+p", 'p', 0xff, NULL, "+0xff"},
    {"%018p", 'p', 0xff, NULL, "0x00000000000000ff"},
    {"%.8p", 'p', 0xff, NULL, "0x000000ff"},
    {"%10p|", 'p', 0, NULL, "     (nil)|"},
    {"%-10p|", 'p', 0, NULL, "(nil)     |"},
    {"%p", 'p', (long long) UINTPTR_MAX, NULL, "0xffffffffffffffff"},
    {"%5%|", 'i', 0, NULL, "%|"},
    {"100%%", 'i', 0, NULL, "100%"},
};

/* The two arguments of each conversion's rows; these must match the values the
 * table was generated with.
 */
static const struct pf_group {
    const char *name;
    char type;
    long long v[2];
    const char *s[2];
    const struct pf_row *rows;
    size_t nrows;
} pf_groups[] = {
#define PF_GROUP(name, type, v0, v1, s0, s1, rows) \
    {name, type, {v0, v1}, {s0, s1}, rows, sizeof(rows) / sizeof((rows)[0])}
    PF_GROUP("%d", 'i', 0, -42, NULL, NULL, pf_d),
    PF_GROUP("%i", 'i', 7, -1234, NULL, NULL, pf_i),
    PF_GROUP("%u", 'u', 0, 42, NULL, NULL, pf_u),
    PF_GROUP("%x", 'u', 0, 0xbeef, NULL, NULL, pf_x),
    PF_GROUP("%X", 'u', 0, 0xbeef, NULL, NULL, pf_xu),
    PF_GROUP("%o", 'u', 0, 8, NULL, NULL, pf_o),
    PF_GROUP("%s", 's', 0, 0, "", "abcde", pf_s),
    PF_GROUP("%c", 'c', 'A', 'z', NULL, NULL, pf_c),
    PF_GROUP("%p", 'p', 0, 0x1234, NULL, NULL, pf_p),
#undef PF_GROUP
};

/* One PASS line per group of table cases; each mismatch was already reported. */
static void pf_report(const char *what, size_t ncases, int bad)
{
    char name[80];

    if (bad)
        return;
    snprintf(name, sizeof name, "printf table %s (%zu cases)", what, ncases);
    test_ok(name);
}

static void test_printf_table(void)
{
    int bad;

    for (size_t g = 0; g < sizeof pf_groups / sizeof pf_groups[0]; g++) {
        const struct pf_group *gp = &pf_groups[g];

        bad = 0;
        for (size_t r = 0; r < gp->nrows; r++)
            for (int k = 0; k < 2; k++)
                bad += pf_check(gp->rows[r].fmt, gp->type, gp->v[k], gp->s[k],
                                gp->rows[r].want[k]);
        pf_report(gp->name, 2 * gp->nrows, bad);
    }

    bad = 0;
    for (size_t i = 0; i < sizeof pf_edge / sizeof pf_edge[0]; i++)
        bad += pf_check(pf_edge[i].fmt, pf_edge[i].type, pf_edge[i].v, pf_edge[i].s,
                        pf_edge[i].want);
    pf_report("extremes and modifiers", sizeof pf_edge / sizeof pf_edge[0], bad);
}

/* Cases outside the glibc table: stack-buffer reuse, argument alignment after
 * an unsupported or placeholder conversion, and the snprintf return value when
 * the output is truncated.
 */
static void test_printf_special(void)
{
    char buf[64];
    int n, ret;

    /* An undefined conversion is copied out literally (RTMux's choice, as
     * glibc does), and the %c after it must not overwrite those bytes: with
     * the queued-buffer bug this printed "AyA".
     */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat"
    ret = snprintf(buf, sizeof buf, "%y%c", 'A');
    expect("%y%c keeps %y", buf, "%yA");
    check(ret == 3, "%y%c returns 3", "wrong return value");
    snprintf(buf, sizeof buf, "[%-5y|%c%c]", 'B', 'C');
#pragma GCC diagnostic pop
    expect("%-5y copies the whole spec", buf, "[%-5y|BC]");

    /* %L consumes a long double, so the conversions after it stay aligned.
     * The float text itself is a placeholder until float output lands.
     */
    snprintf(buf, sizeof buf, "%Lf%d", 1.5L, 7);
    expect("%Lf%d stays aligned", buf, "[float]7");
    snprintf(buf, sizeof buf, "%Le|%s|%Lg|%d", 2.5L, "s", -1.0L, -3);
    expect("%Le%s%Lg%d stays aligned", buf, "[float]|s|[float]|-3");
    snprintf(buf, sizeof buf, "%f|%d", 1.5, 9);
    expect("%f%d stays aligned", buf, "[float]|9");

    /* Star width and precision, including a negative width (left-adjust) and
     * a negative precision (taken as absent).
     */
    snprintf(buf, sizeof buf, "%*d|%-*d|%.*d|%*.*s|", 4, 7, 3, 8, -1, 5, 6, 2, "abc");
    expect("star width and precision", buf, "   7|8  |5|    ab|");
    snprintf(buf, sizeof buf, "%*d|", -4, 7);
    expect("negative star width", buf, "7   |");

    /* %n reports the count so far. */
    n = -1;
    snprintf(buf, sizeof buf, "ab%dcd%n|", 123, &n);
    check(n == 7, "%n counts the output so far", "wrong count");

    /* Each %n modifier stores through a pointer of its own type, writing all
     * of it.
     */
    {
        long ln = -1;
        intmax_t jn = -1;
        size_t zn = (size_t) -1;
        ptrdiff_t tn = -1;
        long long lln = -1;

        snprintf(buf, sizeof buf, "a%lnbc%jndef%zng%tnhi%lln", &ln, &jn, &zn, &tn, &lln);
        check(ln == 1 && jn == 3 && zn == 6 && tn == 7 && lln == 9,
              "%ln %jn %zn %tn %lln store full-width counts", "wrong count");
    }

    /* A %n store that aliases an earlier %s argument must not change the
     * bytes already printed from it.
     */
    {
        char s[] = "abc";

        snprintf(buf, sizeof buf, "%s%hhn", s, (signed char *) s);
        check(!strcmp(buf, "abc") && s[0] == 3, "%n after %s of the same buffer",
              "output changed by the later store");
    }

    /* snprintf returns the untruncated length and always terminates. */
    memset(buf, 'x', sizeof buf);
    ret = snprintf(buf, 5, "%d", 123456);
    check(ret == 6 && !strcmp(buf, "1234"), "snprintf truncates a number",
          "wrong return value or truncation");
    memset(buf, 'x', sizeof buf);
    ret = snprintf(buf, 4, "%10d", 1);
    check(ret == 10 && !strcmp(buf, "   "), "snprintf truncates in the padding",
          "wrong return value or truncation");
    memset(buf, 'x', sizeof buf);
    ret = snprintf(buf, 2, "%#x", 255u);
    check(ret == 4 && !strcmp(buf, "0"), "snprintf truncates in the 0x prefix",
          "wrong return value or truncation");
    memset(buf, 'x', sizeof buf);
    ret = snprintf(buf, 1, "abc%d", -5);
    check(ret == 5 && buf[0] == '\0' && buf[1] == 'x', "snprintf with size 1",
          "wrong return value or wrote past the end");
    ret = snprintf(NULL, 0, "%s-%d-%llu", "ab", -5, 18446744073709551615ULL);
    check(ret == 26, "snprintf(NULL, 0) returns the length", "wrong return value");
    ret = snprintf(buf, sizeof buf, "%-20s|%020lld", "x", -1LL);
    check(ret == 41, "snprintf return counts padding", "wrong return value");

    /* A stream with an 8-byte buffer: the stream drains many times within
     * one call, at every point in a conversion, so a queued pointer to a
     * rewritten stack buffer would show up in the bytes read back.
     */
    int fds[2];
    if (pipe(fds) != 0) {
        test_fail("fprintf through a small buffer", "pipe() failed");
        return;
    }
    FILE *w = fdopen(fds[1], "w");
    static char wbuf[8];
    char rbuf[128];
    const char *want = "%y|A|-0042|0x00ff|017|   ab|(nil)|0xbeef|18446744073709551615|[float]|9";
    size_t len = strlen(want);
    size_t got = 0;
    ssize_t r;

    if (!w || setvbuf(w, wbuf, _IOFBF, sizeof wbuf) != 0) {
        test_fail("fprintf through a small buffer", "fdopen or setvbuf failed");
        if (w)
            fclose(w);
        else
            close(fds[1]);
        close(fds[0]);
        return;
    }
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat"
    ret = fprintf(w, "%y|%c|%05d|%#06x|%#o|%5.2s|%p|%p|%llu|%Lf|%d", 'A', -42, 0xffu, 15u, "abc",
                  (void *) 0, (void *) 0xbeefUL, 18446744073709551615ULL, 1.0L, 9);
#pragma GCC diagnostic pop
    fclose(w);
    while (got < sizeof rbuf - 1 && (r = read(fds[0], rbuf + got, sizeof rbuf - 1 - got)) > 0)
        got += (size_t) r;
    close(fds[0]);
    rbuf[got] = '\0';
    check(ret == (int) len, "fprintf through a small buffer returns the length",
          "wrong return value");
    expect("fprintf through a small buffer", rbuf, want);
}

static int format_unbounded(char *result, const char *format, ...)
{
    va_list ap;
    va_start(ap, format);
    int n = vsprintf(result, format, ap);
    va_end(ap);
    return n;
}

static void test_rewind(void)
{
    FILE *fp = fopen("/bin/sh", "r");
    if (!fp) {
        test_fail("open file for rewind", strerror(errno));
        return;
    }
    int first = fgetc(fp);
    int seek_rc = fseek(fp, 0, SEEK_END);
    int last = fgetc(fp);
    int write_rc = fputc('x', fp);
    check(first != EOF && seek_rc == 0 && last == EOF && feof(fp) &&
              write_rc == EOF && ferror(fp),
          "rewind fixture sets EOF and error", "stream indicators");
    rewind(fp);
    check(!ferror(fp) && !feof(fp) && fgetc(fp) == first,
          "rewind clears indicators and seeks to start", "stream state");
    fclose(fp);
}

enum { POOL_WORKERS = 8, POOL_ROUNDS = 1000 };
static pthread_mutex_t stream_test_lock = PTHREAD_MUTEX_INITIALIZER;
static FILE *open_streams[POOL_WORKERS];
struct stream_worker {
    unsigned index;
    unsigned failures;
    /* Which call failed, and the last errno or byte seen, for the detail. */
    unsigned open_failed, collided, read_failed, close_failed;
    int open_errno, read_byte, read_errno, close_errno;
};

static void *exercise_stream_slot(void *arg)
{
    struct stream_worker *worker = arg;
    for (unsigned round = 0; round < POOL_ROUNDS; round++) {
        FILE *fp = fopen("/bin/sh", "r");
        if (!fp) {
            worker->open_errno = errno;
            worker->open_failed++;
            worker->failures++;
            continue;
        }
        pthread_mutex_lock(&stream_test_lock);
        for (unsigned i = 0; i < POOL_WORKERS; i++) {
            if (open_streams[i] == fp) {
                worker->collided++;
                worker->failures++;
            }
        }
        open_streams[worker->index] = fp;
        pthread_mutex_unlock(&stream_test_lock);
        errno = 0;
        int byte = fgetc(fp);
        if (byte != 0x7f) {
            worker->read_byte = byte;
            worker->read_errno = errno;
            worker->read_failed++;
            worker->failures++;
        }
        pthread_mutex_lock(&stream_test_lock);
        open_streams[worker->index] = NULL;
        pthread_mutex_unlock(&stream_test_lock);
        if (fclose(fp) != 0) {
            worker->close_errno = errno;
            worker->close_failed++;
            worker->failures++;
        }
    }
    return NULL;
}

static void test_stream_pool(void)
{
    FILE *streams[2 * FOPEN_MAX];
    size_t count = 0;
    while (count < sizeof(streams) / sizeof(*streams)) {
        streams[count] = fopen("/bin/sh", "r");
        if (!streams[count])
            break;
        count++;
    }
    check(count == sizeof(streams) / sizeof(*streams),
          "stdio grows beyond the static stream pool", "open failed");
    while (count)
        fclose(streams[--count]);

    struct stream_worker workers[POOL_WORKERS] = {0};
    pthread_t threads[POOL_WORKERS];
    unsigned started = 0, failures = 0;
    for (; started < POOL_WORKERS; started++) {
        workers[started].index = started;
        if (pthread_create(&threads[started], NULL, exercise_stream_slot,
                           &workers[started]) != 0)
            break;
    }
    struct stream_worker sum = {0};
    char stream_detail[200];
    for (unsigned i = 0; i < started; i++) {
        pthread_join(threads[i], NULL);
        failures += workers[i].failures;
        sum.open_failed += workers[i].open_failed;
        sum.collided += workers[i].collided;
        sum.read_failed += workers[i].read_failed;
        sum.close_failed += workers[i].close_failed;
        if (workers[i].open_failed)
            sum.open_errno = workers[i].open_errno;
        if (workers[i].read_failed) {
            sum.read_byte = workers[i].read_byte;
            sum.read_errno = workers[i].read_errno;
        }
        if (workers[i].close_failed)
            sum.close_errno = workers[i].close_errno;
    }
    snprintf(stream_detail, sizeof stream_detail,
             "started=%u fopen failed=%u (errno %d) collisions=%u fgetc "
             "wrong=%u (byte %d errno %d) fclose failed=%u (errno %d)",
             started, sum.open_failed, sum.open_errno, sum.collided,
             sum.read_failed, sum.read_byte, sum.read_errno, sum.close_failed,
             sum.close_errno);
    check(started == POOL_WORKERS && !failures,
          "eight threads open and close 1000 private streams each",
          stream_detail);

    if (sysconf(_SC_OPEN_MAX) > SHRT_MAX + 1L) {
        int fd = fcntl(STDIN_FILENO, F_DUPFD, SHRT_MAX + 1);
        if (fd < 0) {
            test_fail("create high descriptor for fdopen", strerror(errno));
            return;
        }
        errno = 0;
        FILE *fp = fdopen(fd, "r");
        check(!fp && errno == EMFILE && fcntl(fd, F_GETFL, 0) >= 0,
              "fdopen refuses an unrepresentable descriptor and preserves it", "fd ownership or errno");
        if (fp)
            fclose(fp);
        else
            close(fd);
    }
}

/* Print every strerror() message in the log. The harness must not take errno
 * text for a crash: it once ended a run on "Bad file descriptor" printed by a
 * failing check's detail (scripts/crash-signatures.sh).
 */
static void test_strerror_text(void)
{
    char unknown[32];
    int shown = 0, empty = 0;

    for (int e = -1; e <= 300; e++) {
        const char *msg = strerror(e);

        snprintf(unknown, sizeof unknown, "Unknown error %d", e);
        if (!msg || !*msg)
            empty++;
        else if (e < 0 || strcmp(msg, unknown)) {
            printf("    strerror %d: %s\n", e, msg);
            shown++;
        }
    }
    snprintf(unknown, sizeof unknown, "%d shown, %d empty", shown, empty);
    check(shown > 30 && !empty, "strerror text printed for every errno", unknown);
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

    /* %p followed by %u — pointer also uses the ox prefix. %p prints as
     * %#lx, as glibc does.
     */
    snprintf(buf, sizeof buf, "p=%p n=%u", (void *) 0xdeadbeefUL, 1u);
    expect("%p then %u", buf, "p=0xdeadbeef n=1");

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

    check(format_unbounded(buf, "%s %d", "number", 42) == 9 &&
              strcmp(buf, "number 42") == 0,
          "vsprintf writes the formatted string and length", "bad result");
    test_rewind();
    test_stream_pool();
    test_printf_table();
    test_printf_special();
    test_flockfile();
    test_char_unlocked_stdio();
    test_stdio_locked_ops();
    test_setvbuf_failure(0);
    test_setvbuf_failure(1);
    test_strerror_text();

    /* exit() and the streams it flushes. */
    test_stdio_exit();
}
