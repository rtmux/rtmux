/*-
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Copyright (c) 1990, 1993
 * The Regents of the University of California. All rights reserved.
 */

#include <stdio.h>
#include <errno.h>
#include <stdint.h>
#include "local.h"
#include "fvwrite.h"

size_t fwrite(const void *__restrict buf,
              size_t size,
              size_t count,
              FILE *__restrict fp)
{
    size_t n, ret;
    struct __suio uio;
    struct __siov iov;

    if (size != 0 && count > SIZE_MAX / size) {
        errno = EOVERFLOW;
        FLOCKFILE(fp);
        fp->_flags |= __SERR;
        FUNLOCKFILE(fp);
        return 0;
    }
    if ((n = count * size) == 0)
        return 0;

    iov.iov_base = (void *) buf;
    iov.iov_len = n;
    uio.uio_iov = &iov;
    uio.uio_iovcnt = 1;
    uio.uio_resid = n;

    ORIENT(fp, -1);

    FLOCKFILE(fp);
    ret = __sfvwrite(fp, &uio) != 0 ? (n - uio.uio_resid) / size : count;
    FUNLOCKFILE(fp);
    return ret;
}
