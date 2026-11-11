/*
 * RTMux libc — internal signal-state API.
 *
 * RTMux's kernel does not deliver signals; signals are restored as a pure
 * user-space abstraction on top of the per-process notification channel. Every
 * RTMux process already has a "system thread" (see
 * lib/libc/$ARCH/syschan-init.c) that listens on that channel. This header is
 * the contract between three parties:
 *
 *   - the system thread, which calls __libc_signal_notification_recv()
 *     when a _NOTIFICATION_CODE_SIGNAL notification arrives;
 *
 *   - sigaction() / sigprocmask() / raise() / kill() in lib/libc/posix/,
 *     which read and edit the table;
 *
 *   - libc startup (__libc_signal_init), called once before main().
 *
 * The table itself lives in rtmux/signal-state.c and is process-wide. All edits
 * take a single libc-private mutex; handlers run with the mutex *released* so
 * they can safely call back into sigaction(), sigprocmask(), raise(), kill()
 * and friends without deadlocking.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef _RTMUX_LIBC_SIGNAL_STATE_H
#define _RTMUX_LIBC_SIGNAL_STATE_H

#include <signal.h>
#include <sys/cdefs.h>
#include <sys/types.h>

/* Initialize the table to defaults (SIG_DFL everywhere, empty mask, empty
 * pending). Called once from crt0 / syschan_init startup.
 */
void __libc_signal_init(void) __hidden;

/* Called by the system thread when a _NOTIFICATION_CODE_SIGNAL notification is
 * received. Updates pending, runs the handler asynchronously from the system
 * thread if the signal is unblocked.
 */
void __libc_signal_notification_recv(int signo, pid_t sender) __hidden;

/* Synchronous in-thread dispatch. Used by raise(): runs the registered handler
 * from the calling thread, under the standard POSIX semantics (block sig during
 * the handler unless SA_NODEFER).
 */
int __libc_signal_raise(int signo) __hidden;

/* Real implementations the public sig* shims delegate to. They take the
 * libc-private signal lock for the duration.
 */
int __libc_sigaction(int signo,
                     const struct sigaction *act,
                     struct sigaction *oact) __hidden;
int __libc_sigprocmask(int how, const sigset_t *set, sigset_t *oset) __hidden;
int __libc_signal_pause(void) __hidden;

/* Interruptible-wait registry. A blocking libc wrapper (read, write,
 * nanosleep, sem_wait, ...) brackets its kernel call with
 * __libc_wait_enter()/__libc_wait_exit() on a libc_waiter_t that lives on its
 * own stack. While registered, the thread can be broken out of the call with
 * EINTR when a signal with a handler installed without SA_RESTART is delivered
 * (one waiter per signal: the lowest-tid registered thread). When no such
 * handler exists the bracket is a single atomic load.
 */
typedef struct libc_waiter {
    struct libc_waiter *next;
    int tid;      /* 1-based pthread_t; 0 = not registered */
    unsigned seq; /* identity across a re-registration at the same address */
    int hit;      /* chosen by a delivery; do not choose it again */
} libc_waiter_t;

void __libc_wait_enter(libc_waiter_t *w) __hidden;
void __libc_wait_exit(libc_waiter_t *w) __hidden;

/* Unlink every registration of this thread. A cancelled or exiting thread
 * leaves its wrapped call without returning through it, so its on-stack record
 * would otherwise dangle in the list. Called from pthread_exit().
 */
void __libc_wait_purge(int tid) __hidden;

#define __LIBC_PSELECT_WAKE_CODE 1

int __libc_signal_pselect_begin(const sigset_t *set,
                                int coid,
                                sigset_t *oset,
                                unsigned *seq) __hidden;
int __libc_signal_pselect_end(int coid,
                              const sigset_t *oset,
                              unsigned seq) __hidden;

#endif /* _RTMUX_LIBC_SIGNAL_STATE_H */
