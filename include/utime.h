/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* sys/utime.h utimbuf structure and prototypes */
#ifndef _UTIME_H_INCLUDED
#define _UTIME_H_INCLUDED

#ifndef __PLATFORM_H_INCLUDED
#include <sys/platform.h>
#endif

struct utimbuf {
    time_t actime;  /* access time */
    time_t modtime; /* modification time */
};

__BEGIN_DECLS

__END_DECLS
#endif
