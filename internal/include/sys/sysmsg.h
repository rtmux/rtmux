/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* sys/sysmsg.h */
#ifndef __SYSMSG_H_INCLUDED
#define __SYSMSG_H_INCLUDED

#ifndef __PLATFORM_H_INCLUDED
#include <sys/platform.h>
#endif

enum _sys_bases {
    _SYSMSG_BASE = 0x0000,
    _SYS_BASE = 0x0000,
    _SYS_MAX = 0x000f,
    _PROC_BASE = 0x0010,
    _PROC_MAX = 0x003f,
    _VM_BASE = 0x0040,
    _VM_MAX = 0x006f,
    _PATH_MSG_BASE = 0x0070,
    _PATH_MSG_MAX = 0x009f,
    _CPUMSG_BASE = 0x00a0,
    _CPUMSG_MAX = 0x00cf,
    _HWRES_BASE = 0x00d0,
    _HWRES_MAX = 0x00d3,
    _SYSMSG_MAX = 0x00FF
};

enum {
    _SYS_CONF = _SYS_BASE,
    _SYS_CMD,
    _SYS_CONSOLE /* Set system console device */
};

enum { _SYS_SUB_GET };

enum {
    _SYS_CMD_REBOOT,
    _SYS_CMD_SHUTDOWN,
    _SYS_CMD_SYNC,    /* Flush every mount; unprivileged */
    _SYS_CMD_LOG_MARK /* Log the trailing text in roottask's ring; root only */
};

/* struct _sys_cmd flags */
#define _SYS_CMD_FLAG_NOSYNC 0x0001 /* shutdown/reboot: skip the sync sweep */

/* Message of _SYS_CONF */
struct _sys_conf {
    uint16_t type;
    uint16_t subtype;
    uint32_t cmd;
    int32_t name;
    int32_t spare;
    int64_t value;
    /* char                         set_info[] */
};

struct _sys_conf_reply {
    uint32_t zero[3];
    int32_t match;
    int64_t value;
    /* char                         get_info[] */
};

typedef union {
    struct _sys_conf i;
    struct _sys_conf_reply o;
} sys_conf_t;

/* Message of _SYS_CMD */
struct _sys_cmd {
    uint16_t type;
    uint16_t cmd;
    uint16_t flags; /* _SYS_CMD_FLAG_* */
    uint16_t spare;
};

/* _SYS_CMD_LOG_MARK: header, then len bytes of text (no NUL needed), len <= 128. */
struct _sys_cmd_log_mark {
    uint16_t type;
    uint16_t cmd;
    uint16_t flags;
    uint16_t len;
    /* char text[len] */
};

typedef union {
    struct _sys_cmd i;
    struct _sys_cmd_log_mark mark;
} sys_cmd_t;


/* Message of _SYS_CONSOLE — set the system console device.
 *
 * Updates the /dev/console symlink to point to the given path (e.g.
 * "/dev/ser1"). The caller should then "reopen /dev/console" to redirect its
 * own stdio.
 *
 * The path is sent inline after the header (variable-length message).
 */
struct _sys_console {
    uint16_t type;    /* _SYS_CONSOLE */
    uint16_t pathlen; /* strlen(path) + 1 */
    /* char path[pathlen] follows */
};

struct _sys_console_reply {
    uint32_t zero;
};

typedef union {
    struct _sys_console i;
    struct _sys_console_reply o;
} sys_console_t;

#endif
