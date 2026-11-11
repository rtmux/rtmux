/* SPDX-License-Identifier: BSD-3-Clause */

#include <stddef.h>
#include <stdlib.h>

#define COMPAR(x, y) compar(x, y)

/* Perform a binary search.
 *
 * The code below is a bit sneaky. After a comparison fails, we divide the work
 * in half by moving either left or right. If lim is odd, moving left simply
 * involves halving lim: e.g., when lim is 5 we look at item 2, so we change lim
 * to 2 so that we will look at items 0 & 1. If lim is even, the same applies.
 * If lim is odd, moving right again involves halving lim, this time moving the
 * base up one item past p: e.g., when lim is 5 we change base to item 3 and
 * make lim 2 so that we will look at items 3 and 4. If lim is even, however, we
 * have to shrink it by one before halving: e.g., when lim is 4, we still looked
 * at item 2, so we have to make lim 3, then halve, obtaining 1, so that we will
 * only look at item 3.
 */
void *bsearch(const void *key,
              const void *base0,
              size_t nmemb,
              size_t size,
              int (*compar)(const void *, const void *))
{
    const char *base = base0;
    size_t lim;
    int cmp;
    const void *p;

    for (lim = nmemb; lim != 0; lim >>= 1) {
        p = base + (lim >> 1) * size;
        cmp = COMPAR(key, p);
        if (cmp == 0)
            return (void *) p;
        if (cmp > 0) { /* key > p: move right */
            base = (char *) p + size;
            lim--;
        } /* else move left */
    }
    return NULL;
}
