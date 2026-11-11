/*-
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Copyright (c) 1990, 1993
 * The Regents of the University of California. All rights reserved.
 */

#include <stdio.h>
#include "local.h"

int fgetc(FILE *fp)
{
    int r;

    FLOCKFILE(fp);
    r = __sgetc(fp);
    FUNLOCKFILE(fp);
    return r;
}
