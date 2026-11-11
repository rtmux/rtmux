/*-
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Copyright (c) 1990, 1993
 * The Regents of the University of California. All rights reserved.
 */

#include <stdio.h>
#include <string.h>
#include "local.h"
#include "fvwrite.h"

int fputs(const char *__restrict s, FILE *__restrict fp)
{
    struct __suio uio;
    struct __siov iov;
    int ret;

    iov.iov_base = (void *) s;
    iov.iov_len = strlen(s);
    uio.uio_iov = &iov;
    uio.uio_iovcnt = 1;
    uio.uio_resid = iov.iov_len;

    ORIENT(fp, -1);

    FLOCKFILE(fp);
    ret = __sfvwrite(fp, &uio);
    FUNLOCKFILE(fp);
    return ret;
}
