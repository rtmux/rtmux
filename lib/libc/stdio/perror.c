/*-
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Copyright (c) 1990, 1993
 * The Regents of the University of California. All rights reserved.
 */

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include "local.h"
#include "fvwrite.h"

void perror(const char *s)
{
    struct __siov iov[4];
    struct __suio uio;
    int cnt = 0;

    if (s && *s != '\0') {
        iov[cnt].iov_base = (void *) s;
        iov[cnt].iov_len = strlen(s);
        cnt++;
        iov[cnt].iov_base = ": ";
        iov[cnt].iov_len = 2;
        cnt++;
    }
    iov[cnt].iov_base = strerror(errno);
    iov[cnt].iov_len = strlen(iov[cnt].iov_base);
    cnt++;
    iov[cnt].iov_base = "\n";
    iov[cnt].iov_len = 1;
    cnt++;

    uio.uio_iov = iov;
    uio.uio_iovcnt = cnt;
    uio.uio_resid = 0;
    for (int i = 0; i < cnt; i++)
        uio.uio_resid += iov[i].iov_len;

    (void) __sfvwrite(stderr, &uio);
}
