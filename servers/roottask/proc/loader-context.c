/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "externs.h"
#include "private.h"
#include "root-process.h"
#include <sys/mman.h> /* libc mmap(), see loader_context_alloc */

/* Number of loader contexts to keep around, including the spare */
#define MAX_LOADER_CONTEXT 3

static pthread_mutex_t loader_context_mutex = PTHREAD_MUTEX_INITIALIZER;
static struct loader_context *lcp_list;
static int lcp_num;


/* loader_context_alloc — obtain a loader context for a spawn.
 *
 * Reuses one from the free list when the requested size fits the standard slab;
 * otherwise allocates one via roottask_mmap_anon_user so it lands inside the
 * child-visible L2[3] view (where the loader thread, running in the child's
 * pgdir, can dereference it).
 *
 * Parameters:
 *   msgsize : message-buffer size the context must hold
 *   state   : initial loader-context state
 * Return value: the loader context, or NULL on allocation failure.
 */
struct loader_context *loader_context_alloc(unsigned msgsize, int state)
{
    struct loader_context *lcp;
    size_t size;

    if (msgsize < sizeof(union proc_msg_union)) {
        msgsize = sizeof(union proc_msg_union);
    }
    size = offsetof(struct loader_context, msg) + msgsize;

    lcp = NULL;
    pthread_mutex_lock(&loader_context_mutex);
    if (size <= sizeof(*lcp)) {
        lcp = lcp_list;
        if (lcp) {
            lcp_list = lcp->ctx.next;
            lcp_num--;
            size = lcp->ctx.size;
        }
    }
    if (!lcp) {
        uintptr_t va;
        pthread_mutex_unlock(&loader_context_mutex);

        /* lcp must live at a VA reachable from a CHILD's pgdir during the
         * loader-thread phase. The loader thread runs in the child's pgdir with
         * an L2[3] view of roottask's image (mm_roottask_view_install in
         * kernel/mm/roottask-view.c) — that view only covers VA
         * 0xC0000000–0xFFFFFFFF. libc mmap() can land anywhere in roottask's
         * user-VA range, including low addresses outside L2[3]; if it does, the
         * loader thread's first deref of "lcp" (via lcp->stack[]) faults.
         *
         * roottask_mmap_anon_user allocates from root_phys_map_va (≥
         * 0xc1000000) — squarely inside L2[3] — and skips the I/O service
         * message round-trip entirely (it's a single kernel extension).
         */
        if (roottask_mmap_anon_user(size, PROT_READ | PROT_WRITE | PROT_EXEC,
                                    &va) != EOK) {
            return NULL;
        }
        lcp = (void *) va;
        pthread_mutex_lock(&loader_context_mutex);
    }
    pthread_mutex_unlock(&loader_context_mutex);
    /* We don't have to zero the whole thing - just the first part. */
    memset(lcp, 0, sizeof(struct loader_context_base));
#ifndef NDEBUG
    /* Ensure no code assumes this is zeroed */
    memset(&lcp->msg, 0xee, sizeof(lcp->msg));
#endif
    lcp->ctx.size = size;
    lcp->ctx.state = state;
    lcp->ctx.msgsize = msgsize;
    return lcp;
}


/* loader_context_free — return a loader context to the free list.
 *
 * Caches the context for reuse when there is room and it is a standard slab;
 * otherwise unmaps it.
 *
 * Parameters:
 *   lcp : the loader context being released
 */
void loader_context_free(struct loader_context *lcp)
{
    pthread_mutex_lock(&loader_context_mutex);
    if ((lcp_num < MAX_LOADER_CONTEXT) &&
        (lcp->ctx.msgsize <= sizeof(lcp->msg))) {
        lcp->ctx.next = lcp_list;
        lcp_list = lcp;
        lcp_num++;
    } else {
        pthread_mutex_unlock(&loader_context_mutex);
        /* Matched libc mmap above — release via libc munmap. */
        (void) munmap((void *) lcp, lcp->ctx.size);
        pthread_mutex_lock(&loader_context_mutex);
    }
    pthread_mutex_unlock(&loader_context_mutex);
}


/* loader_thread_attr — build the thread attributes for a loader thread.
 *
 * Points the attributes at the loader context's embedded stack and, if a I/O
 * Loader threads use roottask's local scheduler context. Spawn attributes are
 * applied to the child when the loader commits the start state; inheriting a
 * client thread here would require an ambient cross-process scheduler lookup.
 *
 * Parameters:
 *   attr : thread-attribute structure to fill in
 *   lcp  : loader context providing the loader-thread stack
 *   ctp  : I/O service context to inherit scheduling from, or NULL
 */
void loader_thread_attr(struct _thread_attr *attr,
                        struct loader_context *lcp,
                        ioserv_context_t *ctp)
{
    memset(attr, 0x00, sizeof *attr);
    attr->stackaddr = lcp->stack;
    attr->stacksize = sizeof(lcp->stack);
    (void) ctp;
}
