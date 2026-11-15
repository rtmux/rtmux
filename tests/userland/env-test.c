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
#ifdef LIBC_HOST_TEST
    struct host_alloc_stats before, after;
#endif

    environ = NULL;
#ifdef LIBC_HOST_TEST
    host_alloc_snapshot(&before);
#endif
    for (int i = 0; i < MANY && ok; i++) {
        snprintf(name, sizeof name, "V%d", i);
        ok = setenv(name, "x", 1) == 0;
        if (environ != last) {
            moves++;
            last = environ;
        }
    }
    for (int i = 0; i < MANY && ok; i++) {
        snprintf(name, sizeof name, "V%d", i);
        ok = getenv(name) && !strcmp(getenv(name), "x");
    }
    printf("  INFO  %d distinct setenv: environ moved %u times\n", MANY, moves);
    check(ok && moves <= 8, "1000 distinct setenv grow the vector geometrically",
          "a variable was lost or the vector moved too often");
#ifdef LIBC_HOST_TEST
    host_alloc_snapshot(&after);
    printf("  INFO  %d distinct setenv: %lu allocations, %lu bytes\n", MANY,
           after.calls - before.calls, after.bytes - before.bytes);
    /* One string each, plus a vector and a side array per move, at most 4n
     * slots in total over all moves: O(n).
     */
    check(after.calls - before.calls <= MANY + 2 * moves &&
              after.bytes - before.bytes <= MANY * 8 + 8 * MANY * sizeof(char *) * 2,
          "1000 distinct setenv do O(n) allocation in total", "allocation is not linear");
#endif

    char *p = getenv("V0");
    char **vec = environ;
#ifdef LIBC_HOST_TEST
    host_alloc_snapshot(&before);
#endif
    for (int i = 0; i < MANY && ok; i++) {
        snprintf(val, sizeof val, "%c", 'a' + i % 26);
        ok = setenv("V0", val, 1) == 0 && getenv("V0") == p && environ == vec;
    }
    check(ok, "1000 overwrites of one name reuse its buffer", "the string or vector moved");
#ifdef LIBC_HOST_TEST
    host_alloc_snapshot(&after);
    check(after.calls == before.calls && after.live == before.live,
          "1000 overwrites of one name leave the heap flat", "the overwrites allocated");
#endif
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

void test_env(void)
{
    char **saved = environ;

    test_env_basic();
    test_env_putenv();
    test_env_reassign();
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
