/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * The pool of /sys open-file state (sysfs-ocb.c), apart from sysfs.c so that a
 * host test can drive it from many threads.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef SYSFS_OCB_H
#define SYSFS_OCB_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include <sys/pool-canary.h>

/* Per-open state. mode + (data, size) is enough — readdir uses pos. Dynamic
 * entries (synthesized on each open) own a malloc'd buffer in dyn_buf which
 * sysfs_close_ocb frees; static entries leave it NULL.
 */
struct sysfs_ocb {
    POOL_CANARY_HEAD
    uint32_t mode;
    uint64_t start_seq; /* stream entries: ring window [start_seq, end_seq) */
    uint64_t end_seq;
    const char *data;
    size_t size;
    off_t pos;
    int idx;
    char *dyn_buf;
    POOL_CANARY_TAIL
};

#define SYSFS_MAX_OCBS 16

/* Arm the pool's canaries; once, before the first sysfs_ocb_alloc. */
void sysfs_ocb_init(void);
/* A zeroed OCB of the caller's own, or NULL when all are in use. */
struct sysfs_ocb *sysfs_ocb_alloc(void);
/* Give an OCB back; a pointer that is not one of the pool's is ignored. */
void sysfs_ocb_free(struct sysfs_ocb *ocb);

#endif
