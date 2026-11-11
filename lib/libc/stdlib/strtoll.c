/* SPDX-License-Identifier: BSD-3-Clause */

#include <limits.h>
#include <errno.h>
#include <ctype.h>
#include <stdlib.h>

/* Convert a string to a long long integer.
 *
 * Assumes that the upper and lower case alphabets and digits are each
 * contiguous.
 */
long long strtoll(const char *__restrict nptr,
                  char **__restrict endptr,
                  int base)
{
    const char *s;
    unsigned long long acc;
    unsigned char c;
    unsigned long long cutoff;
    unsigned long long cutlim;
    int neg, any;

    /* Skip white space and pick up leading +/- sign if any. If base is 0, allow
     * 0b for binary, 0x for hex, and 0 for octal, else assume decimal; if base
     * is already 2, allow 0b; if base is already 16, allow 0x.
     */
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

    /* Compute the cutoff value between legal numbers and illegal numbers. That
     * is the largest legal value, divided by the base. An input number that is
     * greater than this value, if followed by a legal input character, is too
     * big. One that is equal to this value may be valid or not; the limit
     * between valid and invalid numbers is then based on the last digit. For
     * instance, if the range for quads is
     * [-9223372036854775808..9223372036854775807] and the input base is 10,
     * cutoff will be set to 922337203685477580 and cutlim to either 7 (neg==0)
     * or 8 (neg==1), meaning that if we have accumulated a value >
     * 922337203685477580, or equal but the next digit is > 7 (or 8), the number
     * is too big, and we will return a range error.
     *
     * Set 'any' if any "digits' consumed; make it negative to indicate
     * overflow.
     */
    cutoff = neg ? (unsigned long long) -(LLONG_MIN + LLONG_MAX) + LLONG_MAX
                 : LLONG_MAX;
    cutlim = cutoff % (unsigned long long) base;
    cutoff /= (unsigned long long) base;
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
        acc = neg ? (unsigned long long) LLONG_MIN
                  : (unsigned long long) LLONG_MAX;
        errno = ERANGE;
    } else if (!any) {
    noconv:
        errno = EINVAL;
    } else if (neg)
        acc = -acc;
    if (endptr)
        *endptr = (char *) (any ? s - 1 : nptr);
    return (long long) acc;
}
