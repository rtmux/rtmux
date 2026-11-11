/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <core/globals.h>

#if defined(CONFIG_DEBUG_VECTOR) && !defined(NDEBUG)
static void check_vec(vector_t *vec)
{
    unsigned i, nfree, nused;
    uintptr_t n;
    void **ptr;

    for (i = nfree = nused = 0; i < vec->nentries; ++i) {
        n = (uintptr_t) VEC(vec, i);
        if (n & 1) {
            ++nfree;
            /* Must be in free list */
            for (ptr = vec->free; VECAND(ptr, ~3UL);
                 ptr = *(void **) VECAND(ptr, ~3UL)) {
                if (VECAND(ptr, ~3UL) == VECAND(n, ~3UL)) {
                    break;
                }
            }
            if (!ptr) {
                crash();
            }
        } else if (n == 0) {
            ++nfree;
        } else {
            ++nused;
            /* Must NOT be in free list */
            for (ptr = vec->free; VECAND(ptr, ~3UL);
                 ptr = *(void **) VECAND(ptr, ~3UL)) {
                if (VECAND(ptr, ~3UL) == VECAND(n, ~3UL)) {
                    crash();
                }
            }
        }
    }

    if (nused + nfree != vec->nentries) {
        crash();
    }

    if (nfree != vec->nfree) {
        crash();
    }

    ptr = vec->free;
    while (VECAND(ptr, ~3UL)) {
        n = (uintptr_t) ptr;
        ptr = (void **) (n & ~3UL);
        ptr = *ptr;
        if ((n & 1) == 0) {
            crash();
        }
    }
}

#define CHECK_VEC(v) check_vec(v)
#else
#define CHECK_VEC(v)
#endif

void *rdecl vector_lookup(vector_t *vec, unsigned id)
{
    void *ptr;

    if (vec && id < vec->nentries) {
        return VECP(ptr, vec, id);
    }

    return NULL;
}


void *rdecl vector_lookup2(vector_t *vec, unsigned id)
{
    void *ptr;

    if (vec && id < vec->nentries && VECAND(ptr = VEC(vec, id), 1) == 0) {
        return VECAND(ptr, ~3);
    }

    return NULL;
}


void *rdecl vector_search(vector_t *vec, unsigned id, unsigned *found)
{
    void *ptr;

    if (vec && (vec->nfree < vec->nentries)) {
        while (id < vec->nentries) {
            ptr = VECP2(ptr, vec, id);
            if (found) {
                if (ptr) {
                    *found = id;
                } else {
                    id++;
                    continue;
                }
            }
            return ptr;
        }
    }
    return NULL;
}


/* The bottom bit of a pointer in the vector table vec->vector[n] is used as
 * follows:
 *
 * 0 - A regular pointer (this assumes all pointers are 4 byte aligned) 1 - On
 * the free list which runs through the vector (vec->free)
 *
 * The next bit may be used as a flag for various purposes. Thread vectors use
 * it to indicate a destroyed thread which has not been joined while connections
 * use it to indicate close on exec.
 */
int rdecl vector_add(vector_t *vec, void *object, unsigned index)
{
    unsigned nentries, inc;
    void *ptr, *optr, **prev, **cur;
    ptrdiff_t adjust;

    /* all objects must be 32-bit aligned */
    if (VECAND(object, 3)) {
        crash();
    }
    /* Look for a free spot starting at index. */
    for (prev = &vec->free; (cur = VECAND(*prev, ~3)); prev = cur) {
        unsigned i = (unsigned) (cur - &vec->vector[0]);

        if (i >= index) {
            *prev = *cur;
            *cur = object;
            --vec->nfree;
            CHECK_VEC(vec);
            return (int) i;
        }
    }

    /* No free spot so we must grow the vector. Max it out at 65K-1. */
    if (vec->nentries == VECTOR_MAX) {
        return -1;
    }

    if (index >= vec->nentries) {
        nentries = index + 1; /* Grow to what was asked. */
    } else {
        nentries = (unsigned) vec->nentries +
                   ((unsigned) vec->nentries + 4) / 4; /* Grow by 25% */
    }

    /* Always start out with a vector capable of handling at least 8 entries to
     * avoid memory fragmentation
     */
    if (nentries < 8) {
        nentries = 8;
    }

    if (nentries > VECTOR_MAX) {
        nentries = VECTOR_MAX;
    }

    /* Note: since we need to fix up the entries, we cannot simply realloc a
     * bigger vector as this leave the vectors inconsistent (think timers firing
     * from the clock handler) See PR 24407.
     */
    ptr = _scalloc(nentries * sizeof(void *));

    if (!ptr) {
        return -1;
    }
    optr = vec->vector;
    memcpy(ptr, optr, vec->nentries * sizeof(void *));

    /* Since we always allocate a new vector, we must fixup the freelist since
     * it runs through it.
     */
    adjust = (char *) ptr - (char *) optr;

#ifndef NDEBUG
    if (adjust & 0x03L)
        crash();
#endif

    for (prev = &vec->free; VECAND(*prev, ~3); prev = VECAND(*prev, ~3)) {
        *(intptr_t *) prev += adjust;
    }
    /* This is safe since any IRQ code will only reference valid entries */
    vec->vector = ptr;
    /* We can now free the old vector */
    _sfree(optr, vec->nentries * sizeof(void *));

    /* Link all the new slots in the extended vector. Bottom bit indicates it is
     * on the free list.
     */
    *prev = VECOR(cur = &vec->vector[vec->nentries], 1);
    for (inc = nentries - vec->nentries; --inc; ++cur) {
        *cur = VECOR(cur + 1, 1);
    }
    *cur = VECOR(NULL, 1);

    /* Update the vector info for the new size and possible location. */
    vec->nfree += (uint16_t) (nentries - vec->nentries);
    vec->nentries = (uint16_t) nentries;

    /* Now that we have grown the vector the add can't fail! */
    return object ? vector_add(vec, object, index) : (int) index;
}


void *rdecl vector_rem(vector_t *vec, unsigned index)
{
    void *ptr, **vecp;

    /* Verify index is range and that the entry is not already free. */
    if (index >= vec->nentries || VECAND(ptr = VEC(vec, index), 1)) {
        return NULL;
    }

    ptr = VECAND(ptr, ~3);

    vecp = &VEC(vec, (int) index - 1);
    if (!VECAND(vec->free, ~3) || vecp < (void **) VECAND(vec->free, ~3)) {
        /* Simple case is returning to the start of the list. */
        vecp = &vec->free;
    } else {
        /* Scan backwards for the insertion point (this cannot fail)! */
        while (VECAND(*vecp, 1) == 0) {
            --vecp;
        }
    }

    VEC(vec, index) = *vecp;
    *vecp = VECOR(&VEC(vec, index), 1);
    ++vec->nfree;

    CHECK_VEC(vec);
    return ptr;
}


int rdecl vector_flag(vector_t *vec, unsigned index, int flag)
{
    void *ptr;

    if (index >= vec->nentries || VECAND(ptr = VEC(vec, index), 1)) {
        return -1;
    }

    VEC(vec, index) = flag ? VECOR(ptr, 2) : VECAND(ptr, ~2);

    return 0;
}


void rdecl vector_free(vector_t *vec)
{
    _sfree(vec->vector, vec->nentries * sizeof(void *));
    vec->nentries = vec->nfree = 0;
    vec->vector = NULL;
}
