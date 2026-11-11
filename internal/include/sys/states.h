/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* sys/states.h */
#ifndef __STATES_H_INCLUDED
#define __STATES_H_INCLUDED

#ifndef __PLATFORM_H_INCLUDED
#include <sys/platform.h>
#endif

__BEGIN_DECLS

/* Define the states of a thread THREAD.state */
enum _THREAD_STATE {
    STATE_DEAD,    /* 0    0x00 */
    STATE_RUNNING, /* 1    0x01 */
    STATE_READY,   /* 2    0x02 */
    STATE_STOPPED, /* 3    0x03 */

    STATE_SEND,    /* 4    0x04 */
    STATE_RECEIVE, /* 5    0x05 */
    STATE_REPLY,   /* 6    0x06 */

    STATE_STACK,      /* 7    0x07 */
    STATE_WAITTHREAD, /* 8    0x08 */
    STATE_WAITPAGE,   /* 9    0x09 */

    STATE_SIGSUSPEND,  /* 10   0x0a */
    STATE_SIGWAITINFO, /* 11   0x0b */
    STATE_NANOSLEEP,   /* 12   0x0c */
    STATE_MUTEX,       /* 13   0x0d */
    STATE_CONDVAR,     /* 14   0x0e */
    STATE_JOIN,        /* 15   0x0f */
    STATE_INTR,        /* 16   0x10 */
    STATE_SEM,         /* 17   0x11 */
    STATE_WAITCTX,     /* 18   0x12 */

    STATE_NET_SEND,  /* 19   0x13 */
    STATE_NET_REPLY, /* 20   0x14 */

    STATE_MAX =
        24 /* This cannot be changed. It is the highest we can support */
};

__END_DECLS
#endif
