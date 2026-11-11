/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* Non-portable common devctl definitions. */
#ifndef __DEVCTL_COMMON_H_INCLUDED
#define __DEVCTL_COMMON_H_INCLUDED

#include <devctl.h>

/* Desc: This call gets O_ status flags as described in <fcntl.h>. Args: Pointer
 * to int in which flags are returned. Notes: Clients usually use fcntl(fd,
 * F_GETFL); cover function
 *        to generate this request.
 */
#define DEVCTL_GET_FLAGS __DIOF(DEVCTL_CLASS_COMMON, 1, int)

/* Desc: This call sets O_ status flags as described in <fcntl.h>. Args: Pointer
 * to int from which flags are taken. Notes: Clients usually use fcntl(fd,
 * F_SETFL, flags); cover function
 *        to generate this request.
 */
#define DEVCTL_SET_FLAGS __DIOT(DEVCTL_CLASS_COMMON, 2, int)


/* Desc: This call gets mount ST_* mount Flags as described in
 *        <sys/statvfs.h>
 * Args: Pointer to int in which flags are returned. Notes: Sometimes generated
 * by access() to determine write or
 *        execute modes.
 */
#define DEVCTL_GET_MOUNT_FLAGS __DIOF(DEVCTL_CLASS_COMMON, 3, int)

#include <sys/statvfs.h>
#define DEVCTL_STATVFS __DIOF(DEVCTL_CLASS_COMMON, 7, struct statvfs)

/* Desc: Flush every dirty buffer the server holds for the mount behind the
 *        connection to stable storage.
 * Args: None.
 * Notes: Sent by roottask's sync sweep (sync(), shutdown). A server with no
 *        slower tier to flush may return ENOSYS; the sweep counts that as
 *        success.
 */
#define DEVCTL_FS_SYNC __DION(DEVCTL_CLASS_COMMON, 8)

#endif
