/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * POSIX run-time dynamic loading declarations.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef _DLFCN_H
#define _DLFCN_H

#define __need_size_t
#include <stddef.h>

#include <sys/platform.h>

__BEGIN_DECLS

/* The MODE argument to dlopen() */
#define RTLD_LAZY 0x0001
#define RTLD_NOW 0x0002
#define RTLD_GLOBAL 0x0100
#define RTLD_LOCAL 0x0200

extern char *dlerror(void);
extern void *dlopen(const char *__pathname, int __mode);
extern void *dlsym(void *__handle, const char *__name);
extern int dlclose(void *__handle);

__END_DECLS

#endif
