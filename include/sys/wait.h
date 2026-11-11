/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* sys/wait.h Define system wait types */
#ifndef __WAIT_H_INCLUDED

#ifndef __PLATFORM_H_INCLUDED
#include <sys/platform.h>
#endif

#ifndef _SIGNAL_H_INCLUDED
#include <signal.h>
#endif

/* Defined system wait types */
#define WUNTRACED 0x0004 /* Processes stopped by signals */
#define WNOHANG 0x0040   /* Do not block waiting */
#define WEXITED 0x0001   /* Wait for processes that have exited */
#define WSTOPPED WUNTRACED
#define WCONTINUED 0x0008 /* Processes continued by signals */
#define WNOWAIT 0x0080    /* Don't free waited on child */

typedef enum { P_ALL, P_PID, P_PGID } idtype_t;

/* Define 1003.1 macros. */
#define WIFEXITED(__status) (((__status) & 0xff) == 0)
#define WEXITSTATUS(__status) (((__status) >> 8) & 0xff)
#define WIFSIGNALED(__status) \
    (((__status) & 0xff) != 0 && ((__status) & 0xff00) == 0)
#define WTERMSIG(__status) ((__status) & 0x7f)
#define WIFSTOPPED(__status) \
    (((__status) & 0xff) == 0x7f && ((__status) & 0xff00) != 0)
#define WSTOPSIG(__status) (((__status) >> 8) & 0xff)
#define WIFCONTINUED(__status) (((__status) & 0xffff) == 0xffff)
/* POSIX 1003.1 Prototypes. */
__BEGIN_DECLS

extern pid_t wait(int *__stat_loc);
extern pid_t waitpid(pid_t __pid, int *__stat_loc, int __options);
extern int waitid(idtype_t __idtype,
                  id_t __id,
                  siginfo_t *__infop,
                  int __options);


__END_DECLS

#define __WAIT_H_INCLUDED
#endif
