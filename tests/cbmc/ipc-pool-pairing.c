/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * CBMC harness for the section 13 refinement rung: message-pool alloc/free
 * pairing.
 *
 * The message/notification objects that the IPC fast path allocates and frees
 * (message_souls, notification_souls, connect/channel souls) come from the
 * intrusive singly-linked soul freelist in kernel/object/core.c, where each
 * object's first word is its freelist link. This harness mirrors that pop/push
 * exactly:
 *
 *   pop  (object_alloc, core.c:79-86):  ptr = next; next = *ptr; *ptr = NULL
 *   push (object_free relink):          *ptr = next; next = ptr
 *
 * and proves, over every nondeterministic alloc/free schedule up to CBMC's
 * unwind bound, the pairing-safety properties the fast path relies on:
 *   - no double hand-out (a node handed to a caller is off the freelist),
 *   - no double free / free of a node already on the freelist,
 *   - freelist integrity (no lost node, no cycle),
 *   - node conservation (no leak: handed-out + on-list == pool size).
 *
 * This models the freelist algorithm, not the linked kernel (souls carry heap
 * grow/limits/highwater bookkeeping that does not affect pairing safety). If
 * the core.c pop/push pointer sequence changes, update this harness.
 *
 * Build clean: cbmc tests/cbmc/ipc-pool-pairing.c -> VERIFICATION SUCCESSFUL
 * -DPLANT_BUG: drops the freelist head advance on pop, so the same node is
 *                     handed out twice  -> VERIFICATION FAILED (no double hand-out)
 * -DPLANT_DOUBLE_FREE: keeps a freed node in the caller's held set so it can be
 *                     freed again        -> VERIFICATION FAILED (no double free)
 * The gate requires clean to pass and each mutation to fail, so every
 * advertised assertion is proven to have teeth rather than passing vacuously.
 */

#define POOL 4 /* pool nodes; CBMC explores all schedules over them */
#define OPS 8  /* nondeterministic alloc/free operations */

int nondet_int(void);
_Bool nondet_bool(void);

/* Indices 0..POOL-1 are the "object pointers"; -1 is NULL. */
static int link_[POOL]; /* link_[i] = the *ptr freelist word of node i */
static int head;        /* soulp->next: freelist head, or -1 */
static _Bool alloced[POOL]; /* ghost: node i is currently handed out */

/* pop: object_alloc freelist path. Returns a node index, or -1 if empty. */
static int pool_alloc(void)
{
    int ptr = head; /* ptr = soulp->next */
    if (ptr == -1)
        return -1; /* empty (the real code grows the pool; not modeled) */
#ifndef PLANT_BUG
    head = link_[ptr]; /* soulp->next = *ptr */
#endif
    link_[ptr] = -1;         /* *ptr = NULL */
    __CPROVER_assert(!alloced[ptr], "no double hand-out");
    alloced[ptr] = 1;
    return ptr;
}

/* push: object_free relink path. */
static void pool_free(int ptr)
{
    __CPROVER_assert(alloced[ptr], "free only a handed-out node (no double free)");
    alloced[ptr] = 0;
    link_[ptr] = head; /* *ptr = soulp->next */
    head = ptr;        /* soulp->next = ptr */
}

int main(void)
{
    int handed[POOL];
    int nheld = 0;
    int i;

    for (i = 0; i < POOL; i++) {
        link_[i] = (i + 1 < POOL) ? i + 1 : -1; /* 0->1->..->POOL-1->NULL */
        alloced[i] = 0;
    }
    head = 0;

    for (i = 0; i < OPS; i++) {
        if (nondet_bool()) {
            int p = pool_alloc();
            if (p != -1)
                handed[nheld++] = p;
        } else if (nheld > 0) {
            int idx = nondet_int();
            __CPROVER_assume(idx >= 0 && idx < nheld);
            pool_free(handed[idx]);
#ifndef PLANT_DOUBLE_FREE
            handed[idx] = handed[--nheld]; /* remove from the held set */
#endif
            /* -DPLANT_DOUBLE_FREE leaves the freed node in the held set, so a
             * later iteration can free it again; pool_free's alloced check must
             * catch that. This keeps the no-double-free assertion honest
             * (non-vacuous) rather than merely unreachable by construction.
             */
        }
    }

    /* Freelist integrity + conservation. */
    _Bool on_list[POOL];
    int n_on_list = 0;
    int cur = head;
    int steps = 0;
    for (i = 0; i < POOL; i++)
        on_list[i] = 0;
    while (cur != -1) {
        __CPROVER_assert(steps <= POOL, "freelist has no cycle");
        __CPROVER_assert(!on_list[cur], "no node appears twice on the freelist");
        __CPROVER_assert(!alloced[cur], "a freelist node is not handed out");
        on_list[cur] = 1;
        n_on_list++;
        cur = link_[cur];
        steps++;
    }
    __CPROVER_assert(n_on_list + nheld == POOL,
                     "node conservation: no leak, no phantom");
    return 0;
}
