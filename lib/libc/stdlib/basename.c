/* SPDX-License-Identifier: BSD-2-Clause */

#include <libgen.h>
#include <string.h>

char *(basename) (char *path)
{
    char *ptr;

    /* If path is a null pointer or points to an empty string, basename() shall
     * return a pointer to the string ".".
     */
    if (!path || *path == '\0')
        return __DECONST(char *, ".");

    /* Find end of last pathname component and null terminate it. */
    ptr = path + strlen(path);
    while (ptr > path + 1 && *(ptr - 1) == '/')
        --ptr;
    *ptr-- = '\0';

    /* Find beginning of last pathname component. */
    while (ptr > path && *(ptr - 1) != '/')
        --ptr;
    return ptr;
}
