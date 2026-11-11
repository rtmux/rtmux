/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef _SYS_SCHED_OPS_H_INCLUDED
#define _SYS_SCHED_OPS_H_INCLUDED

/* Private sched_set operation selectors. Public <sched.h> only publishes POSIX
 * scheduling policies.
 */
#define RTMUX_SCHED_NOCHANGE 0
#define RTMUX_SCHED_ADJTOHEAD 5
#define RTMUX_SCHED_ADJTOTAIL 6
#define RTMUX_SCHED_SETPRIO 7

#endif
