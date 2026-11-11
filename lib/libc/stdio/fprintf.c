/*-
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Copyright (c) 1990, 1993
 * The Regents of the University of California. All rights reserved.
 */

#include <stdio.h>
#include <stdarg.h>

int fprintf(FILE *__restrict fp, const char *__restrict fmt, ...)
{
    int ret;
    va_list ap;

    va_start(ap, fmt);
    ret = vfprintf(fp, fmt, ap);
    va_end(ap);
    return ret;
}
