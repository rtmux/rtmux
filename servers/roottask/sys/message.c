/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "externs.h"
#include <startup.h>
#include <runtime.h>
#include <sched.h>
#include <sys/mman.h> /* mmap, MAP_*, PROT_*, NOFD */
#include <errno.h>

static pthread_attr_t thread_attr;

static ioserv_attr_t ioserv_attr = {
    .nparts_max = 10,
    .msg_max_size = sizeof(union proc_msg_union),
};

#ifdef CONFIG_TRACE_ROOTTASK_MSGS

/* Wrapper around dispatch_block() that logs each received message/notification.
 * Used as the block_func in pool_attr so every msg_receive wakeup is visible.
 */
static dispatch_context_t *roottask_dispatch_block(dispatch_context_t *ctp)
{
    ctp = dispatch_block(ctp);
    if (ctp) {
        message_context_t *mctx = &ctp->message_context;
        if (mctx->rcvid > 0) {
            root_log("RT>  rcvid=%-6d pid=%-5d tid=%-2d type=0x%04x\n",
                     mctx->rcvid, mctx->info.pid, mctx->info.tid,
                     (unsigned) mctx->msg->type);
        } else if (mctx->rcvid == 0) {
            root_log("RT>  notification code=%-4d val=%d\n",
                     (int) mctx->msg->notification.code,
                     (int) mctx->msg->notification.value.sival_int);
        }
    }
    return ctp;
}
#define ROOT_BLOCK_FN roottask_dispatch_block
#else
#define ROOT_BLOCK_FN dispatch_block
#endif

static thread_pool_attr_t pool_attr = {
    NULL,                   /* Handle */
    ROOT_BLOCK_FN,          /* Block function */
    NULL,                   /* Unblock func */
    dispatch_handler,       /* Handler function */
    dispatch_context_alloc, /* Context allocation */
    dispatch_context_free,  /* Context free */
    &thread_attr,           /* Thread attribute struct */
    3,                      /* low water mark */
    1,                      /* increment */
    10,                     /* high water mark */
    75,                     /* maximum threads */
};

/* Create roottask's system channel and I/O-service layer.
 *
 * Allocates the roottask channel (must match the bootstrap channel ID), the
 * proc service connection (must match the bootstrap connection ID), and the
 * dispatch handle, then attaches the I/O service layer. Idempotent: returns
 * immediately if already initialized. Any failure is fatal and calls crash().
 */
void message_init(void)
{
    int chid, coid;

    /* Check if already initialized */
    if (dpp) {
        return;
    }

    /* Allocate the roottask main channel at the bootstrap channel ID. */
    chid = channel_create(RTMUX_FLG_CHF_UNBLOCK | RTMUX_FLG_CHF_DISCONNECT);
    root_trace("message_init: channel_create=%d (expect %d)\n", chid,
               ROOTTASK_CHID);
    if (chid != ROOTTASK_CHID) {
        crash();
    }

    /* Allocate the proc service connection at the bootstrap connection ID. */
    coid = connect_attach(0, ROOTTASK_PID, ROOTTASK_CHID, ROOTTASK_COID, 0);
    root_trace("message_init: connect_attach=0x%x (expect 0x%x)\n", coid,
               ROOTTASK_COID);
    if (coid != ROOTTASK_COID) {
        crash();
    }

    dpp = dispatch_create_channel(ROOTTASK_CHID, DISPATCH_FLAG_NOLOCK);
    root_trace("message_init: dispatch_create_channel=%p\n", dpp);
    if (!dpp) {
        crash();
    }

    /* Init the I/O service layer with the right attributes */
    if (ioserv_attach(dpp, &ioserv_attr, NULL, 0, 0, NULL, NULL, NULL) == -1) {
        root_trace("message_init: ioserv_attach FAILED\n");
        crash();
    }
    root_trace("message_init: OK, dpp=%p\n", dpp);
}

static thread_pool_t *tpp;

#define POOL_STACK_SIZE (__PAGESIZE * 2) /* 8 KiB matches thread_attr below */
#define POOL_STACK_COUNT 96 /* generous headroom over POOL_FLAG_RESERVE */

/* Pre-allocate POOL_STACK_COUNT user-VA stacks and register each into the
 * kernel's roottask_stack_cache free list. thread_specret's
 * roottask_stack_alloc pops from this cache for every new pool thread of
 * roottask (pid 1) — no fall-back to the kernel heap (which is PTE_U=0 and
 * therefore unusable as a U-mode stack).
 *
 * Called from message_start BEFORE thread_pool_start so the pool threads find
 * their stacks ready when they hit the WAAA path.
 *
 * Uses roottask_mmap_anon_user (a PRIV kernel extension doing pa_alloc +
 * cpu_pte_manipulate in one BKL-held sweep) rather than libc mmap. libc mmap
 * would route through msg_sendnc(VM_COID) which deadlocks: there is no
 * message-pool receiver yet — that's exactly what we're about to spawn. Same
 * shape as cpiofs_init.
 */
static void prealloc_pool_stacks(void)
{
    extern int RegisterPoolStack(void *vaddr, size_t size);
    unsigned i;
    uintptr_t stk;
    int r;

    for (i = 0; i < POOL_STACK_COUNT; i++) {
        r = roottask_mmap_anon_user(POOL_STACK_SIZE, PROT_READ | PROT_WRITE,
                                    &stk);
        if (r != EOK) {
            root_log(
                "FATAL: prealloc_pool_stacks roottask_mmap_anon_user[%u] "
                "failed: %d\n",
                i, r);
            crash();
        }
        if (RegisterPoolStack((void *) stk, POOL_STACK_SIZE) != EOK) {
            root_log("FATAL: RegisterPoolStack(%p) failed\n", (void *) stk);
            crash();
        }
    }
    root_dbg("prealloc_pool_stacks: %u user-VA stacks of %u bytes registered\n",
             POOL_STACK_COUNT, (unsigned) POOL_STACK_SIZE);
}

/* Bring up the dispatch worker pool and start SMP secondaries.
 *
 * Pre-allocates the user-VA pool stacks, creates and starts the thread pool,
 * triggers SMP bring-up, and waits until at least one worker is blocked in
 * msg_receive on the system channel before returning.
 * Return value: 0 once a worker is ready; otherwise crash() is called.
 */
int message_start(void)
{
    message_init();

    /* Init proc thread attributes */
    (void) pthread_attr_init(&thread_attr);
    (void) pthread_attr_setstacksize(&thread_attr, POOL_STACK_SIZE);

    pool_attr.handle = (void *) dpp;

    /* Pre-allocate user-VA pool stacks BEFORE creating any pool threads — the
     * kernel needs them in the cache by the time thread_pool_start triggers the
     * first WAAA dispatches.
     */
    prealloc_pool_stacks();

    if (!(tpp = thread_pool_create(&pool_attr, POOL_FLAG_RESERVE)))
        crash();

    if (thread_pool_start(tpp) != EOK) {
        root_log("FATAL: thread_pool_start failed\n");
        crash();
    }

    /* Start APs AFTER pool threads are in the dispatch queue. Starting them
     * earlier causes races: APs call idle_pick_thread() while the boot CPU is
     * still creating threads.
     *
     * The kernel-side body cpu_start_others's the secondaries, waits for each
     * to set alives[], pokes them with IPI_RESCHED so they leave WFI, and
     * finally flips smp_dispatch_enabled. All of that is per-CPU kernel state
     * roottask has no business reading directly — one syscall_priv keeps the
     * operation atomic w.r.t. the dispatch-gate flip.
     */
    {
        syscall_priv(__KSYS_PRIV_SMP_BRINGUP, NULL);
    }

    /* Wait until at least one pool thread is blocked in msg_receive on the
     * roottask channel. Without this, user-space processes started after
     * message_start() would send messages to an empty receive queue and block
     * forever.
     *
     * channel_t and the chancons vector live in kernel heap (PTE_U=0) and can't
     * be reached from U-mode roottask, so the channel_has_receivers kernel
     * extension does the lookup + probe in kernel VA and returns the boolean.
     */
    while (!channel_has_receivers(ROOTTASK_PID, ROOTTASK_CHID))
        sched_yield();

    root_trace("message_start: pool thread ready on channel %d\n",
               ROOTTASK_CHID);
    return 0;
}


/* Control the thread pool by adding some extra threads when an operation is
 * going to be performed that may block and then decrementing that thread count
 * when required.
 */
extern int _thread_pool_reserve(thread_pool_t *pool, int wait_count);

/* Called before/after any blocking operation to reserve/ create an extra
 * operating thread.
 *
 * If wait_count >= 0 and there are less than wait_count threads waiting in the
 * thread pool, then we create a new thread and wait for that thread to be going
 * before we continue.
 *
 * If wait_count < 0 then the high water mark is decremented.
 *
 * We always adjust the high water mark to allow us to maintain a simplicity in
 * the functionality.
 *
 * Returns 0 on success -1 on failure
 */

/* Adjust the worker pool reservation, but only inside the proc service.
 *
 * A no-op for any process other than ROOTTASK_PID; otherwise forwards to
 * _thread_pool_reserve. Parameters:
 *   pool       : the thread pool to adjust
 *   wait_count : reserve a spare worker if >= 0; decrement the high
 *                water mark if < 0
 * Return value: 0 on success, -1 on failure.
 */
static int thread_pool_reserve(thread_pool_t *pool, int wait_count)
{
    if (getpid() != ROOTTASK_PID) {
        return 0;
    } else {
        return (_thread_pool_reserve(pool, wait_count));
    }
}

/* Reserve a spare dispatch worker before a blocking operation.
 * Return value: 0 on success, -1 on failure.
 */
int proc_thread_pool_reserve()
{
    /* Reserve a spare dispatch worker before any blocking I/O. Even in the
     * pinned-thread model (no scheduler), this is needed: when the calling
     * worker blocks in msg_sendv, its CPU becomes free, allowing the reserve
     * thread to run and handle the reply.
     *
     * Without this, notification handlers like bootimage_start that self-send
     * messages (sopen, spawn) through the same dispatch channel deadlock
     * non-deterministically when no other worker is in msg_receive yet.
     */
    return thread_pool_reserve(tpp, 1);
}

/* Release the spare dispatch worker reserved by proc_thread_pool_reserve.
 * Return value: 0 on success, -1 on failure.
 */
int proc_thread_pool_reserve_done()
{
    return thread_pool_reserve(tpp, -1);
}
