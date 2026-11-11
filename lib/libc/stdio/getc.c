/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include "local.h"

int getc(FILE *fp)
{
    int r;

    FLOCKFILE(fp);
    r = __sgetc(fp);
    FUNLOCKFILE(fp);
    return r;
}
