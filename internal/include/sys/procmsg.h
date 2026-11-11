/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* sys/procmsg.h */
#ifndef __PROCMSG_H_INCLUDED
#define __PROCMSG_H_INCLUDED

#ifndef __PLATFORM_H_INCLUDED
#include <sys/platform.h>
#endif

#ifndef __SYSMSG_H_INCLUDED
#include <sys/sysmsg.h>
#endif

#ifndef __RTMUX_CORE_H_INCLUDED
#include <sys/rtmux-core.h>
#endif

#ifndef __TYPES_H_INCLUDED
#include <sys/types.h>
#endif

#ifndef _SIGNAL_H_INCLUDED
#include <signal.h>
#endif

#ifndef _SCHED_H_INCLUDED
#include <sched.h>
#endif

#ifndef _SPAWN_H_INCLUDED
#include <spawn.h>
#endif

#ifndef __SIGINFO_H_INCLUDED
#include <sys/siginfo.h>
#endif

#ifndef __RESOURCE_H_INCLUDED
#include <sys/resource.h>
#endif

enum {
    _PROC_SPAWN = _PROC_BASE,
    _PROC_WAIT,
    _PROC_GETSETID,
    _PROC_SETPGID,
    _PROC_UMASK,
    _PROC_SESSION,
    _PROC_EVENT,
    _PROC_RESOURCE,
    _PROC_POSIX_SPAWN,
    _PROC_SYSCHAN, /* Register system channel */
    _PROC_DETACH,  /* Detach from parent (stay resident) */
    _PROC_KILL,    /* Deliver signal as notification to target */
    _PROC_EXIT,    /* Process exit notification */
    _PROC_PHYS_CONTAIN, /* Self-restrict: MAP_PHYS only within own hwres claims */
    _PROC_JOBCTL        /* Job control: stop/continue state, terminal service */
};

enum {
    _PROC_SPAWN_START,
    _PROC_SPAWN_FD,
    _PROC_SPAWN_ARGS,
    _PROC_SPAWN_DONE,
    _PROC_SPAWN_DEBUG,
    _PROC_SPAWN_EXEC,
    _POSIX_SPAWN_START
};

enum {
    _PROC_ID_GETID,
    _PROC_ID_SETUID,
    _PROC_ID_SETGID,
    _PROC_ID_SETEUID,
    _PROC_ID_SETEGID,
    _PROC_ID_SETREUID,
    _PROC_ID_SETREGID,
    _PROC_ID_SETGROUPS
};

enum {
    _PROC_EVENT_TRIGGER,
    _PROC_EVENT_NOTIFY_ADD,
    _PROC_EVENT_NOTIFY_DEL
};

enum { _PROC_UMASK_SET, _PROC_UMASK_GET };

enum { _PROC_RESOURCE_USAGE, _PROC_RESOURCE_GETLIMIT, _PROC_RESOURCE_SETLIMIT };

enum {
    _PROC_SESSION_SETSID,
    _PROC_SESSION_TCSETSID,
    _PROC_SESSION_SETPGRP,
    _PROC_SESSION_VALIDATE_PGRP,
};

struct inheritance {
    unsigned long flags;
    pid_t pgroup;
    sigset_t sigmask;
    sigset_t sigdefault;
    sigset_t sigignore;
    unsigned long stack_max;
#if __INT_BITS__ != 32
    long policy;
#else
    int policy;
#endif
    uint32_t runmask;
    struct sched_param param;
};

#define SPAWN_SETGROUP POSIX_SPAWN_SETPGROUP
#define SPAWN_SETSIGMASK POSIX_SPAWN_SETSIGMASK
#define SPAWN_SETSIGDEF POSIX_SPAWN_SETSIGDEF
#define SPAWN_SETSIGIGN 0x00000008
#define SPAWN_TCSETPGROUP 0x00000080
#define SPAWN_SETSID 0x00000200
#define SPAWN_EXPLICIT_SCHED POSIX_SPAWN_SETSCHEDPARAM
#define SPAWN_EXPLICIT_CPU 0x00000800
#define SPAWN_SETSTACKMAX 0x00001000
#define SPAWN_NOZOMBIE 0x00002000
#define SPAWN_DEBUG 0x00004000
#define SPAWN_EXEC 0x00010000
#define SPAWN_SEARCH_PATH 0x00020000
#define SPAWN_CHECK_SCRIPT 0x00040000

/* Ask the proc service to mint a process capability over the child and return
 * it to the parent in the spawn reply. Opt-in: without this flag the reply is
 * the plain zero-length reply and behaves exactly as before. The caller must
 * also supply a struct _io_native_hdr reply buffer to receive the cap.
 */
#define SPAWN_PROC_CAP 0x00080000
#define SPAWN_FD_SCAN_TAIL 0x00100000
#define SPAWN_ALIGN_DEFAULT 0x00000000
#define SPAWN_ALIGN_FAULT 0x01000000
#define SPAWN_ALIGN_NOFAULT 0x02000000
#define SPAWN_ALIGN_MASK 0x03000000

#define SPAWN_FDCLOSED (-1)
#define SPAWN_FDOPEN (-2)
#define SPAWN_NEWPGROUP 0

pid_t rtmux_spawn(const char *path,
                  int fd_count,
                  const int fd_map[],
                  const struct inheritance *inherit,
                  char *const argv[],
                  char *const envp[]);

/* Like rtmux_spawn, but also requests a process capability over the child. On
 * success proc_cap receives the cap handle in the caller's endpoint table (0 if
 * the proc service declined to mint one). Sets SPAWN_PROC_CAP internally.
 */
pid_t rtmux_spawn_cap(const char *path,
                      int fd_count,
                      const int fd_map[],
                      const struct inheritance *inherit,
                      char *const argv[],
                      char *const envp[],
                      uint64_t *proc_cap);
pid_t rtmux_spawnp(const char *file,
                   int fd_count,
                   const int fd_map[],
                   const struct inheritance *inherit,
                   char *const argv[],
                   char *const envp[]);

/* Message of _PROC_SPAWN/_PROC_SPAWN_START */
struct _proc_spawn {
    uint16_t type;
    uint16_t subtype;
    uint16_t searchlen;
    uint16_t pathlen;
    struct inheritance parms;
    uint16_t nfds;
    uint16_t nargv;
    uint16_t narge;
    uint16_t reserved;
    size_t nbytes; /* Number of bytes of argv + arge with nulls */
    /* int32_t fd_map[nfds] char search[searchlen] char path[pathlen] char
     * argv[nargv][] char arge[narge][]
     */
};

typedef union {
    struct _proc_spawn i;
} proc_spawn_t;

/* Message of _PROC_SPAWN/_PROC_SPAWN_ARGS */
struct _proc_spawn_args {
    uint16_t type;
    uint16_t subtype;
    uint32_t nbytes;
    uintptr_t offset;
    uintptr_t zero;
};

typedef union {
    struct _proc_spawn_args i;
} proc_spawn_args_t;


/* Message of _PROC_SPAWN/_PROC_SPAWN_DEBUG */
struct _proc_spawn_debug {
    uint16_t type;
    uint16_t subtype;
    uint32_t _pad;
    uintptr_t text_addr;
    size_t text_size;
    intptr_t text_reloc;
    uintptr_t data_addr;
    size_t data_size;
    intptr_t data_reloc;
    char name[];
};

typedef union {
    struct _proc_spawn_debug i;
} proc_spawn_debug_t;


/* Message of _PROC_SPAWN/_PROC_SPAWN_FD */
struct _proc_spawn_fd {
    uint16_t type;
    uint16_t subtype;
    uint16_t flags;
    uint16_t _pad;
    int nfds;
    int32_t base;
    pid_t ppid;
    /* int                          fd[nfds] */
};
#define _PROC_SPAWN_FD_LIST \
    0x0001 /* only fds from passed in list (otherwise all fds) */
#define _PROC_SPAWN_FD_REVOKE \
    0x0004 /* scramble the loader thread's key (loading abandoned) */
#define _PROC_SPAWN_FD_NOCLOEXC                   \
    0x0002 /* no fds with CLOEXC set (i.e. spawn) \
            */

struct _proc_spawn_fd_info {
    int fd;
    uint32_t nd;
    uint32_t srcnd;
    pid_t pid;
    int32_t chid;
    int32_t scoid;
    int32_t coid;
    uint32_t key; /* msg_key_data key over the _IO_DUP the loader sends */
    uint32_t _pad;
};

struct _proc_spawn_fd_reply {
    uint16_t flags;
    uint16_t nfds;
    /* struct _proc_spawn_fd_info   info[nfds]; */
};

#define _PROC_SPAWN_FDREPLY_MORE \
    0x00000001 /* More fd's to return, send again */

typedef union {
    struct _proc_spawn_fd i;
    struct _proc_spawn_fd_reply o;
} proc_spawn_fd_t;


/* Message of _PROC_SPAWN/_PROC_SPAWN_DONE */
struct _proc_spawn_done {
    uint16_t type;
    uint16_t subtype;
    int32_t rcvid;
};

typedef union {
    struct _proc_spawn_done i;
} proc_spawn_done_t;

/* Message of _PROC_SPAWN/_PROC_SPAWN_DIR */

enum { _PROC_SPAWN_DIR_ALL, _PROC_SPAWN_DIR_ROOT, _PROC_SPAWN_DIR_CWD };

struct _proc_spawn_dir {
    uint16_t type;
    uint16_t subtype;
    int32_t ppid;
    uint16_t request;
    uint16_t pathmax;
};

struct _proc_spawn_dir_reply {
    uint16_t result;
    uint16_t rootlen;
};

typedef union {
    struct _proc_spawn_dir i;
    struct _proc_spawn_dir_reply o;
} proc_spawn_dir_t;

/* Message of _PROC_POSIX_SPAWN */
struct _proc_posix_spawn {
    uint16_t type;
    uint16_t subtype;
    uint16_t pathlen;
    uint16_t nargv;
    uint16_t narge;
    uint16_t reserved[3];
    uint32_t filact_bytes;  /* size of the _posix_spawn_file_actions_t */
    uint32_t filact_obytes; /* size of the _posix_spawn_file_actions_t.open */
    uint32_t argenv_bytes;  /* Number of bytes of argv + arge with nulls */
    uint32_t attr_bytes;    /* size of the _posix_spawnattr_t */
    /* _posix_spawnattr_t attr; _posix_spawn_file_actions_t filact char
     * path[pathlen] char argv[nargv][] char arge[narge][]
     */
};

typedef union {
    struct _proc_posix_spawn i;
} proc_posixspawn_t;


/* Message of _PROC_GETSETID */
struct _proc_getsetid {
    uint16_t type;
    uint16_t subtype;
    pid_t pid;
    id_t eid;
    id_t rid;
    int32_t ngroups;
    /* gid_t                        groups[ngroups]; */
};

struct _proc_getsetid_reply {
    uint32_t zero;
    pid_t pgrp;
    pid_t ppid;
    pid_t sid;
    uint32_t reserved[3];
    struct _cred_info cred;
};

typedef union {
    struct _proc_getsetid i;
    struct _proc_getsetid_reply o;
} proc_getsetid_t;


/* Message of _PROC_SETPGID */
struct _proc_setpgid {
    uint16_t type;
    uint16_t zero;
    pid_t pid;
    pid_t pgid;
};

typedef union {
    struct _proc_setpgid i;
} proc_setpgid_t;


/* Message of _PROC_WAIT */
struct _proc_wait {
    uint16_t type;
    int16_t idtype;
    int32_t options;
    id_t id;
};

typedef union {
    struct _proc_wait i;
    siginfo_t o;
} proc_wait_t;


/* Message of _PROC_UMASK */
struct _proc_umask {
    uint16_t type;
    uint16_t subtype;
    uint32_t umask;
    pid_t pid;
};

struct _proc_umask_reply {
    uint32_t zero1;
    uint32_t umask;
};

typedef union {
    struct _proc_umask i;
    struct _proc_umask_reply o;
} proc_umask_t;

/* Message of _PROC_SESSION */
struct _proc_session {
    uint16_t type;
    uint16_t subtype;
    pid_t sid;
    int32_t id;
    uint32_t event;
};

typedef union {
    struct _proc_session i;
} proc_session_t;

/* Message of _PROC_EVENT */
struct _proc_event {
    uint16_t type;
    uint16_t subtype;
    uint32_t flags;
    struct sigevent event;
};

typedef union {
    struct _proc_event i;
} proc_event_t;

struct _proc_event_del {
    uint16_t type;
    uint16_t subtype;
    int id;
};

typedef union {
    struct _proc_event_del i;
} proc_event_del_t;

/* Message header for all _PROC_RESOURCE */
struct _proc_resource_hdr {
    uint16_t type;
    uint16_t subtype;
    pid_t pid;
};

/* Message of _PROC_RESOURCE/_PROC_RESOURCE_USAGE */
struct _proc_resource_usage {
    uint16_t type;
    uint16_t subtype;
    pid_t pid;
    uint32_t who;
};

typedef union {
    struct _proc_resource_usage i;
    struct rusage o;
} proc_resource_usage_t;

/* Message of _PROC_RESOURCE/_PROC_RESOURCE_GETLIMIT */
struct _proc_resource_getlimit {
    uint16_t type;
    uint16_t subtype;
    pid_t pid;
    uint32_t count;
    uint32_t reserved;
    uint32_t resource[1];
};

typedef union {
    struct _proc_resource_getlimit i;
    struct rlimit o[1];
} proc_resource_getlimit_t;

/* Message of _PROC_RESOURCE/_PROC_RESOURCE_SETLIMIT */
struct _proc_resource_setlimit {
    uint16_t type;
    uint16_t subtype;
    pid_t pid;
    uint32_t count;
    uint32_t reserved;
    struct _proc_resource_entry {
        uint32_t resource;
        uint32_t reserved;
        struct rlimit limit;
    } entry[1];
};

typedef union {
    struct _proc_resource_setlimit i;
} proc_resource_setlimit_t;

/* Message of _PROC_SYSCHAN — register the process system channel. Sent by crt0
 * before main(). Roottask connect_attach's to the channel and stores the
 * connection in prp->sys_coid for notification delivery.
 */
struct _proc_syschan {
    uint16_t type;    /* _PROC_SYSCHAN */
    uint16_t subtype; /* 0 = register */
    int32_t chid;     /* system channel id (from channel_create) */
};

struct _proc_syschan_reply {
    uint32_t zero; /* EOK on success */
};

typedef union {
    struct _proc_syschan i;
    struct _proc_syschan_reply o;
} proc_syschan_t;

/* Message of _PROC_DETACH — detach from parent process. Unblocks the parent's
 * waitpid() with the given status and reparents the calling process to pid 1.
 */
struct _proc_detach {
    uint16_t type; /* _PROC_DETACH */
    uint16_t zero;
    int32_t status; /* status returned to parent's waitpid */
};

struct _proc_detach_reply {
    uint32_t zero;
};

typedef union {
    struct _proc_detach i;
    struct _proc_detach_reply o;
} proc_detach_t;

/* _PROC_PHYS_CONTAIN — the calling process self-restricts its address space:
 * from here on every specific-address MAP_PHYS is refused (MAP_ANON DMA buffers
 * stay allowed). Irreversible; no payload beyond the verb. Used to confine an
 * LKL domain to the device MMIO windows it mapped during hardware discovery.
 */
struct _proc_phys_contain {
    uint16_t type; /* _PROC_PHYS_CONTAIN */
    uint16_t zero;
};

struct _proc_phys_contain_reply {
    uint32_t zero;
};

typedef union {
    struct _proc_phys_contain i;
    struct _proc_phys_contain_reply o;
} proc_phys_contain_t;

/* Message of _PROC_KILL — deliver a signal as a notification.
 *
 * Roottask looks up the target process, then delivers a
 * _NOTIFICATION_CODE_SIGNAL notification to its system channel. The target's
 * system thread receives and handles it.
 */
struct _proc_kill {
    uint16_t type; /* _PROC_KILL */
    uint16_t zero;
    int32_t pid;   /* target process */
    int32_t signo; /* signal number */
};

struct _proc_kill_reply {
    uint32_t zero;
};

typedef union {
    struct _proc_kill i;
    struct _proc_kill_reply o;
} proc_kill_t;

/* Message of _PROC_JOBCTL — job control.
 *
 * _PROC_JOBCTL_STATE comes from a process about its own state: its libc system
 * thread reports that the process stopped (arg2 = the stop signal) or continued,
 * and which of SIGTTIN/SIGTTOU it blocks or ignores. Roottask records it (the
 * process flags and the siginfo that waitpid reports) and wakes waiters.
 *
 * The others come from a terminal server, never an ordinary process; roottask
 * answers them only for sessions whose controlling terminal that server serves.
 * QUERY describes a client's standing on the terminal, SIGFG signals the
 * foreground group of the session led by pid (VINTR/VQUIT/VSUSP), SIGBG sends
 * SIGTTIN/SIGTTOU to a background client's own group, and SETPGRP validates and
 * records a tcsetpgrp().
 */
enum {
    _PROC_JOBCTL_STATE,
    _PROC_JOBCTL_QUERY,
    _PROC_JOBCTL_SIGFG,
    _PROC_JOBCTL_SIGBG,
    _PROC_JOBCTL_SETPGRP
};

/* _PROC_JOBCTL_STATE events (arg) */
enum { _JOBCTL_STOPPED, _JOBCTL_CONTINUED, _JOBCTL_TTYMASK };

/* _PROC_JOBCTL_STATE _JOBCTL_TTYMASK bits (arg2), and QUERY reply flags */
#define _JOBCTL_HOLD_TTIN 0x01U /* SIGTTIN is blocked or ignored */
#define _JOBCTL_HOLD_TTOU 0x02U /* SIGTTOU is blocked or ignored */
#define _JOBCTL_CTTY 0x10U      /* the server's terminal is the client's own */
#define _JOBCTL_FG 0x20U        /* the client is in the foreground group */
#define _JOBCTL_ORPHAN 0x40U    /* the client's group is orphaned */

struct _proc_jobctl {
    uint16_t type; /* _PROC_JOBCTL */
    uint16_t subtype;
    int32_t pid;  /* QUERY/SIGBG/SETPGRP: client; SIGFG: session leader */
    int32_t arg;  /* STATE: event; SIGFG/SIGBG: signal; SETPGRP: group */
    int32_t arg2; /* STATE: stop signal or _JOBCTL_HOLD_* bits */
};

struct _proc_jobctl_reply {
    int32_t pgrp;    /* the client's process group */
    int32_t sid;     /* the client's session (leader pid), 0 if none */
    int32_t fg_pgrp; /* the foreground group of the client's terminal */
    uint32_t flags;  /* _JOBCTL_* */
};

typedef union {
    struct _proc_jobctl i;
    struct _proc_jobctl_reply o;
} proc_jobctl_t;

/* Message of _PROC_EXIT — process exit notification. Sent by exit() to the proc
 * service. Roottask handles all cleanup (channels, FDs, timers, memory) without
 * creating a terminator thread in the dying process.
 */
struct _proc_exit {
    uint16_t type; /* _PROC_EXIT */
    uint16_t zero;
    int32_t status; /* exit status */
};

struct _proc_exit_reply {
    uint32_t zero; /* not used — process is dead */
};

typedef union {
    struct _proc_exit i;
    struct _proc_exit_reply o;
} proc_exit_t;

#endif
