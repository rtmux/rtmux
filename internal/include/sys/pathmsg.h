/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* sys/pathmsg.h */

#ifndef __PATHMSG_H_INCLUDED
#define __PATHMSG_H_INCLUDED

#ifndef __PLATFORM_H_INCLUDED
#include <sys/platform.h>
#endif

#ifndef __SYSMSG_H_INCLUDED
#include <sys/sysmsg.h>
#endif

#ifndef __IOMSG_H_INCLUDED
#include <sys/iomsg.h>
#endif

#define PATH_PID ROOTTASK_PID
#define PATH_CHID ROOTTASK_CHID
#define PATH_COID ROOTTASK_COID
#define PATH_HANDLE ROOTTASK_HANDLE
#define PATH_HANDLE_ROOT (ROOTTASK_HANDLE + 1)

enum { _PATH_RESOLVE = _PATH_MSG_BASE, _PATH_CHDIR, _PATH_CHROOT };

#ifndef __CDEFS_H_INCLUDED
#include <sys/cdefs.h>
#endif

__BEGIN_DECLS
/* Send _PATH_CHDIR for an absolute path, relative to the process root, to
 * make it the sender's cwd (lib/libc/posix/chdir.c). The spawn loader uses it
 * to give a child the cwd its posix_spawn() chdir file actions chose.
 */
int __chdir_send(const char *path) __hidden;
__END_DECLS

#endif
