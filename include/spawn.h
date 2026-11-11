/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* spawn.h */
#ifndef _SPAWN_H_INCLUDED
#define _SPAWN_H_INCLUDED

#ifndef __TYPES_H_INCLUDED
#include <sys/types.h>
#endif

#ifndef _SIGNAL_H_INCLUDED
#include <signal.h>
#endif

#ifndef __PLATFORM_H_INCLUDED
#include <sys/platform.h>
#endif

__BEGIN_DECLS
#ifndef _SCHED_H_INCLUDED
#include <sched.h>
#endif
/* posix_spawnattr_t */
typedef uintptr_t posix_spawnattr_t;
#define POSIX_SPAWNATTR_INITIALIZER {NULL}

/* posix_spawn_file_actions_t */
typedef uintptr_t posix_spawn_file_actions_t;
#define POSIX_SPAWN_FILE_ACTIONS_INITIALIZER {NULL}

/* prototypes */
extern int posix_spawnattr_init(posix_spawnattr_t *attrp);
extern int posix_spawnattr_destroy(posix_spawnattr_t *attrp);
extern int posix_spawnattr_getflags(const posix_spawnattr_t *restrict attrp,
                                    short *restrict flags_p);
extern int posix_spawnattr_setflags(posix_spawnattr_t *attrp, short flags);
extern int posix_spawnattr_getsigdefault(
    const posix_spawnattr_t *restrict attrp,
    sigset_t *restrict sigset_p);
extern int posix_spawnattr_setsigdefault(posix_spawnattr_t *restrict attrp,
                                         const sigset_t *restrict sigset_p);
extern int posix_spawnattr_getsigmask(const posix_spawnattr_t *restrict attrp,
                                      sigset_t *restrict sigset_p);
extern int posix_spawnattr_setsigmask(posix_spawnattr_t *restrict attrp,
                                      const sigset_t *restrict sigset_p);
extern int posix_spawnattr_getpgroup(const posix_spawnattr_t *restrict attrp,
                                     pid_t *restrict pid_p);
extern int posix_spawnattr_setpgroup(posix_spawnattr_t *attrp, pid_t pid);
extern int posix_spawnattr_getschedparam(
    const posix_spawnattr_t *restrict attrp,
    struct sched_param *restrict sched_p);
extern int posix_spawnattr_setschedparam(
    posix_spawnattr_t *restrict attrp,
    const struct sched_param *restrict sched_p);
extern int posix_spawnattr_getschedpolicy(
    const posix_spawnattr_t *restrict attrp,
    int *restrict policy_p);
extern int posix_spawnattr_setschedpolicy(posix_spawnattr_t *attrp, int policy);

extern int posix_spawn_file_actions_init(posix_spawn_file_actions_t *fact_p);
extern int posix_spawn_file_actions_destroy(posix_spawn_file_actions_t *fact_p);
extern int posix_spawn_file_actions_addclose(posix_spawn_file_actions_t *fact_p,
                                             int fd);
extern int posix_spawn_file_actions_adddup2(posix_spawn_file_actions_t *fact_p,
                                            int fd,
                                            int new_fd);
extern int posix_spawn_file_actions_addopen(
    posix_spawn_file_actions_t *restrict fact_p,
    int new_fd,
    const char *restrict path,
    int oflags,
    mode_t omode);

extern int posix_spawn(pid_t *restrict pid,
                       const char *restrict path,
                       const posix_spawn_file_actions_t *file_actions,
                       const posix_spawnattr_t *restrict __attrp,
                       char *const argv[restrict],
                       char *const envp[restrict]);
extern int posix_spawnp(pid_t *restrict pid,
                        const char *restrict file,
                        const posix_spawn_file_actions_t *file_actions,
                        const posix_spawnattr_t *restrict __attrp,
                        char *const argv[restrict],
                        char *const envp[restrict]);

#define POSIX_SPAWN_SETPGROUP 0x00000001  /* set process group */
#define POSIX_SPAWN_SETSIGMASK 0x00000002 /* set mask to sigmask */
#define POSIX_SPAWN_SETSIGDEF \
    0x00000004 /* set members of sigdefault to SIG_DFL */
#define POSIX_SPAWN_RESETIDS 0x00000020      /* reset effective IDs */
#define POSIX_SPAWN_SETSCHEDULER 0x00000040  /* set scheduling policy */
#define POSIX_SPAWN_SETSCHEDPARAM 0x00000400 /* set scheduling parameters */

extern int rtmux_spawn_waitfd(pid_t *restrict pid,
                              int *restrict waitfd,
                              const char *restrict path,
                              const posix_spawn_file_actions_t *file_actions,
                              const posix_spawnattr_t *restrict __attrp,
                              char *const argv[restrict],
                              char *const envp[restrict]);



__END_DECLS
#endif
