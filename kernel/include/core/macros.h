/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Macros that are private to the kernel itself.
 *
 * SMP kernel serialization uses the global "inkernel" variable as both state
 * tracker and mutex.
 */

/* Process / message IDs */

#define PID_MASK 0xfff
#define PINDEX(pid) ((pid) & PID_MASK)

#define MTINDEX(mid) ((unsigned) (mid) >> 16)
#define MCINDEX(mid) ((unsigned) (mid) & 0xffff)

/* Scheduler dispatching */

/* The dispatch ready queue stores LINK3_NODE pointers; each node lives inside
 * its owning thread_t as the dedicated "link3_link" field (kept disjoint from
 * the PRIL_ENTRY_FIELDS at the head of thread_t so the sync_slock-protected
 * PRIL writes and the sched_slock-protected dispatch writes never collide).
 * DISPATCH_THP() back-converts the head LINK3_NODE* to its enclosing thread_t*.
 */
#define DISPATCH_THP(dpp, pri) link3_to_thread((dpp)->ready[pri].head)
#define DISPATCH_LST(dpp, pri) ((dpp)->ready[pri])

/* Per-CPU sched-lock helpers. Each per-CPU dispatch instance carries its own
 * kspinlock_t lock. Same-CPU dispatch ops use the _LOCAL form; cross-CPU ops
 * pass the target dpp explicitly. Multi-dpp sequences must acquire locks in
 * ascending dpp-pointer order (= ascending hart_id) to avoid deadlock —
 * lock_debug doesn't enforce ordering across same-class locks.
 */
#define LOCAL_DPP (get_pcpu()->pc_dispatch)
#define SCHED_LOCK_LOCAL() kspinlock_lock(&LOCAL_DPP->lock)
#define SCHED_UNLOCK_LOCAL() kspinlock_unlock(&LOCAL_DPP->lock)
#define SCHED_LOCK(dpp) kspinlock_lock(&(dpp)->lock)
#define SCHED_UNLOCK(dpp) kspinlock_unlock(&(dpp)->lock)

#if NUM_PRI <= 8

#define DISPATCH_HIGHEST_PRI(dpp) (byte_log2[(dpp)->lo])
#define DISPATCH_ISSET(dpp, pri) ((dpp)->lo & (1 << pri))
#define DISPATCH_SET(dpp, thp)             \
    {                                      \
        (dpp)->lo |= 1 << (thp)->priority; \
    }
#define DISPATCH_CLR(dpp, thp)                \
    {                                         \
        (dpp)->lo &= ~(1 << (thp)->priority); \
    }

#elif NUM_PRI <= 8 * 8

#define DISPATCH_HI(dpp) byte_log2[(dpp)->hi]
#define DISPATCH_LO(dpp) byte_log2[(dpp)->lo[DISPATCH_HI(dpp)]]
#define DISPATCH_HIGHEST_PRI(dpp) ((DISPATCH_HI(dpp) << 3) | DISPATCH_LO(dpp))
#define DISPATCH_ISSET(dpp, pri) ((dpp)->lo[pri >> 3] & (1 << (pri & 7)))

#define DISPATCH_SET(dpp, thp)                       \
    {                                                \
        register unsigned hi = (thp)->priority >> 3; \
        (dpp)->hi |= 1 << hi;                        \
        (dpp)->lo[hi] |= 1 << ((thp)->priority & 7); \
    }

#define DISPATCH_CLR(dpp, thp)                                     \
    {                                                              \
        register unsigned hi = (thp)->priority >> 3;               \
        if (((dpp)->lo[hi] &= ~(1 << ((thp)->priority & 7))) == 0) \
            (dpp)->hi &= ~(1 << hi);                               \
    }

#elif NUM_PRI <= 8 * 8 * 8

#define DISPATCH_HI(dpp) byte_log2[(dpp)->hi]
#define DISPATCH_MID(dpp) byte_log2[(dpp)->mid[DISPATCH_HI(dpp)]]
#define DISPATCH_LO(dpp) \
    byte_log2[(dpp)->lo[DISPATCH_HI(dpp)][DISPATCH_MID(dpp)]]
#define DISPATCH_HIGHEST_PRI(dpp) \
    ((DISPATCH_HI(dpp) << 6) | (DISPATCH_MID(dpp) << 3) | DISPATCH_LO(dpp))
#define DISPATCH_ISSET(dpp, pri) \
    ((dpp)->lo[pri >> 6][(pri >> 3) & 7] & (1 << (pri & 7)))

#define DISPATCH_SET(dpp, thp)                              \
    {                                                       \
        register unsigned hi = (thp)->priority >> 6;        \
        register unsigned mid = ((thp)->priority >> 3) & 7; \
        (dpp)->hi |= 1 << hi;                               \
        (dpp)->mid[hi] |= 1 << mid;                         \
        (dpp)->lo[hi][mid] |= 1 << ((thp)->priority & 7);   \
    }

#define DISPATCH_CLR(dpp, thp)                                   \
    {                                                            \
        register unsigned hi = (thp)->priority >> 6;             \
        register unsigned mid = ((thp)->priority >> 3) & 7;      \
        if (((dpp)->lo[hi][mid] &=                               \
             (uint8_t) ~(1u << ((thp)->priority & 7))) == 0)     \
            if (((dpp)->mid[hi] &= (uint8_t) ~(1u << mid)) == 0) \
                (dpp)->hi &= (uint8_t) ~(1u << hi);              \
    }

#else
#error NUM_PRI too high
#endif

/* Bit sets
 *
 * Signal macros removed — signal subsystem eliminated. Bit-test macros retained
 * for fltset_t (debugger fault sets).
 */

#define BITSET_INDEX(n) (((n) - 1) >> 5)
#define BITSET_BIT(n) (1u << (((n) - 1) & 0x1f))
#define BITSET_TST(set, n) ((set).bits[BITSET_INDEX(n)] & BITSET_BIT(n))

/* Vectors */

#define VECP(ptr, vec, index) \
    (((uintptr_t) ((ptr) = (vec)->vector[index]) & 3) ? NULL : (ptr))
#define VECP2(ptr, vec, index)                               \
    (((uintptr_t) ((ptr) = (vec)->vector[index]) & 1) ? NULL \
                                                      : VECAND((ptr), ~3UL))
#define VEC(vec, index) ((vec)->vector[index])
#define VECAND(ptr, bits) ((void *) ((uintptr_t) (bits) & (uintptr_t) (ptr)))
#define VECOR(ptr, bits) ((void *) ((bits) | (uintptr_t) (ptr)))

/* FPU save areas
 *
 * Low-order bits of fpudata are overloaded in the SMP case.
 */

#if PROCESSORS_MAX > 8
#define FPUDATA_CPUMAX PROCESSORS_MAX
#else
#define FPUDATA_CPUMAX 8
#endif

#define FPUDATA_ALIGN (FPUDATA_CPUMAX << 1)
#define FPUDATA_MASK (~(uintptr_t) (FPUDATA_ALIGN - 1))
#define FPUDATA_INUSE(ptr) ((uintptr_t) ptr & FPUDATA_BUSY)
#define FPUDATA_CPU(ptr) ((uintptr_t) ptr & FPUDATA_CPUMASK)

#if defined(WANT_SMP_MACROS) || CONFIG_PROCESSORS_MAX > 1
#define FPUDATA_BUSY (FPUDATA_ALIGN >> 1)
#define FPUDATA_CPUMASK ((FPUDATA_ALIGN >> 1) - 1)
#define FPUDATA_PTR(ptr) ((void *) ((uintptr_t) ptr & FPUDATA_MASK))
#else
#define FPUDATA_BUSY 0
#define FPUDATA_CPUMASK 0
#define FPUDATA_PTR(ptr) (ptr)
#endif

/* Kernel-call restart / preemption */

#define KCALL_RESTART(act)                                  \
    {                                                       \
        CRASHCHECK(TYPE_MASK(act->type) != TYPE_THREAD);    \
        KSYS_RESTORE_KCALL_C_ARGS(act);                     \
        SETKTYPE((act), _TRACE_GETSYSCALL((act)->syscall)); \
        SETKIP(act, KIP(act) - KSYS_ENTRY_SIZE);            \
    }

/* NEED_PREEMPT — mid-kernel-extension-loop yield-check.
 *
 * Reads the per-CPU "pc_pending_preempt_prio" sentinel (written by
 * "sigevent_deliver_inline" and "intrevent_preemption") and the "need_to_run"
 * flag. Used by long-running kernel extension loops (xfer copy, async-msg
 * flush, timer batch fire, debug iter, misc iter) to KCALL_RESTART when a
 * higher-priority thread has been made runnable.
 */
#define NEED_PREEMPT(act) \
    (((act)->priority < get_pcpu()->pc_pending_preempt_prio) || need_to_run)

/* Preempt a kernel call if necessary.
 *
 * Kernel-call bodies do not disable preemption around themselves. Preemption
 * protection is provided by the kspinlocks (kspinlock_lock disables IRQs via
 * interrupt_lock). KSYS_PREEMPT just records the restart and returns.
 */
#define KSYS_PREEMPT(act, err) \
    if (NEED_PREEMPT(act)) {   \
        KCALL_RESTART(act);    \
        return err;            \
    }

#define KEXT_PREEMPT(act)    \
    if (NEED_PREEMPT(act)) { \
        KCALL_RESTART(act);  \
        return;              \
    }

#define SPECRET_PREEMPT(act)                                     \
    if ((act)->priority < get_pcpu()->pc_pending_preempt_prio) { \
        unspecret_kernel();                                      \
        return;                                                  \
    }

/* Check for an immediate timeout */
#define IMTO(thp, state)                        \
    (((thp)->timeout_flags & (1 << (state))) && \
     ((thp)->timeout_flags & RTMUX_FLG_TIMEOUT_IMMEDIATE))

/* Check for a pending thread cancel */
#define PENDCAN(flags)                                                \
    (((flags) & (PTHREAD_CANCEL_DISABLE | PTHREAD_CANCEL_PENDING)) == \
     PTHREAD_CANCEL_PENDING)

/* Safe TLS flags access: the thread's TLS may be in user address space, which
 * is not accessible from S-mode when the kernel SATP is active. Only
 * dereference TLS if it's in kernel space.
 */
#define TLS_FLAGS_SAFE(thp)                              \
    (((uintptr_t) (thp)->un.lcl.tls >= KERNEL_VIRT_BASE) \
         ? (thp)->un.lcl.tls->__flags                    \
         : 0)

/* Thread-state bit sets */

#define STATE_CANCEL_BITS                                                \
    ((1 << STATE_SEND) | (1 << STATE_RECEIVE) | (1 << STATE_REPLY) |     \
     (1 << STATE_SIGSUSPEND) | (1 << STATE_SIGWAITINFO) |                \
     (1 << STATE_NANOSLEEP) | (1 << STATE_CONDVAR) | (1 << STATE_JOIN) | \
     (1 << STATE_INTR) | (1 << STATE_SEM))

#define STATE_CANCELABLE(thp)                     \
    (((1 << (thp)->state) & STATE_CANCEL_BITS) && \
     KTYPE(thp) != __KSYS_MSG_SENDVNC)

/* States RTMUX_TCTL_INTERRUPT may break with EINTR: the blocked message,
 * condvar, semaphore, sleep and join waits. A mutex wait, a sigsuspend or
 * sigwaitinfo wait (libc-level here), an interrupt wait and a network send are
 * never interrupted by it.
 */
#define STATE_INTERRUPT_BITS                                             \
    ((1 << STATE_SEND) | (1 << STATE_RECEIVE) | (1 << STATE_REPLY) |     \
     (1 << STATE_NANOSLEEP) | (1 << STATE_CONDVAR) | (1 << STATE_JOIN) | \
     (1 << STATE_SEM))

#define STATE_INTERRUPTIBLE(thp)                     \
    (((1 << (thp)->state) & STATE_INTERRUPT_BITS) && \
     KTYPE(thp) != __KSYS_MSG_SENDVNC)

#define STATE_LAZY_RESCHED_BITS \
    ((1 << STATE_REPLY) | (1 << STATE_WAITPAGE) | (1 << STATE_STACK))

#define STATE_LAZY_RESCHED(thp) ((1 << (thp)->state) & STATE_LAZY_RESCHED_BITS)

/* Mutex sync_t owner field */

#define SYNC_OWNER_BITS(pid, tid) \
    (((unsigned) (pid) << 16 | ((unsigned) (tid) + 1)) & ~RTMUX_SYNC_WAITING)

#define SYNC_OWNER(thp) SYNC_OWNER_BITS((thp)->process->pid, (thp)->tid)

#define SYNC_PINDEX(owner) \
    PINDEX(((unsigned) (owner) & ~RTMUX_SYNC_WAITING) >> 16)

#define SYNC_TID(owner) ((int) ((unsigned) (owner) & 0xffffU) - 1)

/* Soul lists. Designated so an added soul_t field cannot silently shift the
 * initializers (a=type, b=size, c=min seed, d=flags, e=align; every other
 * field, including reserved/counter/total, zero-inits).
 */

#define INITSOUL(a, b, c, d, e) \
    = {.type = (a),             \
       .size = (b),             \
       .min = (c),              \
       .highwater = (c),        \
       .flags = (d),            \
       .align = (e)}

/* Access thread register fields */

#define KTYPE(thp) REGTYPE(&(thp)->reg)
#define KSTATUS(thp) REGSTATUS(&(thp)->reg)
#define KIP(thp) REGIP(&(thp)->reg)
#define KSP(thp) REGSP(&(thp)->reg)

#define SETKTYPE(thp, v) SETREGTYPE(&(thp)->reg, v)
#define SETKSTATUS(thp, v) SETREGSTATUS(&(thp)->reg, v)
#define SETKSP(thp, v) SETREGSP(&(thp)->reg, v)
#define SETKIP(thp, v) SETREGIP(&(thp)->reg, v)

#ifndef SETKIP_FUNC
#define SETKIP_FUNC(thp, v) SETKIP(thp, v)
#endif

/* Thread stack manipulation */

#ifdef STACK_GROWS_UP
#define STACK_INIT(bottom, size) (bottom)
#define STACK_ALLOC(res, new_sp, curr_sp, size)                    \
        (res) = (void *)(curr_sp)),                                         \
        ((new_sp) = ((curr_sp) + (size) + (STACK_ALIGNMENT - 1)) &          \
                    ~(STACK_ALIGNMENT - 1)
#else
#define STACK_INIT(bottom, size) ((bottom) + (size))
#define STACK_ALLOC(res, new_sp, curr_sp, size)                \
    ((new_sp) = ((curr_sp) - (size)) & ~(STACK_ALIGNMENT - 1), \
     (res) = (void *) (new_sp))
#endif

/* Misc kernel helpers */

/* Used to force an EOK status on return from a kernel routine */
#define ENOERROR (-1L)

/* Convert a hook vector number to an interrupt level */
#define HOOK_TO_LEVEL(vector) \
    (-(int) (((vector) + 2) & ~RTMUX_INTR_CLASS_SYNTHETIC))

/* inkernel state manipulation
 *
 * INKERNEL_NOW / INKERNEL_LOCK are not set on the active path. INKERNEL_EXIT is
 * used by kernel_exit as a re-entry guard. INKERNEL_SPECRET is superseded by
 * the per-CPU in_specret state — it remains only so entry.c's
 * set_inkernel_init() and a few diagnostic readers (trace/events.h,
 * stall-detector.c) keep compiling.
 */

#ifndef INKERNEL_BITS_SHIFT
#define INKERNEL_BITS_SHIFT 4
#endif

#define INKERNEL_INTRMASK ((1 << INKERNEL_BITS_SHIFT) - 1)
#define INKERNEL_EXIT (0x0001 << INKERNEL_BITS_SHIFT)
#define INKERNEL_SPECRET (0x0002 << INKERNEL_BITS_SHIFT)
#define INKERNEL_LOCK (0x0004 << INKERNEL_BITS_SHIFT)
#define INKERNEL_NOW (0x0008 << INKERNEL_BITS_SHIFT)

#define get_inkernel() inkernel

/* Restore interrupt depth (byte 0) only — the owner/flag bytes are unused, so
 * this is a plain byte store.
 */
#define set_inkernel(val) (*(volatile uint8_t *) &inkernel = (uint8_t) (val))
/* Full 32-bit write — only for initialization before SMP is active. */
#define set_inkernel_init(val) (inkernel = (val))

#ifndef atomic_order

/* atomic_order() makes sure that the compiler doesn't re-order things across
 * the given statement.
 */
#if (defined(__GNUC__) || defined(__ICC))
static __inline__ void __attribute__((__unused__)) atomic_boundary()
{
    asm volatile("" ::: "memory");
}

#define atomic_order(s) (atomic_boundary(), (s), atomic_boundary())
#else
#error atomic_order not defined for compiler
#endif
#endif

/* specret_kernel / unspecret_kernel stamp the per-CPU in_specret flag. It is
 * the cross-subsystem signal — fault handlers (core.c) and specialret-loop
 * guards (special-return.c) read it to decide whether to take the specret-aware
 * branch.
 *
 * "inspecret" (single global int) is read only by the diagnostic block at
 * special-return.c that prints which flag is currently being processed.
 */
#define specret_kernel() (get_pcpu()->pc_in_specret = 1)
#define unspecret_kernel() (get_pcpu()->pc_in_specret = 0, inspecret = 0)

#ifndef CONDITION_CYCLES
#define CONDITION_CYCLES(c) ((unsigned long) c)
#endif

/* Pointer range checks */

/* A range whose end wraps past UINTPTR_MAX would slip through WITHIN_BOUNDARY
 * (the wrapped end lands below the boundary), so reject the wrap explicitly —
 * the same guard WR_PROBE_PTR carries.
 */
#ifdef CPU_RD_SYSADDR_OK
#define RD_VERIFY_PTR(thp, p, size)                                           \
    if ((uintptr_t) (p) + (size_t) (size) < (uintptr_t) (p) ||                \
        (!WITHIN_BOUNDARY((uintptr_t) (p), (uintptr_t) (p) + (size_t) (size), \
                          (thp)->process->boundary_addr) &&                   \
         !CPU_RD_SYSADDR_OK((thp), (p), (size))))                             \
        return EFAULT;
#else
#define RD_VERIFY_PTR(thp, p, size)                                          \
    if ((uintptr_t) (p) + (size_t) (size) < (uintptr_t) (p) ||               \
        !WITHIN_BOUNDARY((uintptr_t) (p), (uintptr_t) (p) + (size_t) (size), \
                         (thp)->process->boundary_addr))                     \
        return EFAULT;
#endif

#define WR_VERIFY_PTR(thp, p, size)                                          \
    if ((uintptr_t) (p) + (size_t) (size) < (uintptr_t) (p) ||               \
        !WITHIN_BOUNDARY((uintptr_t) (p), (uintptr_t) (p) + (size_t) (size), \
                         (thp)->process->boundary_addr))                     \
        return EFAULT;

#define WR_PROBE_PTR(thp, p, size)                                       \
    (((uintptr_t) (p) + (size_t) (size) >= (uintptr_t) (p)) &&           \
     WITHIN_BOUNDARY((uintptr_t) (p), (uintptr_t) (p) + (size_t) (size), \
                     (thp)->process->boundary_addr))

/* Access-probe a user extent, returning EFAULT rather than faulting the kernel.
 *
 * The preceding RD_VERIFY_PTR / WR_VERIFY_PTR only bounds the address from
 * above, so NULL and in-range unmapped addresses reach the probe. Recovery
 * lives in xfer_probe; see the banner there. Statement macros with an embedded
 * return, matching RD_VERIFY_PTR, so every call site must sit in a kcall
 * returning an errno. thp is ignored — recovery is per-hart state, not
 * per-thread — but is still taken so call sites keep pairing with RD_VERIFY_PTR
 * unchanged.
 */
#define RD_PROBE_INT(thp, p, num)                                           \
    do {                                                                    \
        (void) (thp);                                                       \
        if (xfer_probe((void *) (uintptr_t) (p), (unsigned) (num), 0) != 0) \
            return EFAULT;                                                  \
    } while (0)

#define WR_PROBE_INT(thp, p, num)                                           \
    do {                                                                    \
        (void) (thp);                                                       \
        if (xfer_probe((void *) (uintptr_t) (p), (unsigned) (num), 1) != 0) \
            return EFAULT;                                                  \
    } while (0)

/* WR_PROBE_OPT is deliberately absent. It expanded to nothing, and nothing on
 * any arch ever defined it, so its nine call sites read as "range-checked and
 * probed" while being range-checked only: NULL and every in-range unmapped
 * address reached a raw store with no fault handler armed. That is the same
 * unprivileged panic the probe macros were made fault-safe to close, hidden
 * behind a name that looked like a check. All nine now use WR_PROBE_INT (or
 * xfer_probe where the site cannot return). Do not reintroduce it: an optional
 * probe is not a probe.
 */

/* Time / force / configuration constants */

#define RTMUX_BILLION 1000000000UL
#define RTMUX_MILLION 1000000UL

/* 0x80000000 — sign bit of a 32-bit unsigned int. */
#define _TOPBIT32 (~0u ^ (~0u >> 1))
#define _FORCE_SET_ERROR _TOPBIT32
#define _FORCE_KILL_SELF (_TOPBIT32 >> 1)
#define _FORCE_NO_UNBLOCK (_TOPBIT32 >> 2)

/* If another _FORCE_* flag is added, be sure to add it to _FORCE_BITS below and
 * make sure that dispatch.c is recompiled so that we properly mask all these
 * bits off before passing the errno to kcall_err().
 */
#define _FORCE_BITS (_FORCE_SET_ERROR | _FORCE_KILL_SELF | _FORCE_NO_UNBLOCK)

/* force_ready_msgxfer stores -(errno | _FORCE_*). Decode as unsigned
 * arithmetic: passing the saved negative status straight back to force_ready
 * invents control flags (including KILL_SELF) and corrupts the errno. */
#define MSG_FORCE_ERROR(thp) (0U - (unsigned) KSTATUS(thp))

/* Smallest ticksize allowed (10 us for now) */
#define RTMUX_TICKSIZE_MIN 10000
/* Largest ticksize allowed */
#define RTMUX_TICKSIZE_MAX (INT_MAX / RTMUX_RR_INTERVAL_MUL)

#ifdef CONFIG_TIMER_HZ
#define RTMUX_TICKSIZE_DEFAULT (1000000000UL / CONFIG_TIMER_HZ)
#else
#define RTMUX_TICKSIZE_DEFAULT 1000000 /* 1 ms (1000 Hz) */
#endif

/* Default multiplier of ticksize to round-robin interval */
#define RTMUX_RR_INTERVAL_MUL_DEFAULT 4
/* Actual multiplier of ticksize to round-robin interval */
#define RTMUX_RR_INTERVAL_MUL rr_interval_mul

/* Decode the negative-parts encoding: a message vector with a negative part
 * count is a single buffer whose length is the magnitude of that count.
 *
 * Widening before the negation is the whole point. The counts arrive as int32
 * syscall arguments and reach xferlen() before any XFER_IOV_LIMIT check, so
 * -parts is evaluated on a value the caller chose; at INT32_MIN that negation
 * is undefined, and -O2 without -fwrapv lets the compiler assume it cannot
 * happen and drop range checks downstream. Negating in int64 is defined for
 * every input.
 *
 * One caveat this does not fix: xferlen() returns int, so a single buffer of
 * exactly 2 GiB still cannot be represented in its return value. That is a
 * pre-existing limit of the interface, not of this macro.
 */
#define PARTS_TO_LEN(_parts) ((size_t) (-(int64_t) (_parts)))

/* Thread name pool object size (not max thread-name length) */
#define THREAD_NAME_FIXED_SIZE 24

#define VECTOR_MAX (0xffff - 1)

/* Max number of attachers to one particular interrupt level */
#define INTR_LEVEL_ATTACH_MAX 32

/* Maximum count for compressing notifications */
#define _NOTIFICATION_COUNT_MAX \
    ((2U << (sizeof(((struct notification_entry *) 0)->count) * 8 - 1)) - 1U)

/* Bits used by xfermsg */

#define XFER_SRC_FAULT 0x01
#define XFER_DST_FAULT 0x02

#define XFER_SRC_CHECK 0x01
#define XFER_DST_CHECK 0x02

/* Thread internal_flags */

#define RTMUX_ITF_MSG_DELIVERY 0x01U
#define RTMUX_ITF_MSG_FORCE_RDY 0x02U
#define RTMUX_ITF_NET_UNBLOCK 0x04U /* for vthread only */
#define RTMUX_ITF_SSTEP_SUSPEND 0x08U

/* SPECRET_PENDING: some other thread needs this thread's data for specialret().
 * It is set for a sending thread when a message or notification is sent that
 * causes RCVINFO or SHORT_MSG to be set in the receiver's flags. It is not used
 * when SHORT_MSG is set in a reply, since the thread that receives the reply
 * does not depend on the thread sending the reply.
 */
#define RTMUX_ITF_SPECRET_PENDING 0x10U

/* WAIT_PENDING: this thread is mid-transition into a sync wait state (sem_wait,
 * condvar_wait, mutex_lock-block). Set after pril_add to the wait list, cleared
 * after unready(). Wakers (sync_wakeup, force_ready) that observe a candidate
 * with this flag set must spin-wait until clear before pril_rem-and-ready,
 * otherwise they race the unready and either CRASHCHECK on "thp == active" in
 * ready_default or drop the wakeup. Modelled on RTMUX_ITF_MSG_DELIVERY (the
 * SMP_XFER guard in fastmsg.c).
 */
#define RTMUX_ITF_WAIT_PENDING 0x20U

#define RTMUX_ITF_UNBLOCK_QUEUED 0x40U
#define RTMUX_ITF_RCVNOTIFICATION 0x80U

/* NATIVE_REPLY: the thread has opted in (thread_ctl RTMUX_TCTL_NATIVE_REPLY) to
 * a native capability header in the replies to its requests. ksys_msg_replyv
 * parses a reply as a struct _io_native_hdr, and installs the capabilities it
 * names, only while this bit is set on the client; otherwise the reply bytes
 * are opaque data. The client sets and clears the bit on itself, around the
 * request, so the replier never decides how its own payload is interpreted.
 */
#define RTMUX_ITF_NATIVE_REPLY 0x100U

/* Resume a normal joinable exit after roottask releases its dynamic stack. */
#define RTMUX_ITF_EXIT_STACK 0x200U

/* internal_flags is a single 32-bit word whose bits are written from MULTIPLE
 * CPUs concurrently. A thread sets/clears its OWN WAIT_PENDING /
 * RCVNOTIFICATION / SPECRET_PENDING bits (e.g. ksys_msg_receivev clearing
 * WAIT_PENDING after unready, outside chp->lock), while a sender or waker on
 * ANOTHER CPU sets/clears that same thread's MSG_DELIVERY / MSG_FORCE_RDY bits
 * (e.g. a sender claiming a receiver under chp->lock). A plain "|=" / "&= ~"
 * read-modify-write loses the other CPU's update: the classic failure is a
 * sender's MSG_DELIVERY set (read 0x20, write 0x21) racing the receiver's
 * WAIT_PENDING clear (read 0x20, write 0x00) — the sender's store lands last
 * and RESURRECTS WAIT_PENDING, wedging the sender's WAIT_PENDING spin in
 * ksys_msg_sendv forever. All updates therefore go through these atomic helpers
 * (amoor.w / amoand.w on RISC-V). Reads ("& bit" tests) need no helper — an
 * aligned 32-bit load is already atomic.
 */
#define ITF_SET(thp, bits) \
    atomic_or((volatile unsigned *) &(thp)->internal_flags, (unsigned) (bits))
#define ITF_CLR(thp, bits) \
    atomic_clr((volatile unsigned *) &(thp)->internal_flags, (unsigned) (bits))

/* Async thread flags (set by the kernel) */

#define RTMUX_ATF_TIMESLICE 0x00000001U
#define RTMUX_ATF_FPUSAVE_ALLOC 0x00000002U
#define RTMUX_ATF_SMP_RESCHED 0x00000004U
#define RTMUX_ATF_SMP_EXCEPTION 0x00000008
#define RTMUX_ATF_WAIT_FOR_KER 0x00000010
#define RTMUX_ATF_WATCHPOINT 0x00000020

/* Cross-hart thread cancel: the owning hart turns it into
 * RTMUX_FLG_THR_CANCELSELF in kernel_exit, so only the owner ever rewrites the
 * thread's registers.
 */
#define RTMUX_ATF_CANCEL 0x00000040
#define RTMUX_ATF_REGCTX_ALLOC 0x40000000
#define RTMUX_ATF_FORCED_KERNEL 0x80000000

/* xfer handler accessors */

#define SET_XFER_HANDLER(handler) get_pcpu()->pc_xfer_handlers = handler
#define GET_XFER_HANDLER() (get_pcpu()->pc_xfer_handlers)

/* The xfer fault-recovery longjmp target. Like the handler table above, this
 * MUST be per-CPU: a fault during xfer_memcpy on hart X has to longjmp back
 * into hart X's own xfer frame. A single global jmp_buf pointer would be
 * clobbered by a concurrent xfer on another hart, and the faulting hart would
 * longjmp into a foreign/stale stack frame -> XFER kernel crash.
 *
 * Kept as a hart-indexed array (xfer_env[], in core.c) rather than a pcpu
 * field: struct pcpu is held at a power-of-2 size for the AP boot asm and has
 * no spare slot. Same pattern as the per-CPU trace rings.
 */
#define SET_XFER_ENV(env) (xfer_env[get_cpunum()] = (env))
#define GET_XFER_ENV() (xfer_env[get_cpunum()])

/* sync_mutex_lock flags */

#define _MUTEXLOCK_FLAGS_NOCEILING 0x1

/* Notification / signal priority flags */

#define _NOTIFICATION_PRIO_DPP_SHIFT 8
#define _NOTIFICATION_PRIO_DPP_MASK 0x00000f00U
#define _NOTIFICATION_PRIO_DPP_VALID 0x00001000U
#define _NOTIFICATION_PRIO_CRITICAL 0x00002000U
#define _NOTIFICATION_PRIO_BOOST 0x00004000U
#define _NOTIFICATION_PRIO_REPLACE 0x10000000U
#define _NOTIFICATION_PRIO_SIGNAL 0x20000000U
#define _NOTIFICATION_PRIO_VTID 0x40000000U
#define _NOTIFICATION_PRIO_HEAD 0x80000000U

#define _NOTIFICATION_PRIO_PUBLIC_MASK 0xffffU

/* Round-robin scheduling */

#define RR_MAXTICKS (RTMUX_RR_INTERVAL_MUL * 2)
#define RR_FULLTICK (2)
#define RR_PREEMPT_TICK (1)

#define IS_SCHED_RR(thp) \
    (((thp)->policy == SCHED_RR) || ((thp)->policy == SCHED_OTHER))

#define RR_ADD_FULLTICK(thp)                                           \
    if (IS_SCHED_RR((thp))) {                                          \
        (thp)->schedinfo.rr_ticks =                                    \
            ((thp)->schedinfo.rr_ticks <= (RR_MAXTICKS - RR_FULLTICK)) \
                ? (thp)->schedinfo.rr_ticks + RR_FULLTICK              \
                : RR_MAXTICKS;                                         \
    }

#define RR_ADD_PREEMPT_TICK(thp)                                           \
    if (IS_SCHED_RR((thp))) {                                              \
        (thp)->schedinfo.rr_ticks =                                        \
            ((thp)->schedinfo.rr_ticks <= (RR_MAXTICKS - RR_PREEMPT_TICK)) \
                ? (thp)->schedinfo.rr_ticks + RR_PREEMPT_TICK              \
                : RR_MAXTICKS;                                             \
    }

#define RR_RESET_TICK(thp)             \
    if (IS_SCHED_RR((thp))) {          \
        (thp)->schedinfo.rr_ticks = 0; \
    }

#define RR_GET_TICKS(thp) ((thp)->schedinfo.rr_ticks)

/* Sporadic scheduling */

#define IS_SCHED_SS(thp) ((thp)->policy == SCHED_SPORADIC)

/* Note the running time for this thread. This may cause a call to adjust
 * priority, dropping the priority of the thread, so only call it when the
 * actives are stable and thp is on the appropriate priority list. This call
 * will only adjust the priority of the specified thread downward.
 */
#define SS_STOP_RUNNING(thp, preempted)     \
    if (IS_SCHED_SS((thp))) {               \
        sched_ss_block((thp), (preempted)); \
    }

/* The same, for a caller that already holds the dispatch lock and has just
 * blocked thp. adjust_priority takes that lock itself and must not be entered
 * with it held, so a budget-exhausted drop is applied directly: thp is off
 * every ready queue, and only its priority field needs to change.
 */
#define SS_STOP_RUNNING_LOCKED(thp, preempted)          \
    do {                                                \
        if (IS_SCHED_SS((thp))) {                       \
            (thp)->schedinfo.ss_info->sched_locked = 1; \
            sched_ss_block((thp), (preempted));         \
            (thp)->schedinfo.ss_info->sched_locked = 0; \
        }                                               \
    } while (0)

/* Note the time that this thread was first made ready/running. This is used for
 * scheduling the replenishment operations later.
 */
#define SS_MARK_ACTIVATION(thp) \
    if (IS_SCHED_SS((thp))) {   \
        sched_ss_queue((thp));  \
    }

/* Since the scheduler can be entered from many points, this macro allows us to
 * check whether the SS thread specified has expired. This call could drop the
 * priority of the thread; any actual code should only execute when the caller
 * is coming through from the interrupt handler, since that is the only place
 * where curr_budget is zero'd at high priority.
 */
#define SS_CHECK_EXPIRY(thp)                                                \
    if (IS_SCHED_SS((thp)) && (thp)->schedinfo.ss_info->curr_budget == 0 && \
        (thp)->schedinfo.ss_info->org_priority == 0) {                      \
        sched_ss_update((thp), (thp)->schedinfo.ss_info,                    \
                        &((thp)->schedinfo.ss_info->replenishment), 1);     \
    }

/* External-scheduler identification Assigned to core/globals.h: scheduler_type
 */

#define SCHEDULER_TYPE_DEFAULT 0

/* Critical-event propagation
 *
 * These flags are read/written across fastmsg, message, event, notification,
 * interrupt, thread and ext/page to carry the "this event originated in an IO
 * interrupt" marker used by the SIGEV_FLAG_CRITICAL mechanism.
 */

/* Values for thread_entry.sched_flags */

/* Is our thread critical now? */
#define CRITICAL_RUN_ACTIVE 0x00000001
/* Thread always runs critical: CRITICAL_RUN_ACTIVE is never cleared. */
#define CRITICAL_RUN_PERMANENT 0x00000002
/* Not all critical time is billed as critical.  Set = bill to crit budget. */
#define CRITICAL_RUN_BILLED 0x00000004

#define CRITICAL_CLEAR(thp)                                              \
    ((thp)->sched_flags &= ~(uint32_t) CRITICAL_RUN_BILLED,              \
     (thp)->sched_flags &= ((thp)->sched_flags & CRITICAL_RUN_PERMANENT) \
                               ? ~0u                                     \
                               : ~(uint32_t) CRITICAL_RUN_ACTIVE)

#define CRITICAL_INHERIT(to_thp, from_thp)                          \
    ((to_thp)->sched_flags =                                        \
         (~(uint32_t) (CRITICAL_RUN_ACTIVE | CRITICAL_RUN_BILLED) & \
          (to_thp)->sched_flags) |                                  \
         ((from_thp)->sched_flags &                                 \
          (CRITICAL_RUN_ACTIVE | CRITICAL_RUN_BILLED)))

#define CRITICAL_MARK_THREAD(thp) ((thp)->sched_flags |= CRITICAL_RUN_ACTIVE)

/* Allow intrevent_add() to determine whether it's being called from an IO
 * interrupt. (If intrevent_add is called from IO, it will set the
 * SIGEV_FLAG_CRITICAL bit to make the final receiving thread run critical.) Use
 * INTREVENT_FROM_IO(thp) to pass the thp parameter. Example:
 *     intrevent_add(evp, INTREVENT_FROM_IO(thp), 1);
 */
#define INTREVENT_FROM_IO_FLAG 1
#define INTREVENT_FROM_IO(thp) \
    ((thread_t *) ((uintptr_t) (thp) | INTREVENT_FROM_IO_FLAG))

/* Notification-delivery / thread-create flags */

/* Flag options for notification.c::notification_deliver() */
#define NOTIFICATION_DELIVER_CRITICAL_FLAG 0x00000001

/* Deliver from the allocation-free realtime reserve (IRQ/timer sources that
 * reserved a notification soul at attach/create). Selects
 * notification_add_reserved with full-queue coalescing.
 */
#define NOTIFICATION_DELIVER_RESERVED_FLAG 0x00000002

/* Flag options for lifecycle.c::thread_create() */
#define THREAD_CREATE_BLOCK_FLAG 0x00000001
#define THREAD_CREATE_CRITICAL_FLAG 0x00000002

/* Inline time snapshot */

/* Read the monotonic base from arch_timer_now_ns (free-running cycle counter),
 * not the published qtimeptr->nsec, so the value stays current across a
 * tickless idle gap where the boot CPU has not republished nsec. The monotonic
 * part is inherently consistent, so the old spin against the stable copy is
 * gone; a REALTIME read adds nsec_tod_adjust and retries if a concurrent
 * clock_settime moves it, matching snap_time() so both see a consistent
 * (monotonic, adjust) pair. See snap_time() for the pairing with the timer
 * wheel.
 */
#define SNAP_TIME_INLINE(tsp, incl_tod)                            \
    {                                                              \
        uint64_t _adj;                                             \
        do {                                                       \
            _adj = qtimeptr->nsec_tod_adjust;                      \
            (tsp) = arch_timer_now_ns(qtimeptr);                   \
            if ((tsp) == (uint64_t) - 1) {                         \
                snap_time_wakeup();                                \
            }                                                      \
        } while ((incl_tod) && _adj != qtimeptr->nsec_tod_adjust); \
        if ((incl_tod)) {                                          \
            (tsp) += _adj;                                         \
        }                                                          \
    }

/* CPU-specific overrides for intr_slock manipulation */

#ifndef CPU_SLOCK_INTR_LOCK
#define CPU_SLOCK_INTR_LOCK(l) INTR_LOCK(l)
#endif
#ifndef CPU_SLOCK_INTR_UNLOCK
#define CPU_SLOCK_INTR_UNLOCK(l) INTR_UNLOCK(l)
#endif
#ifndef CPU_SLOCK_LOCK
#define CPU_SLOCK_LOCK(l) SPINLOCK(l)
#endif
#ifndef CPU_SLOCK_UNLOCK
#define CPU_SLOCK_UNLOCK(l) SPINUNLOCK(l)
#endif

/* Pril list manipulation (unordered)
 *
 * TODO: figure out a better way of handling these later.
 */

#define LINKPRIL_REM(_object) LINK2_REM((LINK3_NODE *) (_object))

#define LINKPRIL_BEG(_queue, _object, _type) \
    LINK2_BEG(*(LINK3_NODE **) &(_queue), (LINK3_NODE *) (_object), _type)
