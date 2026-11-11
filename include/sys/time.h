/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* sys/time.h UNIX98 time types */
#ifndef _SYS_TIME_H_
#define _SYS_TIME_H_

#include <sys/_timetypes.h>

#ifndef __PLATFORM_H_INCLUDED
#include <sys/platform.h>
#endif


__BEGIN_DECLS
extern int gettimeofday(struct timeval *__tp, void *__tzp);
__END_DECLS

#endif /* !_SYS_TIME_H_ */
