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

int puts(const char *s)
{
    int retval;
    size_t c = strlen(s);
    struct __suio uio;
    struct __siov iov[2];

    iov[0].iov_base = (void *) s;
    iov[0].iov_len = c;
    iov[1].iov_base = "\n";
    iov[1].iov_len = 1;
    uio.uio_iov = &iov[0];
    uio.uio_iovcnt = 2;
    uio.uio_resid = c + 1;

    ORIENT(stdout, -1);

    FLOCKFILE(stdout);
    retval = __sfvwrite(stdout, &uio);
    FUNLOCKFILE(stdout);
    return retval ? EOF : '\n';
}
