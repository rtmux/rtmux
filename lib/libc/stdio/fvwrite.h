/*-
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Copyright (c) 1990, 1993
 * The Regents of the University of California. All rights reserved.
 */

/* I/O descriptors for __sfvwrite(). */
#include <sys/cdefs.h>

struct __siov {
    void *iov_base;
    size_t iov_len;
};

struct __suio {
    struct __siov *uio_iov;
    int uio_iovcnt;
    size_t uio_resid;
};

extern int __sfvwrite(FILE *, struct __suio *) __hidden;
