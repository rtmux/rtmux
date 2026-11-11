/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ftype.h>
#include <sys/mman.h>
#include <unistd.h>

#include "connect.h"

#define SHM_PREFIX "/dev/shmem"

int shm_open(const char *path, int oflag, mode_t mode)
{
    if (!path || path[0] != '/') {
        errno = EINVAL;
        return -1;
    }
    return _connect_object(path, SHM_PREFIX, mode, oflag, _FTYPE_SHMEM, 0, 0,
                           NULL);
}

int shm_unlink(const char *path)
{
    char *full;

    if (!path || path[0] != '/') {
        errno = EINVAL;
        return -1;
    }
    if (strchr(path + 1, '/')) {
        errno = EINVAL;
        return -1;
    }
    full = malloc(sizeof(SHM_PREFIX) + strlen(path));
    if (!full) {
        errno = ENOMEM;
        return -1;
    }
    (void) strcpy(full, SHM_PREFIX);
    (void) strcat(full, path);
    {
        int ret = unlink(full);
        free(full);
        return ret;
    }
}
