/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef _KERNEL_LOCK_H
#define _KERNEL_LOCK_H

#include <inttypes.h>
#include <stddef.h>
#include <sys/spinlock.h>
#include <cpu-inline.h> /* interrupt_lock / interrupt_unlock */
#include "percpu.h"

/* Kernel locking primitives — per-subsystem fine-grained lock taxonomy.
 *
 * Three named tiers, all aliasing primitives that already exist:
 *
 *   kspinlock_t       — leaf locks shared with ISR handlers; short hold;
 *                       acquire disables interrupts.  Aliases intrspin_t
 *                       (internal/include/sys/spinlock.h).
 *
 *   kspinlock_irq_t   — same as kspinlock_t, but the caller may already
 *                       have IRQs in either state and wants the lock to
 *                       save/restore the prior flags.  Carries a saved
 *                       sstatus snapshot alongside the spin word.
 *
 *   kmutex_t          — sleepable; for paths that may block on IPC or
 *                       page-fault.  See include/kmutex.h.
 *
 * "krwlock_t" is a forward-declaration only. Reader/writer locks are not
 * provided until a measured workload demonstrates a clear read-mostly hot path;
 * adding them speculatively burns cache-line bandwidth with no benefit.
 *
 * Lock-class registration (LOCK_CLASS_*) supplies a static rank for each class
 * so the lock-rank checker can detect invalid nesting. Instances pass the class
 * as the second argument to kspinlock_init.
 *
 * lock.h pulls in <sys/spinlock.h> for intrspin_t and "percpu.h" for the
 * DECLARE_PER_CPU macro, so it is self-contained from a kernel translation
 * unit's point of view.
 */

#ifndef __ASSEMBLY__

/* Lock types */

/* kspinlock_t — fine-grained kernel spinlock with a class tag.
 *
 * The actual mutual-exclusion primitive is the embedded intrspin,
 * acquired/released by the existing arch interrupt_lock/Unlock pair. The
 * class_id field carries a LOCK_CLASS_* value so the lock-rank checker (under
 * CONFIG_LOCK_DEBUG) can verify that every nested acquire goes from lower-rank
 * to higher-rank.
 *
 * One byte of class plus the natural padding costs eight bytes per lock
 * instance compared to a bare intrspin_t — negligible.
 */
typedef struct kspinlock {
    struct intrspin spin;
    uint8_t class_id;

    /* Owner-hart instrumentation. Written by kspinlock_lock AFTER successful
     * acquire, cleared by kspinlock_unlock BEFORE release. 0xFF = unheld; any
     * other byte is the hart ID of the current owner. Lets gdb read who holds
     * the slock a spinner is wedged on.
     */
    uint8_t owner_hart;
} kspinlock_t;

typedef struct kspinlock_irq {
    struct intrspin lock;
    unsigned long saved_flags;
} kspinlock_irq_t;

/* Forward declaration only — real type appears when readers/writers are first
 * needed.
 */
struct krwlock;
typedef struct krwlock krwlock_t;


/* Lock classes (for the lock-rank checker) */

enum lock_class {
    LOCK_CLASS_UNCLASSIFIED = 0, /* placeholder for un-annotated locks */

    /* Numerically lower = acquired first in any nesting; declarations here
     * freeze the rank order so new locks can't silently invert it.
     */
    LOCK_CLASS_TURNSTILE,       /* sync_t chain locks (turnstile-style;
                                 * held across the dispatcher swap by
                                 * unready_release/block_release, so it
                                 * must sit BELOW SCHED in rank order).
                                 */
    LOCK_CLASS_TURNSTILE_OWNER, /* owner-held PI turnstile lists; acquired
                                 * after a sync chain lock and before SCHED
                                 * priority adjustments.
                                 */
    LOCK_CLASS_SCHED,           /* per-CPU ready queues */
    LOCK_CLASS_TIMER,           /* timer wheels */
    LOCK_CLASS_INTREVENT,       /* interrupt event queues */
    /* LOCK_CLASS_PROCVEC — per-process vector slock (process_t.vec_slock). Rank
     * intentionally LOWER than CHANNEL/CONNECT so a vec_lookup under
     * prp->vec_slock can be followed by acquisition of the found object's
     * per-instance lock (chp->lock / cop->lock). This is the canonical "find
     * then act" pattern for IPC namespace kcalls (ksys_connect_attach / detach
     * / *_info, ksys_channel_*).
     */
    LOCK_CLASS_PROCVEC, /* per-process fdcons/chancons vector ops */
    LOCK_CLASS_CHANNEL, /* per-channel state */
    LOCK_CLASS_CONNECT, /* per-connection state */
    LOCK_CLASS_SYNC,    /* sync_t hash + ksync_t */
    LOCK_CLASS_PROCESS, /* per-process struct (non-vec scalar fields) */
    LOCK_CLASS_THREAD,  /* per-thread struct */
    LOCK_CLASS_SOUL,    /* object soul freelists + accounting (soul_slock):
                         * ranked after the object locks that callers hold on
                         * an object_alloc/object_free path (CHANNEL, PROCVEC,
                         * THREAD, TIMER, INTREVENT) and before MEMPART so a
                         * grow/shrink may nest SOUL -> MEMPART into the heap.
                         */
    LOCK_CLASS_MEMPART, /* memory partition / heap */
    LOCK_CLASS_PA,      /* physical quantum allocator (pa_slock):
                         * leaf, ranked after MEMPART so the heap
                         * (alloc_slock, MEMPART) may call pa_alloc
                         * while held.  pa_alloc/_pa_free take no
                         * other lock while holding it.
                         */
    LOCK_CLASS_DEBUG,   /* debugger interlock */

    LOCK_CLASS_NR /* must stay last */
};


/* kspinlock API — wraps existing intrspin_t primitives The acquisition
 * primitive is the existing arch interrupt_lock / interrupt_unlock pair
 * (declared static inline in arch/<cpu>/include/cpu-inline.h), which on RISC-V
 * uses AMOSWAP.W with acquire/release ordering and unconditionally disables
 * interrupts for the held interval. These wrappers exist so that call sites
 * read in terms of the lock taxonomy, and so that the lock-rank checker can
 * hook the class argument without further header churn. Translation units
 * calling kspinlock_lock must have cpu-inline.h in scope (the kernel include
 * chain via core/globals.h supplies it transitively).
 */

#ifdef CONFIG_LOCK_DEBUG

/* The instance pointer ("inst") is the lock pointer itself — passes "&l->spin"
 * from kspinlock_lock so the held-stack can distinguish per-object locks of the
 * same class (per-channel_t, per-connect_t, per-process_t). Same-class
 * same-instance acquires remain forbidden (recursive deadlock); same-class
 * distinct-instance is legal.
 */
void lock_debug_record_acquire(uint8_t class_id, void *inst);
void lock_debug_record_release(uint8_t class_id, void *inst);
void lock_debug_allow_turnstile_pair(void);
uint8_t lock_debug_held_depth(void);
#else
#define lock_debug_allow_turnstile_pair() ((void) 0)
#endif

/* Initialize a kspinlock: clear the spin word, record the lock name and class,
 * and mark it unheld. Parameters:
 *   l    : lock to initialize
 *   class_id : LOCK_CLASS_* rank for the debug checker
 *   name : human-readable lock name (for diagnostics)
 */
static inline void kspinlock_init(kspinlock_t *l,
                                  enum lock_class class_id,
                                  char *name)
{
    l->spin.value = 0;
    l->spin.name = name;
    l->class_id = (uint8_t) class_id;
    l->owner_hart = 0xFF;
}

/* Acquire a kspinlock: disable interrupts, spin-acquire, then record the owner
 * hart and bump the per-CPU nesting depth. Parameters:
 *   l : lock to acquire
 */
#if defined(CONFIG_INSTRUMENTATION) && defined(KERNEL_INTERNAL)

/* section 18 interrupt-disabled window instrumentation (globals.c). Opened on
 * the outermost kspinlock acquire, closed on the outermost release.
 */
void irqoff_open(int class);
void irqoff_close(void);
#endif

static inline void kspinlock_lock(kspinlock_t *l)
{
    /* interrupt_lock disables SIE then spin-acquires. After acquire, bump the
     * per-CPU nesting depth so the matching unlock knows whether it's the
     * outermost (must restore SIE) or inner (must leave SIE off — outer lock
     * still expects IRQs disabled).
     *
     * Order: spin-acquire first, then depth++ / owner=me. Cross-hart spinners
     * only observe the held lock; they don't read our pcpu_array slot.
     */
    interrupt_lock(&l->spin);
#ifdef KERNEL_INTERNAL
    pcpu_array[get_cpunum()].pc_kspin_depth++;
    l->owner_hart = (uint8_t) get_cpunum();
#if defined(CONFIG_INSTRUMENTATION)
    if (pcpu_array[get_cpunum()].pc_kspin_depth == 1)
        irqoff_open(l->class_id);
#endif
#endif
#ifdef CONFIG_LOCK_DEBUG
    lock_debug_record_acquire(l->class_id, &l->spin);
#endif
}

/* Release a kspinlock: mark it unheld, then on the outermost unlock (nesting
 * depth 1 -> 0) re-enable interrupts via interrupt_unlock. Parameters:
 *   l : lock to release
 */
static inline void kspinlock_unlock(kspinlock_t *l)
{
#ifdef CONFIG_LOCK_DEBUG
    lock_debug_record_release(l->class_id, &l->spin);
#endif
#ifdef KERNEL_INTERNAL
    l->owner_hart = 0xFF;

    /* Inner-unlock path: release the spin with amoswap.w.rl but KEEP SIE off,
     * because a still-held outer kspinlock requires IRQs disabled. Only on the
     * outermost unlock (depth 1 → 0) do we re-enable SIE via interrupt_unlock's
     * interrupt_enable.
     *
     * Re-enabling SIE on an inner unlock would leave a window — between the
     * inner unlock and the outer unlock — where a SEXT could trap back into the
     * kernel with the outer kspinlock still held; the trap path's call to
     * ready() would then take the same outer lock and self-deadlock.
     */
    if (--pcpu_array[get_cpunum()].pc_kspin_depth > 0) {
        __atomic_store_n(&l->spin.value, 0, __ATOMIC_RELEASE);
        return;
    }
#if defined(CONFIG_INSTRUMENTATION)
    irqoff_close();
#endif
#endif
    interrupt_unlock(&l->spin);
}

/* kspinlock_lock_irqsave / kspinlock_unlock_irqrestore — acquire/release a
 * kspinlock while saving and restoring the caller's prior SIE state.
 *
 * Use this when the caller may already be running with interrupts disabled
 * (e.g. inside an ISR, like timer_expiry() in the clock trap) and must NOT have
 * SIE unconditionally re-enabled by the outermost unlock. The plain
 * kspinlock_lock/unlock pair re-enables SIE on the outermost unlock, which is
 * correct in kernel-call context but wrong inside a trap whose handler runs
 * with SIE off: re-enabling SIE mid-ISR would let a nested clock IRQ re-enter
 * and corrupt the outer frame's per-CPU state.
 *
 * The save/restore semantics are otherwise identical to kspinlock_lock/unlock:
 * depth bookkeeping, owner-hart marking, and lock-rank recording all match, so
 * mixed nesting with the plain pair stays consistent.
 */
static inline unsigned long kspinlock_lock_irqsave(kspinlock_t *l)
{
    unsigned long sie = (unsigned long) intr_get();
    kspinlock_lock(l);
    return sie;
}

static inline void kspinlock_unlock_irqrestore(kspinlock_t *l,
                                               unsigned long sie)
{
#ifdef CONFIG_LOCK_DEBUG
    lock_debug_record_release(l->class_id, &l->spin);
#endif
#ifdef KERNEL_INTERNAL
    l->owner_hart = 0xFF;

    /* Inner-unlock path: identical to kspinlock_unlock — release with
     * amoswap.w.rl but keep SIE off, since a still-held outer lock needs IRQs
     * disabled.
     */
    if (--pcpu_array[get_cpunum()].pc_kspin_depth > 0) {
        __atomic_store_n(&l->spin.value, 0, __ATOMIC_RELEASE);
        return;
    }
#if defined(CONFIG_INSTRUMENTATION)

    /* Close the window opened by kspinlock_lock: the irqsave path acquires
     * through kspinlock_lock too, so it must close here or the open leaks and
     * poisons the next window. section 18.
     */
    irqoff_close();
#endif

    /* Outermost unlock: release the spin word WITHOUT unconditionally enabling
     * SIE (unlike kspinlock_unlock, which calls interrupt_unlock →
     * interrupt_enable), then restore only the caller's prior SIE state.
     */
    __atomic_store_n(&l->spin.value, 0, __ATOMIC_RELEASE);
    if (sie)
        interrupt_enable();
#else
    (void) l;
    (void) sie;
#endif
}


/* Per-CPU object-allocator override.
 *
 * Expresses "for *this* allocation, override the soul's default critical-or-not
 * classification." Per-CPU storage carries each call's intent without mutating
 * shared state — a global toggle of notification_souls.flags's SOUL_CRITICAL
 * bit would be racy across CPUs: thread A clearing the bit could starve thread
 * B's legitimate critical allocation, and vice-versa.
 *
 * Values:
 *   SOUL_OVERRIDE_NONE         — use the soul's default flags.
 *   SOUL_OVERRIDE_NONCRITICAL  — treat as non-critical regardless of
 *                                soulp->flags & SOUL_CRITICAL
 *                                (used by ksys_msg_deliver_event,
 *                                ksys_msg_sendnotification — user-facing
 *                                notification alloc).
 *   SOUL_OVERRIDE_CRITICAL     — treat as critical
 *                                (used by force_ready's UNBLOCK
 *                                notification — must succeed even under
 *                                heap pressure).
 *
 * object_alloc consults this counter and falls back to the soul's own
 * SOUL_CRITICAL bit when the override is NONE.
 */
#define SOUL_OVERRIDE_NONE 0
#define SOUL_OVERRIDE_NONCRITICAL 1
#define SOUL_OVERRIDE_CRITICAL 2

/* soul_alloc_override[] now lives at pcpu_array[hart].pc_soul_alloc_override */


#endif /* __ASSEMBLY__ */

#endif /* _KERNEL_LOCK_H */
