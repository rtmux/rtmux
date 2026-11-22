/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef PROCESS_PRIVATE_H
#define PROCESS_PRIVATE_H

#define PROC_CODE_TERM (SI_MINAVAIL + 1)
#define PROC_CODE_STOP (SI_MINAVAIL + 2)
#define PROC_CODE_CONT (SI_MINAVAIL + 3)

struct wait_entry {
    struct wait_entry *next;
    int rcvid;
    id_t id;
    short idtype;
    short options;
};

extern soul_t wait_souls;

#include <process.h>

static inline int spawn_add_size(size_t *sum, size_t add)
{
    if (add > SIZE_MAX - *sum)
        return E2BIG;
    *sum += add;
    return EOK;
}

/* A copy of one process's group and session state, taken under its slot lock
 * (proc_snapshot, jobctl.c): what setpgid and the job control checks both read.
 * The slot lock makes the process's own fields (ppid, pgrp, sid, flags)
 * consistent with each other. fg and fd belong to the session that every member
 * shares and are changed under pgrp_mutex and another member's slot lock, so
 * they are only a reading at one moment: a caller that needs them to hold
 * across a decision takes pgrp_mutex, as setpgid does.
 */
struct proc_snap {
    pid_t ppid;
    pid_t pgrp;
    pid_t sid; /* session leader pid; roottask's for the system session */
    pid_t fg;  /* foreground group of the session's terminal */
    int fd;    /* roottask's descriptor to the terminal, or -1 */
    unsigned flags;
};

int proc_snapshot(pid_t pid, struct proc_snap *s);

#endif
