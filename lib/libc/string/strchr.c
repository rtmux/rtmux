/* SPDX-License-Identifier: MIT */

#include <string.h>

char *strchr(const char *s, int c)
{
    unsigned char ch = (unsigned char) c;

    for (;; s++) {
        if ((unsigned char) *s == ch)
            return (char *) s;
        if (!*s)
            return NULL;
    }
}
