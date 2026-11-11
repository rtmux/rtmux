/* SPDX-License-Identifier: BSD-3-Clause */

#include <string.h>

/* Concatenate src on the end of dst. At most strlen(dst)+n+1 bytes are written
 * at dst (at most n+1 bytes being appended).
 *
 * Return dst.
 */
char *strncat(char *__restrict dst, const char *__restrict src, size_t n)
{
    if (n != 0) {
        char *d = dst;
        const char *s = src;

        while (*d != 0)
            d++;
        do {
            if ((*d = *s++) == 0)
                break;
            d++;
        } while (--n != 0);
        *d = 0;
    }
    return dst;
}
