/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * ksys_priv — kcall slot 2 (__KSYS_PRIV) dispatch.
 *
 * Replaces the historical "ring 0" mechanism, where kcall_args.func was a
 * kernel-VA function pointer supplied by the caller. The new shape:
 *
 *   - libc weak stub  syscall_priv(int id, void *arg)  issues
 *     ECALL with a7 = __KSYS_PRIV, a0 = id, a1 = arg.
 *   - Kernel hits ksys_call_table[__KSYS_PRIV] = ksys_priv (this
 *     function), which range-checks "id" against __KSYS_PRIV_NR, gates on the
 *     per-op process_t.priv_caps bit, and calls the handler at
 *     priv_table[id].
 *
 * Function pointers no longer cross the kernel/roottask boundary. Sub-IDs are
 * fixed enum constants in <private/priv-calls.h>. The dispatch table is the
 * whitelist; out-of-range IDs and non-privileged callers get ENOSYS / EPERM
 * with no execution.
 */

#include <core/globals.h>

/* Forward declarations of every privileged handler in the sub-namespace.
 *
 * Each handler has the historical kext_*-style signature "void f(void *data)".
 * It uses SETKSTATUS(act, ...) on the active thread to communicate its return
 * value; the dispatcher itself always returns ENOERROR so the kcall framework
 * leaves whatever the handler set in act->reg.gpr[a0] alone.
 */
extern void do_shutdown(void *);

extern void ext_vaddrinfo(void *);
extern void sysaddr_map(void *);
extern void ksys_manipulate(void *);
extern void ksys_synchash_rem(void *);
extern void reboot(void *);

extern void kext_clock_start(void *);
extern void kext_connect_detach(void *);
extern void kext_cred_dirty(void *);
extern void kext_cred_get(void *);
extern void kext_cred_set(void *);

extern void kext_debug_attach(void *);
extern void kext_debug_detach(void *);
extern void kext_debug_install(void *);
extern void kext_debug_process(void *);

extern void kext_exit_destroy_threads(void *);

extern void kext_pa_alloc_full(void *);
extern void kext_pa_alloc_given(void *);
extern void kext_pa_pq_op(void *);
extern void kext_pa_free(void *);
extern void kext_pa_free_info(void *);
extern void kext_pa_free_paddr(void *);
extern void kext_pa_free_list(void *);
extern void kext_pa_init(void *);
extern void kext_pa_start_background(void *);
extern void kext_pa_sysram_overlap(void *);
extern void kext_pa_get_sizes(void *);
extern void kext_pa_quantum_to_paddr(void *);
extern void kext_pa_paddr_to_quantum(void *);

extern void kext_con_getc(void *);

extern void kext_register_mm_helpers_kex(void *);
extern void kext_legal_cpu_mask(void *);
extern void kext_get_kern_pgdir(void *);
extern void kext_heartbeat(void *);
extern void kext_stop_threads(void *);

extern void kext_process_bind(void *);
extern void kext_process_create(void *);
extern void kext_process_destroy(void *);
extern void kext_process_destroyall(void *);
extern void kext_process_exec(void *);
extern void kext_process_shutdown(void *);
extern void kext_process_startup(void *);

extern void kext_query_object(void *);

extern void kext_register_proc_hooks(void *);

extern void kext_reparent(void *);
extern void kext_self_prp(void *);
extern void kext_self_adp(void *);
extern void kext_signal_deliver(void *);
extern void kext_smp_bringup(void *);

extern void kext_stack_cont(void *);
extern void kext_stack_purge(void *);
extern void kext_stack_wait_info(void *);

extern void kext_thread_ctl(void *);
extern void kext_thread_tls(void *);

extern void kext_timer_destroy(void *);
extern void kext_trace(void *);

extern void kext_as_memcpy(void *);
extern void kext_kpage_zero(void *);
extern void kext_icache_sync_all(void *);
extern void kext_register_pool_stack(void *);
extern void kext_query_object_done(void *);
extern void kext_proc_query(void *);
extern void kext_channel_has_receivers(void *);
extern void kext_root_mmap_phys_user(void *);
extern void kext_root_mmap_anon_user(void *);
extern void kext_proc_set_spawn(void *);
extern void kext_proc_register_syschan(void *);
extern void kext_proc_list_children(void *);
extern void kext_proc_wait_finalize(void *);
extern void kext_proc_nozombie_mark(void *);
extern void kext_proc_exit_mark(void *);
extern void kext_proc_destroy_chancons(void *);
extern void kext_proc_destroy_fdcons(void *);
extern void kext_proc_clear_pls(void *);
extern void kext_proc_exit_finalize(void *);
extern void kext_proc_flags_op(void *);
extern void kext_proc_set_rlimit(void *);
extern void kext_proc_clear_lcp(void *);
extern void kext_proc_index_to_pid(void *);
extern void kext_proc_set_ocb_list(void *);
extern void kext_proc_cap_stats(void *);
extern void kext_proc_endpoint_cap(void *);
extern void kext_proc_cap_mint(void *);
extern void kext_soul_stats(void *);
#ifdef CONFIG_DEBUG_INJECT
extern void kext_debug_inject(void *);
#endif

static void (*const priv_table[__KSYS_PRIV_NR])(void *) = {
#define PRIV_TABLE_ROW(id, cap, handler) [__KSYS_PRIV_##id] = handler,
    RTMUX_PRIV_CALL_LIST(PRIV_TABLE_ROW)
#undef PRIV_TABLE_ROW
};

static const uint64_t priv_caps[__KSYS_PRIV_NR] = {
#define PRIV_CAP_ROW(id, cap, handler) \
    [__KSYS_PRIV_##id] = RTMUX_PRIV_CAP_##cap,
    RTMUX_PRIV_CALL_LIST(PRIV_CAP_ROW)
#undef PRIV_CAP_ROW
};

/* Catch enum/table drift at compile time. */
_Static_assert(sizeof(priv_table) / sizeof(priv_table[0]) == __KSYS_PRIV_NR,
               "priv_table size disagrees with __KSYS_PRIV_NR");
_Static_assert(sizeof(priv_caps) / sizeof(priv_caps[0]) == __KSYS_PRIV_NR,
               "priv_caps size disagrees with __KSYS_PRIV_NR");

int priv_check(process_t *prp, unsigned id)
{
    /* A caller holding no privilege at all (priv_caps == 0) is denied uniformly
     * with EPERM before the table is consulted, so it can't tell implemented
     * sub-IDs (EPERM) from unbound ones (ENOSYS). Only roottask, which actually
     * holds caps, sees the finer-grained ENOSYS.
     */
    if (!prp || prp->priv_caps == 0)
        return EPERM;
    if (id >= __KSYS_PRIV_NR || !priv_table[id])
        return ENOSYS;
    if ((prp->priv_caps & priv_caps[id]) == 0)
        return EPERM;
    return EOK;
}

int priv_dispatch(process_t *prp, unsigned id, void *arg)
{
    int err;

    err = priv_check(prp, id);
    if (err != EOK)
        return err;
    priv_table[id](arg);
    return EOK;
}

/* Privileged roottask kcall dispatcher — slot 2 of ksys_call_table.
 *
 * Range-checks the sub-ID, gates on the per-op capability class, and invokes
 * the handler at priv_table[id]. The handler communicates its return value via
 * SETKSTATUS on the active thread. Parameters:
 *   act : the calling (active) thread
 *   kap : kcall args; kap->id selects the handler, kap->arg is its data
 * Return value: ENOERROR on dispatch; EPERM if not privileged; ENOSYS
 *               for an out-of-range or unbound sub-ID.
 */
int kdecl ksys_priv(thread_t *act, struct kcall_args_priv *kap)
{
    int err;

    err = priv_dispatch(act->process, (unsigned) kap->id, kap->arg);
    if (err != EOK)
        return err;

    return ENOERROR;
}

/* ksys_nop — slot 0. Used for async_flags processing in the SMP kernel.
 * Parameters:
 *   act : the calling (active) thread (unused)
 *   kap : unused
 * Return value: ENOERROR.
 */
int kdecl ksys_nop(thread_t *act, struct kcall_args_null *kap)
{
    (void) act;
    (void) kap;
    return ENOERROR;
}
