/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* fnmatch.h */

#ifndef _FNMATCH_H_INCLUDED
#define _FNMATCH_H_INCLUDED

#ifndef __PLATFORM_H_INCLUDED
#include <sys/platform.h>
#endif


#define FNM_PATHNAME 0x01
#define FNM_PERIOD 0x02
#define FNM_NOESCAPE 0x04

#define FNM_NOMATCH 1


__BEGIN_DECLS extern int fnmatch(const char *__expr,
                                 const char *__str,
                                 int __flags);
__END_DECLS
#endif
