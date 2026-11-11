/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef PROCFS_H
#define PROCFS_H

#include <stdbool.h>

#include "externs.h"

#define PROCFS_FLAG_AS 0x0001U
#define PROCFS_FLAG_SELF 0x0002U
#define PROCFS_FLAG_MOUNT 0x0008U /* Process mount information */
#define PROCFS_FLAG_DIR 0x0010U   /* This entry a dir (for mount) */
#define PROCFS_FLAG_PROC 0x0020U  /* This is the "/proc" entry */
#define PROCFS_FLAG_NAME 0x0040U  /* /proc/<pid>/name regular file */
#define PROCFS_FLAG_FD 0x0080U    /* /proc/<pid>/fd descriptor directory */
#define PROCFS_FLAG_WAIT 0x0100U  /* /proc/<pid>/wait process handle */

/* Mask for "is this an abstract / non-per-pid-regular-file flag?". AS, SELF,
 * NAME are per-pid regular-file flags and stay OUT of the mask so the "if (pid
 * && !(flags & MASK))" checks on the per-pid read paths still match. FD and
 * WAIT are per-pid procfs nodes with explicit read/stat paths, so they stay IN
 * the mask to avoid debugger-address-space OCB handling.
 */
#define PROCFS_FLAG_MASK                                                       \
    (PROCFS_FLAG_MOUNT | PROCFS_FLAG_DIR | PROCFS_FLAG_PROC | PROCFS_FLAG_FD | \
     PROCFS_FLAG_WAIT)


struct procfs_ocb {
    struct procfs_ocb *next;
    pid_t pid;
    int ioflag;
    unsigned flags;
    bool stale;
    int tid;
    uintptr_t offset;
    int rcvid;
    struct sigevent event;
    char tail[1];
};

struct procfs_waiting {
    struct procfs_waiting *next;
    int rcvid;
};

#endif

/* end of procfs.h */
