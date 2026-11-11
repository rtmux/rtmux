/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* sys/path-registry.h */
#ifndef __PATH_REGISTRY_H_INCLUDED
#define __PATH_REGISTRY_H_INCLUDED

#ifndef __PLATFORM_H_INCLUDED
#include <sys/platform.h>
#endif

#ifndef __FTYPE_H_INCLUDED
#include <sys/ftype.h>
#endif

#ifndef _UNISTD_H_INCLUDED
#include <unistd.h>
#endif

#ifndef _FCNTL_H_INCLUDED
#include <fcntl.h>
#endif

#define NAMESPACE_FLAG_BEFORE                                                  \
    0x0001 /* Force path to be resolved before others at the same mountpoint \
            */
#define NAMESPACE_FLAG_AFTER \
    0x0002 /* Force path to be resolved after others at the same mountpoint */
#define NAMESPACE_FLAG_OPAQUE \
    0x0004 /* Don't resolve to mountpoints with shorter pathname matches */
#define NAMESPACE_FLAG_FTYPEONLY \
    0x0008 /* Matching ftype is required on the path */
#define NAMESPACE_FLAG_FTYPEALL \
    0x0010 /* Matching all ftypes (for redirecting servers) */
#define NAMESPACE_FLAG_STICKY                                               \
    0x0020 /* No ID is associated with pathname; remove by owner detach. */
#define NAMESPACE_FLAG_DIR 0x0100  /* Allow resolving of longer pathnames */
#define NAMESPACE_FLAG_SELF 0x0200 /* Allow resolving names to itself */
#define NAMESPACE_FLAG_UNCACHEABLE \
    0x0400 /* Directory lookups bypass any namespace dentry cache */

__BEGIN_DECLS

extern int namespace_link(const char *__path,
                        uint32_t __nd,
                        pid_t __pid,
                        int __chid,
                        unsigned __handle,
                        enum _file_type __file_type,
                        unsigned __flags);
extern int namespace_symlink(const char *__path, const char *__symlink);

/* Hook for roottask to override namespace_link with its own implementation that
 * manipulates the pathname tables directly (no IPC).
 */
extern __hidden int (*_namespace_link_hook)(const char *__path,
                                            uint32_t __nd,
                                            pid_t __pid,
                                            int __chid,
                                            unsigned __handle,
                                            enum _file_type __file_type,
                                            unsigned __flags);

__END_DECLS

#endif
