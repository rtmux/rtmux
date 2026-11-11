/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef STRUCT_H
#define STRUCT_H

#include <signal.h>
#include <sys/auxv.h>
#include <sys/iomsg.h>
#include <sys/procmsg.h>
#include <sys/pathmsg.h>
#include <sys/memmsg.h>
#include <sys/sysmsg.h>
#include <smpswitch.h>


#define PROC_INIT_PRIORITY 10 /* priority to start init thread at */

union proc_msg_union {
    uint16_t type;
    proc_spawn_t spawn;
    proc_spawn_args_t spawn_args;
    proc_spawn_debug_t spawn_debug;
    proc_spawn_fd_t spawn_fd;
    proc_spawn_done_t spawn_done;
    proc_spawn_dir_t spawn_dir;
    proc_getsetid_t getsetid;
    proc_setpgid_t setpgid;
    proc_wait_t wait;
    proc_umask_t umask;
    proc_resource_usage_t resource_usage;
    proc_resource_getlimit_t resource_getlimit;
    proc_resource_setlimit_t resource_setlimit;
    mem_map_t mmap;
    mem_ctrl_t mctrl;
    mem_info_t minfo;
    mem_offset_t moffset;
    sys_conf_t conf;
    sys_cmd_t command;
    io_open_t open;
    io_dup_t dup;
    io_read_t read;
    io_write_t write;
    io_lseek_t lseek;
    io_stat_t stat;
    struct _notification notification;
    char filler[sizeof(struct _io_connect_link_reply) +
                sizeof(struct _io_connect_entry) * SYMLOOP_MAX + PATH_MAX + 1];
};

#define PROC_LF_LAZYSTACK 0x80000000

struct loader_startup {
    uintptr_t start_ip;
    uintptr_t start_sp;
    uintptr_t stackaddr;  /* prefered address of stack */
    uintptr_t stackalloc; /* minimum number of bytes needed in stack */
    uintptr_t stacksize;  /* maximum stack size */
    unsigned flags;
    auxv_t aux[AT_NUM + 1];
};

enum lc_state {
    LC_FREE,
    LC_SPAWN,
    LC_EXEC_SWAP,
    LC_TERM,
    LC_CROSS_ENDIAN = 0x40
};

struct loader_context_base {
    struct loader_context *next;
    size_t size;
    iov_t iov[10];
    struct process_entry *process;
    int rcvid;
    sigset_t mask;
    siginfo_t info;
    pid_t pid;
    pid_t ppid;
    int fds_done; /* fd inheritance closed: no more _PROC_SPAWN_FD keys */
    int tid;
    unsigned long flags;
    unsigned msgsize;
    int state;
    struct loader_startup start;
    int fault_errno;

    /* Bootstrap capabilities the manifest grants this child, resolved by the
     * loader from the absolute path it opened. Committed at _PROC_SPAWN_DONE.
     */
    uint64_t grant_caps;
    /* True when the image is a bootstrap-manifest system server. */
    int grant_system;
};

#define LOADER_STACK_SIZE                                  \
    (5 * __PAGESIZE - sizeof(struct loader_context_base) - \
     sizeof(union proc_msg_union))

struct loader_context {
    struct loader_context_base ctx;
    uint32_t stack[LOADER_STACK_SIZE / sizeof(uint32_t)];

    /* The 'msg' field has to go on the end so that we can expand it out for
     * long spawns.
     */
    union proc_msg_union msg;
};

/* Make a unique inode number */
#define PID_TO_INO(pid, subtype) \
    ((ino_t) ((unsigned) (pid) ^ (subtype ? UINT_MAX : (unsigned) INT_MAX)))

#endif
