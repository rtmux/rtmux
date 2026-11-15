/*-
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Copyright (c) 1992, 1993
 * The Regents of the University of California. All rights reserved.
 *
 * The median selection and the three-way partition keep the structure of the
 * BSD qsort, from Bentley and McIlroy's "Engineering a Sort Function"; the
 * depth budget, heapsort fallback, swaps, and qsort_r are RTMux additions.
 */

/*
 * qsort and qsort_r: introsort.
 *
 * Median-of-three (pseudo-median of nine for large ranges) quicksort with
 * Bentley-McIlroy three-way partitioning, so runs of equal keys cost linear
 * time. Each level recurses only into the smaller partition and loops on the
 * larger, which bounds the stack at log2(n) frames. A budget of
 * 2*floor(log2(n)) caps the quicksort phase: a balanced split spends one unit
 * and a lopsided one two. A range that exhausts the budget is finished with
 * bottom-up heapsort, so the worst case is O(n log n) comparisons whatever the
 * comparator answers. Ranges of at most INSERTION_MAX elements use insertion
 * sort.
 *
 * No memory is allocated and every element move is a swap, so an element is
 * never held outside the array. The sort is not stable.
 */

#include <minmax.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define INSERTION_MAX 16
#define NINTHER_MIN 40

/* The comparator, either form. Keeping both behind one branch, rather than
 * wrapping qsort's comparator in a qsort_r-style trampoline, costs plain qsort
 * one indirect call per comparison instead of two.
 */
struct cmp {
    int (*plain)(const void *, const void *);
    int (*with_ctx)(const void *, const void *, void *);
    void *ctx;
};

static inline int cmp(const struct cmp *c, const void *a, const void *b)
{
    return c->with_ctx ? c->with_ctx(a, b, c->ctx) : c->plain(a, b);
}

/* Swap n bytes at a with n bytes at b. The ranges are identical or disjoint.
 * Words move through word-sized temporaries with memcpy, which is alias-safe
 * for any element type; the long path is taken when both pointers and n are
 * long-aligned, the 32-bit path (int and float arrays) when they are 4-aligned.
 */
static void swap(char *a, char *b, size_t n)
{
    uintptr_t align = (uintptr_t) a | (uintptr_t) b | n;

    if ((align & (sizeof(unsigned long) - 1)) == 0) {
        unsigned long ta, tb;

        for (; n != 0; n -= sizeof ta, a += sizeof ta, b += sizeof tb) {
            memcpy(&ta, a, sizeof ta);
            memcpy(&tb, b, sizeof tb);
            memcpy(a, &tb, sizeof tb);
            memcpy(b, &ta, sizeof ta);
        }
    } else if ((align & (sizeof(uint32_t) - 1)) == 0) {
        uint32_t ta, tb;

        for (; n != 0; n -= sizeof ta, a += sizeof ta, b += sizeof tb) {
            memcpy(&ta, a, sizeof ta);
            memcpy(&tb, b, sizeof tb);
            memcpy(a, &tb, sizeof tb);
            memcpy(b, &ta, sizeof ta);
        }
    } else {
        for (; n != 0; n--, a++, b++) {
            char t = *a;

            *a = *b;
            *b = t;
        }
    }
}

static char *med3(char *a, char *b, char *c, const struct cmp *f)
{
    if (cmp(f, a, b) < 0) {
        if (cmp(f, b, c) < 0)
            return b;
        return cmp(f, a, c) < 0 ? c : a;
    }
    if (cmp(f, b, c) > 0)
        return b;
    return cmp(f, a, c) < 0 ? a : c;
}

static void insertion_sort(char *a, size_t n, size_t es, const struct cmp *f)
{
    char *end = a + n * es;

    for (char *i = a + es; i < end; i += es)
        for (char *j = i; j > a && cmp(f, j - es, j) > 0; j -= es)
            swap(j - es, j, es);
}

/* Bottom-up heapsort (Floyd, Wegener). A sift walks to a leaf along the larger
 * children, climbs back to where the displaced element belongs, then rotates
 * the path with swaps. That costs about log2(n) comparisons per sift instead of
 * the 2*log2(n) of the textbook version.
 */
static void sift(char *a, size_t root, size_t n, size_t es, const struct cmp *f)
{
    size_t j = root;
    size_t child;
    unsigned depth = 0;

    /* Descend to a leaf through the larger child at each level. */
    while ((child = 2 * j + 1) < n) {
        if (child + 1 < n && cmp(f, a + child * es, a + (child + 1) * es) < 0)
            child++;
        j = child;
        depth++;
    }
    /* Climb back to the first node not smaller than the root's element. */
    while (j != root && cmp(f, a + root * es, a + j * es) > 0) {
        j = (j - 1) / 2;
        depth--;
    }

    /* Rotate the path root..j: every element moves up one level and the root's
     * element lands at j. The node s levels above j is ((j + 1) >> s) - 1, so
     * swapping each parent with its child on the way down carries the root's
     * element along.
     */
    for (size_t parent = root; depth-- > 0;) {
        size_t k = ((j + 1) >> depth) - 1;

        swap(a + parent * es, a + k * es, es);
        parent = k;
    }
}

static void heap_sort(char *a, size_t n, size_t es, const struct cmp *f)
{
    for (size_t i = n / 2; i-- > 0;)
        sift(a, i, n, es, f);
    while (n > 1) {
        n--;
        swap(a, a + n * es, es);
        sift(a, 0, n, es, f);
    }
}

static void introsort(char *a,
                      size_t n,
                      size_t es,
                      const struct cmp *f,
                      unsigned budget)
{
    while (n > INSERTION_MAX) {
        if (budget == 0) {
            heap_sort(a, n, es, f);
            return;
        }

        char *end = a + n * es;
        char *lo = a;
        char *mid = a + (n / 2) * es;
        char *hi = end - es;

        if (n > NINTHER_MIN) {
            size_t d = (n / 8) * es;

            lo = med3(lo, lo + d, lo + 2 * d, f);
            mid = med3(mid - d, mid, mid + d, f);
            hi = med3(hi - 2 * d, hi - d, hi, f);
        }
        swap(a, med3(lo, mid, hi, f), es);

        /* Bentley-McIlroy partition around the pivot at a[0]. Keys equal to the
         * pivot collect at both ends, [a+es, pa) and (pd, end); then [pa, pb) <
         * pivot and (pc, pd] > pivot.
         */
        char *pa = a + es, *pb = a + es;
        char *pc = end - es, *pd = pc;
        int r;

        for (;;) {
            while (pb <= pc && (r = cmp(f, pb, a)) <= 0) {
                if (r == 0) {
                    swap(pa, pb, es);
                    pa += es;
                }
                pb += es;
            }
            while (pb <= pc && (r = cmp(f, pc, a)) >= 0) {
                if (r == 0) {
                    swap(pc, pd, es);
                    pd -= es;
                }
                pc -= es;
            }
            if (pb > pc)
                break;
            swap(pb, pc, es);
            pb += es;
            pc -= es;
        }

        /* Move the equal keys from both ends into the middle. */
        size_t s = min_size((size_t) (pa - a), (size_t) (pb - pa));

        swap(a, pb - s, s);
        s = min_size((size_t) (pd - pc), (size_t) (end - pd) - es);
        swap(pb, end - s, s);

        size_t nl = (size_t) (pb - pa) / es; /* elements less than the pivot */
        size_t nr =
            (size_t) (pd - pc) / es; /* elements greater than the pivot */

        /* A level costs one unit of the budget. A split whose smaller side
         * holds under an eighth of what is left to sort (keys equal to the
         * pivot are done) does little work for its n comparisons, so it costs
         * two: an adversarial comparator then reaches heapsort after about
         * log2(n) levels instead of 2*log2(n), which roughly halves the
         * comparisons spent before the fallback. Balanced splits are
         * unaffected.
         */
        unsigned cost = min_size(nl, nr) < (nl + nr) / 8 ? 2 : 1;

        budget = budget > cost ? budget - cost : 0;

        /* Recurse into the smaller side, loop on the larger. A side of fewer
         * than two elements is already sorted and may start one past the end.
         */
        if (nl <= nr) {
            if (nl > 1)
                introsort(a, nl, es, f, budget);
            a = end - nr * es;
            n = nr;
        } else {
            if (nr > 1)
                introsort(end - nr * es, nr, es, f, budget);
            n = nl;
        }
    }
    if (n > 1)
        insertion_sort(a, n, es, f);
}

/* Both entry points call this directly, so an interposed qsort_r cannot change
 * how qsort behaves.
 */
static void sort(void *base, size_t n, size_t es, const struct cmp *f)
{
    unsigned budget = 0;

    if (n < 2 || es == 0 || n > SIZE_MAX / es)
        return;
    for (size_t m = n; m > 1; m >>= 1)
        budget += 2;
    introsort(base, n, es, f, budget);
}

void qsort_r(void *base,
             size_t n,
             size_t es,
             int (*compar)(const void *, const void *, void *),
             void *ctx)
{
    struct cmp f = {NULL, compar, ctx};

    sort(base, n, es, &f);
}

void qsort(void *base,
           size_t n,
           size_t es,
           int (*compar)(const void *, const void *))
{
    struct cmp f = {compar, NULL, NULL};

    sort(base, n, es, &f);
}
