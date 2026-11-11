/* SPDX-License-Identifier: BSD-3-Clause */

#include <string.h>

/* Find the first occurrence in s1 of a character in s2 (excluding NUL). */
char *strpbrk(const char *s1, const char *s2)
{
    const char *scanp;
    int c, sc;

    while ((c = *s1++) != 0) {
        for (scanp = s2; (sc = *scanp++) != '\0';)
            if (sc == c)
                return (char *) (s1 - 1);
    }
    return NULL;
}
