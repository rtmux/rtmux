/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* sys/auxv.h */
#ifndef __AUXV_H_INCLUDED
#define __AUXV_H_INCLUDED

#ifndef __PLATFORM_H_INCLUDED
#include <sys/platform.h>
#endif

__BEGIN_DECLS typedef struct {
    long a_type;
    union {
        long a_val;
        void *a_ptr;
        void (*a_fcn)(void);
    } a_un;
} auxv_t;

enum _auxv_e_at {
    AT_NULL = 0,
    AT_IGNORE,
    AT_EXECFD,
    AT_PHDR,
    AT_PHENT,
    AT_PHNUM,
    AT_PAGESZ,
    AT_BASE,
    AT_FLAGS,
    AT_ENTRY,
    AT_INTP_DEVICE = 45,
    AT_INTP_INODE = 46,
    AT_EXEFILE = 47,
    AT_LIBPATH = 48,
    AT_DATA = 49,
    /* a_val 1: the image is set-id and its spawner is not the owner, so the
     * dynamic loader must ignore LD_* overrides (issetugid()). */
    AT_RTMUX_SECURE = 50,
    AT_NUM
};

__END_DECLS
#endif
