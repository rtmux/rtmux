/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * signal.h — Signal number definitions and POSIX signal types.
 *
 * RTMux implements POSIX signal state in libc. Kernel delivery remains
 * notification-based on the per-process system channel.
 *
 * This header exposes signal number constants used by fault codes, debugger,
 * exit status encoding, sigset_t manipulation, and POSIX signal APIs.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef _SIGNAL_H_INCLUDED

#include <sys/types.h>

#define _SIGNAL_H_INCLUDED

#ifndef _SIGNAL_H_DECLARED
#define _SIGNAL_H_DECLARED

#ifndef __SIGINFO_H_INCLUDED
#include <sys/siginfo.h>
#endif

#define SIG_ERR ((void (*)(int)) - 1)
#define SIG_DFL ((void (*)(int)) 0)
#define SIG_IGN ((void (*)(int)) 1)

/* Signal numbers — retained for fault codes, debugger protocol, CLD_* exit
 * status encoding, and MAKE_SIGCODE() in trap handlers.
 */
#define SIGHUP 1   /* hangup */
#define SIGINT 2   /* interrupt */
#define SIGQUIT 3  /* quit */
#define SIGILL 4   /* illegal instruction */
#define SIGTRAP 5  /* trace trap */
#define SIGABRT 6  /* used by abort */
#define SIGFPE 8   /* floating point exception */
#define SIGKILL 9  /* unconditional process kill */
#define SIGBUS 10  /* bus error */
#define SIGSEGV 11 /* segmentation violation */
#define SIGSYS 12  /* bad argument to system call */
#define SIGPIPE 13 /* write on pipe with no reader */
#define SIGALRM 14 /* alarm */
#define SIGTERM 15 /* software termination */
#define SIGUSR1 16 /* user defined signal 1 */
#define SIGUSR2 17 /* user defined signal 2 */
#define SIGCHLD 18 /* child status change */
#define SIGCLD SIGCHLD
#define SIGWINCH 20  /* window change */
#define SIGURG 21    /* urgent I/O condition */
#define SIGPOLL 22   /* pollable event */
#define SIGSTOP 23   /* stop (cannot be caught) */
#define SIGTSTP 24   /* stop from tty */
#define SIGCONT 25   /* continue stopped process */
#define SIGTTIN 26   /* background tty read */
#define SIGTTOU 27   /* background tty write */
#define SIGVTALRM 28 /* virtual timer */
#define SIGPROF 29   /* profiling timer */
#define SIGXCPU 30   /* CPU time limit exceeded */
#define SIGXFSZ 31   /* file size limit exceeded */

#define _SIGMIN 1
#define _SIGMAX 64
#define _NSIG 57
#define NSIG _NSIG
#define MAXSIG 64
#define SIGRTMIN 41
#define SIGRTMAX 56

/* sigprocmask() flags — used by posix_spawn attributes */
#define SIG_BLOCK 0
#define SIG_UNBLOCK 1
#define SIG_SETMASK 2
#define SIG_PENDING 5

/* sigaction flags — used by struct sigaction */
#define SA_NOCLDSTOP 0x0001
#define SA_SIGINFO 0x0002
#define SA_RESETHAND 0x0004
#define SA_RESTART 0x0008
#define SA_NODEFER 0x0010
#define SA_NOCLDWAIT 0x0020
#define SA_MASK 0x00ff

__BEGIN_DECLS

/* POSIX signal APIs. Implementations are in libc. */
extern int raise(int __sig);
extern void (*signal(int __sig, void (*__func)(int)))(int);
extern int kill(pid_t __pid, int __signum);
extern int killpg(pid_t __pgrp, int __signum);
extern int sigaction(int __signo,
                     const struct sigaction *__act,
                     struct sigaction *__oact);
extern int sigaddset(sigset_t *__set, int __signo);
extern int sigdelset(sigset_t *__set, int __signo);
extern int sigemptyset(sigset_t *__set);
extern int sigfillset(sigset_t *__set);
extern int sigismember(const sigset_t *__set, int __signo);
extern int sigprocmask(int __how, const sigset_t *__set, sigset_t *__oset);

/* Per-thread signal APIs. RTMux's signal state is process-global (one shared
 * mask and handler table), so these operate on that shared state: the mask is
 * process-wide and a kill collapses to process delivery. See the .c files.
 */
extern int pthread_sigmask(int __how, const sigset_t *__set, sigset_t *__oset);
extern int pthread_kill(pthread_t __thr, int __sig);

__END_DECLS
#endif

#endif
