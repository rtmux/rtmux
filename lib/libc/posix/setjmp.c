/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <setjmp.h>
#include <signal.h>
#include <string.h>

/* Older headers set __flg to 1 without saving a signal mask. */
enum { JMP_SAVED_SIGMASK = 2 };

/* The assembly entry saves the caller's frame after this helper returns. */
void __jmp_save_mask(jmp_buf env, int save)
    __attribute__((visibility("hidden")));
void __jmp_save_mask(jmp_buf env, int save)
{
    sigset_t mask;
    _Static_assert(sizeof(mask) <= sizeof(env[0].__msk),
                   "jmp_buf mask storage");
    env[0].__flg = 0;
    if (save && sigprocmask(SIG_SETMASK, NULL, &mask) == 0) {
        memcpy(env[0].__msk, &mask, sizeof(mask));
        env[0].__flg = JMP_SAVED_SIGMASK;
    }
}

void siglongjmp(sigjmp_buf env, int val)
{
    if (env[0].__flg == JMP_SAVED_SIGMASK) {
        sigset_t mask;
        memcpy(&mask, env[0].__msk, sizeof(mask));
        sigprocmask(SIG_SETMASK, &mask, NULL);
    }
    _longjmp(env, val);
}

void longjmp(jmp_buf env, int val)
{
    siglongjmp(env, val);
}
