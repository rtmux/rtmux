/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* sys/roottask.h */
#ifndef __ROOTTASK_H_INCLUDED
#define __ROOTTASK_H_INCLUDED

#ifndef __PLATFORM_H_INCLUDED
#include <sys/platform.h>
#endif

#ifndef _UNISTD_H_INCLUDED
#include <unistd.h>
#endif

__BEGIN_DECLS

extern int roottask_console_set(const char *__path);

__END_DECLS

#endif
