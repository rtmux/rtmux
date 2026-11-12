/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* test.h — shared declarations for the syscall test suite. */

#ifndef _RTMUX_TEST_H
#define _RTMUX_TEST_H

#include <pthread.h>

struct pshared_region {
    pthread_mutex_t mtx;
    pthread_cond_t cv;
    volatile long counter;
    volatile int state;
};
#define PSHARED_SHM_NAME "/rtmux-pshared-mtx"
#define PSHARED_ITERS 300
#define PSHARED_ITERS_ARG "300"

void test_ok(const char *name);
void test_fail(const char *name, const char *detail);
void check(int cond, const char *name, const char *detail);
/* A known failure: `expected` is its stable id, `observed` the id seen now (NULL
 * when the case passed). Matching is a GAP; a pass or another failure is a FAIL. */
void test_gap(const char *name, const char *anchor, const char *expected, const char *observed);
/* A case that did not run; the reason is mandatory. */
void test_skip(const char *name, const char *reason);

void test_timers(void);
int rlimit_cpu_child(void);
int rlimit_inherit_child(void);
int rlimit_inherit_grandchild(void);
void test_sync(void);
int sync_pi_large_pid_child(void);
void test_printf(void);
void test_setjmp(void);
void test_signals(void);
void test_vfs(void);
int spawn_echo_child(const char *tag);
void test_posix_cap_boundary(void);
int posix_cap_boundary_child(void);
void test_notifications(void);
void test_serial_unblock(void);
int serial_unblock_child(int announce);
#define DISCONNECT_SKIP 77
int disconnect_under_alloc_failure(void);
int disconnect_child(const char *pid_arg, const char *chid_arg);
void test_posix_options(void);
void test_capgate(void);
void test_leak(void);
void test_gauges(void);
int killspawn_child(const char *fd_arg);
void test_orphan_send_kill(void);
int gauge_child(int argc, char **argv);
int leak_lap_check(int lap);
void leak_suite_delta(int lap, const char *suite);
/* Malformed-user-pointer test helpers, shared by the negative suites.
 *
 * TEST_KERNEL_ADDR is canonical Sv39 supervisor space, above every user
 * boundary_addr, so a kernel range check must reject it outright.
 * test_unmapped_user_addr returns an address that is inside the user half (so
 * it passes a range check) but unmapped (so only an access probe can catch it),
 * or NULL if the mapping could not be arranged.
 */
#define TEST_KERNEL_ADDR 0xffffffc000000000UL

void *test_unmapped_user_addr(void);

void test_trace_negative(void);
void test_spawn_concurrent(int force);
void test_user_ptr_negative(void);
int capgate_child_connect(const char *chid_arg);
int capgate_child_dup_attack(int argc, char **argv);
int capgate_child_dup_replay(const char *hex);
int nvme_raw_child(void);
int sys_log_child(void);
void test_sys_log_wrap(void);
int fp_isolation_child(void);
int cancel_tls_child(void);
int kill_child(int argc, char **argv);

#endif
