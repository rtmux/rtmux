/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* devctl.h */

#ifndef _DEVCTL_H_INCLUDED
#define _DEVCTL_H_INCLUDED


#ifndef __TYPES_H_INCLUDED
#include <sys/types.h>
#endif

#ifndef _LIMITS_H_INCLUDED
#include <limits.h>
#endif

#ifndef __PLATFORM_H_INCLUDED
#include <sys/platform.h>
#endif

#ifndef __CDEFS_H_INCLUDED
#include <sys/cdefs.h>
#endif


/* Flag bits applied to a devctl command. The low 16 bits carry the command
 * number; the high bits carry transfer direction.
 */

#define _POSIX_DEVDIR_NONE 0u
#define _POSIX_DEVDIR_TO 0x80000000u
#define _POSIX_DEVDIR_FROM 0x40000000u
#define _POSIX_DEVDIR_TOFROM (_POSIX_DEVDIR_TO | _POSIX_DEVDIR_FROM)

__BEGIN_DECLS struct iovec;
extern int devctl(int fd,
                  unsigned devctl_cmd,
                  void *dev_data_ptr,
                  size_t nbytes,
                  int *dev_info_ptr);


/* ioctl is built on top of devctl(). Both devctl() and ioctl() encode the
 * direction in the command using the same 2 high-order bits.
 *
 * Devctl command codes are subdivided into the following live RTMux classes:
 *
 *      Reserved                0x000 - 0x0ff
 *      common (all io servers) 0x100 - 0x1ff
 *      Character               0x300 - 0x3ff
 *      Misc                    0x500 - 0x5ff
 *      Proc                    0x800 - 0x8ff
 */
#define DEVCTL_CLASS_COMMON 0x01
#define DEVCTL_CLASS_TTY 0x03
#define DEVCTL_CLASS_MISC 0x05
#define DEVCTL_CLASS_PROC 0x08

#define __DIOF(class, cmd, data)                                              \
    ((unsigned) (((unsigned) (sizeof(data)) << 16) + ((class) << 8) + (cmd) + \
                 _POSIX_DEVDIR_FROM))
#define __DIOT(class, cmd, data)                                              \
    ((unsigned) (((unsigned) (sizeof(data)) << 16) + ((class) << 8) + (cmd) + \
                 _POSIX_DEVDIR_TO))
#define __DIOTF(class, cmd, data)                                             \
    ((unsigned) (((unsigned) (sizeof(data)) << 16) + ((class) << 8) + (cmd) + \
                 _POSIX_DEVDIR_TOFROM))

#define __DION(class, cmd) \
    ((unsigned) (((class) << 8) + (cmd) + _POSIX_DEVDIR_NONE))
#define _DEVCTL_DATA(msg) ((void *) (sizeof(msg) + (char *) (&msg)))

#define _DEVCTL_FLAG_NORETVAL 0x00000001
#define _DEVCTL_FLAG_NOTTY 0x00000002
#define _DEVCTL_FLAG_NOCANCEL 0x00000004

extern int _devctl(int fd,
                   unsigned devctl_cmd,
                   void *data_ptr,
                   size_t nbytes,
                   unsigned flags) __hidden;

__END_DECLS
#endif
