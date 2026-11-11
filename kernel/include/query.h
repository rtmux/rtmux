/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define _QUERY_SOULS 0 /* Query soul vector, index1 as follows */
#define _QUERY_SOULS_PROCESS 0
#define _QUERY_SOULS_THREAD 1
#define _QUERY_SOULS_CHANNEL 2
#define _QUERY_SOULS_CONNECT 3
#define _QUERY_SOULS_NOTIFICATION 4
#define _QUERY_SOULS_INTERRUPT 5
#define _QUERY_SOULS_SYNC 6
#define _QUERY_SOULS_SIGTABLE 7
#define _QUERY_SOULS_TIMER 8
#define _QUERY_SOULS_INTREVENT 9
#define _QUERY_SOULS_FPU 10
#define _QUERY_SOULS_CLIENT 11
#define _QUERY_SOULS_CREDENTIAL 12
#define _QUERY_SOULS_LIMITS 13
#define _QUERY_SOULS_VTHREAD 14
#define _QUERY_SOULS_NUM 15
#define _QUERY_PROCESS 1        /* Query a process, subtype follows */
#define _QUERY_PROCESS_VECTOR 0 /* Just return process vector */
#define _QUERY_PROCESS_CHANCONS 1
#define _QUERY_PROCESS_FDCONS 2
#define _QUERY_PROCESS_THREADS 3
#define _QUERY_PROCESS_TIMERS 4
#define _QUERY_PROCESS_SYNCS 5
#define _QUERY_INTERRUPT 2
#define _QUERY_VTHREAD 3
#define _QUERY_MEMORY_PARTITION 4
#define _QUERY_SCHEDULER_PARTITION 5
