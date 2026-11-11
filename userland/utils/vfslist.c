/*
 * vfslist — VFS type list management
 *
 * Based on FreeBSD vfslist.c, adapted for RTMux.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <stdlib.h>
#include <string.h>

static int skipvfs;

int checkvfsname(const char *vfsname, const char **vfslist)
{
    if (!vfslist)
        return 0;
    while (*vfslist) {
        if (!strcmp(vfsname, *vfslist))
            return skipvfs;
        ++vfslist;
    }
    return !skipvfs;
}

const char **makevfslist(char *fslist)
{
    const char **av;
    int i;
    char *nextcp;

    if (!fslist)
        return NULL;
    if (fslist[0] == 'n' && fslist[1] == 'o') {
        fslist += 2;
        skipvfs = 1;
    }
    for (i = 0, nextcp = fslist; *nextcp; nextcp++)
        if (*nextcp == ',')
            i++;
    av = malloc((size_t) (i + 2) * sizeof(char *));
    if (!av)
        return NULL;
    nextcp = fslist;
    i = 0;
    av[i++] = nextcp;
    while ((nextcp = strchr(nextcp, ','))) {
        *nextcp++ = '\0';
        av[i++] = nextcp;
    }
    av[i++] = NULL;
    return av;
}
