/*-
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Copyright (c) 1990, 1993
 * The Regents of the University of California. All rights reserved.
 *
 * Adapted for RTMux: removed O_VERIFY.
 */

#include <sys/types.h>
#include <fcntl.h>
#include <stdio.h>
#include <errno.h>
#include "local.h"

/* Return the (stdio) flags for a given mode. Store the flags to be passed to an
 * open() syscall through *optr.
 * Return 0 on error.
 */
int __sflags(const char *mode, int *optr)
{
    int ret, m, o, known;

    switch (*mode++) {
    case 'r':
        ret = __SRD;
        m = O_RDONLY;
        o = 0;
        break;
    case 'w':
        ret = __SWR;
        m = O_WRONLY;
        o = O_CREAT | O_TRUNC;
        break;
    case 'a':
        ret = __SWR;
        m = O_WRONLY;
        o = O_CREAT | O_APPEND;
        break;
    default:
        errno = EINVAL;
        return 0;
    }

    do {
        known = 1;
        switch (*mode++) {
        case 'b':
            break;
        case '+':
            ret = __SRW;
            m = O_RDWR;
            break;
        case 'x':
            o |= O_EXCL;
            break;
        case 'e':
            o |= O_CLOEXEC;
            break;
        default:
            known = 0;
            break;
        }
    } while (known);

    if ((o & O_EXCL) != 0 && m == O_RDONLY) {
        errno = EINVAL;
        return 0;
    }

    *optr = m | o;
    return ret;
}
