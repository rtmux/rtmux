/* SPDX-License-Identifier: BSD-3-Clause */

#include <string.h>

/* Copy src to dst, truncating or null-padding to always copy n bytes.
 * Return dst.
 */
char *strncpy(char *__restrict dst, const char *__restrict src, size_t n)
{
    if (n != 0) {
        char *d = dst;
        const char *s = src;

        do {
            if ((*d++ = *s++) == '\0') {
                /* NUL pad the remaining n-1 bytes */
                while (--n != 0)
                    *d++ = '\0';
                break;
            }
        } while (--n != 0);
    }
    return dst;
}
