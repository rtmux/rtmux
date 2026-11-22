/*-
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Copyright (c) 1990, 1993
 * The Regents of the University of California. All rights reserved.
 */

#include <limits.h>
#include <stdio.h>
#include <stdarg.h>

int vsprintf(char *__restrict str, const char *__restrict fmt, va_list ap)
{
    /* The string sink accepts INT_MAX bytes of output and the final NUL. */
    return vsnprintf(str, (size_t) INT_MAX + 1, fmt, ap);
}

int sprintf(char *__restrict str, const char *__restrict fmt, ...)
{
    int ret;
    va_list ap;

    va_start(ap, fmt);
    ret = vsprintf(str, fmt, ap);
    va_end(ap);
    return ret;
}
