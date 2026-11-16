/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * env-test.c — setenv, unsetenv, putenv, and getenv: POSIX.1-2024 errors,
 * bounded allocation, pointer lifetime, and environ reassignment.
 *
 * tests/host/test-libc-algorithms.c builds this against the RTMux getenv.c
 * with LIBC_HOST_TEST and a counting allocator (tests/host/libc-host-shim.h),
 * which adds the allocation-count and allocation-failure cases. The guest build
 * adds a spawn that must inherit a variable set with setenv.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "test.h"

#ifdef LIBC_HOST_TEST
#include "libc-host-shim.h"
#else
#include <spawn.h>
#include <sys/wait.h>
#endif

extern char **environ;

#define MANY 1000

static int env_einval(void)
{
    /* The host headers declare these names nonnull, so only the guest passes
     * NULL (an RTMux extension).
     */
#ifdef LIBC_HOST_TEST
    static const char *const bad[] = {"", "A=B", "="};
#else
    static const char *const bad[] = {"", "A=B", "=", NULL};
#endif
    int ok = 1;

    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        errno = 0;
        ok = ok && setenv(bad[i], "v", 1) == -1 && errno == EINVAL;
        errno = 0;
        ok = ok && unsetenv(bad[i]) == -1 && errno == EINVAL;
    }
    errno = 0;
    ok = ok && putenv("NOEQUALS") == -1 && errno == EINVAL;
    errno = 0;
    ok = ok && putenv("=v") == -1 && errno == EINVAL;
    return ok;
}

static void test_env_basic(void)
{
    char *initial[] = {"HOME=/root", "A=1", NULL};
    char *const snapshot[] = {initial[0], initial[1], NULL};
    char *p;
    int ok;

    check(env_einval(), "setenv/unsetenv EINVAL for an empty, NULL, or '=' name",
          "a bad name must fail with EINVAL");

    environ = NULL;
    check(unsetenv("X") == 0, "unsetenv on an empty environment returns 0", "returned -1");
    ok = setenv("X", "1", 0) == 0 && (p = getenv("X")) && !strcmp(p, "1");
    check(ok, "setenv with environ == NULL creates the vector", "X not set");

    /* The first change copies the initial vector instead of writing into it. */
    environ = initial;
    ok = setenv("B", "2", 1) == 0 && environ != initial && initial[0] == snapshot[0] &&
         initial[1] == snapshot[1] && initial[2] == NULL && (p = getenv("A")) &&
         !strcmp(p, "1") && (p = getenv("B")) && !strcmp(p, "2");
    check(ok, "setenv copies the initial vector off its original storage",
          "initial vector modified or a variable lost");

    ok = setenv("B", "3", 0) == 0 && (p = getenv("B")) && !strcmp(p, "2");
    check(ok, "setenv with overwrite 0 keeps the value", "value replaced");

    /* An overwrite that fits rewrites in place; a longer one moves, and the old
     * string stays readable.
     */
    p = getenv("B");
    ok = setenv("B", "4", 1) == 0 && getenv("B") == p && !strcmp(p, "4");
    ok = ok && setenv("B", getenv("B"), 1) == 0 && !strcmp(getenv("B"), "4");
    ok = ok && setenv("B", "abc", 1) == 0 && setenv("B", getenv("B") + 1, 1) == 0 &&
         !strcmp(getenv("B"), "bc");
    ok = ok && setenv("B", "a much longer value", 1) == 0 && !strcmp(p, "4") &&
         !strcmp(getenv("B"), "a much longer value");
    check(ok, "setenv reuses a fitting buffer and keeps a replaced one readable",
          "pointer moved on a fitting overwrite or old string changed");

    ok = unsetenv("A") == 0 && !getenv("A") && !strcmp(getenv("HOME"), "/root");
    check(ok, "unsetenv removes only the named variable", "wrong variable removed");
}

static void test_env_putenv(void)
{
    char buf[] = "P=1";
    char repl[] = "P=other";
    char *p;
    int ok;

    ok = putenv(buf) == 0 && (p = getenv("P")) && p == buf + 2;
    buf[2] = '2';
    ok = ok && !strcmp(getenv("P"), "2");
    check(ok, "putenv uses the caller's string itself", "string copied");

    ok = setenv("P", "3", 1) == 0 && !strcmp(getenv("P"), "3") && !strcmp(buf, "P=2");
    check(ok, "setenv over a putenv string leaves the caller's buffer alone",
          "caller-owned string written");

    ok = putenv(repl) == 0 && getenv("P") == repl + 2;
    check(ok, "putenv replaces an existing variable", "old value kept");
    ok = unsetenv("P") == 0 && !getenv("P") && !strcmp(repl, "P=other");
    check(ok, "unsetenv of a putenv string leaves it intact", "caller string changed");
}

/* The allocator's counters: the host counting allocator, or in the guest the
 * libc's own statistics (__rtmux_mallocstats). calls is the allocations made so
 * far, live the bytes held, and bytes (host only) the bytes ever requested.
 */
struct alloc_snap {
    unsigned long long calls, live, bytes;
};

static void alloc_snap(struct alloc_snap *s)
{
#ifdef LIBC_HOST_TEST
    struct host_alloc_stats h;

    host_alloc_snapshot(&h);
    s->calls = h.calls;
    s->live = h.live;
    s->bytes = h.bytes;
#else
    struct __rtmux_mallocstats m;

    __rtmux_mallocstats(&m);
    s->calls = m.allocs;
    s->live = m.bytes_in_use;
    s->bytes = 0;
#endif
}

static size_t env_len(void)
{
    size_t n = 0;

    while (environ && environ[n])
        n++;
    return n;
}

static void test_env_reassign(void)
{
    char *mine[] = {"Z=9", "D=1", "E=2", "D=3", NULL};
    char *p;
    int ok;

    environ = mine;
    ok = !getenv("B") && (p = getenv("Z")) && !strcmp(p, "9");
    ok = ok && setenv("Y", "8", 1) == 0 && environ != mine && mine[4] == NULL &&
         !strcmp(getenv("Z"), "9") && !strcmp(getenv("Y"), "8");
    check(ok, "a reassigned environ is copied on the next change, not grown",
          "program's vector written or variable lost");

    /* An overwrite after reassignment must not reuse a buffer libc does not own
     * through the new vector.
     */
    ok = setenv("Z", "7", 1) == 0 && !strcmp(mine[0], "Z=9") && !strcmp(getenv("Z"), "7");
    check(ok, "setenv over an inherited string leaves it intact", "inherited string written");

    ok = unsetenv("D") == 0 && !getenv("D") && !strcmp(getenv("E"), "2");
    check(ok, "unsetenv removes every entry for a name", "a duplicate survived");

    char *fixed[] = {"K=1", "L=2", NULL};

    environ = fixed;
    ok = unsetenv("NOT_SET") == 0 && environ == fixed;
    check(ok, "unsetenv of an absent name changes nothing", "the vector was copied");
    ok = unsetenv("K") == 0 && !getenv("K") && !strcmp(getenv("L"), "2") && environ != fixed &&
         !strcmp(fixed[0], "K=1") && !strcmp(fixed[1], "L=2") && fixed[2] == NULL;
    check(ok, "unsetenv copies a supplied vector instead of editing it",
          "the program's vector was written");
}

/* Strings libc allocated, reached again through the program's own vectors or
 * through the arguments of the call that replaces its vector: none of them may
 * be freed, and a copied vector keeps libc's ownership, so copying environ in
 * every round of a loop leaves the heap flat too.
 */
static void test_env_alias(void)
{
    static char *empty[] = {NULL};
    static char *joined[] = {"UU=1", NULL};
    char *held[2], *copy[16], *both[3], val[16];
    char *interior, *w = NULL, *name, *astart = NULL;
    struct alloc_snap first = {0}, last;
    int ok;

    /* A pointer into the middle of an allocated string is a live entry. */
    ok = setenv("X", "Y=1", 1) == 0;
    interior = getenv("X");
    held[0] = interior;
    held[1] = NULL;
    environ = held;
    ok = ok && setenv("Q", "1", 1) == 0 && !strcmp(interior, "Y=1") &&
         !strcmp(getenv("Y"), "1") && !strcmp(getenv("Q"), "1");
    check(ok, "a vector entry pointing into a libc string keeps it allocated",
          "the string was freed or lost");

    /* Two entries into one buffer: A=B=old, and B=old inside it. Changing A
     * must not reuse the buffer under B.
     */
    ok = setenv("A", "B=old", 1) == 0;
    for (size_t i = 0; environ[i]; i++) {
        if (!strncmp(environ[i], "A=", 2))
            astart = environ[i];
    }
    both[0] = astart;
    both[1] = astart ? astart + 2 : NULL;
    both[2] = NULL;
    environ = both;
    ok = ok && astart && setenv("A", "x", 1) == 0 && !strcmp(getenv("A"), "x") &&
         getenv("B") && !strcmp(getenv("B"), "old");
    check(ok, "a buffer two entries point into is not reused in place", "B was overwritten");

    /* putenv of a string libc allocated, after the program emptied environ. */
    ok = setenv("W", "v", 1) == 0;
    for (size_t i = 0; environ[i]; i++) {
        if (!strncmp(environ[i], "W=", 2))
            w = environ[i];
    }
    environ = empty;
    ok = ok && w && putenv(w) == 0 && !strcmp(getenv("W"), "v");
    check(ok, "putenv of a libc string survives the replacement of its vector",
          "the argument was freed before it was stored");

    /* unsetenv with a name that points into a libc string. */
    ok = setenv("K1", "UU", 1) == 0;
    name = getenv("K1");
    environ = joined;
    ok = ok && unsetenv(name) == 0 && !getenv("UU") && !strcmp(name, "UU");
    check(ok, "unsetenv with a name inside a libc string reads it to the end",
          "the name was freed under the call");

    /* Copy, assign, change: ownership follows the copy, so nothing leaks. */
    ok = 1;
    for (int i = 0; i < MANY && ok; i++) {
        size_t n = env_len();

        if (n >= sizeof copy / sizeof *copy)
            break;
        memcpy(copy, environ, (n + 1) * sizeof *copy);
        environ = copy;
        snprintf(val, sizeof val, "c%04d", i);
        ok = setenv("CP", val, 1) == 0 && !strcmp(getenv("CP"), val);
        if (i == 1)
            alloc_snap(&first);
    }
    alloc_snap(&last);
    check(ok, "1000 copies of environ with setenv keep every value", "a value was wrong");
    check(last.live == first.live, "1000 copies of environ leave the heap flat",
          "owned strings lost their ownership and leaked");
    /* environ must not outlive the vectors on this frame. */
    environ = NULL;
}

/* A program that keeps assigning environ and then changing it: libc frees the
 * vector it replaces, and the strings it allocated that the new vector does not
 * hold, so the heap stays flat. A string the program's vector does hold (a copy
 * of environ) survives the replacement.
 */
static void test_env_reassign_loop(void)
{
    static char *base[] = {"T=1", "U=2", NULL};
    char *copy[16];
    char val[16];
    int ok = 1;
    struct alloc_snap first = {0}, last;

    for (int i = 0; i < MANY && ok; i++) {
        environ = base;
        snprintf(val, sizeof val, "r%04d", i); /* same length each round */
        ok = setenv("A", val, 1) == 0 && setenv("T", val, 1) == 0 && !strcmp(getenv("A"), val) &&
             !strcmp(getenv("T"), val) && !strcmp(getenv("U"), "2") && !strcmp(base[0], "T=1");
        if (i == 1)
            alloc_snap(&first);
    }
    alloc_snap(&last);
    check(ok, "1000 environ reassignments with setenv keep every value", "a value was wrong");
    check(last.live == first.live, "1000 environ reassignments leave the heap flat",
          "the vectors or strings leaked");

    /* The program copies environ, assigns the copy, and libc replaces it. */
    ok = setenv("KEEP", "kept", 1) == 0 && env_len() < sizeof copy / sizeof *copy;
    if (!ok)
        return;
    memcpy(copy, environ, (env_len() + 1) * sizeof *copy);
    environ = copy;
    ok = setenv("OTHER", "o", 1) == 0 && !strcmp(getenv("KEEP"), "kept") &&
         !strcmp(getenv("OTHER"), "o");
    check(ok, "a string held by the program's own vector survives", "KEEP was freed or lost");

    environ = NULL;
    ok = setenv("N", "1", 1) == 0 && !strcmp(getenv("N"), "1") && !getenv("KEEP");
    check(ok, "environ = NULL then setenv starts afresh", "old variables remained");

    test_env_alias();
}

/* 1000 distinct names: the vector grows geometrically, so environ changes
 * O(log n) times; 1000 same-size overwrites of one name allocate nothing, so
 * neither the string nor environ moves.
 */
static void test_env_growth(void)
{
    char name[16], val[16];
    char **last = NULL;
    unsigned moves = 0;
    int ok = 1;
    struct alloc_snap before, after;

    environ = NULL;
    alloc_snap(&before);
    for (int i = 0; i < MANY && ok; i++) {
        snprintf(name, sizeof name, "V%d", i);
        ok = setenv(name, "x", 1) == 0;
        if (environ != last) {
            moves++;
            last = environ;
        }
    }
    alloc_snap(&after);
    for (int i = 0; i < MANY && ok; i++) {
        snprintf(name, sizeof name, "V%d", i);
        ok = getenv(name) && !strcmp(getenv(name), "x");
    }
    printf("  INFO  %d distinct setenv: environ moved %u times, %llu allocations\n", MANY, moves,
           after.calls - before.calls);
    check(ok && moves <= 8, "1000 distinct setenv grow the vector geometrically",
          "a variable was lost or the vector moved too often");
    /* One string each, plus a vector and a side array per move, at most 4n
     * slots in total over all moves: O(n).
     */
    check(after.calls - before.calls <= MANY + 2 * moves,
          "1000 distinct setenv do O(n) allocations in total", "allocation count is not linear");
    {
        char detail[64];

        snprintf(detail, sizeof detail, "%llu allocations, want 1014", after.calls - before.calls);
        check(after.calls - before.calls == 1014, "1000 distinct setenv make 1014 allocations",
              detail);
    }
#ifdef LIBC_HOST_TEST
    check(after.bytes - before.bytes <= MANY * 8 + 8 * MANY * sizeof(char *) * 2,
          "1000 distinct setenv do O(n) allocated bytes in total", "allocated bytes not linear");
#endif

    char *p = getenv("V0");
    char **vec = environ;
    alloc_snap(&before);
    for (int i = 0; i < MANY && ok; i++) {
        snprintf(val, sizeof val, "%c", 'a' + i % 26);
        ok = setenv("V0", val, 1) == 0 && getenv("V0") == p && environ == vec;
    }
    alloc_snap(&after);
    check(ok, "1000 overwrites of one name reuse its buffer", "the string or vector moved");
    check(after.calls == before.calls && after.live == before.live,
          "1000 overwrites of one name leave the heap flat", "the overwrites allocated");
}

#ifdef LIBC_HOST_TEST
static void test_env_alloc_failure(void)
{
    char *initial[] = {"A=1", NULL};
    int ok = 1;

    /* Fail the string, then the vector, then the side array. */
    for (long k = 0; k < 3; k++) {
        environ = initial;
        host_alloc_fail_after(k);
        errno = 0;
        ok = ok && setenv("B", "2", 1) == -1 && errno == ENOMEM && !getenv("B") &&
             !strcmp(getenv("A"), "1") && environ == initial;
        host_alloc_fail_after(-1);
    }
    environ = initial;
    ok = ok && setenv("B", "2", 1) == 0 && !strcmp(getenv("B"), "2");
    check(ok, "setenv out of memory fails with ENOMEM and changes nothing",
          "environment changed or wrong errno");
}
#else
/* posix_spawn with a NULL envp reads environ under the environment lock: the
 * child must see a variable set just before.
 */
static void test_env_spawn(void)
{
    char *argv[] = {"syscall_testing", "--env-expect", "RTMUX_ENV_PROBE=42", NULL};
    pid_t pid;
    int status = 0, ok;

    ok = setenv("RTMUX_ENV_PROBE", "42", 1) == 0 &&
         posix_spawn(&pid, "/bin/syscall_testing", NULL, NULL, argv, NULL) == 0 &&
         waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0;
    check(ok, "posix_spawn passes a variable set with setenv", "child did not see it");
    (void) unsetenv("RTMUX_ENV_PROBE");
}

int env_expect_child(const char *assignment)
{
    const char *eq = strchr(assignment, '=');
    char name[64];
    const char *v;

    if (!eq || (size_t) (eq - assignment) >= sizeof name)
        return 2;
    memcpy(name, assignment, (size_t) (eq - assignment));
    name[eq - assignment] = '\0';
    v = getenv(name);
    return v && !strcmp(v, eq + 1) ? 0 : 1;
}
#endif

/* A private copy of the environment: a vector and strings of the test's own,
 * which libc never frees. The tests assign environ freely, and libc frees what
 * it replaces, so the vector and strings in place when the suite starts cannot
 * be restored afterwards.
 */
static char **env_copy(void)
{
    size_t n = env_len();
    char **vec = calloc(n + 1, sizeof *vec);

    for (size_t i = 0; vec && i < n; i++) {
        if (!(vec[i] = strdup(environ[i]))) {
            while (i--)
                free(vec[i]);
            free(vec);
            return NULL;
        }
    }
    return vec;
}

void test_env(void)
{
    char **saved = env_copy();

    if (!saved) {
        test_skip("environment", "no memory to save the environment");
        return;
    }
    test_env_basic();
    test_env_putenv();
    test_env_reassign();
    test_env_reassign_loop();
    test_env_growth();
#ifdef LIBC_HOST_TEST
    test_env_alloc_failure();
#endif
    /* Back to the process environment before anything spawns. */
    environ = saved;
#ifndef LIBC_HOST_TEST
    test_env_spawn();
#endif
}
