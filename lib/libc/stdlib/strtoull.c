/* SPDX-License-Identifier: BSD-3-Clause */

#include <limits.h>
#include <errno.h>
#include <ctype.h>
#include <stdlib.h>

/* Convert a string to an unsigned long long integer.
 *
 * Assumes that the upper and lower case alphabets and digits are each
 * contiguous.
 */
unsigned long long strtoull(const char *__restrict nptr,
                            char **__restrict endptr,
                            int base)
{
    const char *s;
    unsigned long long acc;
    unsigned char c;
    unsigned long long cutoff;
    unsigned long long cutlim;
    int neg, any;

    /* See strtoq for comments as to the logic used. */
    s = nptr;
    do {
        c = *s++;
    } while (isspace((unsigned char) c));
    if (c == '-') {
        neg = 1;
        c = *s++;
    } else {
        neg = 0;
        if (c == '+')
            c = *s++;
    }
    if ((base == 0 || base == 16) && c == '0' && (*s == 'x' || *s == 'X') &&
        ((s[1] >= '0' && s[1] <= '9') || (s[1] >= 'A' && s[1] <= 'F') ||
         (s[1] >= 'a' && s[1] <= 'f'))) {
        c = s[1];
        s += 2;
        base = 16;
    }
    if ((base == 0 || base == 2) && c == '0' && (*s == 'b' || *s == 'B') &&
        (s[1] >= '0' && s[1] <= '1')) {
        c = s[1];
        s += 2;
        base = 2;
    }
    if (base == 0)
        base = c == '0' ? 8 : 10;
    acc = 0;
    any = 0;
    if (base < 2 || base > 36)
        goto noconv;

    cutoff = ULLONG_MAX / (unsigned long long) base;
    cutlim = ULLONG_MAX % (unsigned long long) base;
    for (;; c = *s++) {
        if (c >= '0' && c <= '9')
            c -= '0';
        else if (c >= 'A' && c <= 'Z')
            c -= 'A' - 10;
        else if (c >= 'a' && c <= 'z')
            c -= 'a' - 10;
        else
            break;
        if (c >= (unsigned char) base)
            break;
        if (any < 0 || acc > cutoff || (acc == cutoff && c > cutlim))
            any = -1;
        else {
            any = 1;
            acc *= (unsigned long long) base;
            acc += c;
        }
    }
    if (any < 0) {
        acc = ULLONG_MAX;
        errno = ERANGE;
    } else if (!any) {
    noconv:
        errno = EINVAL;
    } else if (neg)
        acc = -acc;
    if (endptr)
        *endptr = (char *) (any ? s - 1 : nptr);
    return acc;
}
