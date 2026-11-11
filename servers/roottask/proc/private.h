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

#endif
