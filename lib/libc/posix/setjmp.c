/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * lib/libc/posix/setjmp.c — C wrappers for the setjmp / longjmp family.
 *
 * The __asm__ core lives in posix/$(ARCH)/_setjmp.S and provides _setjmp and
 * _longjmp. This file ties them into the POSIX-flavoured siglongjmp / longjmp
 * pair; <setjmp.h> keeps setjmp() / sigsetjmp() as macros so _setjmp captures
 * the caller's frame.
 *
 * Signal-mask semantics
 *
 * RTMux's POSIX signal API is a stub today (see include/signal.h):
 * sigprocmask() exists but the user-level notification-listener thread that
 * backs real signal delivery is not yet in libc. So <setjmp.h>'s sigsetjmp()
 * macro records an empty mask and siglongjmp falls straight through to
 * _longjmp. When the two-thread signal model lands, this file is the one place
 * to teach about restoring the mask.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <setjmp.h>

/* siglongjmp — restore the saved mask if env requested it, then jump. Right now
 * the mask side is a no-op for the same reason.
 */
__attribute__((__noreturn__)) void siglongjmp(sigjmp_buf env, int val)
{
    /* Future:
     *     if (env[0].__flg) {
     *         sigset_t saved;
     *         memcpy(&saved, env[0].__msk, sizeof(saved));
     *         sigprocmask(SIG_SETMASK, &saved, NULL);
     *     }
     */
    _longjmp(env, val);
}

/* Function-form longjmp. Callable as a real symbol from code that #undefs the
 * longjmp() macro or takes its address. Just delegates to siglongjmp — no
 * register-save concerns since longjmp doesn't return.
 *
 * Function-form setjmp() and sigsetjmp() cannot be written in plain C: invoking
 * _setjmp from a C body creates an extra stack frame that _setjmp captures, and
 * the resulting longjmp would resume into a frame that has already been unwound
 * (undefined behavior). The setjmp() / sigsetjmp() macros in <setjmp.h>
 * sidestep this by inlining the prolog and calling _setjmp directly from the
 * caller's frame; that path is the one to use. If a real symbol is ever needed
 * it must be written in assembly.
 */

#undef longjmp

__attribute__((__noreturn__)) void longjmp(jmp_buf env, int val)
{
    siglongjmp(env, val);
}
