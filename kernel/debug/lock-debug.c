/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * lock_debug — lock-rank checker for the post-BKL fine-grained locks.
 *
 * Every kspinlock_lock and kspinlock_unlock call lands here when
 * CONFIG_LOCK_DEBUG is set. Each per-CPU "held stack" records the (class,
 * instance) pair currently held by this CPU. On acquire:
 *
 *   - class_id < held[i].class_id  →  rank inversion (reject).
 *   - class_id == held[i].class_id && inst == held[i].inst →  recursive
 *     lock on the same kspinlock instance — would deadlock the
 *     AMOSWAP-based primitive (reject).
 *   - class_id == held[i].class_id && inst != held[i].inst →  OK, except
 *     TURNSTILE pairs must be authorized by sync_lock_two().
 *     (per-object same-class acquires are legal because we have
 *     per-channel_t / per-connect_t / per-process_t locks).
 *
 * On release, a matching (class, instance) entry must exist on the stack;
 * out-of-LIFO release is allowed. Either rule violation yields a kprintf and a
 * crash() — the design assumption is that rank checking is on for development
 * builds, not for production, and a fast hard fault is the most useful failure
 * mode while we are sprinkling new locks across the kernel.
 *
 * The held stack is small (eight entries) on the assumption that post-BKL code
 * paths never nest more than a handful of fine-grained locks; if a phase ends
 * up exceeding that, raise LOCK_HELD_MAX rather than spilling silently.
 */

#include <core/globals.h>

#ifdef CONFIG_LOCK_DEBUG

#define LOCK_HELD_MAX 8

struct lock_held_entry {
    void *inst; /* lock pointer — distinguishes per-object instances */
    uint8_t class_id;
};

struct lock_held {
    struct lock_held_entry slots[LOCK_HELD_MAX];
    uint8_t depth;
};

DEFINE_PER_CPU(struct lock_held, lock_held);
DEFINE_PER_CPU(uint8_t, lock_turnstile_pair_ok);

/* Map a lock-class number to its display name. Parameters:
 *   k : LOCK_CLASS_* value.
 * Return value: a static name string ("?" for unknown classes).
 */
static const char *class_name(uint8_t class_id)
{
    switch (class_id) {
    case LOCK_CLASS_UNCLASSIFIED:
        return "UNCLASSIFIED";
    case LOCK_CLASS_TURNSTILE:
        return "TURNSTILE";
    case LOCK_CLASS_TURNSTILE_OWNER:
        return "TURNSTILE_OWNER";
    case LOCK_CLASS_SCHED:
        return "SCHED";
    case LOCK_CLASS_TIMER:
        return "TIMER";
    case LOCK_CLASS_INTREVENT:
        return "INTREVENT";
    case LOCK_CLASS_PROCVEC:
        return "PROCVEC";
    case LOCK_CLASS_CHANNEL:
        return "CHANNEL";
    case LOCK_CLASS_CONNECT:
        return "CONNECT";
    case LOCK_CLASS_SYNC:
        return "SYNC";
    case LOCK_CLASS_PROCESS:
        return "PROCESS";
    case LOCK_CLASS_THREAD:
        return "THREAD";
    case LOCK_CLASS_SOUL:
        return "SOUL";
    case LOCK_CLASS_MEMPART:
        return "MEMPART";
    case LOCK_CLASS_PA:
        return "PA";
    case LOCK_CLASS_DEBUG:
        return "DEBUG";
    default:
        return "?";
    }
}

/* Record a lock acquire on this CPU's held-stack, enforcing the rank-checking
 * rules: reject rank inversion (acquiring a lower class while holding a higher
 * one) and recursive acquire of the same instance. Crashes on any violation.
 * Parameters:
 *   class_id : lock class of the lock being acquired.
 *   inst     : lock instance pointer (distinguishes per-object locks).
 */
void lock_debug_record_acquire(uint8_t class_id, void *inst)
{
    struct lock_held *h = this_cpu_ptr(lock_held);
    uint8_t *turnstile_pair_ok = this_cpu_ptr(lock_turnstile_pair_ok);
    uint8_t i;

    if (h->depth >= LOCK_HELD_MAX) {
        kprintf(
            "lock_debug: per-CPU held-stack overflow on cpu %u "
            "(>%d locks held); raise LOCK_HELD_MAX\n",
            get_cpunum(), LOCK_HELD_MAX);
        crash();
    }

    for (i = 0; i < h->depth; i++) {
        /* Strict rank inversion (lower class while holding higher). */
        if (class_id < h->slots[i].class_id) {
            kprintf(
                "lock_debug: rank inversion on cpu %u: "
                "trying to acquire %s/%p (rank %u) while holding %s/%p (rank "
                "%u)\n",
                get_cpunum(), class_name(class_id), inst, (unsigned) class_id,
                class_name(h->slots[i].class_id), h->slots[i].inst,
                (unsigned) h->slots[i].class_id);
            crash();
        }

        /* Same-class, same-instance: recursive acquire on the same intrspin
         * would deadlock the AMOSWAP-based primitive.
         */
        if (class_id == h->slots[i].class_id && h->slots[i].inst == inst) {
            kprintf(
                "lock_debug: recursive acquire on cpu %u: "
                "%s/%p already held\n",
                get_cpunum(), class_name(class_id), inst);
            crash();
        }

        /* Same-class, distinct-instance: legal for per-object locks. TURNSTILE
         * is the exception: two sync-chain locks must go through
         * sync_lock_two() so the address-order rule is centralized.
         */
        if (class_id == LOCK_CLASS_TURNSTILE &&
            class_id == h->slots[i].class_id && h->slots[i].inst != inst) {
            if (*turnstile_pair_ok == 0) {
                kprintf(
                    "lock_debug: ad hoc TURNSTILE pair on cpu %u: "
                    "trying to acquire %s/%p while holding %s/%p; "
                    "use sync_lock_two()\n",
                    get_cpunum(), class_name(class_id), inst,
                    class_name(h->slots[i].class_id), h->slots[i].inst);
                crash();
            }
            (*turnstile_pair_ok)--;
        }
    }

    h->slots[h->depth].class_id = class_id;
    h->slots[h->depth].inst = inst;
    h->depth++;
}

/* Authorize one address-ordered two-chain TURNSTILE acquire. This is consumed
 * by the next distinct-instance TURNSTILE nesting observed on this CPU.
 */
void lock_debug_allow_turnstile_pair(void)
{
    uint8_t *turnstile_pair_ok = this_cpu_ptr(lock_turnstile_pair_ok);

    (*turnstile_pair_ok)++;
}

uint8_t lock_debug_held_depth(void)
{
    return this_cpu_ptr(lock_held)->depth;
}

/* Record a lock release, removing the matching (class, instance) entry from
 * this CPU's held-stack. Out-of-LIFO release is allowed. Crashes if the stack
 * is empty or no matching acquire is found. Parameters:
 *   class_id : lock class of the lock being released.
 *   inst     : lock instance pointer.
 */
void lock_debug_record_release(uint8_t class_id, void *inst)
{
    struct lock_held *h = this_cpu_ptr(lock_held);
    uint8_t i, j;
    unsigned long ra = (unsigned long) __builtin_return_address(0);

    if (h->depth == 0) {
        kprintf(
            "lock_debug: release of %s/%p with empty held-stack on cpu %u "
            "(caller ra=%lx)\n",
            class_name(class_id), inst, get_cpunum(), ra);
        crash();
    }

    /* Release is typically last-in-first-out: the unlock matches the
     * top-of-stack entry. Out-of-LIFO release is allowed. Match by (class,
     * instance) so per-object locks of the same class don't confuse the
     * matcher.
     */
    for (i = h->depth; i > 0; i--) {
        if (h->slots[i - 1].class_id == class_id &&
            h->slots[i - 1].inst == inst) {
            for (j = i - 1; j < h->depth - 1; j++)
                h->slots[j] = h->slots[j + 1];
            h->depth--;
            return;
        }
    }

    kprintf("lock_debug: release of %s/%p with no matching acquire on cpu %u\n",
            class_name(class_id), inst, get_cpunum());
    crash();
}

#endif /* CONFIG_LOCK_DEBUG */
