/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stddef.h>
#include <stdint.h>

/* Included for the declarations, not for anything it defines: without it the
 * compiler never sees that these definitions must match <string.h>, and they
 * silently drifted to int/unsigned counters. Frama-C's whole-program link stage
 * caught it; keep the include so the compiler catches the next one.
 */
#include <string.h>

void *memset(void *dst, int c, size_t n)
{
    unsigned char *d = (unsigned char *) dst;
    unsigned char byte = (unsigned char) c;

    /* Align to 8-byte boundary */
    while (n > 0 && ((uintptr_t) d & 7)) {
        *d++ = byte;
        n--;
    }

    /* Fill 8 bytes at a time */
    if (n >= 8) {
        uint64_t word = byte;
        word |= word << 8;
        word |= word << 16;
        word |= word << 32;

        uint64_t *w = (uint64_t *) d;
        while (n >= 8) {
            *w++ = word;
            n -= 8;
        }
        d = (unsigned char *) w;
    }

    /* Remaining bytes */
    while (n-- > 0)
        *d++ = byte;

    return dst;
}

int memcmp(const void *v1, const void *v2, size_t n)
{
    const unsigned char *s1 = v1, *s2 = v2;

    while (n-- > 0) {
        if (*s1 != *s2)
            return *s1 - *s2;
        s1++;
        s2++;
    }

    return 0;
}

void *memmove(void *dst, const void *src, size_t n)
{
    const char *s = src;
    char *d = dst;

    if (s < d && s + n > d) {
        s += n;
        d += n;
        while (n-- > 0)
            *--d = *--s;
    } else {
        while (n-- > 0)
            *d++ = *s++;
    }

    return dst;
}

void *memcpy(void *dst, const void *src, size_t n)
{
    return memmove(dst, src, n);
}

void *memccpy(void *restrict t, const void *restrict f, int c, size_t n)
{
    if (n) {
        unsigned char *tp = t;
        const unsigned char *fp = f;
        unsigned char uc = (unsigned char) c;
        do {
            if ((*tp++ = *fp++) == uc)
                return tp;
        } while (--n != 0);
    }
    return 0;
}

int strncmp(const char *p, const char *q, size_t n)
{
    while (n > 0 && *p && *p == *q)
        n--, p++, q++;
    if (n == 0)
        return 0;
    return (unsigned char) *p - (unsigned char) *q;
}

char *strncpy(char *s, const char *t, size_t n)
{
    char *os;

    os = s;

    /* Not the "while (n-- > 0)" idiom: on size_t the count wraps to SIZE_MAX
     * when the copy loop drains it, and the pad loop below would then run
     * forever. Decrement inside the body instead.
     */
    while (n > 0) {
        n--;
        if ((*s++ = *t++) == 0)
            break;
    }
    while (n > 0) {
        *s++ = 0;
        n--;
    }
    return os;
}

size_t strlen(const char *s)
{
    size_t n;

    for (n = 0; s[n]; n++)
        ;
    return n;
}

int strcmp(const char *p, const char *q)
{
    while (*p && *p == *q)
        p++, q++;
    return (unsigned char) *p - (unsigned char) *q;
}

char *strcpy(char *s, const char *t)
{
    char *os;

    os = s;
    while ((*s++ = *t++) != 0)
        ;
    return os;
}
