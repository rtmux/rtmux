/*-
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Copyright (c) 1990, 1993
 * The Regents of the University of California. All rights reserved.
 */

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include "local.h"

int vsnprintf(char *__restrict str,
              size_t n,
              const char *__restrict fmt,
              va_list ap)
{
    size_t on;
    int ret;
    FILE f = FAKE_FILE;

    on = n;
    if (n != 0)
        n--;
    if (n > INT_MAX) {
        errno = EOVERFLOW;
        *str = '\0';
        return EOF;
    }
    f._flags = __SWR | __SSTR;
    f._bf._base = f._p = (unsigned char *) str;
    f._bf._size = f._w = (int) n;

    ret = __vfprintf(&f, fmt, ap);

    if (on > 0)
        *f._p = '\0';
    return ret;
}
