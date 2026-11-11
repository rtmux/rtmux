/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * POSIX system type definitions.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef _SYS_TYPES_H
#define _SYS_TYPES_H
#define __TYPES_H_INCLUDED

#include <sys/cdefs.h>
#include <sys/platform.h>

#include <sys/_offsettypes.h>
#include <sys/_idtypes.h>
#include <sys/_sizetypes.h>
#include <sys/_filetypes.h>
#include <sys/_multibytetypes.h>
#include <sys/_clocktypes.h>
#include <sys/_timetypes.h>
#include <sys/_pthreadtypes.h>

typedef struct {
    uint32_t bits[2];
} sigset_t;

typedef int sig_atomic_t;
typedef void (*sig_t)(int);

union sigval {
    int sival_int;
    void *sival_ptr;
};

struct sigevent {
    int sigev_notify;
    union {
        int sigev_signo;
        int sigev_coid;
        int sigev_id;
        void (*sigev_notify_function)(union sigval);
    };

    union sigval sigev_value;
    union {
        struct {
            short sigev_code;
            short sigev_priority;
        };
        pthread_attr_t *sigev_notify_attributes;
    };
};

typedef struct _siginfo {
    int si_signo;
    int si_code;
    int si_errno;
    union {
        int __pad[7];
        struct {
            pid_t __pid;
            union {
                struct {
                    uid_t __uid;
                    union sigval __value;
                } __kill;
                struct {
                    clock_t __utime;
                    int __status;
                    clock_t __stime;
                } __chld;
            } __pdata;
        } __proc;
        struct {
            int __fltno;
            void *__fltip;
            void *__addr;
            int __bdslot;
        } __fault;
    };
} siginfo_t;

struct sigaction {
    union {
        void (*sa_handler)(int);
        void (*sa_sigaction)(int, siginfo_t *, void *);
    };
    int sa_flags;
    sigset_t sa_mask;
};

#endif
