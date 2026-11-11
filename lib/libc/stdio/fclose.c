/*-
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Copyright (c) 1990, 1993
 * The Regents of the University of California. All rights reserved.
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include "local.h"

int fclose(FILE *fp)
{
    int r;

    /* Lock the teardown so it does not race a concurrent op, and re-check the
     * open flag under the lock: two threads racing fclose(fp) must not both
     * tear down (which would double-close the fd). The per-FILE mutex itself is
     * left attached to the (bounded pool) slot for reuse, not destroyed --
     * using a closed FILE is undefined, so there is nothing to unblock.
     */
    FLOCKFILE(fp);
    if (fp->_flags == 0) {
        FUNLOCKFILE(fp);
        errno = EBADF;
        return EOF;
    }

    r = fp->_flags & __SWR ? __sflush(fp) : 0;
    if (fp->_close && (*fp->_close)(fp->_cookie) < 0)
        r = EOF;
    if (fp->_flags & __SMBF)
        free((char *) fp->_bf._base);
    if (HASUB(fp))
        FREEUB(fp);
    fp->_file = -1;
    fp->_r = fp->_w = 0;
    fp->_flags = 0;
    FUNLOCKFILE(fp);
    return r;
}
