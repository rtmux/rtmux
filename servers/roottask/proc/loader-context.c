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
#include <sys/spawn-limits.h>

/* Every loader context ever allocated, once free. They come from the
 * roottask_mmap_anon_user window, which only grows (kernel/ext/query.c,
 * root_phys_map_va), so a context can never be returned; keeping each one for
 * reuse bounds the window by the peak number of spawns in flight per size class
 * (see loader_context_alloc).
 */
static pthread_mutex_t loader_context_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Largest loader context: a spawn needing more is refused rather than kept.
 * No request that passes the checks of proc_pspawn() and
 * proc_spawn_with_actions() needs more. A native request's context holds the
 * head (descriptor map, search path, cwd and path) without the arguments,
 * unless the whole request fit the receive buffer, which is far smaller.
 * A message rebuilt from a posix_spawn request is kept whole: with legacy file
 * actions it has a full descriptor map, a cwd, a path and ARG_MAX of
 * arguments; with packed ones the path, the arguments and the action records
 * all come out of one PSPAWN_MSG_MAX, and a bitmap of source descriptors
 * follows.
 */
#define LOADER_CLASS_MAX (512u * 1024u)
#define LOADER_CONTEXT_HEAD \
    (offsetof(struct loader_context, msg) + sizeof(struct _proc_spawn))
_Static_assert(LOADER_CONTEXT_HEAD + UINT16_MAX * sizeof(int32_t) +
                       2u * UINT16_MAX + ARG_MAX <=
                   LOADER_CLASS_MAX,
               "a rebuilt spawn message with a full fd map must fit the "
               "largest loader context");
_Static_assert(LOADER_CONTEXT_HEAD + UINT16_MAX * sizeof(int32_t) +
                       3u * UINT16_MAX <=
                   LOADER_CLASS_MAX,
               "the head of a native spawn request must fit the largest "
               "loader context");
_Static_assert(LOADER_CONTEXT_HEAD + PSPAWN_MSG_MAX + UINT16_MAX / 8u + 1u <=
                   LOADER_CLASS_MAX,
               "a spawn with packed file actions must fit the largest loader "
               "context");
static struct loader_context *lcp_list;


/* A spawn is about to fail with ENOMEM for want of a loader context, which is
 * not roottask's heap: the context is contiguous physical memory mapped by the
 * kernel (kext_root_mmap_anon_user). `why` names the cause: a request over the
 * largest class, or the error of that mapping, ENOMEM when no contiguous run of
 * `size` bytes is free. Rate limited.
 */
static void loader_context_note(size_t size, const char *why)
{
    static atomic_uint nfail;
    unsigned n = root_warn_due(&nfail);

    if (n)
        root_warn("spawn: no loader context of %zu bytes (failure %u): %s\n",
                  size, n, why);
}

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
    if (size > LOADER_CLASS_MAX) {
        loader_context_note(size, "over the largest class");
        return NULL;
    }
    class = sizeof(struct loader_context);
    while (class < size)
        class <<= 1;

    /* The base is not a power of two, so the doubling can step past the cap;
     * the largest class is the cap itself.
     */
    if (class > LOADER_CLASS_MAX)
        class = LOADER_CLASS_MAX;
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
    if (fit) {
        lcp = *fit;
        *fit = lcp->ctx.next;
        size = lcp->ctx.size;
    } else {
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
        int rc = roottask_mmap_anon_user(
            size, PROT_READ | PROT_WRITE | PROT_EXEC, &va);

        if (rc != EOK) {
            loader_context_note(size, strerror(rc));
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
