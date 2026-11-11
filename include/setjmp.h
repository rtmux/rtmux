/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* setjmp.h */
#ifndef _SETJMP_H_INCLUDED

#ifndef __PLATFORM_H_INCLUDED
#include <sys/platform.h>
#endif

#define _SETJMP_H_INCLUDED

#ifndef _SETJMP_H_DECLARED
#define _SETJMP_H_DECLARED


__BEGIN_DECLS

typedef struct _jmp_buf {
    union {
        unsigned int __savearea[__JMPBUFSIZE];
        __jmpbufalign __alignment;
    } __jmpbuf_un;
    int __flg;
    long __msk[2];
} jmp_buf[1];

extern int _setjmp(jmp_buf __env);
extern void _longjmp(jmp_buf __env, int __val) __attribute__((__noreturn__));

extern void longjmp(jmp_buf __env, int __val) __attribute__((__noreturn__));

/* POSIX requires a setjmp() function prototype but suppressing the macro
 * definition or defining an external identifier with the name setjmp will cause
 * undefined behavior
 */
extern int setjmp(jmp_buf);
extern int sigsetjmp(jmp_buf __env, int __msk);

typedef jmp_buf sigjmp_buf;
extern void siglongjmp(sigjmp_buf __env, int __val)
    __attribute__((__noreturn__));

#define sigsetjmp(__env, __savemask)                                   \
    ((__env)[0].__flg = (__savemask) ? 1 : 0, (__env)[0].__msk[0] = 0, \
     (__env)[0].__msk[1] = 0, _setjmp(__env))
#define setjmp(__env) sigsetjmp(__env, 1)
#define longjmp(__env, __val) siglongjmp((__env), (__val))

__END_DECLS
#endif

#endif
