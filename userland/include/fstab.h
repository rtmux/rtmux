/*
 * fstab — /etc/fstab definitions
 *
 * Based on FreeBSD fstab.h, adapted for RTMux.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _FSTAB_H_
#define _FSTAB_H_

#define FSTAB_RW "rw"
#define FSTAB_RO "ro"
#define FSTAB_OCB "allservers"
#define FSTAB_IMPLIED "implied"
#define FSTAB_XX "xx"

#define NULL_ENTRY "-"
#define _PATH_FSTAB "/etc/fstab"

struct fstab {
    char *fs_spec;       /* block special device name */
    char *fs_file;       /* filesystem path prefix */
    char *fs_vfstype;    /* type of filesystem */
    char *fs_mntops;     /* comma separated mount options */
    char *fs_type;       /* rw, ro, xx */
    unsigned init_flags; /* flags to OR in */
    unsigned init_mask;  /* flags to AND out */
};

struct fstab *getfsent(void);
struct fstab *getfsspec(const char *name);
struct fstab *getfsfile(const char *name);
int setfsent(void);
void endfsent(void);

#endif
