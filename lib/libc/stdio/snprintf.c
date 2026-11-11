/*-
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Copyright (c) 1990, 1993
 * The Regents of the University of California. All rights reserved.
 */

#include <limits.h>
#include <stdio.h>
#include <stdarg.h>

int snprintf(char *__restrict str, size_t n, const char *__restrict fmt, ...)
{
    int ret;
    va_list ap;

    va_start(ap, fmt);
    ret = vsnprintf(str, n, fmt, ap);
    va_end(ap);
    return ret;
}
