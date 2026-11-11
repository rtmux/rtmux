/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * Small parsers for ldso startup options
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <limits.h>

#include "parse.h"

int ldso_parse_decimal_int(const char *str)
{
    int n = 0;

    if (!str || !*str)
        return -1;

    for (const unsigned char *p = (const unsigned char *) str; *p; p++) {
        if (*p < '0' || *p > '9')
            return -1;

        int digit = *p - '0';
        if (n > (INT_MAX - digit) / 10)
            return -1;
        n = n * 10 + digit;
    }

    return n;
}
