/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* kernel/options.h — Boot-time kernel options.
 *
 * All command-line tunables live in "struct kernel_options kopt". Any code
 * referencing kopt.foo immediately shows it's a boot tunable.
 *
 * License: Apache 2.0
 */

#ifndef _KERNEL_OPTIONS_H
#define _KERNEL_OPTIONS_H

#include <stdbool.h>
#include <sys/types.h>
#include <sys/rtmux-core.h> /* LIMITS_NUM */

struct kernel_options {
    /* Startup options (set during early boot, before kernel_init) */
    /* Timebase (RISC-V rdtime) tick rate from FDT cpus/timebase-frequency,
     * not the CPU clock. Feeds qtime->cycles_per_sec; see sys/syspage.h.
     */
    unsigned long timebase_freq; /* -f <timebase>[,<timer>] */
    unsigned long timer_freq;    /* -f <timebase>,<timer> */
    size_t reserved_size;        /* -R <size>[,<align>] */
    unsigned reserved_align;     /* -R <size>,<align>   */
    unsigned max_cpus;           /* -P <num>            */
    int loglevel;                /* -v (repeatable)     */

    /* Kernel options (set in kernel_init, before roottask) */
    int nohalt;                /* -h  */
    bool nopreempt;            /* -p  */
    int align_fault;           /* default 0 (silent)  */
    bool auto_trace;           /* -T  auto-start kernel tracing */
    bool disable_timer;        /* --disable-timer  hard-off the S-mode timer */
    unsigned max_fds;          /* -F <count>          */
    unsigned fd_close_timeout; /* not wired (was -T)  */
    unsigned priv_prio;        /* -Z <priority>       */
    mode_t procfs_umask;       /* -u <umask>          */
    long limits_max[LIMITS_NUM]; /* -l <csv>        */
    size_t heap_pregrow;         /* -H <size>           */
    size_t ram_limit;            /* --limit-ram=<size>  */
    const char *mem_config;      /* -m <config>         */
};

extern struct kernel_options kopt;

/* Static initializer with defaults */
#define KERNEL_OPTIONS_INIT              \
    {                                    \
        .timebase_freq = 0,              \
        .timer_freq = 0,                 \
        .reserved_size = 0,              \
        .reserved_align = 0,             \
        .max_cpus = ~0U,                 \
        .loglevel = 0,                   \
        .nohalt = 0,                     \
        .nopreempt = 0,                  \
        .align_fault = 0,                \
        .auto_trace = 0,                 \
        .disable_timer = 0,              \
        .max_fds = 1000,                 \
        .fd_close_timeout = 30,          \
        .priv_prio = 64,                 \
        .procfs_umask = 0022,            \
        .limits_max = {0},               \
        .heap_pregrow = 8 * 1024 * 1024, \
        .ram_limit = 0,                  \
        .mem_config = "",                \
    }

#endif /* _KERNEL_OPTIONS_H */
