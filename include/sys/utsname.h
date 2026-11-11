/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* utsname.h System name structure (P1003.1) */
#ifndef __SYS_UTSNAME_H_INCLUDED
#define __SYS_UTSNAME_H_INCLUDED

#ifndef __PLATFORM_H_INCLUDED
#include <sys/platform.h>
#endif

/* Every field is a NUL-terminated string of at most 63 characters. */
struct utsname {
    char sysname[64];  /* "RTMux" */
    char nodename[64]; /* /etc/hostname, else "rtmux" */
    char release[64];  /* kernel release, from the syspage */
    char version[64];  /* kernel build id, from the syspage */
    char machine[64];  /* "riscv64" */
};

__BEGIN_DECLS

extern int uname(struct utsname *__name);

__END_DECLS

#endif
