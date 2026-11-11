/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* sys/calls.h — private kernel/libc kcall-slot ABI.
 *
 * The __KSYS_* enum below numbers the kernel trap slots used by libc and
 * privileged in-tree callers. The values are part of the kernel ↔ libc binary
 * interface: every libc syscall stub at lib/libc/riscv/syscalls.c traps via
 * ECALL with one of these IDs in a7, and the kernel-side dispatch table at
 * kernel/call-table.c is indexed by the same ID.
 *
 * Slot 2 (__KSYS_PRIV) is the entry point of a private sub-namespace shared
 * between the kernel and roottask. Those sub-IDs live in
 * <private/priv-calls.h>.
 */
#ifndef __SYS_CALLS_H_INCLUDED
#define __SYS_CALLS_H_INCLUDED

enum {
    __KSYS_NOP,         /*  0       0x00 */
    __KSYS_TRACE_EVENT, /*  1       0x01 */
    __KSYS_PRIV,        /*  2       0x02 — roottask-privileged subspace */
    __KSYS_DBGPRINT,    /*  3       0x03 */
    __KSYS_SPARE2,      /*  4       0x04 */
    __KSYS_SPARE3,      /*  5       0x05 */
    __KSYS_SPARE4,      /*  6       0x06 */

    __KSYS_SYS_CPUPAGE_GET, /*  7       0x07 */
    __KSYS_SYS_CPUPAGE_SET, /*  8       0x08 */
    __KSYS_SYS_SPARE1,      /*  9       0x09 */

    __KSYS_MSG_CURRENT,              /* 10       0x0a */
    __KSYS_MSG_SENDV,                /* 11       0x0b */
    __KSYS_MSG_SENDVNC,              /* 12       0x0c */
    __KSYS_MSG_ERROR,                /* 13       0x0d */
    __KSYS_MSG_RECEIVEV,             /* 14       0x0e */
    __KSYS_MSG_REPLYV,               /* 15       0x0f */
    __KSYS_MSG_READV,                /* 16       0x10 */
    __KSYS_MSG_WRITEV,               /* 17       0x11 */
    __KSYS_MSG_READWRITEV,           /* 18       0x12 */
    __KSYS_MSG_INFO,                 /* 19       0x13 */
    __KSYS_MSG_SEND_NOTIFICATION,    /* 20       0x14 */
    __KSYS_MSG_DELIVER_EVENT,        /* 21       0x15 */
    __KSYS_MSG_KEYDATA,              /* 22       0x16 */
    __KSYS_MSG_READIOV,              /* 23       0x17 */
    __KSYS_MSG_RECEIVENOTIFICATIONV, /* 24       0x18 */
    __KSYS_MSG_VERIFY_EVENT,         /* 25       0x19 */

    __KSYS_CHANNEL_CREATE = 35, /* 35       0x23 */
    __KSYS_CHANNEL_DESTROY,     /* 36       0x24 */
    __KSYS_CHANNEL_SPARE0,      /* 37       0x25 */
    __KSYS_CHANNEL_SPARE1,      /* 38       0x26 */

    __KSYS_CONNECT_ATTACH,      /* 39       0x27 */
    __KSYS_CONNECT_DETACH,      /* 40       0x28 */
    __KSYS_CONNECT_SERVER_INFO, /* 41       0x29 */
    __KSYS_CONNECT_CLIENT_INFO, /* 42       0x2a */
    __KSYS_CONNECT_FLAGS,       /* 43       0x2b */
    __KSYS_CONNECT_SPARE1,      /* 44       0x2c */
    __KSYS_CONNECT_SPARE2,      /* 45       0x2d */

    __KSYS_THREAD_CREATE,     /* 46       0x2e */
    __KSYS_THREAD_DESTROY,    /* 47       0x2f */
    __KSYS_THREAD_DESTROYALL, /* 48       0x30 */
    __KSYS_THREAD_DETACH,     /* 49       0x31 */
    __KSYS_THREAD_JOIN,       /* 50       0x32 */
    __KSYS_THREAD_CANCEL,     /* 51       0x33 */
    __KSYS_THREAD_CTL,        /* 52       0x34 */
    __KSYS_THREAD_SPARE1,     /* 53       0x35 */
    __KSYS_THREAD_SPARE2,     /* 54       0x36 */

    __KSYS_INTERRUPT_ATTACH_THREAD, /* 55       0x37 */
    __KSYS_INTERRUPT_ATTACH_EVENT,  /* 56       0x38 */
    __KSYS_INTERRUPT_DETACH,        /* 57       0x39 */
    __KSYS_INTERRUPT_WAIT,          /* 58       0x3a */
    __KSYS_INTERRUPT_MASK,          /* 59       0x3b */
    __KSYS_INTERRUPT_UNMASK,        /* 60       0x3c */
    __KSYS_INTERRUPT_SPARE1,        /* 61       0x3d */
    __KSYS_INTERRUPT_SPARE2,        /* 62       0x3e */
    __KSYS_INTERRUPT_SPARE3,        /* 63       0x3f */
    __KSYS_INTERRUPT_SPARE4,        /* 64       0x40 */

    __KSYS_CLOCK_TIME,   /* 65       0x41 */
    __KSYS_CLOCK_SPARE0, /* 66       0x42 */
    __KSYS_CLOCK_PERIOD, /* 67       0x43 */
    __KSYS_CLOCK_ID,     /* 68       0x44 */
    __KSYS_CLOCK_SPARE2, /* 69       0x45 */

    __KSYS_TIMER_CREATE,  /* 70       0x46 */
    __KSYS_TIMER_DESTROY, /* 71       0x47 */
    __KSYS_TIMER_SETTIME, /* 72       0x48 */
    __KSYS_TIMER_INFO,    /* 73       0x49 */
    __KSYS_TIMER_SPARE0,  /* 74       0x4a */
    __KSYS_TIMER_TIMEOUT, /* 75       0x4b */
    __KSYS_TIMER_SPARE1,  /* 76       0x4c */
    __KSYS_TIMER_SPARE2,  /* 77       0x4d */

    __KSYS_SYNC_CREATE,         /* 78       0x4e */
    __KSYS_SYNC_DESTROY,        /* 79       0x4f */
    __KSYS_SYNC_MUTEX_LOCK,     /* 80       0x50 */
    __KSYS_SYNC_MUTEX_UNLOCK,   /* 81       0x51 */
    __KSYS_SYNC_CONDVAR_WAIT,   /* 82       0x52 */
    __KSYS_SYNC_CONDVAR_SIGNAL, /* 83       0x53 */
    __KSYS_SYNC_SEM_POST,       /* 84       0x54 */
    __KSYS_SYNC_SEM_WAIT,       /* 85       0x55 */
    __KSYS_SYNC_CTL,            /* 86       0x56 */
    __KSYS_SYNC_MUTEX_REVIVE,   /* 87       0x57 */

    __KSYS_SCHED_GET,    /* 88       0x58 */
    __KSYS_SCHED_SET,    /* 89       0x59 */
    __KSYS_SCHED_YIELD,  /* 90       0x5a */
    __KSYS_SCHED_INFO,   /* 91       0x5b */
    __KSYS_SCHED_SPARE0, /* 92       0x5c */

    __KSYS_NET_SPARE0, /* 93       0x5d */
    __KSYS_NET_SPARE1, /* 94       0x5e */
    __KSYS_NET_SPARE2, /* 95       0x5f */
    __KSYS_NET_SPARE3, /* 96       0x60 */
    __KSYS_NET_SPARE4, /* 97       0x61 */

    __KSYS_NET_SPARE5, /* 98       0x62 */
    __KSYS_NET_SPARE6, /* 99       0x63 */
    __KSYS_NET_SPARE7, /* 100      0x64 */

    __KSYS_BAD /* 101      0x65 */
};

#endif
