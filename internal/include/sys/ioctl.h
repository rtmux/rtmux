/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef __IOCTL_H_INCLUDED
#define __IOCTL_H_INCLUDED

#ifndef __PLATFORM_H_INCLUDED
#include <sys/platform.h>
#endif

#ifndef __TYPES_H_INCLUDED
#include <sys/types.h>
#endif

#define FIONREAD 0x4004667f

__BEGIN_DECLS

int ioctl(int __fd, int __cmd, ...);

__END_DECLS

#endif
