/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * Debug-only redzone canaries for fixed server object pools.
 *
 * A server that keeps a statically sized array of reusable objects (an OCB or
 * request pool) embeds POOL_CANARY_HEAD as the first member of the pooled
 * struct and POOL_CANARY_TAIL as the last. On alloc the pool arms both to a
 * known magic; on free and before a reply that carries pooled data it verifies
 * them. Because the head is the first member and the tail the last, an overflow
 * that runs past one object writes into the adjacent object's head canary, so
 * object-boundary corruption is caught at the next check with the pool name and
 * slot index.
 *
 * This is an object-boundary redzone, not an inter-field guard: a small
 * overflow that stays inside the object (spilling one buffer into the next
 * struct member) is not caught here. The target is corruption that crosses an
 * object boundary and would otherwise become cross-request or cross-service
 * confusion.
 *
 * With CONFIG_SERVER_POOL_CANARY unset the members vanish and every hook is a
 * no-op, so release builds pay no layout or runtime cost.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef _SYS_POOL_CANARY_H
#define _SYS_POOL_CANARY_H

#ifdef CONFIG_SERVER_POOL_CANARY

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

/* Non-zero, non-ASCII, unlikely to appear as a stray write (golden ratio). */
#define POOL_CANARY_MAGIC 0x9e3779b97f4a7c15ULL

/* First and last members of a pooled struct. Each macro carries its own
 * semicolon and is used without one, so the release definitions below expand to
 * nothing and leave no stray declaration behind.
 */
#define POOL_CANARY_HEAD uint64_t __canary_head;
#define POOL_CANARY_TAIL uint64_t __canary_tail;

/* Arm both canaries. Call right after the pool clears a slot, both on alloc and
 * on free: a free slot stays poisoned so an overflow from a live neighbor into
 * it is caught when that slot is next allocated instead of being erased.
 */
#define pool_canary_arm(obj)                      \
    do {                                          \
        (obj)->__canary_head = POOL_CANARY_MAGIC; \
        (obj)->__canary_tail = POOL_CANARY_MAGIC; \
    } while (0)

/* Arm every slot of a pool once, before the first alloc, so free slots carry
 * the magic from the start and the alloc-time check has a known-good baseline.
 */
#define pool_canary_init(pool, n)                 \
    do {                                          \
        for (int __pci = 0; __pci < (n); __pci++) \
            pool_canary_arm(&(pool)[__pci]);      \
    } while (0)

/* Verify both canaries, aborting with the pool name and slot index if either
 * was overwritten. Call on free and before replying with pooled data.
 */
#define pool_canary_check(obj, name, idx) \
    __pool_canary_verify((obj)->__canary_head, (obj)->__canary_tail, (name), (idx))

static inline void __pool_canary_verify(uint64_t head, uint64_t tail,
                                        const char *name, int idx)
{
    if (head == POOL_CANARY_MAGIC && tail == POOL_CANARY_MAGIC)
        return;
    fprintf(stderr, "pool canary corrupt: %s[%d] head=%#llx tail=%#llx\n", name,
            idx, (unsigned long long) head, (unsigned long long) tail);
    abort();
}

#else /* !CONFIG_SERVER_POOL_CANARY */

#define POOL_CANARY_HEAD
#define POOL_CANARY_TAIL
#define pool_canary_arm(obj) ((void) 0)
#define pool_canary_init(pool, n) ((void) 0)
#define pool_canary_check(obj, name, idx) ((void) 0)

#endif /* CONFIG_SERVER_POOL_CANARY */

#endif /* _SYS_POOL_CANARY_H */
