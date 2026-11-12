/* SPDX-License-Identifier: BSD-3-Clause */

#include <sys/types.h>

typedef intptr_t word; /* "word" used for optimal copy speed */

#define wsize sizeof(word)
#define wmask (wsize - 1)

/* Copy a block of memory, handling overlap. This is the routine that actually
 * implements the portable versions of memcpy and memmove.
 */
#if !defined(MEMCOPY) && !defined(MEMMOVE)
#error Direct build unsupported
#endif

#include <string.h>

void *
#ifdef MEMCOPY
memcpy
#else
memmove
#endif
    (void *dst0, const void *src0, size_t length)
{
    char *dst = dst0;
    const char *src = src0;
    size_t t;

    if (length == 0 || dst == src) { /* nothing to do */
        goto done;
    }

    /* Macros: loop-t-times; and loop-t-times, t>0 */
#define TLOOP(s) \
    if (t)       \
    TLOOP1(s)
#define TLOOP1(s) \
    do {          \
        s;        \
    } while (--t)

    if ((unsigned long) dst < (unsigned long) src) {
        /* Copy forward. */
        t = (uintptr_t) src; /* only need low bits */
        if ((t | (uintptr_t) dst) & wmask) {
            /* Try to align operands. This cannot be done unless the low bits
             * match.
             */
            if ((t ^ (uintptr_t) dst) & wmask || length < wsize)
                t = length;
            else
                t = wsize - (t & wmask);
            length -= t;
            TLOOP1(*dst++ = *src++);
        }
        /* Copy whole words, then mop up any trailing bytes. */
        t = length / wsize;
        TLOOP(*(word *) (void *) dst = *(const word *) (const void *) src;
              src += wsize; dst += wsize);
        t = length & wmask;
        TLOOP(*dst++ = *src++);
    } else {
        /* Copy backwards. Otherwise essentially the same. Alignment works as
         * before, except that it takes (t&wmask) bytes to align, not
         * wsize-(t&wmask).
         */
        src += length;
        dst += length;
        t = (uintptr_t) src;
        if ((t | (uintptr_t) dst) & wmask) {
            if ((t ^ (uintptr_t) dst) & wmask || length <= wsize)
                t = length;
            else
                t &= wmask;
            length -= t;
            TLOOP1(*--dst = *--src);
        }
        t = length / wsize;
        TLOOP(src -= wsize; dst -= wsize;
              *(word *) (void *) dst = *(const word *) (const void *) src);
        t = length & wmask;
        TLOOP(*--dst = *--src);
    }
done:
    return dst0;
}
