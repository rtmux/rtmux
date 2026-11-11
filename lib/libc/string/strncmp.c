/* SPDX-License-Identifier: BSD-3-Clause */

#include <string.h>

int strncmp(const char *s1, const char *s2, size_t n)
{
    if (n == 0)
        return 0;
    do {
        if (*s1 != *s2++)
            return *(const unsigned char *) s1 -
                   *(const unsigned char *) (s2 - 1);
        if (*s1++ == '\0')
            break;
    } while (--n != 0);
    return 0;
}
