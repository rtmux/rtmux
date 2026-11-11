/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* pwd.h Password operations */
#ifndef _PWD_H_INCLUDED
#define _PWD_H_INCLUDED

#ifndef __TYPES_H_INCLUDED
#include <sys/types.h>
#endif

#ifndef __PLATFORM_H_INCLUDED
#include <sys/platform.h>
#endif

struct passwd {
    char *pw_name; /*  User's login name           */
    char *pw_passwd;
    uid_t pw_uid; /*  User ID number              */
    gid_t pw_gid; /*  Group ID number             */
    char *pw_age;
    char *pw_comment;
    char *pw_gecos;
    char *pw_dir;   /*  Initial Working Directory   */
    char *pw_shell; /*  Initial User Program        */
};

__BEGIN_DECLS
/* POSIX 1003.1 Prototypes. */
extern struct passwd *getpwent(void);
extern struct passwd *getpwnam(const char *name);
extern struct passwd *getpwuid(uid_t uid);
extern int getpwnam_r(const char *__name,
                      struct passwd *__pwd,
                      char *__buf,
                      size_t __buflen,
                      struct passwd **__result);
extern int getpwuid_r(uid_t __uid,
                      struct passwd *__pwd,
                      char *__buf,
                      size_t __buflen,
                      struct passwd **__result);
extern void endpwent(void);
extern void setpwent(void);
__END_DECLS
#endif
