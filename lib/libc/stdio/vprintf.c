/*-
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Copyright (c) 1990, 1993
 * The Regents of the University of California. All rights reserved.
 */

#include <stdio.h>
#include <stdarg.h>

int vprintf(const char *__restrict fmt, va_list ap)
{
    return vfprintf(stdout, fmt, ap);
}
