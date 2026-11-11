/*
 * fstab — /etc/fstab parser
 *
 * Based on FreeBSD fstab.c, adapted for RTMux.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <sys/types.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>

#include "fstab.h"

static FILE *_fs_fp;
static struct fstab _fs_fstab;

#define MAXLINELENGTH 1024

static int fstabscan(void)
{
    char *cp;
    static char line[MAXLINELENGTH];
    char subline[MAXLINELENGTH];
    int typexx;
    char *lastp;
    static const char ws[] = " \t\n";

    for (;;) {
        size_t llen;

        if (!(cp = fgets(line, sizeof line, _fs_fp)))
            return 0;

        /* If the line filled the buffer without a newline, discard the rest of
         * the physical line so we don't parse its tail as a new entry.
         */
        llen = strlen(cp);
        if (llen == sizeof(line) - 1 && cp[llen - 1] != '\n') {
            int ch;

            while ((ch = fgetc(_fs_fp)) != EOF && ch != '\n')
                ;
            errno = ENAMETOOLONG;
            continue;
        }

        _fs_fstab.fs_spec = strtok_r(cp, ws, &lastp);
        if (!_fs_fstab.fs_spec || *_fs_fstab.fs_spec == '#')
            continue;
        _fs_fstab.fs_file = strtok_r(NULL, ws, &lastp);
        _fs_fstab.fs_vfstype = strtok_r(NULL, ws, &lastp);
        _fs_fstab.fs_mntops = strtok_r(NULL, ws, &lastp);
        if (!_fs_fstab.fs_mntops)
            goto bad;

        _fs_fstab.init_flags = 0;
        _fs_fstab.init_mask = _MFLAG_OCB;
        _fs_fstab.fs_type = NULL;

        strncpy(subline, _fs_fstab.fs_mntops, sizeof subline - 1);
        subline[sizeof subline - 1] = '\0';
        for (typexx = 0, cp = strtok_r(subline, ",", &lastp); cp;
             cp = strtok_r(NULL, ",", &lastp)) {
            if (!strcmp(cp, FSTAB_OCB)) {
                _fs_fstab.init_flags |= _MFLAG_OCB;
                _fs_fstab.init_mask &= ~_MFLAG_OCB;
            } else if (!strcmp(cp, FSTAB_IMPLIED)) {
                _fs_fstab.init_flags |= _MOUNT_IMPLIED;
                _fs_fstab.init_mask &= ~_MOUNT_IMPLIED;
            } else if (!strcmp(cp, FSTAB_RW)) {
                _fs_fstab.fs_type = FSTAB_RW;
            } else if (!strcmp(cp, FSTAB_RO)) {
                _fs_fstab.fs_type = FSTAB_RO;
            } else if (!strcmp(cp, FSTAB_XX)) {
                _fs_fstab.fs_type = FSTAB_XX;
                typexx++;
            }
        }
        if (typexx)
            continue;
        if (_fs_fstab.fs_type)
            return 1;

    bad:
        errno = ENOENT;
    }
}

struct fstab *getfsent(void)
{
    if ((!_fs_fp && !setfsent()) || !fstabscan())
        return NULL;
    return &_fs_fstab;
}

struct fstab *getfsspec(const char *name)
{
    if (setfsent())
        while (fstabscan())
            if (!strcmp(_fs_fstab.fs_spec, name))
                return &_fs_fstab;
    return NULL;
}

struct fstab *getfsfile(const char *name)
{
    if (setfsent())
        while (fstabscan())
            if (!strcmp(_fs_fstab.fs_file, name))
                return &_fs_fstab;
    return NULL;
}

int setfsent(void)
{
    if (_fs_fp) {
        fseek(_fs_fp, 0, SEEK_SET);
        return 1;
    }
    /* ipc-send: utils -> roottask-fsio,roottask-core,romfs,lkl-fsd,lkl-netd,dev-nvme,dev-serial,pipe,logger-ring */
    if ((_fs_fp = fopen(_PATH_FSTAB, "r")))
        return 1;
    return 0;
}

void endfsent(void)
{
    if (_fs_fp) {
        fclose(_fs_fp);
        _fs_fp = NULL;
    }
}
