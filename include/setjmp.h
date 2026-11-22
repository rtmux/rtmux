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

extern int _setjmp(jmp_buf __env) __attribute__((__returns_twice__));
extern void _longjmp(jmp_buf __env, int __val) __attribute__((__noreturn__));

extern void longjmp(jmp_buf __env, int __val) __attribute__((__noreturn__));

extern int setjmp(jmp_buf) __attribute__((__returns_twice__));
extern int sigsetjmp(jmp_buf __env, int __msk) __attribute__((__returns_twice__));

typedef jmp_buf sigjmp_buf;
extern void siglongjmp(sigjmp_buf __env, int __val)
    __attribute__((__noreturn__));

__END_DECLS
#endif

#endif
