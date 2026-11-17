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

/* A consistent copy of one process's group and session state, taken under its
 * slot lock (proc_snapshot, jobctl.c): what setpgid and the job control checks
 * both read.
 */
struct proc_snap {
    pid_t ppid;
    pid_t pgrp;
    pid_t sid; /* session leader pid; roottask's for the system session */
    pid_t fg;  /* foreground group of the session's terminal */
    int fd;    /* roottask's descriptor to the terminal, or -1 */
    unsigned flags;
    unsigned char execed; /* it has replaced its image */
    channel_t *chp;       /* the process's system channel */
};

int proc_snapshot(pid_t pid, struct proc_snap *s);

#endif
