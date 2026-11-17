/*
 * Host-side tests for the process-group table.
 *
 * A group exists while it has a member, and a number a later group reuses must
 * not take a count from the one that ended: a pid wraps only after half a
 * billion processes, which no guest test can wait for.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include "pgrp-table.h"

static struct pgrp_table t;

int main(void)
{
    uint32_t g1, g2, g3, gx;
    pid_t sid;

    pgrp_table_init(&t);

    /* A group starts with its first member, and is its members. */
    assert(pgrp_table_find(&t, 7, NULL) == ESRCH);
    assert(pgrp_table_enter(&t, 7, 5, PGRP_JOIN, &g1) == EPERM);
    assert(pgrp_table_enter(&t, 7, 5, PGRP_JOIN_OR_CREATE, &g1) == EOK);
    assert(pgrp_table_find(&t, 7, &sid) == EOK && sid == 5);
    assert(pgrp_table_enter(&t, 7, 5, PGRP_JOIN, &g2) == EOK && g2 == g1);

    /* Another session cannot join it, and a leader's new group must be new. */
    assert(pgrp_table_enter(&t, 7, 6, PGRP_JOIN, &gx) == EPERM);
    assert(pgrp_table_enter(&t, 7, 6, PGRP_JOIN_OR_CREATE, &gx) == EPERM);
    assert(pgrp_table_enter(&t, 7, 5, PGRP_CREATE, &gx) == EPERM);

    /* It lasts until the last member leaves. */
    pgrp_table_leave(&t, 7, g1);
    assert(pgrp_table_find(&t, 7, NULL) == EOK);
    pgrp_table_leave(&t, 7, g2);
    assert(pgrp_table_find(&t, 7, NULL) == ESRCH);

    /* A later group of the same number is another incarnation: a member of the
     * first that leaves late takes nothing from it.
     */
    assert(pgrp_table_enter(&t, 7, 9, PGRP_CREATE, &g3) == EOK && g3 != g1);
    pgrp_table_leave(&t, 7, g1);
    assert(pgrp_table_find(&t, 7, &sid) == EOK && sid == 9);
    pgrp_table_leave(&t, 7, g3);
    assert(pgrp_table_find(&t, 7, NULL) == ESRCH);
    pgrp_table_leave(&t, 7, g3); /* a second leave is dropped */

    /* Numbers that share a bucket stay apart, and unlink from the middle. */
    {
        pid_t a = 3, b = 3 + PGRP_TABLE_BUCKETS, c = 3 + 2 * PGRP_TABLE_BUCKETS;
        uint32_t ga, gb, gc;

        assert(pgrp_table_enter(&t, a, 1, PGRP_CREATE, &ga) == EOK);
        assert(pgrp_table_enter(&t, b, 1, PGRP_CREATE, &gb) == EOK);
        assert(pgrp_table_enter(&t, c, 1, PGRP_CREATE, &gc) == EOK);
        pgrp_table_leave(&t, b, gb);
        assert(pgrp_table_find(&t, a, NULL) == EOK);
        assert(pgrp_table_find(&t, b, NULL) == ESRCH);
        assert(pgrp_table_find(&t, c, NULL) == EOK);
        pgrp_table_leave(&t, a, ga);
        pgrp_table_leave(&t, c, gc);
    }

    /* Every slot can lead a group at once, and two more fit for a move. */
    for (pid_t p = 1; p <= 4096; p++)
        assert(pgrp_table_enter(&t, p, 1, PGRP_CREATE, &gx) == EOK);
    assert(pgrp_table_enter(&t, 5000, 1, PGRP_CREATE, &gx) == EOK);
    assert(pgrp_table_enter(&t, 5001, 1, PGRP_CREATE, &gx) == EOK);
    assert(pgrp_table_enter(&t, 5002, 1, PGRP_CREATE, &gx) == ENOMEM);

    puts("pgrp-table-test: PASS");
    return 0;
}
