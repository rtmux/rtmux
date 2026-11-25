/* SPDX-License-Identifier: Apache-2.0 */
#ifndef RTMUX_EXEC_PATH_H
#define RTMUX_EXEC_PATH_H

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <unistd.h>

/* Consume exactly one PATH component, including leading and trailing empty
 * components. NULL ends the iteration. Resolve relative entries against the
 * caller now when subsequent spawn actions may change directory.
 */
static inline int exec_path_candidate(const char **list,
                                      const char *file,
                                      char *out,
                                      size_t size,
                                      bool absolute)
{
    const char *dir = *list, *colon = strchr(dir, ':');
    size_t len = colon ? (size_t) (colon - dir) : strlen(dir);
    size_t used = 0, name = strlen(file);
    *list = colon ? colon + 1 : NULL;
    if (absolute && (!len || dir[0] != '/')) {
        int retries = 5;
        while (!getcwd(out, size)) {
            if (errno != EAGAIN || --retries == 0)
                return errno;
        }
        used = strlen(out);
        if (used && out[used - 1] != '/') {
            if (used >= size - 1)
                return ENAMETOOLONG;
            out[used++] = '/';
        }
    }
    if (len >= size - used)
        return ENAMETOOLONG;
    memcpy(out + used, dir, len);
    used += len;
    if (used && out[used - 1] != '/') {
        if (used >= size - 1)
            return ENAMETOOLONG;
        out[used++] = '/';
    }
    if (name >= size - used)
        return ENAMETOOLONG;
    memcpy(out + used, file, name + 1);
    return 0;
}

static inline bool exec_path_continue(int err, int *saved)
{
    if (err == EACCES || (err == ENAMETOOLONG && *saved != EACCES))
        *saved = err;
    return err == ENOENT || err == ENOTDIR || err == EACCES ||
           err == ENAMETOOLONG;
}
#endif
