/*
 * Copyright (c) The FreeBSD Project. All rights reserved.
 * Authored by Chris Kingsley <kingsley@cit-20>
 *
 * This is a very fast storage allocator. It allocates blocks of a small number
 * of different sizes, and keeps free lists of each size. Blocks that don't
 * exactly fit are passed up to the next larger size. In this implementation,
 * the available sizes are 2^n-4 (or 2^n-10) bytes long. This is designed for
 * use in a virtual memory environment.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <sys/mman.h>
#include <errno.h>
#include <stdarg.h>
#include <stdalign.h>
#include <stddef.h>
#include <string.h>
#include <unistd.h>
#ifdef IN_LDSO
#include "dynlink.h"
#include "printf.h"
#include "paths.h"
#include "libc.h"
#endif
#include "malloc.h"

/* Pre-allocate mmap'ed pages */
#define NPOOLPAGES (128 * 1024 / pagesz)
static caddr_t pagepool_start, pagepool_end;

#ifdef IN_LDSO

/* Static bootstrap pool: during early ldso bootstrap, mmap is not available.
 * Seed the page pool with a static buffer so that xmalloc/xstrdup work without
 * hitting mmap. 32 KB is more than enough for the initial needs.
 */
#define LDSO_BOOTSTRAP_POOL_SIZE (32 * 1024)
static alignas(16) char ldso_bootstrap_pool[LDSO_BOOTSTRAP_POOL_SIZE];
static int ldso_pool_initialized;
static int ldso_pool_from_mmap; /* 0 = bootstrap static pool, 1 = mmap'd pool */
#endif

/* The overhead on a block is at least 4 bytes. When free, this space contains a
 * pointer to the next free block, and the bottom two bits must be zero. When in
 * use, the first byte is set to MAGIC, and the second byte is the size index.
 * The remaining bytes are for alignment.
 */
union overhead {
    union overhead *ov_next; /* when free */
    struct {
        uint16_t ovu_index; /* bucket # */
        uint8_t ovu_magic;  /* magic number */
    } ovu;
#define ov_magic ovu.ovu_magic
#define ov_index ovu.ovu_index
};

static void morecore(int bucket);
static int morepages(size_t n);

#define MAGIC 0xef  /* magic # on accounting info */
#define AMAGIC 0xdf /* magic # for aligned alloc */

/* nextf[i] is the pointer to the next free block of size (FIRST_BUCKET_SIZE <<
 * i). The overhead information precedes the data area returned to the user.
 */
#define LOW_BITS 3
#define FIRST_BUCKET_SIZE (1U << LOW_BITS)
#define NBUCKETS 30
static union overhead *nextf[NBUCKETS];

static size_t pagesz; /* page size */

/* The array of supported page sizes is provided by the user, i.e., the program
 * that calls this storage allocator. That program must initialize the array
 * before making its first call to allocate storage. The array must contain at
 * least one page size. The page sizes must be stored in increasing order.
 */

static void *cp2op(void *cp)
{
    return (((caddr_t) cp - sizeof(union overhead)));
}

void *__crt_malloc(size_t nbytes)
{
    union overhead *op;
    int bucket;
    size_t amt;

    /* First time malloc is called, setup page size. */
    if (pagesz == 0)
        pagesz = pagesizes ? pagesizes[0] : 4096;
#ifdef IN_LDSO
    if (!ldso_pool_initialized) {
        ldso_pool_initialized = 1;
        pagepool_start = ldso_bootstrap_pool;
        pagepool_end = ldso_bootstrap_pool + LDSO_BOOTSTRAP_POOL_SIZE;
    }
#endif

    /* Convert amount of memory requested into closest block size stored in hash
     * buckets which satisfies request. Account for space used per block for
     * accounting.
     */
    amt = FIRST_BUCKET_SIZE;
    bucket = 0;
    while (nbytes > amt - sizeof(*op)) {
        amt <<= 1;
        bucket++;
        if (amt == 0 || bucket >= NBUCKETS)
            return (NULL);
    }

    /* If nothing in hash bucket right now, request more memory from the system.
     */
    if (!(op = nextf[bucket])) {
        morecore(bucket);
        if (!(op = nextf[bucket]))
            return (NULL);
    }
    /* remove from linked list */
    nextf[bucket] = op->ov_next;
    op->ov_magic = MAGIC;
    op->ov_index = (uint16_t) bucket;
    return ((char *) (op + 1));
}

void *__crt_calloc(size_t num, size_t size)
{
    void *ret;

    if (size != 0 && (num * size) / size != num) {
        /* size_t overflow. */
        return (NULL);
    }

    if ((ret = __crt_malloc(num * size)))
        memset(ret, 0, num * size);

    return (ret);
}

void *__crt_aligned_alloc_offset(size_t align, size_t size, size_t offset)
{
    void *mem, *ov;
    union overhead ov1;
    uintptr_t x;

    if (align < FIRST_BUCKET_SIZE)
        align = FIRST_BUCKET_SIZE;
    offset &= align - 1;
    mem = __crt_malloc(size + align + offset + sizeof(union overhead));
    if (!mem)
        return (NULL);
    x = lib_align_up((uintptr_t) mem + sizeof(union overhead), align);
    x += offset;
    ov = cp2op((void *) x);
    ov1.ov_magic = AMAGIC;
    ov1.ov_index = (uint16_t) (x - (uintptr_t) mem + sizeof(union overhead));
    memcpy(ov, &ov1, sizeof(ov1));
    return ((void *) x);
}

/* Allocate more memory to the indicated bucket. */
static void morecore(int bucket)
{
    union overhead *op;
    size_t sz;    /* size of desired block */
    size_t amt;   /* amount to allocate */
    size_t nblks; /* how many blocks we get */

    sz = FIRST_BUCKET_SIZE << bucket;
    if (sz < pagesz) {
        amt = pagesz;
        nblks = amt / sz;
    } else {
        amt = sz;
        nblks = 1;
    }
    if (amt > pagepool_end - pagepool_start)
        if (morepages(amt / pagesz + NPOOLPAGES) == 0 &&
            /* Retry with min required size */
            morepages(amt / pagesz) == 0)
            return;
    op = (union overhead *) pagepool_start;
    pagepool_start += amt;

    /* Add new memory allocated to that on free list for this hash bucket. */
    nextf[bucket] = op;
    while (--nblks > 0) {
        op->ov_next = (union overhead *) ((caddr_t) op + sz);
        op = (union overhead *) ((caddr_t) op + sz);
    }
}

void __crt_free(void *cp)
{
    union overhead *op, op1;
    void *opx;
    int size;

    if (!cp)
        return;
    opx = cp2op(cp);
    memcpy(&op1, opx, sizeof(op1));
    op = op1.ov_magic == AMAGIC ? (void *) ((caddr_t) cp - op1.ov_index) : opx;
    if (op->ov_magic != MAGIC)
        return; /* sanity */
    size = op->ov_index;
    op->ov_next = nextf[size]; /* also clobbers ov_magic */
    nextf[size] = op;
}

void *__crt_realloc(void *cp, size_t nbytes)
{
    size_t onb;
    size_t prev_sz;
    int i;
    union overhead *op;
    char *res;

    if (!cp)
        return (__crt_malloc(nbytes));
    op = cp2op(cp);
    if (op->ov_magic != MAGIC)
        return (NULL); /* Double-free or bad argument */
    i = op->ov_index;
    onb = (size_t) 1 << (i + 3);
    if (onb < pagesz)
        onb -= sizeof(*op);
    else
        onb += pagesz - sizeof(*op);
    /* avoid the copy if same size block */
    prev_sz = 0;
    if (i != 0) {
        prev_sz = (size_t) 1 << (i + 2);
        if (prev_sz < pagesz)
            prev_sz -= sizeof(*op);
        else
            prev_sz += pagesz - sizeof(*op);
    }
    if (nbytes <= onb && nbytes > prev_sz)
        return (cp);
    if (!(res = __crt_malloc(nbytes)))
        return (NULL);
    memmove(res, cp, (nbytes < onb) ? nbytes : onb);
    __crt_free(cp);
    return (res);
}

static int morepages(size_t n)
{
    caddr_t addr;
    size_t offset;

    if (ldso_pool_from_mmap && pagepool_end - pagepool_start > pagesz) {
        addr = (caddr_t) lib_align_up((uintptr_t) pagepool_start,
                                      (uintptr_t) pagesz);
        if (munmap(addr, (size_t) (pagepool_end - addr)) != 0) {
#ifdef IN_LDSO
            ldso_fdprintf(STDERR_FILENO,
                          _BASENAME_LDSO
                          ": "
                          "morepages: cannot munmap %p: %s\n",
                          addr, ldso_strerror(errno));
#endif
        }
    }

    offset = (uintptr_t) pagepool_start -
             lib_align_down((uintptr_t) pagepool_start, (uintptr_t) pagesz);

    addr = mmap(0, n * pagesz, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE,
                -1, 0);
    if (addr == MAP_FAILED) {
#ifdef IN_LDSO
        ldso_fdprintf(STDERR_FILENO,
                      _BASENAME_LDSO
                      ": morepages: "
                      "cannot mmap anonymous memory: %s\n",
                      ldso_strerror(errno));
#endif
        pagepool_start = pagepool_end = NULL;
        return (0);
    }
    pagepool_start = addr;
    pagepool_end = pagepool_start + n * pagesz;
    pagepool_start += offset;
    ldso_pool_from_mmap = 1;

    return (int) n;
}
