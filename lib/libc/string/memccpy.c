/* SPDX-License-Identifier: BSD-3-Clause */

#include <string.h>

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
