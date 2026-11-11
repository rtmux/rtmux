/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef __KERNEL_DEBUG
#define __KERNEL_DEBUG

#include <sys/debug.h>

/* Internal data structure used between procfs and kernel extension */
typedef struct debug_breaklist {
    int offset;      /* Offset at which to start */
    int count;       /* IN: Number to fill OUT: Number filled */
    int total_count; /* Total number of breakpoints (only filled in for offset
                        == 0)
                        */
    debug_break_t *breakpoints; /* Space for at least count breakpoints */
} debug_breaklist_t;


union rtmux_debug_data {
    debug_process_t process;
    debug_thread_t thread;
    debug_greg_t greg;
    debug_fpreg_t fpreg;
    debug_altreg_t altreg;
    debug_perfreg_t perfreg;
    debug_run_t run;
    debug_break_t brk;
    debug_breaklist_t brklist;
    uint32_t flags;
};

enum rtmux_debug_request {
    RTMUX_DEBUG_PROCESS_INFO,  /* pid,na,debug_process_t */
    RTMUX_DEBUG_THREAD_INFO,   /* pid,tid,debug_thread_t */
    RTMUX_DEBUG_GET_GREG,      /* pid,tid,debug_greg_t */
    RTMUX_DEBUG_SET_GREG,      /* pid,tid,debug_greg_t */
    RTMUX_DEBUG_GET_FPREG,     /* pid,tid,debug_fpreg_t */
    RTMUX_DEBUG_SET_FPREG,     /* pid,tid,debug_fpreg_t */
    RTMUX_DEBUG_STOP,          /* pid,na,debug_thread_t */
    RTMUX_DEBUG_RUN,           /* pid,na,debug_run_t */
    RTMUX_DEBUG_CURTHREAD,     /* pid,tid,NULL */
    RTMUX_DEBUG_FREEZE,        /* pid,tid,NULL */
    RTMUX_DEBUG_THAW,          /* pid,tid,NULL */
    RTMUX_DEBUG_BREAK,         /* pid,na,debug_break_t */
    RTMUX_DEBUG_SET_FLAG,      /* pid,na,uint32_t */
    RTMUX_DEBUG_CLEAR_FLAG,    /* pid,na,uint32_t */
    RTMUX_DEBUG_GET_ALTREG,    /* pid,tid,debug_altreg_t */
    RTMUX_DEBUG_SET_ALTREG,    /* pid,tid,debug_altreg_t */
    RTMUX_DEBUG_GET_PERFREG,   /* pid,tid, */
    RTMUX_DEBUG_SET_PERFREG,   /* pid,tid, */
    RTMUX_DEBUG_GET_BREAKLIST, /* pid,na,debug_break_list_t */
};

#endif
