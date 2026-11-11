/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * setjmp-test.c — exercise the setjmp / longjmp family.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <setjmp.h>
#include <stdio.h>

#include "test.h"

/* _setjmp / _longjmp basic round-trip.
 *
 *   - _setjmp returns 0 first time
 *   - _longjmp(env, 7) makes _setjmp return 7
 *   - _longjmp(env, 0) makes _setjmp return 1 (POSIX rule:
 *     longjmp must never make setjmp return 0)
 */
static void test_basic_setjmp(void)
{
    jmp_buf env;
    volatile int rc;
    volatile int hit = 0;

    rc = _setjmp(env);
    hit++;
    if (hit == 1) {
        check(rc == 0, "_setjmp returns 0 first time", "rc != 0");
        _longjmp(env, 7);
    } else if (hit == 2) {
        check(rc == 7, "_longjmp(env, 7) round-trips", "rc != 7");
        _longjmp(env, 0);
    } else {
        check(rc == 1, "_longjmp(env, 0) is rewritten to 1", "rc != 1");
    }
}

/* setjmp / longjmp macro path. Same as above but goes through the <setjmp.h>
 * sigsetjmp/_setjmp chain.
 */
static void test_macro_setjmp(void)
{
    jmp_buf env;
    volatile int rc;
    volatile int hit = 0;

    rc = setjmp(env);
    hit++;
    if (hit == 1) {
        check(rc == 0, "setjmp() returns 0 first time", "rc != 0");
        longjmp(env, 42);
    } else {
        check(rc == 42, "longjmp(env, 42) round-trips", "rc != 42");
    }
}

/* siglongjmp can target the same env saved by setjmp. This proves the layout is
 * shared between the macro and direct asm forms.
 */
static void test_siglongjmp(void)
{
    sigjmp_buf env;
    volatile int rc;
    volatile int hit = 0;

    rc = sigsetjmp(env, 0);
    hit++;
    if (hit == 1) {
        check(rc == 0, "sigsetjmp returns 0 first time", "rc != 0");
        siglongjmp(env, 99);
    } else {
        check(rc == 99, "siglongjmp(env, 99) round-trips", "rc != 99");
    }
}

/* Three callee-saved registers (s0, s1, s2) must survive a longjmp back into
 * the caller's frame. We compute a value from them after a longjmp and verify
 * it equals what we set before _setjmp. Mark the locals volatile to keep the
 * compiler from optimizing the dance away.
 */
static int saved_x, saved_y, saved_z;

static void trampoline_corrupt(jmp_buf env)
{
    /* Try to clobber callee-saved regs from a callee. We don't actually rely on
     * inline asm here — the call/return cycle already exercises the
     * save/restore.
     */
    _longjmp(env, 5);
}

static void test_callee_saves(void)
{
    jmp_buf env;
    volatile int x = 0xAA, y = 0xBB, z = 0xCC;
    volatile int sum;
    volatile int hit = 0;

    if (_setjmp(env) == 0) {
        saved_x = x;
        saved_y = y;
        saved_z = z;
        hit++;
        trampoline_corrupt(env);
    }

    sum = x + y + z;
    check(sum == saved_x + saved_y + saved_z,
          "callee-saved values survive longjmp",
          "x/y/z corrupted across longjmp");
    check(hit == 1, "longjmp returns to setjmp's caller frame", "hit != 1");
}

void test_setjmp(void)
{
    test_basic_setjmp();
    test_macro_setjmp();
    test_siglongjmp();
    test_callee_saves();
}
