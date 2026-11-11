/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* grp.h Group operations */

#ifndef _GRP_H_INCLUDED
#define _GRP_H_INCLUDED

#ifndef __TYPES_H_INCLUDED
#include <sys/types.h>
#endif

#ifndef __PLATFORM_H_INCLUDED
#include <sys/platform.h>
#endif

struct group {
    char *gr_name;
    char *gr_passwd;
    gid_t gr_gid;
    char **gr_mem;
};

/* POSIX 1003.1 Prototypes. */

__BEGIN_DECLS extern void endgrent(void);
extern struct group *getgrnam(const char *__name);
extern struct group *getgrgid(gid_t __gid);
extern int getgrnam_r(const char *__name,
                      struct group *__grp,
                      char *__buf,
                      size_t __buflen,
                      struct group **__result);
extern int getgrgid_r(gid_t __gid,
                      struct group *__grp,
                      char *__buf,
                      size_t __buflen,
                      struct group **__result);
extern struct group *getgrent(void);
extern void setgrent(void);

__END_DECLS
#endif
