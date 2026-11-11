/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* sched.h */
#ifndef _SCHED_H_INCLUDED
#define _SCHED_H_INCLUDED

#ifndef __TYPES_H_INCLUDED
#include <sys/types.h>
#endif

#ifndef _TIME_H_INCLUDED
#include <time.h>
#endif

#ifndef __PLATFORM_H_INCLUDED
#include <sys/platform.h>
#endif

__BEGIN_DECLS

#define SCHED_FIFO 1
#define SCHED_RR 2
#define SCHED_OTHER 3
#define SCHED_SPORADIC 4

/* 1003.1b */
extern int sched_getparam(pid_t __pid, struct sched_param *__parms);
extern int sched_getscheduler(pid_t __pid);
extern int sched_setparam(pid_t __pid, const struct sched_param *__parms);
extern int sched_setscheduler(pid_t __pid,
                              int __policy,
                              const struct sched_param *__parms);
extern int sched_yield(void);
extern int sched_rr_get_interval(pid_t __pid, struct timespec *__t);
extern int sched_get_priority_min(int __alg);
extern int sched_get_priority_max(int __alg);

__END_DECLS

#endif
