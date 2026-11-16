/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* Debug-only fault injection; built only with CONFIG_DEBUG_INJECT. */

#include <core/globals.h>
#include <private/inject.h>
#include <private/kstats.h>

/* The armed one-shot allocation failure. Two words, both written only here and
 * consumed with an atomic exchange of `armed` in inject_should_fail_alloc, so a
 * failure is delivered to exactly one caller.
 */
static atomic_uint armed_soul = KSTAT_SOUL_COUNT; /* KSTAT_SOUL_COUNT: none */
static atomic_int armed_pid;
static atomic_int consumed;

/* The armed SchedSet failures: the pid they are armed for, how many are left,
 * and how many were delivered. A failure goes to exactly one caller by a
 * compare-and-swap of `sched_left`.
 */
static atomic_int sched_armed_pid = -1;
static atomic_int sched_left;
static atomic_int sched_consumed;
static atomic_int sched_displaced;
static _Atomic(thread_t *) sched_failed_thr; /* the caller just failed */

static inline bool inject_op_is_status(unsigned op)
{
    return op == INJECT_OP_ALLOC_STATUS || op == INJECT_OP_SCHED_SET_STATUS ||
           op == INJECT_OP_SCHED_SET_DISPLACED;
}

atomic_ullong inject_spin_state;

/* Called from ksys_sched_set once the change is applied; `lowered` says the
 * caller's priority went down. Only a lowering call fails, so the caller can
 * raise itself again with the failure still armed.
 */
bool inject_should_fail_sched_set(thread_t *act, bool lowered)
{
    int left;

    if (!lowered || !act || !act->process ||
        act->process->pid !=
            atomic_load_explicit(&sched_armed_pid, memory_order_acquire))
        return false;
    left = atomic_load_explicit(&sched_left, memory_order_relaxed);
    do {
        if (left <= 0)
            return false;
    } while (!atomic_compare_exchange_weak_explicit(
        &sched_left, &left, left - 1, memory_order_acq_rel,
        memory_order_relaxed));
    atomic_store_explicit(&sched_failed_thr, act, memory_order_release);
    atomic_fetch_add_explicit(&sched_consumed, 1, memory_order_release);
    return true;
}

/* Called from the trap handler on every error return. If that error is the
 * SchedSet failure just injected for act, consume the mark, and count it when
 * the caller was already switched out, so a test can tell a status that
 * survived a switch from one that never met one. Consuming the mark on an
 * undisplaced return keeps it from matching a later, unrelated error.
 */
void inject_note_sched_return(thread_t *act, bool displaced)
{
    thread_t *expect = act;

    if (atomic_compare_exchange_strong_explicit(&sched_failed_thr, &expect,
                                                NULL, memory_order_acq_rel,
                                                memory_order_relaxed) &&
        displaced)
        atomic_fetch_add_explicit(&sched_displaced, 1, memory_order_release);
}

/* The pid whose next turnstile allocation fails, or -1: consumed like
 * armed_soul so exactly one allocation takes the failure.
 */
static atomic_int armed_ts_pid = -1;

int inject_should_fail_turnstile(process_t *prp)
{
    int pid = atomic_load_explicit(&armed_ts_pid, memory_order_acquire);

    if (pid < 0 || !prp || prp->pid != pid ||
        !atomic_compare_exchange_strong_explicit(&armed_ts_pid, &pid, -1,
                                                 memory_order_acq_rel,
                                                 memory_order_relaxed))
        return 0;
    atomic_store_explicit(&consumed, 1, memory_order_release);
    return 1;
}

/* Called from object_alloc before it touches the free list. Ordinary allocation
 * only: the reserved path (object_alloc_reserved) must never fail this way.
 */
int inject_should_fail_alloc(process_t *prp, soul_t *soulp)
{
    static soul_t *const pools[KSTAT_SOUL_COUNT] = {
#define KSTAT_SOUL_PTR(name, var) [KSTAT_SOUL_##name] = &var,
        KSTAT_SOUL_LIST(KSTAT_SOUL_PTR)
#undef KSTAT_SOUL_PTR
    };
    unsigned id = atomic_load_explicit(&armed_soul, memory_order_acquire);

    if (id >= KSTAT_SOUL_COUNT || pools[id] != soulp || !prp ||
        prp->pid != atomic_load_explicit(&armed_pid, memory_order_relaxed))
        return 0;
    /* Only the caller that swaps the arm away delivers the failure. */
    if (atomic_exchange_explicit(&armed_soul, KSTAT_SOUL_COUNT,
                                 memory_order_acq_rel) != id)
        return 0;
    atomic_store_explicit(&consumed, 1, memory_order_release);
    return 1;
}

/* kext_debug_inject — the DEBUG_INJECT private call. */
void kext_debug_inject(void *data)
{
    struct kcall_args_debug_inject *kap = data;
    thread_t *act = get_active_thread(get_cpunum());
    unsigned self = get_cpunum();
    int ret = EOK;

    switch (kap->op) {
    case INJECT_OP_SPIN_IRQOFF:
    case INJECT_OP_SPIN_CONSOLE:
    case INJECT_OP_HANG_HART: {
        unsigned hart;
        unsigned ms;
        unsigned live = 0;

        /* Range-check the full 64-bit arguments before narrowing them. */
        if (kap->arg0 >= CONFIG_PROCESSORS_MAX ||
            (kap->op != INJECT_OP_HANG_HART &&
             (kap->arg1 == 0 || kap->arg1 > INJECT_SPIN_MAX_MS))) {
            ret = EINVAL;
            break;
        }
        hart = (unsigned) kap->arg0;
        ms = kap->op == INJECT_OP_HANG_HART ? UINT32_MAX : (unsigned) kap->arg1;

        for (unsigned c = 0; c < num_processors; c++)
            live += cpu_online(c) ? 1u : 0u;

        /* Refuse the caller's own hart, a hart that is not alive, and the last
         * live hart: any of them would turn a test into an unrecoverable stop
         * rather than a stall to be diagnosed.
         */
        if (hart == self || !cpu_online(hart) || live < 2) {
            ret = EINVAL;
            break;
        }
        pcpu_array[hart].pc_inject_spin_ms = ms;
        pcpu_array[hart].pc_inject_hold_console =
            kap->op == INJECT_OP_SPIN_CONSOLE;
        SENDIPI(hart, IPI_DEBUG_SPIN);
        break;
    }
    case INJECT_OP_FAIL_NEXT_ALLOC:
        if (kap->arg0 >= KSTAT_SOUL_COUNT) {
            ret = EINVAL;
            break;
        }
        atomic_store_explicit(&consumed, 0, memory_order_relaxed);
        atomic_store_explicit(&armed_pid, (int) kap->arg1,
                              memory_order_relaxed);
        atomic_store_explicit(&armed_soul, (unsigned) kap->arg0,
                              memory_order_release);
        break;
    case INJECT_OP_FAIL_NEXT_TURNSTILE:
        atomic_store_explicit(&consumed, 0, memory_order_relaxed);
        atomic_store_explicit(&armed_ts_pid, (int) kap->arg0,
                              memory_order_release);
        break;
    case INJECT_OP_ALLOC_STATUS:
        ret = atomic_load_explicit(&consumed, memory_order_acquire);
        break;
    case INJECT_OP_FAIL_NEXT_SCHED_SET:
        if (kap->arg0 > INT32_MAX || kap->arg1 > INT32_MAX) {
            ret = EINVAL;
            break;
        }
        atomic_store_explicit(&sched_consumed, 0, memory_order_relaxed);
        atomic_store_explicit(&sched_displaced, 0, memory_order_relaxed);
        atomic_store_explicit(&sched_left, (int) kap->arg1,
                              memory_order_relaxed);
        atomic_store_explicit(&sched_armed_pid, (int) kap->arg0,
                              memory_order_release);
        break;
    case INJECT_OP_SCHED_SET_STATUS:
        ret = atomic_load_explicit(&sched_consumed, memory_order_acquire);
        break;
    case INJECT_OP_SCHED_SET_DISPLACED:
        ret = atomic_load_explicit(&sched_displaced, memory_order_acquire);
        break;
    default:
        ret = EINVAL;
        break;
    }
    kap->ret = ret;

    /* The *_STATUS and *_DISPLACED ops answer through kap->ret; a nonzero
     * status would read as a refused call to syscall_priv callers.
     */
    SETKSTATUS(act, inject_op_is_status(kap->op) ? EOK : ret);
}
