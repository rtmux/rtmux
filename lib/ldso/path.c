/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * Path helpers for ldso.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "ldso-path.h"

void _ldso_error(const char *, ...);
const char *ldso_strerror(int errnum);
char *realpath(const char *path, char *resolved);

const char *ldso_basename(const char *name)
{
    const char *p = strrchr(name, '/');

    return p ? p + 1 : name;
}

int ldso_dirname(const char *path, char *bname)
{
    const char *endp;

    if (!path || *path == '\0') {
        bname[0] = '.';
        bname[1] = '\0';
        return 0;
    }

    endp = path + strlen(path) - 1;
    while (endp > path && *endp == '/')
        endp--;

    while (endp > path && *endp != '/')
        endp--;

    if (endp == path) {
        bname[0] = *endp == '/' ? '/' : '.';
        bname[1] = '\0';
        return 0;
    }

    do {
        endp--;
    } while (endp > path && *endp == '/');

    if (endp - path + 2 > PATH_MAX) {
        _ldso_error("Filename is too long: %s", path);
        return -1;
    }

    strncpy(bname, path, (size_t) (endp - path + 1));
    bname[endp - path + 1] = '\0';
    return 0;
}

int ldso_dirname_abs(const char *path, char *base)
{
    char *last;

    if (!realpath(path, base)) {
        _ldso_error("realpath \"%s\" failed (%s)", path, ldso_strerror(errno));
        return -1;
    }

    last = strrchr(base, '/');
    if (!last) {
        _ldso_error("non-abs result from realpath \"%s\"", path);
        return -1;
    }

    if (last != base)
        *last = '\0';
    return 0;
}
