/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* sys/mount.h */

#ifndef __MOUNT_H_INCLUDED
#define __MOUNT_H_INCLUDED

#ifndef __PLATFORM_H_INCLUDED
#include <sys/platform.h>
#endif

#ifndef __STATVFS_H_INCLUDED
#include <sys/statvfs.h>
#endif

/* These flags match the statvfs flags and can be masked directly */
#define _MOUNT_READONLY ST_RDONLY /* read only */
#define _MOUNT_NOEXEC ST_NOEXEC   /* can't exec from filesystem */
#define _MOUNT_NOSUID ST_NOSUID   /* don't honor setuid bits on fs */

/* These flags are mount specific */
#define _MOUNT_REMOUNT \
    0x00002000U /* This path is already mounted, perform an update */
#define _MOUNT_ENUMERATE 0x00008000U /* Auto-detect on this device */
#define _MOUNT_IMPLIED                                     \
    0x00080000U /* The mountpoint is unspecified by client \
                 */

#define _MFLAG_OCB \
    0x80000000U /* Attempt to open the device and send and ocb to server */

#endif
