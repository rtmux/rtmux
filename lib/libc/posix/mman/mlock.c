/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <sys/mman.h>

int mlock(const void *addr, size_t len)
{
    if (!addr && len != 0) {
        errno = ENOMEM;
        return -1;
    }
    return 0;
}

int munlock(const void *addr, size_t len)
{
    if (!addr && len != 0) {
        errno = ENOMEM;
        return -1;
    }
    return 0;
}

int mlockall(int flags)
{
    if (flags & ~(MCL_CURRENT | MCL_FUTURE)) {
        errno = EINVAL;
        return -1;
    }
    return 0;
}

int munlockall(void)
{
    return 0;
}
