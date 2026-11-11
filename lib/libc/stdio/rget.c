/*-
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Copyright (c) 1990, 1993
 * The Regents of the University of California. All rights reserved.
 */

#include <stdio.h>
#include "local.h"

/* Handle getc() when the buffer ran out: Refill, then return the first
 * character in the newly-filled buffer.
 */
int __srget(FILE *fp)
{
    if (__srefill(fp) == 0) {
        fp->_r--;
        return *fp->_p++;
    }
    return EOF;
}
