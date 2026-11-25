/* SPDX-License-Identifier: Apache-2.0 */
#ifndef LIBC_EXEC_PATH_DEFAULT_H
#define LIBC_EXEC_PATH_DEFAULT_H

#include <errno.h>
#include <stdlib.h>
#include <unistd.h>

/* confstr may consult roottask. Call outside the environment lock. */
static inline char *exec_path_default(void)
{
    errno = 0;
    size_t size = confstr(_CS_PATH, NULL, 0);
    while (size) {
        char *buf = malloc(size);
        if (!buf)
            return NULL;
        errno = 0;
        size_t need = confstr(_CS_PATH, buf, size);
        if (need && need <= size)
            return buf;
        int err = errno;
        free(buf);
        errno = err;
        size = need;
    }
    if (!errno)
        errno = ENOENT;
    return NULL;
}
#endif
