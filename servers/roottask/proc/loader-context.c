/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "externs.h"
#include "private.h"
#include "root-process.h"
#include <sys/mman.h> /* PROT_* for roottask_mmap_anon_user */

/* Every loader context ever allocated, once free. They come from the
 * roottask_mmap_anon_user window, which only grows (kernel/ext/query.c,
 * root_phys_map_va), so a context can never be returned; keeping each one for
 * reuse bounds the window by the peak number of spawns in flight per size class
 * (see loader_context_alloc).
 */
static pthread_mutex_t loader_context_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Largest loader context: a spawn needing more is refused rather than kept. */
#define LOADER_CLASS_MAX (512u * 1024u)
static struct loader_context *lcp_list;


/* loader_context_alloc — obtain a loader context for a spawn.
 *
 * Reuses the smallest free context of the request's size class or larger;
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
    struct loader_context *lcp, **fit = NULL;
    size_t size, class;

    if (msgsize < sizeof(union proc_msg_union)) {
        msgsize = sizeof(union proc_msg_union);
    }
    size = offsetof(struct loader_context, msg) + msgsize;

    /* Size classes. The client sets the message size, so exact sizes would let
     * a stream of ever-larger spawns allocate a new context each time from the
     * window that never shrinks. The standard context (most spawns) is the
     * smallest class and each larger one doubles it, up to LOADER_CLASS_MAX
     * (ARG_MAX of arguments plus a full descriptor map fit well below it).
     * Reuse takes an exact class first, so a big context is not used up by a
     * small spawn.
     */
    class = sizeof(struct loader_context);
    while (class < size)
        class <<= 1;
    if (class > LOADER_CLASS_MAX)
        return NULL;
    size = class;

    pthread_mutex_lock(&loader_context_mutex);
    for (struct loader_context **pp = &lcp_list; *pp; pp = &(*pp)->ctx.next) {
        size_t have = (*pp)->ctx.size;

        if (have == size) {
            fit = pp;
            break;
        }
        if (have > size && (!fit || have < (*fit)->ctx.size))
            fit = pp;
    }
    lcp = fit ? *fit : NULL;
    if (lcp) {
        *fit = lcp->ctx.next;
        size = lcp->ctx.size;
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
 * Always kept for reuse: its memory came from the grow-only
 * roottask_mmap_anon_user window, which libc munmap cannot give back. Unmapping
 * it here used to leak that window by one context per spawn beyond a
 * three-entry cache, until roottask's own allocations from the window failed.
 *
 * Parameters:
 *   lcp : the loader context being released
 */
void loader_context_free(struct loader_context *lcp)
{
    pthread_mutex_lock(&loader_context_mutex);
    lcp->ctx.next = lcp_list;
    lcp_list = lcp;
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
