/* SPDX-License-Identifier: BSD-3-Clause */

#include <sys/types.h>

#include <limits.h>

#define wsize sizeof(u_long)
#define wmask (wsize - 1)

#include <string.h>

#define RETURN return (dst0)
#define VAL c0
#define WIDEVAL c

void *memset(void *dst0, int c0, size_t length)
{
    size_t t;
    u_long c;
    u_char *dst;

    dst = dst0;

    /* If not enough words, just fill bytes. A length >= 2 words guarantees that
     * at least one of them is "complete' after any necessary alignment. For
     * instance:
     *
     *  |-----------|-----------|-----------|
     *  |00|01|02|03|04|05|06|07|08|09|0A|00|
     *            ^---------------------^
     *       dst         dst+length-1
     *
     * but we use a minimum of 3 here since the overhead of the code to do word
     * writes is substantial.
     *
     * TODO: This threshold might not be sensible for 64-bit u_long. We should
     * benchmark and revisit this decision.
     */
    if (length < 3 * wsize) {
        while (length != 0) {
            *dst++ = (u_char) VAL;
            --length;
        }
        RETURN;
    }
    if ((c = (u_char) c0) != 0) { /* Fill the word. */
        c = (c << 8) | c;         /* u_long is 16 bits. */
#if ULONG_MAX > 0xffff
        c = (c << 16) | c; /* u_long is 32 bits. */
#endif
#if ULONG_MAX > 0xffffffff
        c = (c << 32) | c; /* u_long is 64 bits. */
#endif
    }
    /* Align destination by filling in bytes. */
    if ((t = (long) ((uintptr_t) dst & wmask)) != 0) {
        t = wsize - t;
        length -= t;
        do {
            *dst++ = (u_char) VAL;
        } while (--t != 0);
    }

    /* Fill words.  Length was >= 2*words so we know t >= 1 here. */
    t = length / wsize;
    do {
        *(u_long *) (void *) dst = WIDEVAL;
        dst += wsize;
    } while (--t != 0);

    /* Mop up trailing bytes, if any. */
    t = length & wmask;
    if (t != 0)
        do {
            *dst++ = (u_char) VAL;
        } while (--t != 0);
    RETURN;
}
