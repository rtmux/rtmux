/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* times.h Process timing definitions (P1003.1) */
#ifndef __TIMES_H_INCLUDED

#ifndef __PLATFORM_H_INCLUDED
#include <sys/platform.h>
#endif

#include <sys/_clocktypes.h>

struct tms {
    clock_t tms_utime;
    clock_t tms_stime;
    clock_t tms_cutime;
    clock_t tms_cstime;
};

__BEGIN_DECLS

extern clock_t times(struct tms *__time_info);

__END_DECLS

#define __TIMES_H_INCLUDED
#endif
