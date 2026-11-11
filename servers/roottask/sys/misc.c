/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* misc.c -- assorted small bits of roottask that don't deserve a file each:
 * storage definitions for the EXT-prefixed globals declared in "externs.h", the
 * build-time timestamp / version string, and the U-mode aliases for the
 * historical _s* heap functions.
 */

/* 1. Storage for EXT-prefixed globals declared in "externs.h".
 *
 * "externs.h" uses the EXT macro (extern by default). Defining PROCHDR before
 * including it turns each declaration into a real definition — this TU is the
 * unique storage point. Must come first in the file; nothing else may include
 * "externs.h" until after this header has been processed.
 */
#define PROCHDR
#include "externs.h"


/* 2. Build-time timestamp and OS version string.
 *
 * This TU is recompiled on every link so the timestamp tracks the actual build
 * time. os_version_string is the canonical release string from
 * RTMUX_VERSION_STRING.
 */
#include <sys/rtmux-core.h>

const char timestamp[] = __DATE__ " " __TIME__;
const char os_version_string[] = RTMUX_VERSION_STRING;


/* 3. U-mode roottask heap wrappers for _s* allocation calls.
 *
 * U-mode roottask has its own libc heap. These wrappers map the local allocator
 * calls onto plain libc malloc/free. The size argument to _sfree / _srealloc is
 * retained for the existing call signature and is ignored. here.
 */
#include <stddef.h>
#include <string.h>

void *_smalloc(size_t size)
{
    return malloc(size);
}

void *_scalloc(size_t size)
{
    void *p = malloc(size);
    if (p)
        memset(p, 0, size);
    return p;
}

void *_srealloc(void *data, size_t old_size, size_t new_size)
{
    (void) old_size;
    return realloc(data, new_size);
}

void _sfree(void *data, size_t size)
{
    (void) size;
    free(data);
}


/* 4. soul_t-based object pool — roottask-private souls only.
 *
 * RTMux's kernel has its own object_alloc/object_free for the kernel souls
 * (process_souls, thread_souls, credential_souls, …); this is an independent
 * implementation for the two roottask-private souls (open_souls in path-open.c,
 * wait_souls in wait.c). The two implementations never touch each other's
 * memory: kernel souls live in kernel heap, roottask souls in libc heap; the
 * soul descriptors themselves are static globals in their owning binary.
 *
 * proc_object_alloc / proc_object_free in support.c serialize calls with
 * object_allocator_mutex, so this body doesn't take its own lock. Pure freelist
 * arithmetic.
 *
 * POOL_GROW=16 is a heuristic — roottask's two souls have single- digit
 * high-water marks in normal use, so a single grow is enough. align is 0 for
 * both roottask souls, so no extra alignment slot is reserved.
 */
#include <objects.h> /* soul_t */

#define POOL_GROW 16

/* object_alloc / object_free — roottask's local soul_t allocator. Roottask
 * never passes a process_t pointer here, keeping its heap disjoint from the
 * kernel's. Parameters:
 *   soulp : the soul descriptor whose freelist to allocate from
 * Return value: a zeroed object, or NULL if the batch grow failed.
 */
void *object_alloc(soul_t *soulp)
{
    void **ptr;

    if (!(ptr = soulp->next)) {
        size_t sz = soulp->size + soulp->align;
        char *batch;
        unsigned i;

        if (sz < sizeof(void *))
            sz = sizeof(void *);
        batch = _scalloc(POOL_GROW * sz);
        if (!batch)
            return NULL;
        for (i = 0; i < POOL_GROW; i++) {
            void **f = (void **) (batch + i * sz);
            *f = soulp->next;
            soulp->next = f;
        }
        soulp->total += POOL_GROW;
        ptr = soulp->next;
    }

    soulp->next = *ptr;
    *ptr = NULL;
    soulp->used++;
    return ptr;
}


/* Return an object to its soul's freelist. Parameters:
 *   soulp : the soul descriptor the object belongs to
 *   ptr   : the object to free
 */
void object_free(soul_t *soulp, void *ptr)
{
    soulp->used--;
    memset(ptr, 0, soulp->size);
    *(void **) ptr = soulp->next;
    soulp->next = ptr;
}


/* 4b. Globals that used to live in the kernel but only roottask uses them at
 * runtime (or the value is a per-binary compile-time constant that never
 * diverges).
 *
 *   proc_service_scoid — server-connection-ID of roottask's own
 *                        channel_attach; written once during process dispatch
 *                        init and read by wait, exit, and coredump handlers to
 *                        filter incoming notifications.  Pure roottask.
 *
 *   user_boundary_addr — top of user-space VA range.  Both kernel
 *                       and roottask read it; both initialize from
 *                       the same per-arch CPU_USER_VADDR_END /
 *                       VM_USER_SPACE_BOUNDARY constant; neither
 *                       mutates after init, so disjoint copies
 *                       cannot diverge.  Kernel keeps its own
 *                       (kernel/include/core/globals.h:EXT INIT1).
 */
int proc_service_scoid;
unsigned user_boundary_addr;
unsigned legal_cpu_bitmask; /* populated once by roottask_main
                             * via __KSYS_PRIV_LEGAL_CPU_MASK
                             */


/* 5b. crt0 entry point — libc/riscv/crt0.S calls main(), but roottask's entry
 * is roottask_main(). Tiny shim.
 *
 * IMPORTANT: this function MUST NOT return. crt0 calls "exit(rv)" when main
 * returns, and "exit()" ultimately routes through "_exit()" → "_PROC_EXIT" to
 * proc service (which IS roottask); proc service would then ask the kernel to
 * tear down all of roottask's threads, which is catastrophic. After
 * roottask_main → message_start sets up the pool, the boot thread has nothing
 * left to do — terminate it cleanly with pthread_exit so only THIS thread goes
 * away while the pool keeps running.
 *
 * (kernel/ext/exit.c also has a defensive guard that refuses
 * process_threads_destroy on ROOTTASK_PID — belt-and-suspenders.)
 */
#include <pthread.h>

extern int roottask_main(int argc, char **argv);

int main(int argc, char **argv)
{
    (void) roottask_main(argc, argv);
    pthread_exit(NULL);

    /* NOTREACHED — but the compiler doesn't know pthread_exit is noreturn under
     * our libc; loop here so we never fall back into crt0's exit() path.
     */
    for (;;)
        pthread_exit(NULL);
}


/* 5c. Thin wrappers over the __KSYS_PRIV_* slots for the kernel-resident
 * helpers roottask still calls.
 */

struct kcall_args_register_mm {
    uintptr_t *va_rover_p;
    int (*pte_map)(address_t *,
                   uintptr_t,
                   uintptr_t,
                   int,
                   path_object_t *,
                   paddr_t,
                   unsigned);
    struct pa_restrict **restrict_proc_pp;
    struct cpu_mm_as **pgtbl_list_pp;
};

/* Hand the kernel-side mm kernel extensions the roottask-side pointers they
 * call into.
 *
 * Forwards the four pointers through the __KSYS_PRIV_REGISTER_MM_HELPERS slot
 * so kernel-resident VM helpers can reach them. Parameters:
 *   va_rover_p       : pointer to roottask's VA rover
 *   pte_map          : roottask's PTE-map helper
 *   restrict_proc_pp : pointer to the restrict_proc pointer
 *   pgtbl_list_pp    : pointer to the page-table list pointer
 */
void kext_register_mm_helpers(uintptr_t *va_rover_p,
                              int (*pte_map)(address_t *,
                                             uintptr_t,
                                             uintptr_t,
                                             int,
                                             path_object_t *,
                                             paddr_t,
                                             unsigned),
                              struct pa_restrict **restrict_proc_pp,
                              struct cpu_mm_as **pgtbl_list_pp)
{
    struct kcall_args_register_mm args = {
        .va_rover_p = va_rover_p,
        .pte_map = pte_map,
        .restrict_proc_pp = restrict_proc_pp,
        .pgtbl_list_pp = pgtbl_list_pp,
    };
    (void) syscall_priv(__KSYS_PRIV_REGISTER_MM_HELPERS, &args);
}

/* There is no lookup_rcvid wrapper here: a kernel-VA connect_t* / thread_t*
 * would be useless to U-mode roottask (kernel heap is PTE_U=0). If a future
 * code path needs to inspect a thread's state by rcvid, add a copy-out kernel
 * extension that returns specific scalars (process_query_info-style), not the
 * raw pointers.
 */

struct kcall_args_stop_threads {
    process_t *prp;
    unsigned flags;
    unsigned iflags;
};

/* Stop the threads of a process via the __KSYS_PRIV_STOP_THREADS slot.
 * Parameters:
 *   prp    : the target process
 *   flags  : stop flags
 *   iflags : internal flags
 */
void stop_threads(process_t *prp, unsigned flags, unsigned iflags)
{
    struct kcall_args_stop_threads args = {
        .prp = prp,
        .flags = flags,
        .iflags = iflags,
    };
    syscall_priv(__KSYS_PRIV_STOP_THREADS, &args);
}

/* 5. Dynamic-array (vector_t) helpers.
 *
 * The implementation mirrors kernel/object/vector.c; the two copies operate on
 * disjoint vector instances:
 *
 *   - Kernel vectors (prp->threads, prp->chancons, prp->fdcons, the
 *     global query/process/soul/interrupt/etc. vectors) are grown
 *     by kernel-side code (kernel/init/init.c, sched/thread.c, ipc/connect.c).
 *     Roottask read-only walks use vector_lookup and do not touch any
 *     allocator.
 *
 *   - Roottask vectors (vm_fd_vector in mem/vm-fd.c, the
 *     per-process prp->events allocated and grown by event.c)
 *     are grown by roottask-side code only.  This implementation's
 *     _scalloc/_sfree calls land in the libc heap.
 *
 * No vector instance is grown by both sides, so the two heaps stay disjoint —
 * same logic as the object pool above.
 */

/* Local copies of the VEC* macros (kernel/core/macros.h is kernel- internal —
 * roottask shouldn't include it).
 */
#define VEC(vec, index) ((vec)->vector[index])
#define VECP(ptr, vec, index) \
    (((uintptr_t) ((ptr) = (vec)->vector[index]) & 3) ? NULL : (ptr))
#define VECP2(ptr, vec, index)                        \
    (((uintptr_t) ((ptr) = (vec)->vector[index]) & 1) \
         ? NULL                                       \
         : (void *) ((uintptr_t) (ptr) & ~(uintptr_t) 3))
#define VECAND(ptr, bits) ((void *) ((uintptr_t) (bits) & (uintptr_t) (ptr)))
#define VECOR(ptr, bits) ((void *) ((bits) | (uintptr_t) (ptr)))
#define VECTOR_MAX (0xffff - 1)


/* Look up the object stored at an index, ignoring freelist slots. Parameters:
 *   vec : the vector
 *   id  : the index
 * Return value: the object pointer, or NULL if out of range or on the
 *               freelist.
 */
void *vector_lookup(vector_t *vec, unsigned id)
{
    void *ptr;

    if (vec && id < vec->nentries)
        return VECP(ptr, vec, id);
    return NULL;
}


/* Insert an object into the vector at or after a given index, growing the
 * backing array if necessary.
 *
 * The bottom bit of a pointer in the vector table vec->vector[n] is used as
 * follows:
 *   0 = a regular pointer (assumes 4-byte alignment of the object)
 *   1 = on the freelist running through the vector (vec->free)
 * The next bit is a per-caller flag (e.g. thread vectors use it to mark a
 * destroyed but unjoined thread; connection vectors use it for close-on-exec).
 * Parameters:
 *   vec    : the vector
 *   object : the object to insert (must be 4-byte aligned)
 *   index  : earliest acceptable slot
 * Return value: the index used, or -1 if the vector is full or a grow
 *               allocation failed.
 */
int vector_add(vector_t *vec, void *object, unsigned index)
{
    unsigned nentries, inc;
    void *ptr, *optr, **prev, **cur;
    ptrdiff_t adjust;

    if (VECAND(object, 3))
        crash();

    /* Look for a free spot at-or-after index. */
    for (prev = &vec->free; (cur = VECAND(*prev, ~3)); prev = cur) {
        unsigned i = (unsigned) (cur - &vec->vector[0]);

        if (i >= index) {
            *prev = *cur;
            *cur = object;
            --vec->nfree;
            return (int) i;
        }
    }

    if (vec->nentries == VECTOR_MAX)
        return -1;

    if (index >= vec->nentries)
        nentries = index + 1;
    else
        nentries =
            (unsigned) vec->nentries + ((unsigned) vec->nentries + 4) / 4;
    if (nentries < 8)
        nentries = 8;
    if (nentries > VECTOR_MAX)
        nentries = VECTOR_MAX;

    /* Allocate a fresh array (cannot realloc — freelist runs through the array
     * and pointers must be fixed up).
     */
    ptr = _scalloc(nentries * sizeof(void *));
    if (!ptr)
        return -1;
    optr = vec->vector;
    memcpy(ptr, optr, vec->nentries * sizeof(void *));

    adjust = (char *) ptr - (char *) optr;
    for (prev = &vec->free; VECAND(*prev, ~3); prev = VECAND(*prev, ~3))
        *(intptr_t *) prev += adjust;

    vec->vector = ptr;
    _sfree(optr, vec->nentries * sizeof(void *));

    /* Link the new tail slots onto the freelist. */
    *prev = VECOR(cur = &vec->vector[vec->nentries], 1);
    for (inc = nentries - vec->nentries; --inc; ++cur)
        *cur = VECOR(cur + 1, 1);
    *cur = VECOR(NULL, 1);

    vec->nfree += (uint16_t) (nentries - vec->nentries);
    vec->nentries = (uint16_t) nentries;

    return object ? vector_add(vec, object, index) : (int) index;
}


/* Remove the object at an index, returning it to the vector freelist.
 * Parameters:
 *   vec   : the vector
 *   index : the slot to free
 * Return value: the removed object (tag bits cleared), or NULL if the
 *               slot was empty or already free.
 */
void *vector_rem(vector_t *vec, unsigned index)
{
    void *ptr, **vecp;

    if (index >= vec->nentries || VECAND(ptr = VEC(vec, index), 1))
        return NULL;
    ptr = VECAND(ptr, ~3);

    vecp = &VEC(vec, (int) index - 1);
    if (!VECAND(vec->free, ~3) || vecp < (void **) VECAND(vec->free, ~3)) {
        vecp = &vec->free;
    } else {
        while (VECAND(*vecp, 1) == 0)
            --vecp;
    }

    VEC(vec, index) = *vecp;
    *vecp = VECOR(&VEC(vec, index), 1);
    ++vec->nfree;
    return ptr;
}


/* Free the vector's backing array and reset it to empty. Parameters:
 *   vec : the vector to free
 */
void vector_free(vector_t *vec)
{
    _sfree(vec->vector, vec->nentries * sizeof(void *));
    vec->nentries = vec->nfree = 0;
    vec->vector = NULL;
}
