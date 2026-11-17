/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * The process groups that exist, and how many processes are in each.
 *
 * A group exists while it has a member, a zombie included, and ends when its
 * last member is reaped, not when the process it is named for goes. Each node
 * carries the session its members are in and a generation, so that a number a
 * later group reuses is never confused with the one that ended: a member leaves
 * the node it entered, and a leave that names a node that has been replaced is
 * dropped.
 *
 * No locking and no allocation: a fixed pool, so entering a group cannot fail
 * for want of memory. The caller serializes (proc/group.c).
 */

#ifndef PGRP_TABLE_H
#define PGRP_TABLE_H

#include <errno.h>
#include <stdint.h>
#include <sys/types.h>

#ifndef EOK
#define EOK 0 /* the host's errno.h has none */
#endif

/* Groups that can exist at once: one per process slot, and two spare for a
 * move, which enters the new group before it leaves the old one.
 */
#define PGRP_TABLE_NODES (4096 + 2)
#define PGRP_TABLE_BUCKETS 1024

/* How pgrp_table_enter treats a group that is, or is not, there. */
enum pgrp_enter_mode {
    PGRP_JOIN,           /* it must exist */
    PGRP_JOIN_OR_CREATE, /* join it, or start it with this member */
    PGRP_CREATE          /* it must not exist: a session leader's new group */
};

struct pgrp_node {
    pid_t pgid;
    pid_t sid;        /* the session of every member */
    uint32_t gen;     /* tells this group from a later one of the same id */
    uint32_t members; /* 0 only in a free node */
    uint16_t next; /* index + 1 of the next node in its bucket or free list */
};

struct pgrp_table {
    struct pgrp_node node[PGRP_TABLE_NODES];
    uint16_t head[PGRP_TABLE_BUCKETS]; /* index + 1, 0 for none */
    uint16_t free;                     /* index + 1 of the first free node */
    uint32_t next_gen;
};

void pgrp_table_init(struct pgrp_table *t);

/* Add a member to group pgid of session sid. EOK with the node's generation in
 * *gen, EPERM when the group is absent (PGRP_JOIN), present (PGRP_CREATE) or in
 * another session, ENOMEM when the pool is exhausted.
 */
int pgrp_table_enter(struct pgrp_table *t,
                     pid_t pgid,
                     pid_t sid,
                     enum pgrp_enter_mode mode,
                     uint32_t *gen);

/* Remove a member that entered generation gen of group pgid; the group ends
 * with its last member. A node that is gone or of another generation is left
 * alone.
 */
void pgrp_table_leave(struct pgrp_table *t, pid_t pgid, uint32_t gen);

/* EOK with the group's session in *sid (if not NULL), or ESRCH. */
int pgrp_table_find(const struct pgrp_table *t, pid_t pgid, pid_t *sid);

#endif /* PGRP_TABLE_H */
