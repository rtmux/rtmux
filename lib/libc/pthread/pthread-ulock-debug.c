/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* Debug-only userspace lock-rank (lockdep-style) checker.
 *
 * Each thread keeps a stack of the mutexes it currently holds (in a per-process
 * table indexed by tid, malloc'd lazily). On every acquire we record a directed
 * "held-before" edge between LOCK ADDRESSES in a global graph: for each
 * already-held lock A, edge A -> new-lock. If the reverse edge (new-lock -> A)
 * is already present, some thread once took them in the opposite order (a
 * classic AB/BA deadlock cycle), so we report and abort.
 *
 * The lock address is the class key; a static server mutex is one stable
 * instance, so this never false-positives, it only under-classes distinct heap
 * per-object locks (acceptable MVP tradeoff). The acquire-site PC is carried
 * only as diagnostic data, printed alongside the addresses so addr2line can
 * resolve each acquire to a file:line.
 *
 * The whole translation unit is compiled away when CONFIG_ULOCK_DEBUG is unset,
 * so the Makefile can list it unconditionally.
 */

#ifdef CONFIG_ULOCK_DEBUG

#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include "cpucfg.h"

/* malloc uses a raw atomic alloc_lock and pthread_spin_* is a bare atomic word,
 * so neither recurses back into pthread_mutex_lock, so the checker may use both
 * freely. stdio and pthread_setspecific are off-limits (they take mutexes).
 */

#define ULOCK_HELD_MAX 16
#define ULOCK_EDGE_SLOTS 4096
#define ULOCK_TID_SLOTS 4096

/* Per-thread held-lock stack, keyed directly by LIBC_TLS()->__tid.
 *
 * The held stack is NOT kept in the TCB: __reserved2[0] is the kernel's cpupage
 * pointer, and the one spare slot __reserved2[1] is unusable here because a new
 * thread's TCB is byte-copied from its parent (kernel/ext/process.c): a child
 * would inherit and then corrupt the parent's stack pointer, and a fresh
 * process's slot is not zero-initialized. Instead a per-process table indexed
 * by tid holds one lazily-malloc'd stack per thread.
 *
 * The table is indexed by tid directly (not hashed): the kernel thread vector
 * reuses freed slots (kernel/object/vector.c), so a process's live tids stay
 * dense and bounded by its peak concurrent thread count, well under
 * ULOCK_TID_SLOTS for any realistic server. Direct indexing is therefore
 * collision-free among live threads, and the malloc'd stacks are bounded by
 * peak concurrency. A tid at or above ULOCK_TID_SLOTS (more concurrent threads
 * than the table covers) is left untracked with a one-shot diagnostic rather
 * than risking a wrong slot. A tid reused after a clean exit reuses the same
 * slot; the previous owner released all its locks, so the stack is already
 * empty. Every table and stack access happens under ulock_lock.
 */
struct ulock_held {
    unsigned depth;
    struct {
        const void *lock;
        const void *pc;
    } slots[ULOCK_HELD_MAX];
};

static struct ulock_held *ulock_tid_slots[ULOCK_TID_SLOTS];
static int ulock_tid_overflow; /* one-shot "tid out of table range" guard */

/* Global directed edge set: open-addressed hash of (from_lock, to_lock) address
 * pairs, guarded by the ulock_lock spinlock below.
 */
struct ulock_edge {
    const void *from;
    const void *to;
};

static struct ulock_edge ulock_edges[ULOCK_EDGE_SLOTS];
static int ulock_edges_full;    /* set once the edge table overflows */
static int ulock_edges_diagged; /* one-shot edge-table-full diagnostic guard */

/* A pthread_spinlock_t is a single _Atomic int whose unlocked state is 0
 * (SPIN_UNLOCKED), so static zero-initialization IS a valid unlocked lock, so
 * no pthread_spin_init / pthread_once is needed, and avoiding pthread_once
 * matters because it would recurse back through pthread_mutex_lock into this
 * hook.
 */
static pthread_spinlock_t ulock_lock;

/* hand-rolled, lock-free formatting (stdio takes locks) */

static size_t ulock_puts(char *buf, size_t pos, const char *s)
{
    while (*s)
        buf[pos++] = *s++;
    return pos;
}

static size_t ulock_puthex(char *buf, size_t pos, uintptr_t val)
{
    static const char hex[] = "0123456789abcdef";
    char tmp[2 * sizeof(uintptr_t)];
    unsigned n = 0;

    buf[pos++] = '0';
    buf[pos++] = 'x';
    do {
        tmp[n++] = hex[val & 0xf];
        val >>= 4;
    } while (val != 0);
    while (n != 0)
        buf[pos++] = tmp[--n];
    return pos;
}

static size_t ulock_putint(char *buf, size_t pos, int val)
{
    char tmp[3 * sizeof(int)];
    unsigned n = 0;
    unsigned u;

    if (val < 0) {
        buf[pos++] = '-';
        u = (unsigned) -(val + 1) + 1u;
    } else {
        u = (unsigned) val;
    }
    do {
        tmp[n++] = (char) ('0' + (u % 10u));
        u /= 10u;
    } while (u != 0);
    while (n != 0)
        buf[pos++] = tmp[--n];
    return pos;
}

/* One write(2) call; buffer is comfortably larger than the worst-case line. */
static void ulock_report_cycle(const void *lock_a,
                               const void *pc_a,
                               const void *lock_b,
                               const void *pc_b)
{
    char buf[256];
    size_t pos = 0;

    pos = ulock_puts(buf, pos, "ulock-rank: cycle tid=");
    pos = ulock_putint(buf, pos, LIBC_TLS()->__tid);
    pos = ulock_puts(buf, pos, " lockA=");
    pos = ulock_puthex(buf, pos, (uintptr_t) lock_a);
    pos = ulock_puts(buf, pos, " pcA=");
    pos = ulock_puthex(buf, pos, (uintptr_t) pc_a);
    pos = ulock_puts(buf, pos, " lockB=");
    pos = ulock_puthex(buf, pos, (uintptr_t) lock_b);
    pos = ulock_puts(buf, pos, " pcB=");
    pos = ulock_puthex(buf, pos, (uintptr_t) pc_b);
    pos = ulock_puts(buf, pos, " (addr2line -e <server> the pcs)\n");
    (void) write(2, buf, pos);
}

static void ulock_diag(const char *s)
{
    char buf[128];
    size_t pos = ulock_puts(buf, 0, s);
    (void) write(2, buf, pos);
}

/* edge graph */

/* Mix the two pointers into a slot index; addresses are pointer-aligned, so
 * shift off the dead low bits before mixing.
 */
static size_t ulock_edge_slot(const void *from, const void *to)
{
    uintptr_t h = ((uintptr_t) from >> 3) * 0x9e3779b1u;
    h ^= ((uintptr_t) to >> 3) * 0x85ebca77u;
    return (size_t) (h & (ULOCK_EDGE_SLOTS - 1));
}

static int ulock_edge_present(const void *from, const void *to)
{
    size_t i = ulock_edge_slot(from, to);
    size_t probe;

    for (probe = 0; probe < ULOCK_EDGE_SLOTS; probe++) {
        struct ulock_edge *e = &ulock_edges[i];
        if (e->from == NULL)
            return 0; /* empty slot: not inserted (linear probe stops here) */
        if (e->from == from && e->to == to)
            return 1;
        i = (i + 1) & (ULOCK_EDGE_SLOTS - 1);
    }
    return 0;
}

/* Insert edge from -> to if absent. Caller holds ulock_lock. On overflow set
 * the one-shot full flag but do NOT emit the diagnostic here: write(2) can
 * block and would strand every other hook spinning on ulock_lock. The caller
 * emits it after unlocking.
 */
static void ulock_edge_insert(const void *from, const void *to)
{
    size_t i = ulock_edge_slot(from, to);
    size_t probe;

    for (probe = 0; probe < ULOCK_EDGE_SLOTS; probe++) {
        struct ulock_edge *e = &ulock_edges[i];
        if (e->from == NULL) {
            e->from = from;
            e->to = to;
            return;
        }
        if (e->from == from && e->to == to)
            return; /* already present */
        i = (i + 1) & (ULOCK_EDGE_SLOTS - 1);
    }
    ulock_edges_full = 1;
}

/* per-thread held stack */

/* Return this thread's held stack, allocating it on first use. Caller holds
 * ulock_lock.
 *
 * Returns NULL if tid is outside the table range (more concurrent threads than
 * the table covers) or the debug aid is out of memory; the caller then skips
 * tracking for this acquire.
 */
static struct ulock_held *ulock_held_for(int tid)
{
    struct ulock_held **pp;
    struct ulock_held *h;

    if ((unsigned) tid >= ULOCK_TID_SLOTS)
        return NULL;
    pp = &ulock_tid_slots[tid];
    h = *pp;
    if (h == NULL) {
        h = malloc(sizeof(*h));
        if (h == NULL)
            return NULL;
        h->depth = 0;
        *pp = h;
    }
    return h;
}

void __ulock_debug_acquire(const void *lock, const void *pc)
{
    int tid = LIBC_TLS()->__tid;
    struct ulock_held *h;
    unsigned i;
    int diag_tid = 0, diag_edges = 0;

    (void) pthread_spin_lock(&ulock_lock);
    h = ulock_held_for(tid);
    if (h == NULL) {
        if ((unsigned) tid >= ULOCK_TID_SLOTS && !ulock_tid_overflow) {
            ulock_tid_overflow = 1;
            diag_tid = 1;
        }
        (void) pthread_spin_unlock(&ulock_lock);
        if (diag_tid)
            ulock_diag(
                "ulock-rank: tid beyond table range, thread untracked\n");
        return;
    }

    /* Recursive re-lock of an already-held lock: no push, no edge. */
    for (i = 0; i < h->depth; i++) {
        if (h->slots[i].lock == lock) {
            (void) pthread_spin_unlock(&ulock_lock);
            return;
        }
    }

    for (i = 0; i < h->depth; i++) {
        const void *held = h->slots[i].lock;
        /* Reverse edge already recorded means an AB/BA cycle. */
        if (ulock_edge_present(lock, held)) {
            const void *held_pc = h->slots[i].pc;
            (void) pthread_spin_unlock(&ulock_lock);
            ulock_report_cycle(held, held_pc, lock, pc);
            abort();
        }
        ulock_edge_insert(held, lock);
    }

    if (h->depth == ULOCK_HELD_MAX) {
        (void) pthread_spin_unlock(&ulock_lock);
        ulock_diag("ulock-rank: held-stack overflow, raise ULOCK_HELD_MAX\n");
        abort();
    }
    h->slots[h->depth].lock = lock;
    h->slots[h->depth].pc = pc;
    h->depth++;
    if (ulock_edges_full && !ulock_edges_diagged) {
        ulock_edges_diagged = 1;
        diag_edges = 1;
    }
    (void) pthread_spin_unlock(&ulock_lock);

    /* Emit the edge-table-full diagnostic outside the lock (write can block).
     */
    if (diag_edges)
        ulock_diag("ulock-rank: edge table full, coverage truncated\n");
}

void __ulock_debug_release(const void *lock)
{
    int tid = LIBC_TLS()->__tid;
    struct ulock_held *h;
    unsigned i;

    if ((unsigned) tid >= ULOCK_TID_SLOTS)
        return;
    (void) pthread_spin_lock(&ulock_lock);
    h = ulock_tid_slots[tid];
    if (h != NULL) {
        /* Search from the top, shift the rest down (LIFO-tolerant). A lock not
         * found was acquired before the stack existed, ignore it.
         */
        for (i = h->depth; i > 0; i--) {
            if (h->slots[i - 1].lock == lock) {
                unsigned j;
                for (j = i - 1; j + 1 < h->depth; j++)
                    h->slots[j] = h->slots[j + 1];
                h->depth--;
                break;
            }
        }
    }
    (void) pthread_spin_unlock(&ulock_lock);
}

#endif /* CONFIG_ULOCK_DEBUG */
