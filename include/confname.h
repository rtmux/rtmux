/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* confname.h */

#ifndef _CONFNAME_H_INCLUDED
#define _CONFNAME_H_INCLUDED

#define _CS_NONE 0 /* End of lists in syspage */
/* 1-256 are Unix International GET commands */
#define _CS_PATH 1 /* default path to find system utilities */

#define _CS_HOSTNAME                                                         \
    2                 /* Name of this node within the communications network \
                       */
#define _CS_RELEASE 3 /* Current release level of this implementation */
#define _CS_VERSION 4 /* Current version of this release */
#define _CS_MACHINE \
    5 /* Name of the hardware type on which the system is running */
#define _CS_ARCHITECTURE 6 /* Name of the instructions set architechure */
#define _CS_VENDOR 513     /* Start of vendor specific commands */


/* Symbolic constants for sysconf */
#define _SC_ARG_MAX 1
#define _SC_CHILD_MAX 2
#define _SC_CLK_TCK 3
#define _SC_NGROUPS_MAX 4
#define _SC_OPEN_MAX 5
#define _SC_JOB_CONTROL 6
#define _SC_SAVED_IDS 7
#define _SC_VERSION 8
#define _SC_LOGNAME_MAX 10
#define _SC_PAGESIZE 11
#define _SC_PAGE_SIZE _SC_PAGESIZE
#define _SC_STREAM_MAX 13
#define _SC_TZNAME_MAX 14

#define _SC_AIO_LISTIO_MAX 15
#define _SC_AIO_MAX 16
#define _SC_AIO_PRIO_DELTA_MAX 17
#define _SC_DELAYTIMER_MAX 18
#define _SC_MQ_OPEN_MAX 19
#define _SC_MQ_PRIO_MAX 20
#define _SC_RTSIG_MAX 21
#define _SC_SEM_NSEMS_MAX 22
#define _SC_SEM_VALUE_MAX 23
#define _SC_SIGQUEUE_MAX 24
#define _SC_TIMER_MAX 25

#define _SC_ASYNCHRONOUS_IO 26
#define _SC_FSYNC 27
#define _SC_MAPPED_FILES 28
#define _SC_MEMLOCK 29
#define _SC_MEMLOCK_RANGE 30
#define _SC_MEMORY_PROTECTION 31
#define _SC_MESSAGE_PASSING 32
#define _SC_PRIORITIZED_IO 33
#define _SC_PRIORITY_SCHEDULING 34
#define _SC_REALTIME_SIGNALS 35
#define _SC_SEMAPHORES 36
#define _SC_SHARED_MEMORY_OBJECTS 37
#define _SC_SYNCHRONIZED_IO 38
#define _SC_TIMERS 39

#define _SC_GETGR_R_SIZE_MAX 40
#define _SC_GETPW_R_SIZE_MAX 41
#define _SC_THREAD_DESTRUCTOR_ITERATIONS 43
#define _SC_THREAD_KEYS_MAX 44
#define _SC_THREAD_STACK_MIN 45
#define _SC_THREAD_THREADS_MAX 46

#define _SC_THREADS 48
#define _SC_THREAD_ATTR_STACKADDR 49
#define _SC_THREAD_ATTR_STACKSIZE 50
#define _SC_THREAD_PRIORITY_SCHEDULING 51
#define _SC_THREAD_PRIO_INHERIT 52
#define _SC_THREAD_PRIO_PROTECT 53
#define _SC_THREAD_PROCESS_SHARED 54
#define _SC_THREAD_SAFE_FUNCTIONS 55

#define _SC_2_CHAR_TERM 56
#define _SC_2_C_BIND 57
#define _SC_2_C_DEV 58
#define _SC_2_C_VERSION 59
#define _SC_2_LOCALEDEF 62
#define _SC_2_SW_DEV 63
#define _SC_2_UPE 64
#define _SC_2_VERSION 65
#define _SC_ATEXIT_MAX 66
#define _SC_AVPHYS_PAGES 67
#define _SC_BC_BASE_MAX 68
#define _SC_BC_DIM_MAX 69
#define _SC_BC_SCALE_MAX 70
#define _SC_BC_STRING_MAX 71
#define _SC_CHARCLASS_NAME_MAX 72
#define _SC_CHAR_BIT 73
#define _SC_CHAR_MAX 74
#define _SC_CHAR_MIN 75
#define _SC_COLL_WEIGHTS_MAX 76
#define _SC_EQUIV_CLASS_MAX 77
#define _SC_EXPR_NEST_MAX 78
#define _SC_INT_MAX 79
#define _SC_INT_MIN 80
#define _SC_LINE_MAX 81
#define _SC_LONG_BIT 82
#define _SC_MB_LEN_MAX 83
#define _SC_NL_ARGMAX 84
#define _SC_NL_LANGMAX 85
#define _SC_NL_MSGMAX 86
#define _SC_NL_NMAX 87
#define _SC_NL_SETMAX 88
#define _SC_NL_TEXTMAX 89
#define _SC_NPROCESSORS_CONF 90
#define _SC_NPROCESSORS_ONLN 91
#define _SC_NZERO 92
#define _SC_PHYS_PAGES 93
#define _SC_POLL 104
#define _SC_RE_DUP_MAX 105
#define _SC_SCHAR_MAX 106
#define _SC_SCHAR_MIN 107
#define _SC_SELECT 108
#define _SC_SHRT_MAX 109
#define _SC_SHRT_MIN 110
#define _SC_SSIZE_MAX 111
#define _SC_T_IOV_MAX 112
#define _SC_UCHAR_MAX 113
#define _SC_UINT_MAX 114
#define _SC_UIO_MAXIOV 115
#define _SC_ULONG_MAX 116
#define _SC_USHRT_MAX 117
#define _SC_WORD_BIT 118

/* POSIX 1003.1d-1999 */
#define _SC_ADVISORY_INFO 131
#define _SC_CPUTIME 132
#define _SC_SPAWN 133
#define _SC_SPORADIC_SERVER 134
#define _SC_THREAD_CPUTIME 135
#define _SC_THREAD_SPORADIC_SERVER 136
#define _SC_TIMEOUTS 137

/* POSIX 1003.1j-2000 */
#define _SC_BARRIERS 138
#define _SC_CLOCK_SELECTION 139
#define _SC_MONOTONIC_CLOCK 140
#define _SC_READER_WRITER_LOCKS 141
#define _SC_SPIN_LOCKS 142
#define _SC_TYPED_MEMORY_OBJECTS 143

/* POSIX 1003.1-2001 */
#define _SC_2_PBS 148
#define _SC_2_PBS_ACCOUNTING 149
#define _SC_2_PBS_CHECKPOINT 150
#define _SC_2_PBS_LOCATE 151
#define _SC_2_PBS_MESSAGE 152
#define _SC_2_PBS_TRACK 153
#define _SC_HOST_NAME_MAX 154
#define _SC_IOV_MAX 155
#define _SC_IPV6 156
#define _SC_RAW_SOCKETS 157
#define _SC_REGEXP 158
#define _SC_SHELL 159
#define _SC_SYMLOOP_MAX 161
/* Symbolic constants for pathconf fpathconf */
#define _PC_LINK_MAX 1
#define _PC_MAX_CANON 2
#define _PC_MAX_INPUT 3
#define _PC_NAME_MAX 4
#define _PC_PATH_MAX 5
#define _PC_PIPE_BUF 6
#define _PC_NO_TRUNC 7
#define _PC_VDISABLE 8
#define _PC_CHOWN_RESTRICTED 9

#define _PC_ASYNC_IO 12
#define _PC_PRIO_IO 13
#define _PC_SYNC_IO 14

#define _PC_FILESIZEBITS 16

#define _PC_SYMLINK_MAX 17
#define _PC_SYMLOOP_MAX 18

#define _PC_2_SYMLINKS 20
#define _PC_ALLOC_SIZE_MIN 21
#define _PC_REC_INCR_XFER_SIZE 22
#define _PC_REC_MAX_XFER_SIZE 23
#define _PC_REC_MIN_XFER_SIZE 24
#define _PC_REC_XFER_ALIGN 25

#endif
