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

atomic_ullong inject_spin_state;

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

        for (unsigned c = 0; c < CONFIG_PROCESSORS_MAX; c++)
            live += pcpu_array[c].pc_alive ? 1u : 0u;

        /* Refuse the caller's own hart, a hart that is not alive, and the last
         * live hart: any of them would turn a test into an unrecoverable stop
         * rather than a stall to be diagnosed.
         */
        if (hart == self || !pcpu_array[hart].pc_alive || live < 2) {
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
    case INJECT_OP_ALLOC_STATUS:
        ret = atomic_load_explicit(&consumed, memory_order_acquire);
        break;
    default:
        ret = EINVAL;
        break;
    }
    kap->ret = ret;

    /* ALLOC_STATUS answers through kap->ret; a nonzero status would read as a
     * refused call to syscall_priv callers.
     */
    SETKSTATUS(act, kap->op == INJECT_OP_ALLOC_STATUS ? EOK : ret);
}
