/* SPDX-License-Identifier: BSD-3-Clause */

#include <stdlib.h> /* ldiv_t */

ldiv_t ldiv(long num, long denom)
{
    ldiv_t r;

    /* see div.c for comments */

    r.quot = num / denom;
    r.rem = num % denom;
#if !defined(__STDC_VERSION__) || (__STDC_VERSION__ < 199901L)
    if (num >= 0 && r.rem < 0) {
        r.quot++;
        r.rem -= denom;
    }
#endif
    return r;
}
