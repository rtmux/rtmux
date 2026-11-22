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
#include <stdatomic.h>
#include <stdint.h>
#include "outcome.h"

struct pshared_region {
    pthread_mutex_t mtx;
    pthread_cond_t cv;
    volatile long counter;
    volatile int state;
};
#define PSHARED_SHM_NAME "/rtmux-pshared-mtx"
#define PSHARED_ITERS 300
#define PSHARED_ITERS_ARG "300"

/* Counters behind the hooks below (report.c). */
extern struct test_counts suite_counts;

void test_ok(const char *name);
void test_fail(const char *name, const char *detail);
void check(int cond, const char *name, const char *detail);

/* A known failure: `expected` is its stable id, `observed` the id seen now
 * (NULL when the case passed). Matching is a GAP; a pass or another failure is
 * a FAIL.
 */
void test_gap(const char *name, const char *anchor, const char *expected, const char *observed);
/* A case that did not run; the reason is mandatory. */
void test_skip(const char *name, const char *reason);

void test_timers(void);
void test_nanosleep_1ns_storm(void);
int rlimit_cpu_child(void);
int rlimit_inherit_child(void);
int rlimit_inherit_grandchild(void);
void test_mutex_owner_death(void);
void named_sem_timedwait_loop(int n);
void sync_path_cost(void);
void test_sync(void);
void test_pi_owned_turnstiles(void);
void test_pi_two_mutex_cycle_detects_deadlock(void);
void test_cond_reacquire_cycle(void);
void test_cond_reacquire_above_ceiling(void);
void test_cancel_before_block(void);
void test_block_kill_cancel(void);
void test_prio_loan(void);
int sync_pi_large_pid_child(void);
void test_printf(void);
void test_setjmp(void);
void test_time(void);
void test_strftime(void);
void test_qsort(void);
void test_getopt(void);
void test_env(void);
void test_malloc_stats(void);
int env_expect_child(const char *assignment);
void test_signals(void);
void test_vfs(void);
void test_native_file_io(void);
void test_native_io_boundaries(void);
int spawn_echo_child(const char *tag);
int spawn_bigargs_child(int argc, char *argv[]);
void test_posix_cap_boundary(void);
int posix_cap_boundary_child(void);
void test_notifications(void);
void test_serial_unblock(void);
void test_tty_discipline(int fd, void (*inject_byte)(int));
void test_serial_poll(void);
void serial_inject(int c);
void test_sigthread_prio_stable(void);
int serial_priority_spinner(void);
void test_console_poll(void);
void test_device_identity(const char *second_mount);
void console_inject(int c);
void sleep_ms(long ms);
int serial_unblock_child(int announce);
int disconnect_under_alloc_failure(void);
struct stall_watch {
    pid_t pid;
    int events, release;
};
int stall_watch_start(struct stall_watch *, const char *site);
void stall_watch_release(struct stall_watch *);
void stall_watch_finish(struct stall_watch *);
int stall_watch_entered(struct stall_watch *);
int inject_run(const char *a1, const char *a2, const char *a3);
int test_thread_blocked(pthread_t tid, int state);
int test_chan_open(unsigned flags, int *chid, int *coid);
int test_wait_flag(atomic_int *flag, int want, int ms);
void test_receive_within(uint64_t ns);
int test_reply_claim(void);
int test_kill_slow_reap(void);
int slow_reap_child(void);
int test_xfer_resume(void);
int test_cap_step(void);
int test_rcvid_reuse(void);
int test_remote_ipc(void);
void test_reply_fault(void);
int test_pin_mask(unsigned mask);
int condloan_create(pthread_t *tid, int prio, void *(*fn)(void *), void *arg);
int sched_set_error_after_lowering(void);
int loan_settle_context(void);
int sched_set_error_while_migrating(void);
int condvar_reacquire_alloc_failure(void);
int mutex_waiting_republish(void);
int mutex_handoff_loan(void);
int mutex_waiting_withdrawn(void);
int condvar_interrupt_stale(void);
int wait_rewait(void);
int sync_kill_handoff(void);
int block_request_race(void);
int dispatch_window(void);
int k65_windows(void);
int boot_tick_catch_up(void);
int ns_open_deferral(void);
int ns_stat_unlink(void);
int ns_open_detach(void);
void test_ioserv_lost_reply_binding(void);
int test_ioserv_lost_reply_unblock(void);
int pte_partial_map(void);
void test_shm_past_end(void);
int shm_past_end_child(const char *what);
int anon_map_failure(void);
int loader_share_child(void);
void test_loader_context_share(void);
long kstats_value(const char *key);
int pgrp_races(void);
int pgrp_debug_access(void);
int pgrp_tty_other(void);
int pgrp_exec_hold(void);
void pgrp_exec_eacces(void);
void test_exec(void);
int exec_swap_child(const char *what);
int exec_swap_report(const char *arg);
int exec_swap_chain(const char *count);
int pgrp_tty_child(int race);
int disconnect_child(const char *pid_arg, const char *chid_arg);
void test_posix_options(void);
void test_sysconf(void);

/* The initial envp, found after main()'s argv rather than through environ,
 * which libc or a setenv() may have moved. The auxiliary vector follows its
 * NULL terminator.
 */
extern char **test_initial_envp;
void test_capgate(void);
void test_leak(void);
void test_gauges(void);
void test_detach_race_laps(int laps);
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
void test_spawn_concurrent(void);
void test_spawn_contract(void);
void test_sysfs_open_stress(void);
void test_loader_fault(void);
void test_spawn_loader_einval_log(void);
void test_spawn_concurrent_arg_max(void);
void test_spawn_heap_gate(void);
void test_spawn_arg_max(void);
int exec_arg_max_child(void);
void test_spawn_actions(void);
void test_orphan_exit(void);
int orphan_exit_mid(void);
void test_spawn_storm(unsigned residents);
void test_kill_busy(void);
int kill_busy_child(void);
int kill_resolve_child(void);
int kill_stuck_handler_child(void);
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
