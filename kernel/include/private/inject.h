/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Debug-only fault injection (CONFIG_DEBUG_INJECT). One private call,
 * DEBUG_INJECT, gated by its own capability RTMUX_PRIV_CAP_INJECT, so tests can
 * produce a deterministic stall or allocation failure instead of waiting for a
 * race. With the option off the call is unbound (ENOSYS) and the code is not
 * built. See docs/capability-abi.md.
 */
#ifndef _PRIVATE_INJECT_H
#define _PRIVATE_INJECT_H

#include <stdatomic.h>
#include <stdint.h>

/* SPIN_IRQOFF(hart, ms): hart busy-waits ms (<= 10000) with interrupts off. */
#define INJECT_OP_SPIN_IRQOFF 1
/* HANG_HART(hart): hart busy-waits for ever with interrupts off. */
#define INJECT_OP_HANG_HART 2

/* SPIN_CONSOLE(hart, ms): like SPIN_IRQOFF, but hart holds the console lock for
 * the whole spin, so any other hart's ordinary kprintf waits (the fixture for
 * the watchdog's lock-free report).
 */
#define INJECT_OP_SPIN_CONSOLE 5

/* FAIL_NEXT_ALLOC(kstat_soul_id, pid): the next ordinary object_alloc of that
 * pool charged to that process fails once.
 */
#define INJECT_OP_FAIL_NEXT_ALLOC 3
/* ALLOC_STATUS: ret is 1 if the armed failure was consumed, else 0. */
#define INJECT_OP_ALLOC_STATUS 4

/* FAIL_NEXT_SCHED_SET(pid, count): the next count SchedSet kcalls by that
 * process that lower the caller's own priority fail with EINVAL, after they
 * have applied the change. A kcall that reschedules its caller and then fails
 * is otherwise unreachable, and the test of the error status reaching a thread
 * that was switched out needs one. Each failure first waits a short window with
 * the other harts kicked, so a caller exposed to them inside the call is
 * resumed there rather than racing the status store by luck.
 */
#define INJECT_OP_FAIL_NEXT_SCHED_SET 6
/* SCHED_SET_STATUS: ret is how many armed failures have been delivered. */
#define INJECT_OP_SCHED_SET_STATUS 7

/* SCHED_SET_DISPLACED: ret is how many injected failures found their caller
 * switched out, or its switch still owed to kernel_exit, when the error status
 * was written.
 */
#define INJECT_OP_SCHED_SET_DISPLACED 8

/* FAIL_NEXT_TURNSTILE(pid): the next allocation of a PI turnstile on behalf of
 * that process fails once, whether it is a condvar wait's reservation or a
 * contended lock's own. SITE_STATUS(TURNSTILE) reports whether it was consumed.
 */
#define INJECT_OP_FAIL_NEXT_TURNSTILE 9

/* STALL_MUTEX_LOCK(pid, ms): the next contended PI mutex lock by that process
 * stalls for ms milliseconds in the window where it has published its wait but
 * not yet queued, with the chain lock dropped. SITE_STATUS(STALL_LOCK) reports
 * whether the stall was taken. A race that needs another thread to act in that
 * window is otherwise a matter of luck.
 */
#define INJECT_OP_STALL_MUTEX_LOCK 10

/* STALL_FORCE_READY(pid, ms): the next force_ready of a thread of that process
 * that read it as a condvar, semaphore or mutex waiter stalls for ms
 * milliseconds before it takes the wait chain lock, the window in which the
 * waiter can be woken and block elsewhere. SITE_STATUS(STALL_FORCE_READY)
 * reports whether the stall was taken.
 */
#define INJECT_OP_STALL_FORCE_READY 11

/* PTE_SUPERPAGE(pid, va): make the 2 MiB slot at va, which must be empty and
 * under a table that exists, a 2 MiB leaf in that process's page table, so a
 * MAP over it meets a superpage. PTE_CLEAR_SUPERPAGE(pid, va) takes it out
 * again. Planting also clears MANIP_PARTIAL. The leaf is read-only and maps
 * physical page 0; the test must not touch it. The target must be a live
 * process that is not exiting (a test names itself): an address space is freed
 * without waiting for a reader.
 */
#define INJECT_OP_PTE_SUPERPAGE 12
#define INJECT_OP_PTE_CLEAR_SUPERPAGE 13

/* PTE_PROBE(pid, va): ret is 1 if that process's page table has a valid leaf
 * mapping va, else 0, or minus an errno if the process or address is bad.
 */
#define INJECT_OP_PTE_PROBE 14

/* MANIP_PARTIAL: ret is how many pages the most recent MAP that failed in
 * cpu_pte_manipulate had done when it failed, as the kernel reported it to
 * roottask (0 if none failed since a PTE_SUPERPAGE).
 */
#define INJECT_OP_MANIP_PARTIAL 15

/* FAIL_PTE_MAP(pid, pages | va): the next MAP of more than pages pages in that
 * process's address space stops after pages pages with ENOMEM, as if the page
 * table allocator ran dry there. The page-aligned va ORed into the low pages
 * bits (pages is under 4096) limits it to the MAP that starts at va; 0 means
 * any. ALLOC_STATUS reports whether it was taken. Planting a superpage or
 * arming this clears MANIP_PARTIAL.
 */
#define INJECT_OP_FAIL_PTE_MAP 16

/* STALL_MUTEX_EARLY(pid, ms): like STALL_MUTEX_LOCK, but the stall comes
 * earlier, with the chain lock held, after the contended lock saw the mutex
 * busy and before it publishes WAITING. A holder that releases in user space
 * meanwhile leaves a free word for the waiter to publish onto.
 */
#define INJECT_OP_STALL_MUTEX_EARLY 17

/* STALL_MUTEX_PROPAGATED(pid, ms): like STALL_MUTEX_LOCK, but the stall comes
 * after the waiter has lent its priority to the owner and before it takes the
 * chain lock again, the window in which an unlock hands the mutex to another
 * waiter that was not lent it.
 */
#define INJECT_OP_STALL_MUTEX_PROPAGATED 18

/* PRIO_SETTLE_STATS(what): what 0, ret is how many deferred priority settles
 * (K29) ran in thread context since the last reset; 1, how many priority
 * recomputations ran inside force_ready's client departure, which must be none:
 * force_ready runs in timer and event context under caller locks; 2 resets the counters and 3 reads
 * the subset of deferred settles run on idle harts with interrupts off.
 */
#define INJECT_OP_PRIO_SETTLE_STATS 19

/* SITE_STATUS(site) returns one after that particular injection was taken.
 * SITE_RELEASE(site) ends the current stall before its watchdog expires. */
#define INJECT_OP_SITE_STATUS 20
#define INJECT_OP_SITE_RELEASE 21
#define INJECT_OP_STALL_SYNC_WAKER 22
#define INJECT_OP_SYNC_WAKE_CLAIMS 23
#define INJECT_OP_INTR_BEFORE_SWITCH 24

/* STALL_BLOCK(pid, tid << 32 | ms): thread tid (as pthread_t) of pid stalls
 * the next time it is about to publish a blocked state: past every pending
 * check of the kernel call, still running. A kill or a cancel posted during
 * the stall finds a running thread and must still reach it once it blocks.
 */
#define INJECT_OP_STALL_BLOCK 25

/* CRASH(how): the calling hart reports a fatal error and takes the kernel's
 * crash path, as an assertion failure does (how 0), or the shutdown() path a
 * fatal kernel fault takes, which prints the Shutdown[...] banner and powers
 * the machine off through the SBI (how INJECT_CRASH_SHUTDOWN). For the
 * harness's crash dump.
 */
#define INJECT_OP_CRASH 26
#define INJECT_CRASH_SHUTDOWN 1

/* STALL_REPLY_CLAIM(pid, ms): the next reply, error or follow-up copy by a
 * thread of that process stalls after the rcvid lookup and before it reads the
 * connection's channel, until a channel destroy on another hart has detached
 * the connection or ms milliseconds pass. SITE_STATUS(STALL_REPLY_CLAIM)
 * reports whether the stall was taken, and
 * SITE_STATUS(INJECT_SITE_REPLY_DETACHED) whether a detach ended it.
 */
#define INJECT_OP_STALL_REPLY_CLAIM 27

/* PREEMPT_REPLY_UNBLOCK(pid): the next reply by a thread of that process that
 * removes a queued unblock notification restarts as if a preemption were
 * pending (after a 1 ms stall). SITE_STATUS(STALL_REPLY_UNBLOCK) reports
 * whether it was.
 */
#define INJECT_OP_PREEMPT_REPLY_UNBLOCK 28

/* XFER_TRACE(pid): count, for the threads of that process, what its stepped
 * message copies do from now on. XFER_STATS(what) reads a counter:
 * INJECT_XFER_STARTS stepped copies begun from byte 0, INJECT_XFER_YIELDS
 * steps that ended in a restart, INJECT_XFER_PEEKS transfers of a message's
 * head to or from a kernel buffer (the header peek and its write-back), and
 * INJECT_XFER_BUDGETS steps (of a copy or a vector scan) that ended because
 * their time was up, not because a preemption was pending.
 */
#define INJECT_OP_XFER_TRACE 29
#define INJECT_OP_XFER_STATS 30

/* STALL_CLAIM_HELD(pid, ms): the next msg_readv by a thread of that process
 * stalls with its claim on the client held, until SITE_RELEASE or ms.
 * STALL_FORCE_CLAIMED(pid, ms): the next force_ready of a thread of that
 * process that finds it claimed releases the STALL_CLAIM_HELD stall and then
 * waits, before it hands its wake to the claimer, for the claim to go or ms
 * to pass. Together they put a claimer's release inside a waker's decision.
 */
#define INJECT_OP_STALL_CLAIM_HELD 33
#define INJECT_OP_STALL_FORCE_CLAIMED 34

/* STALL_CAP_PEEK(pid, tid << 32 | ms): thread tid (as pthread_t) of pid
 * stalls the next time it has copied a long message and is about to look at
 * its head for a native capability header: the window in which a sibling of
 * the sender can rewrite the header the receiver was already given.
 */
#define INJECT_OP_STALL_CAP_PEEK 35

enum {
    INJECT_XFER_STARTS,
    INJECT_XFER_YIELDS,
    INJECT_XFER_PEEKS,
    INJECT_XFER_BUDGETS,
    INJECT_XFER_COUNTERS
};

/* INTR_AFTER_SELECT(pid): like INTR_BEFORE_SWITCH, but the interrupt arrives
 * after the blocking IST has chosen the next thread, the window in which the
 * scheduler drops the dispatch lock to look for a thread to steal.
 */
#define INJECT_OP_INTR_AFTER_SELECT 31

/* INTR_SELECT_SEEN: ret is what the last INTR_AFTER_SELECT injection found: 0
 * not taken, 1 no thread had been chosen, or 2 plus the chosen thread's
 * pthread_t shifted left by 8. Reading it resets it.
 */
#define INJECT_OP_INTR_SELECT_SEEN 32

enum inject_site {
    STALL_LOCK,
    STALL_LOCK_EARLY,
    STALL_LOCK_PROPAGATED,
    STALL_FORCE_READY,
    STALL_SYNC_WAKER,
    STALL_BLOCK,
    STALL_REPLY_CLAIM,
    STALL_REPLY_UNBLOCK,
    STALL_CLAIM_HELD,
    STALL_FORCE_CLAIMED,
    STALL_CAP_PEEK,
    STALL_SITES,
    INJECT_SITE_TURNSTILE = STALL_SITES,
    INJECT_SITE_REPLY_DETACHED
};

#define INJECT_SPIN_MAX_MS 10000

/* Injected interrupts-off spins, for intrevent_flush()'s debug spin watchdog: a
 * flush that waits on a deliberately stalled hart is the injected condition,
 * not a kernel bug, so it must not crash() while the stall lasts. One word
 * keeps the snapshot coherent: the high half counts spins ever started, the low
 * half harts now inside one (a hung hart never leaves). The spinning hart's IPI
 * handler starts a spin with inject_spin_begin() and ends it with
 * inject_spin_end(). A waiter takes inject_stall_token() and later asks
 * inject_stall_overlapped(): true if a spin was active at the token or any spin
 * began or ended since, even one that finished while the waiter was itself held
 * up.
 */
extern atomic_ullong inject_spin_state;

static inline void inject_spin_begin(void)
{
    atomic_fetch_add_explicit(&inject_spin_state, (1ULL << 32) + 1,
                              memory_order_relaxed);
}

static inline void inject_spin_end(void)
{
    atomic_fetch_sub_explicit(&inject_spin_state, 1, memory_order_relaxed);
}

static inline unsigned long long inject_stall_token(void)
{
    return atomic_load_explicit(&inject_spin_state, memory_order_relaxed);
}

static inline int inject_stall_overlapped(unsigned long long token0)
{
    return (token0 & 0xffffffffULL) != 0 || inject_stall_token() != token0;
}

struct kcall_args_debug_inject {
    unsigned op;
    uint64_t arg0;
    uint64_t arg1;
    int ret; /* 0 / EINVAL / ..., or the ALLOC_STATUS answer */
};

#endif
