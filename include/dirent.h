/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* dirent.h Portable directory entry definitions */

#ifndef _DIRENT_H_INCLUDED
#define _DIRENT_H_INCLUDED

#ifndef __PLATFORM_H_INCLUDED
#include <sys/platform.h>
#endif

#ifndef __STAT_H_INCLUDED
#include <sys/stat.h>
#endif

struct dirent {
    ino_t d_ino; /* File serial number.                  */
    off_t d_offset;
    uint16_t d_reclen;
    uint16_t d_namelen;
    char d_name[];
};

enum dirent_extra_type { _DTYPE_NONE, _DTYPE_STAT, _DTYPE_LSTAT };

struct dirent_extra {
    uint16_t d_datalen; /* data size (without header) */
    uint16_t d_type;
    uint32_t d_reserved;
    /* unsigned char    d_data[d_datalen] */
};

struct dirent_extra_stat {
    uint16_t d_datalen; /* sizeof(struct stat) */
    uint16_t d_type;
    uint32_t d_reserved;
    struct stat d_stat;
};


/* The following macros can be used to extract the data. e.g. for(extra =
 * _DEXTRA_FIRST(dirent); _DEXTRA_VALID(extra, dirent); extra =
 * _DEXTRA_NEXT(extra)) { switch(extra->d_type) {
 */
#define _DEXTRA_FIRST(dirent)                                   \
    ((struct dirent_extra *) ((uintptr_t) (dirent) +            \
                              ((((uintptr_t) (dirent)->d_name - \
                                 (uintptr_t) (dirent)) +        \
                                (dirent)->d_namelen + 1 + 7) &  \
                               ~(uintptr_t) 7)))
#define _DEXTRA_VALID(extra, dirent)                                   \
    ((intptr_t) (((uintptr_t) (extra) - (uintptr_t) (dirent)) +        \
                 sizeof(struct dirent_extra)) <= (dirent)->d_reclen && \
     (intptr_t) (((uintptr_t) (extra) - (uintptr_t) (dirent)) +        \
                 sizeof(struct dirent_extra) +                         \
                 ((struct dirent_extra *) (extra))->d_datalen) <=      \
         (dirent)->d_reclen)
#define _DEXTRA_NEXT(last)                                                     \
    ((struct dirent_extra *) (((uintptr_t) (last) +                            \
                               sizeof(struct dirent_extra) +                   \
                               (((struct dirent_extra *) (last))->d_datalen) + \
                               7) &                                            \
                              ~(uintptr_t) 7))


typedef struct _dir DIR;


__BEGIN_DECLS
/* POSIX 1003.1 Prototypes. */
extern int closedir(DIR *__dirp);
extern DIR *opendir(__const char *__dirname);
extern struct dirent *readdir(DIR *__dirp);
extern int readdir_r(DIR *__dirp,
                     struct dirent *__entry,
                     struct dirent **__result);
extern void rewinddir(DIR *__dirp);

__END_DECLS
#endif
