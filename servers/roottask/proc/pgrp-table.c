/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <stddef.h>
#include "pgrp-table.h"

/* A pid is a 12-bit slot index under a generation, and each reuse of a slot
 * adds 4096, a multiple of the bucket count: the index alone would put every
 * generation of one slot in one chain. Fold the generation into the index so
 * that groups named for successive holders of a slot spread out.
 */
static unsigned pgrp_bucket(pid_t pgid)
{
    unsigned p = (unsigned) pgid;

    return (p ^ (p >> 12)) % PGRP_TABLE_BUCKETS;
}

/* The node of group pgid, or NULL. *link, if link is not NULL, is left at the
 * reference that holds the node, for the caller that unlinks it.
 */
static struct pgrp_node *pgrp_lookup(struct pgrp_table *t,
                                     pid_t pgid,
                                     uint16_t **link)
{
    uint16_t *l = &t->head[pgrp_bucket(pgid)];

    while (*l) {
        struct pgrp_node *n = &t->node[*l - 1];

        if (n->pgid == pgid) {
            if (link)
                *link = l;
            return n;
        }
        l = &n->next;
    }
    return NULL;
}

void pgrp_table_init(struct pgrp_table *t)
{
    for (unsigned i = 0; i < PGRP_TABLE_BUCKETS; i++)
        t->head[i] = 0;
    t->free = 0;
    for (unsigned i = PGRP_TABLE_NODES; i-- > 0;) {
        t->node[i].members = 0;
        t->node[i].next = t->free;
        t->free = (uint16_t) (i + 1);
    }
    t->next_gen = 1;
}

int pgrp_table_enter(struct pgrp_table *t,
                     pid_t pgid,
                     pid_t sid,
                     enum pgrp_enter_mode mode,
                     uint32_t *gen)
{
    struct pgrp_node *n = pgrp_lookup(t, pgid, NULL);
    uint16_t idx;

    if (n) {
        if (mode == PGRP_CREATE || n->sid != sid)
            return EPERM;
        n->members++;
        *gen = n->gen;
        return EOK;
    }
    if (mode == PGRP_JOIN)
        return EPERM;
    if (!(idx = t->free))
        return ENOMEM;
    n = &t->node[idx - 1];
    t->free = n->next;
    n->pgid = pgid;
    n->sid = sid;
    n->gen = t->next_gen++;
    n->members = 1;
    n->next = t->head[pgrp_bucket(pgid)];
    t->head[pgrp_bucket(pgid)] = idx;
    *gen = n->gen;
    return EOK;
}

void pgrp_table_leave(struct pgrp_table *t, pid_t pgid, uint32_t gen)
{
    uint16_t *found;
    struct pgrp_node *n = pgrp_lookup(t, pgid, &found);
    uint16_t idx;

    if (!n || n->gen != gen || --n->members > 0)
        return;
    idx = *found;
    *found = n->next;
    n->next = t->free;
    t->free = idx;
}

int pgrp_table_find(const struct pgrp_table *t, pid_t pgid, pid_t *sid)
{
    const struct pgrp_node *n =
        pgrp_lookup((struct pgrp_table *) t, pgid, NULL);

    if (!n)
        return ESRCH;
    if (sid)
        *sid = n->sid;
    return EOK;
}
