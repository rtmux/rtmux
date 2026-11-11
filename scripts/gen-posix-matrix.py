#!/usr/bin/env python3
#
# Generate docs/posix-compliance.md from exported headers, libc POSIX stubs,
# sysconf option reporting, and the userland conformance test registry.
#
# Drift detection has three layers:
#   1. POSIX_NAMES — curated set the matrix knows how to classify.
#   2. STUB_OVERRIDES — force certain libc-implemented names to "stub" or
#      "partial" when the kernel-side contract is not yet honored.
#   3. POSIX_PATTERNS — regex sweep of include/ for declarations whose names
#      look POSIX-shaped but are not in POSIX_NAMES.  `--check` fails when
#      such drift is found.
#

import argparse
import collections
import pathlib
import re
import sys

from posix_profile import (LKL_FLOOR, OPTION_CAPABILITY_ONLY,
                           OPTION_NONSTANDARD, OPTION_REQUIREMENTS,
                           POSIX_PROFILES, PSE51_REQUIRED_SYMBOLS,
                           advertised_posix_options, posix_header_files,
                           validate_profiles)


ROOT = pathlib.Path(__file__).resolve().parents[1]
OUT = ROOT / "docs" / "posix-compliance.md"
PORTING_DOC = ROOT / "docs" / "porting.md"

# PSE51-docs (externals/PSE51-docs/pse51/<fn>/README.md) is the per-function
# semantic oracle traced to POSIX.1-2024.  It is referenced, never vendored:
# the check below only asserts a page (or a documented no-page reason) exists
# for every PSE51-mandatory interface, so the matrix's "documented semantics"
# clause has a citable source instead of an assertion.
PSE51_DOC_ROOT = ROOT / "externals" / "PSE51-docs" / "pse51"
OPTS_PSE51_MANIFEST = ROOT / "scripts" / "opts" / "pse51-subset.tsv"
OPTS_PSE51_EXCLUSIONS = ROOT / "scripts" / "opts" / "pse51-exclusions.tsv"

# PSE51-mandatory symbols the doc set has no exact page for.  The set is traced
# to POSIX.1-2024 and broader than the mandatory profile, yet still omits these
# primitives; each reason names where the semantics come from instead.  A
# required symbol absent from both PSE51-docs and this map fails the gate.
PSE51_NO_PAGE = {
    "clock_getcpuclockid": "no standalone page; CPU-time clock-id helper, "
    "semantics traced to the clock_* pages and _POSIX_CPUTIME",
    "getppid": "single-process profile primitive; the PSE51-docs set carries "
    "no getpid/getppid page",
    "munlockall": "no exact page; semantics are the union of the mlockall and "
    "munlock pages",
    "sched_yield": "no exact page; scheduling semantics traced to the sched_* "
    "pages and _POSIX_PRIORITY_SCHEDULING",
    "umask": "single-process file-mode primitive; the PSE51-docs set carries "
    "no umask page",
}


POSIX_NAMES = {
    "clock_getcpuclockid",
    "clock_nanosleep",
    "clock_getres",
    "clock_gettime",
    "clock_settime",
    "cfgetispeed",
    "cfgetospeed",
    "cfsetispeed",
    "cfsetospeed",
    "getrusage",
    "kill",
    "killpg",
    "mlock",
    "mlockall",
    "mmap",
    "mprotect",
    "msync",
    "munlock",
    "munlockall",
    "munmap",
    "pause",
    "poll",
    "pselect",
    "raise",
    "sched_get_priority_max",
    "sched_get_priority_min",
    "sched_getparam",
    "sched_getscheduler",
    "sched_rr_get_interval",
    "sched_setparam",
    "sched_setscheduler",
    "sched_yield",
    "select",
    "sem_close",
    "sem_destroy",
    "sem_getvalue",
    "sem_init",
    "sem_open",
    "sem_post",
    "sem_timedwait",
    "sem_trywait",
    "sem_unlink",
    "sem_wait",
    "sigaction",
    "sigaddset",
    "sigdelset",
    "sigemptyset",
    "sigfillset",
    "sigismember",
    "siglongjmp",
    "sigsetjmp",
    "signal",
    "sigprocmask",
    "shm_open",
    "shm_unlink",
    "confstr",
    "uname",
    "fpathconf",
    "pathconf",
    "fstatvfs",
    "statvfs",
    # Core file-I/O primitives (POSIX.1 base, all exercised by the suite).
    "_exit",
    "open",
    "openat",
    "creat",
    "close",
    "read",
    "write",
    "pread",
    "pwrite",
    "lseek",
    "dup",
    "dup2",
    "pipe",
    "access",
    "faccessat",
    "fcntl",
    "unlink",
    "unlinkat",
    # File-status and directory-stream interfaces (POSIX.1 base).
    "stat",
    "fstat",
    "lstat",
    "fstatat",
    "opendir",
    "readdir",
    "readdir_r",
    "closedir",
    "rewinddir",
    "rand_r",
    # Process credential get/set interfaces (POSIX.1 base).
    "getuid",
    "geteuid",
    "getgid",
    "getegid",
    "getgroups",
    "getlogin",
    "getlogin_r",
    "setuid",
    "seteuid",
    "setgid",
    "setegid",
    # setreuid/setregid are XSI extensions, not POSIX.1 base, but RTMux
    # advertises and implements them, so the matrix tracks them too.
    "setreuid",
    "setregid",
    # Working-directory, symlink, and terminal-query interfaces (POSIX.1 base).
    "getcwd",
    "chdir",
    "readlink",
    "readlinkat",
    "isatty",
    "ttyname",
    "ttyname_r",
    # exec family (POSIX.1 base); success replaces the image, so only the
    # not-found failure path is testable in-process.
    "execl",
    "execle",
    "execlp",
    "execv",
    "execve",
    "execvp",
    # Child-process reaping (POSIX.1 base; sys/wait.h).
    "wait",
    "waitpid",
    "waitid",
    # Standalone utilities (POSIX.1 base): synchronization, timing, option parse.
    "fsync",
    "fdatasync",
    "sync",
    "sleep",
    "getopt",
    # Process identity, session, and job-control interfaces (POSIX.1 base);
    # maturity varies, see STUB_OVERRIDES for getpgrp/setsid/tcgetpgrp/tcsetpgrp.
    "getpid",
    "setpgid",
    "getpgrp",
    "setsid",
    "tcgetpgrp",
    "tcsetpgrp",
    # crypt is an XSI extension (not POSIX.1 base); RTMux implements and uses it
    # for login, so the matrix tracks it.
    "crypt",
    "tcdrain",
    "tcflush",
    "tcflow",
    "tcsendbreak",
    "times",
    "timer_create",
    "timer_delete",
    "timer_getoverrun",
    "timer_gettime",
    "timer_settime",
    "umask",
    "getppid",
    "pthread_cancel",
    "pthread_create",
    "pthread_detach",
    "pthread_equal",
    "pthread_exit",
    "pthread_getconcurrency",
    "pthread_getcpuclockid",
    "pthread_getschedparam",
    "pthread_getspecific",
    "pthread_join",
    "pthread_kill",
    "pthread_sigmask",
    "pthread_mutex_lock",
    "pthread_mutex_timedlock",
    "pthread_mutex_unlock",
    "pthread_mutex_trylock",
    "pthread_cond_destroy",
    "pthread_cond_wait",
    "pthread_cond_init",
    "pthread_cond_timedwait",
    "pthread_cond_signal",
    "pthread_cond_broadcast",
    "pthread_condattr_destroy",
    "pthread_condattr_getclock",
    "pthread_condattr_getpshared",
    "pthread_condattr_init",
    "pthread_condattr_setclock",
    "pthread_condattr_setpshared",
    "pthread_attr_init",
    "pthread_attr_destroy",
    "pthread_attr_getdetachstate",
    "pthread_attr_getguardsize",
    "pthread_attr_getinheritsched",
    "pthread_attr_getschedparam",
    "pthread_attr_getschedpolicy",
    "pthread_attr_getscope",
    "pthread_attr_getstack",
    "pthread_attr_getstackaddr",
    "pthread_attr_getstacksize",
    "pthread_attr_setdetachstate",
    "pthread_attr_setguardsize",
    "pthread_attr_setinheritsched",
    "pthread_attr_setschedparam",
    "pthread_attr_setschedpolicy",
    "pthread_attr_setscope",
    "pthread_attr_setstack",
    "pthread_attr_setstackaddr",
    "pthread_attr_setstacksize",
    "pthread_key_create",
    "pthread_key_delete",
    "pthread_mutex_destroy",
    "pthread_mutex_init",
    "pthread_mutexattr_destroy",
    "pthread_mutexattr_init",
    "pthread_mutexattr_getprioceiling",
    "pthread_mutexattr_getprotocol",
    "pthread_mutexattr_getpshared",
    "pthread_mutexattr_gettype",
    "pthread_mutexattr_setprioceiling",
    "pthread_mutexattr_setprotocol",
    "pthread_mutexattr_setpshared",
    "pthread_mutexattr_settype",
    "pthread_mutex_getprioceiling",
    "pthread_mutex_setprioceiling",
    "pthread_barrier_init",
    "pthread_barrier_destroy",
    "pthread_barrier_wait",
    "pthread_barrierattr_init",
    "pthread_barrierattr_destroy",
    "pthread_barrierattr_getpshared",
    "pthread_barrierattr_setpshared",
    "pthread_rwlock_init",
    "pthread_rwlock_destroy",
    "pthread_rwlock_rdlock",
    "pthread_rwlock_tryrdlock",
    "pthread_rwlock_timedrdlock",
    "pthread_rwlock_wrlock",
    "pthread_rwlock_trywrlock",
    "pthread_rwlock_timedwrlock",
    "pthread_rwlock_unlock",
    "pthread_rwlockattr_init",
    "pthread_rwlockattr_destroy",
    "pthread_rwlockattr_getpshared",
    "pthread_rwlockattr_setpshared",
    "pthread_once",
    "pthread_self",
    "pthread_spin_init",
    "pthread_spin_destroy",
    "pthread_spin_lock",
    "pthread_spin_trylock",
    "pthread_spin_unlock",
    "pthread_setcancelstate",
    "pthread_setcanceltype",
    "pthread_setconcurrency",
    "pthread_setschedparam",
    "pthread_setschedprio",
    "pthread_setspecific",
    "pthread_testcancel",
    "posix_spawn",
    "posix_spawn_file_actions_addclose",
    "posix_spawn_file_actions_adddup2",
    "posix_spawn_file_actions_addopen",
    "posix_spawn_file_actions_destroy",
    "posix_spawn_file_actions_init",
    "posix_spawnattr_destroy",
    "posix_spawnattr_getflags",
    "posix_spawnattr_getpgroup",
    "posix_spawnattr_getschedparam",
    "posix_spawnattr_getschedpolicy",
    "posix_spawnattr_getsigdefault",
    "posix_spawnattr_getsigmask",
    "posix_spawnattr_init",
    "posix_spawnattr_setflags",
    "posix_spawnattr_setpgroup",
    "posix_spawnattr_setschedparam",
    "posix_spawnattr_setschedpolicy",
    "posix_spawnattr_setsigdefault",
    "posix_spawnattr_setsigmask",
    "posix_spawnp",
    "posix_memalign",
    "posix_mem_offset",
    "posix_typed_mem_get_info",
    "posix_typed_mem_open",
    "getpwnam_r",
    "getpwuid_r",
    "getgrnam_r",
    "getgrgid_r",
    "strerror_r",
    "asctime_r",
    "ctime_r",
    # Remaining [TSF] surface: the time/string reentrant pair and the stdio
    # locking primitives + _unlocked accessors.
    "gmtime_r",
    "localtime_r",
    "strtok_r",
    "flockfile",
    "ftrylockfile",
    "funlockfile",
    "getc_unlocked",
    "getchar_unlocked",
    "putc_unlocked",
    "putchar_unlocked",
    "mq_open",
    "mq_close",
    "mq_unlink",
    "mq_send",
    "mq_receive",
    "mq_timedsend",
    "mq_timedreceive",
    "mq_getattr",
    "mq_setattr",
    "sysconf",
}

POSIX_NAMES |= PSE51_REQUIRED_SYMBOLS

# Names that have a libc body but should not be advertised as fully
# implemented because the contract they require is incomplete elsewhere
# in the system.  Each entry forces a status and adds a notes blurb.
STUB_OVERRIDES = {
    "setsid": (
        "implemented",
        "marshals _PROC_SESSION_SETSID to roottask and returns the caller's pid "
        "as the new session id; a process-group leader fails with EPERM "
        "(regression-tested)",
    ),
    "tcgetpgrp": (
        "implemented",
        "queries a terminal's foreground group via DEVCTL_TTY_GET_PGRP; a "
        "non-terminal descriptor fails closed; terminal round-trip is "
        "regression-tested",
    ),
    "tcsetpgrp": (
        "implemented",
        "rejects non-positive process-group ids with EINVAL and non-terminal "
        "descriptors with ENOTTY; the terminal server (roottask for /dev/text, "
        "dev-serial otherwise) then requires the terminal to be the caller's "
        "controlling terminal (ENOTTY) and the group to exist in the caller's "
        "session (EPERM), also for a raw DEVCTL_TTY_SET_PGRP. A background "
        "caller that neither blocks nor ignores SIGTTOU gets SIGTTOU and EINTR, "
        "or EIO when its group is orphaned. Controlling terminals are bound by "
        "DEVCTL_TTY_SET_SID; regression-tested in signals-test.c and vfs.c",
    ),
    "tcflush": (
        "implemented",
        "rejects a bad queue selector (EINVAL), then validates the descriptor "
        "via isatty() (EBADF for a bad fd, ENOTTY for a non-terminal); on a real "
        "terminal a DEVCTL_TTY_FLUSH devctl discards pending input for "
        "TCIFLUSH/TCIOFLUSH (roottask /dev/text drains the console, the serial "
        "driver clears its cooked+raw buffers).  TCOFLUSH is a successful no-op "
        "since terminal output is transmitted synchronously (no queued output)",
    ),
    "tcdrain": (
        "implemented",
        "validates the descriptor via isatty() (EBADF for a bad fd, ENOTTY for "
        "a non-terminal) then blocks via a DEVCTL_TTY_DRAIN devctl until the "
        "driver's transmitter is idle: the roottask /dev/text console writes "
        "synchronously so it returns at once, while the serial drivers poll the "
        "UART (8250 waits on LSR TEMT, SiFive on the TX watermark) until the "
        "FIFO/shift register has drained",
    ),
    "tcflow": (
        "implemented",
        "rejects a bad action with EINVAL and validates the descriptor via "
        "isatty() (EBADF for a bad fd, ENOTTY for a non-terminal).  TCIOFF/"
        "TCION issue DEVCTL_TTY_FLOW and transmit the terminal VSTOP/VSTART "
        "characters; TCOOFF/TCOON issue DEVCTL_TTY_FLOW and succeed as no-ops "
        "on RTMux's synchronous terminals because there is no queued terminal "
        "output to suspend",
    ),
    "tcsendbreak": (
        "implemented",
        "validates the descriptor via isatty() (EBADF for a bad fd, ENOTTY for "
        "a non-terminal) then issues a DEVCTL_TTY_SENDBREAK devctl.  Per POSIX, "
        "a terminal that is not an async serial line takes no action and "
        "succeeds — the roottask /dev/text SBI console does exactly that; the "
        "8250 UART asserts a real LCR break for an implementation-defined "
        "interval, while the SiFive UART (no break-control bit) reports ENOSYS",
    ),
    "fsync": (
        "implemented",
        "marshals _IO_SYNC; a bad fd is EBADF.  Every native filesystem is "
        "RAM-backed (read-only romfs/cpiofs/capfs, mmap-backed shmem), so there "
        "is no slower durable tier to flush and their sync handlers return 0; "
        "lkl-fsd performs a real sync.  A char device or pipe without a sync "
        "handler still returns ENOSYS",
    ),
    "sync": (
        "implemented",
        "asks roottask to sweep every mounted filesystem server with "
        "DEVCTL_FS_SYNC (a server that does not know it answers ENOSYS, which "
        "counts as flushed), under one 2 s bound for the whole sweep; sync() "
        "has no failure indication, so a refused or timed-out sweep is silent "
        "(roottask logs it).  Only lkl-fsd has a durable tier to flush.  "
        "shutdown and reboot run the same sweep before powering off",
    ),
    "fdatasync": (
        "implemented",
        "marshals _IO_SYNC; a bad fd is EBADF.  Every native filesystem is "
        "RAM-backed (read-only romfs/cpiofs/capfs, mmap-backed shmem), so there "
        "is no slower durable tier to flush and their sync handlers return 0; "
        "lkl-fsd performs a real sync.  A char device or pipe without a sync "
        "handler still returns ENOSYS",
    ),
    "openat": (
        "implemented",
        "AT_FDCWD, absolute paths, and a real dirfd with a relative path all "
        "resolve (the relative path is joined against the directory the fd was "
        "opened as; libc tracks fd->path and lazily seeds inherited dirfds from "
        "_IO_FDINFO, regression-tested in vfs.c)",
    ),
    "fstatat": (
        "implemented",
        "AT_FDCWD, absolute paths, and a real dirfd with a relative path all "
        "resolve (relative joined against the fd's directory; libc fd->path "
        "tracking plus _IO_FDINFO for inherited dirfds, regression-tested)",
    ),
    "faccessat": (
        "implemented",
        "AT_FDCWD, absolute paths, and a real dirfd with a relative path all "
        "resolve (AT_EACCESS selects effective ids, the default uses real ids "
        "like access; AT_SYMLINK_NOFOLLOW checks the link itself; libc fd->path "
        "tracking plus _IO_FDINFO for inherited dirfds, regression-tested)",
    ),
    "unlinkat": (
        "partial",
        "AT_FDCWD, absolute paths, and a real dirfd with a relative path all "
        "resolve and delegate to unlink (libc fd->path tracking plus _IO_FDINFO "
        "for inherited dirfds, regression-tested). Missing paths fail ENOENT, "
        "bad relative dirfds fail EBADF, and undefined flags fail EINVAL. "
        "AT_REMOVEDIR classifies targets before failing closed: missing paths "
        "fail ENOENT, non-directory targets fail ENOTDIR and are preserved, and "
        "directories fail ENOSYS because RTMux provides no native directory "
        "removal",
    ),
    "fcntl": (
        "implemented",
        "fd-management commands (F_DUPFD/F_DUPFD_CLOEXEC/F_GETFD/F_SETFD/"
        "F_GETFL/F_SETFL) work; record locking works for BYTE-RANGE and "
        "whole-file locks on roottask-served /dev/shmem files "
        "(F_GETLK/F_SETLK/F_SETLKW/F_UNLCK, overlap conflict, range "
        "split/merge, per-process ownership, release-on-close; F_SETLKW blocks "
        "on a conflict and is granted when the holder releases; roottask/devmem "
        "mem_lock). Descriptor access checks fail EBADF. Read-only image "
        "filesystems intentionally provide no advisory-lock state and fail "
        "closed with ENOSYS; optional EDEADLK deadlock detection is not "
        "implemented",
    ),
    "fpathconf": (
        "implemented",
        "answers the system-invariant names from libc (_PC_NAME_MAX/PATH_MAX/"
        "LINK_MAX/SYMLOOP_MAX/NO_TRUNC/VDISABLE/CHOWN_RESTRICTED/ASYNC_IO/"
        "PRIO_IO/SYNC_IO/FILESIZEBITS/SYMLINK_MAX) and forwards _PC_PIPE_BUF "
        "to the pipe service (non-pipe falls back to PIPE_BUF); bad fd is "
        "EBADF, an unknown name is EINVAL; valid names with no fixed value and "
        "no service answer (MAX_CANON/MAX_INPUT/ALLOC_SIZE_MIN/REC_*) return "
        "-1 with errno left clear",
    ),
    "pathconf": (
        "implemented",
        "resolves the path with access(F_OK) -- search permission only, no "
        "read-open, so no spurious EACCES and no device/server open side "
        "effect -- then answers the system-invariant names from libc; the "
        "service-specific names have no path-based handler (no FS _IO_PATHCONF, "
        "no named FIFOs) and fall back to the fpathconf defaults or report "
        "indeterminate values; an unknown name is EINVAL and an unresolvable "
        "path fails with the "
        "access() errno",
    ),
    "pthread_mutexattr_setprotocol": (
        "implemented",
        "attribute round-trip (all 3 protocols, invalid EINVAL); PRIO_INHERIT "
        "enforced by kernel turnstiles (pi-inversion-xfail passes); PRIO_PROTECT "
        "boost/deboost enforced, out-of-order nested release drops the boost to "
        "the next-highest held ceiling (52130762), and the ceiling now lives in "
        "its own count bits disjoint from the recursion depth (9c395eb1), so the "
        "ceiling-violation EINVAL is read correctly (no window, no ceiling-1 "
        "flip) and PRIO_PROTECT + RECURSIVE mutexes recurse per POSIX.  All "
        "regression-tested in sync.c",
    ),
    "pthread_mutexattr_getprotocol": (
        "implemented",
        "attribute round-trip (all 3 protocols); see "
        "pthread_mutexattr_setprotocol for enforcement status",
    ),
    "pthread_mutexattr_setprioceiling": (
        "implemented",
        "attribute ceiling storage for PRIO_PROTECT mutex creation",
    ),
    "pthread_mutexattr_getprioceiling": (
        "implemented",
        "attribute round-trip only; see pthread_mutexattr_setprioceiling",
    ),
    "pthread_mutex_setprioceiling": (
        "implemented",
        "updates the ceiling for unlocked PRIO_PROTECT mutexes; returns "
        "EINVAL for non-ceiling mutexes",
    ),
    "pthread_mutex_getprioceiling": (
        "implemented",
        "returns the ceiling for PRIO_PROTECT mutexes",
    ),
    "pthread_mutex_timedlock": (
        "implemented",
        "uses CLOCK_REALTIME absolute timeout for contended mutexes",
    ),
    "pthread_mutexattr_getpshared": (
        "implemented",
        "process-shared mutexes are physical-address keyed; shared PI/PP "
        "mutex creation still fails closed",
    ),
    "pthread_mutexattr_setpshared": (
        "implemented",
        "accepts PTHREAD_PROCESS_PRIVATE and PTHREAD_PROCESS_SHARED",
    ),
    "pthread_getconcurrency": (
        "implemented",
        "process-local hint storage only; scheduler ignores the hint",
    ),
    "pthread_setconcurrency": (
        "implemented",
        "process-local hint storage only; scheduler ignores the hint",
    ),
    "posix_spawn_file_actions_addopen": (
        "implemented",
        "open actions are materialized before the spawn request using "
        "close-on-exec temporary descriptors, then reduced to the roottask "
        "close/dup2 fd-map path; success, failure-before-spawn, descriptor "
        "replacement, open/dup2/close ordering, identity temp fd handling, and "
        "close-on-exec final-state ordering for addopen plus unrelated high "
        "inherited parent descriptors within the default RTMux fd scan bound "
        "are regression-tested",
    ),
    "posix_spawnattr_setflags": (
        "implemented",
        "stores supported spawn flags, including POSIX_SPAWN_RESETIDS; "
        "RESETIDS resets child effective ids to real ids unless setuid/setgid "
        "file bits override them",
    ),
    "poll": (
        "implemented",
        "non-blocking and blocking readiness via _IO_NOTIFY; POLLIN/POLLOUT/"
        "POLLHUP/POLLERR; timed and indefinite waits (no POLLPRI: no "
        "out-of-band source)",
    ),
    "pselect": (
        "implemented",
        "select() with a timespec, POSIX nfds/timeout validation, and an atomic "
        "temporary signal-mask swap around the poll-backed wait under RTMux's "
        "process-wide signal-mask model; timeout, ready-fd, signal-interrupt, "
        "and descriptor-error paths restore the caller mask",
    ),
    "select": (
        "implemented",
        "read/write readiness and timeout over poll(); exceptfds maps to POLLPRI, "
        "which no service reports, so it is always empty (as on a pipe)",
    ),
    "pthread_testcancel": (
        "implemented",
        "exits with PTHREAD_CANCELED when cancellation is pending and enabled",
    ),
    "pthread_cancel": (
        "partial",
        "requests cancellation; bounded child-process regressions cover "
        "pthread_testcancel, disabled-then-enabled delivery, asynchronous "
        "delivery, sem_wait, pthread_cond_wait, pthread_cond_timedwait, "
        "nanosleep, and pthread_join. The broader POSIX cancellation-point "
        "catalog is not claimed unless a row names a tested point.",
    ),
    "pthread_condattr_getpshared": (
        "implemented",
        "process-shared condition variables are physical-address keyed",
    ),
    "pthread_condattr_setpshared": (
        "implemented",
        "accepts PTHREAD_PROCESS_PRIVATE and PTHREAD_PROCESS_SHARED",
    ),
    "sched_getparam": (
        "implemented",
        "supports pid 0/current process; live and stale foreign pids fail closed "
        "with ENOSYS for RTMux's current-thread scheduling model",
    ),
    "sched_getscheduler": (
        "implemented",
        "supports pid 0/current process; live and stale foreign pids fail closed "
        "with ENOSYS for RTMux's current-thread scheduling model",
    ),
    "sched_setparam": (
        "implemented",
        "supports pid 0/current process; live and stale foreign pids fail closed "
        "with ENOSYS for RTMux's current-thread scheduling model",
    ),
    "sched_setscheduler": (
        "implemented",
        "supports pid 0/current process; live and stale foreign pids fail closed "
        "with ENOSYS for RTMux's current-thread scheduling model",
    ),
    "sched_rr_get_interval": (
        "implemented",
        "supports pid 0/current process; live and stale foreign pids fail closed "
        "with ENOSYS for RTMux's current-thread scheduling model",
    ),
}

DESIGN_NOTES = {
    "crypt": "XSI; SHA-512 modular form ($6$salt$hash) only (from FreeBSD)",
    "pthread_attr_setscope": "PTHREAD_SCOPE_SYSTEM only; PTHREAD_SCOPE_PROCESS is "
    "correctly ENOTSUP and an unknown scope is EINVAL (RTMux threads are 1:1 "
    "system-scope)",
    "kill": "signal delivery is notification-backed through roottask; roottask "
    "checks sender credentials (uid match, root, or SIGCONT in one session) "
    "and supports pid, pgrp, and broadcast targets",
    "raise": "runs the registered handler synchronously in the calling thread",
    "sigaction": "signal handlers are notification-backed and intentionally scoped; "
    "a caught signal installed without SA_RESTART interrupts one blocked call "
    "(the lowest-tid registered waiter) with EINTR, SA_RESTART interrupts none "
    "(RTMUX_TCTL_INTERRUPT, signals-test.c eintr_*/restart_read)",
    "signal": "signal handlers are notification-backed and intentionally scoped; "
    "signal() installs without SA_RESTART, so a caught signal interrupts a "
    "blocked call with EINTR",
    "sigprocmask": "signal masks retained for POSIX signal APIs",
    "waitpid": "returns -1/EINTR for a caught signal (signals-test.c eintr_waitpid)",
    "pthread_cond_timedwait": "a caught signal is a spurious wakeup returning 0, never EINTR",
    "pthread_cond_wait": "a caught signal is a spurious wakeup returning 0, never EINTR (signals-test.c eintr_cond_wait)",
    "sem_timedwait": "returns -1/EINTR for a caught signal on unnamed semaphores",
    "sem_wait": "returns -1/EINTR for a caught signal (signals-test.c eintr_sem_wait, eintr_stress)",
    "sleep": "returns the unslept seconds for a caught signal (signals-test.c eintr_sleep)",
    "clock_nanosleep": "returns EINTR (relative sleeps set the remainder) for a caught signal",
    "nanosleep": "returns EINTR with the remainder set for a caught signal (signals-test.c eintr_nanosleep)",
    "write": "blocked writes return EINTR for a caught signal without SA_RESTART",
    "read": "blocked reads return EINTR for a caught signal without SA_RESTART (signals-test.c eintr_read_pipe, restart_read)",
}

# Function names matching these patterns in include/ but not in POSIX_NAMES
# are flagged as drift.  Edit the curated list (and STUB_OVERRIDES when
# appropriate) when a new POSIX entry point lands.
POSIX_PATTERNS = [
    re.compile(r"^pthread_[a-z0-9_]+$"),
    re.compile(r"^posix_[a-z0-9_]+$"),
    re.compile(r"^sig[a-z0-9_]+$"),
    re.compile(r"^sched_[a-z0-9_]+$"),
    re.compile(r"^sem_[a-z0-9_]+$"),
    re.compile(r"^aio_[a-z0-9_]+$"),
    re.compile(r"^mq_[a-z0-9_]+$"),
    re.compile(r"^clock_[a-z0-9_]+$"),
    re.compile(r"^timer_[a-z0-9_]+$"),
]

def read(rel):
    return (ROOT / rel).read_text(encoding="utf-8")


def strip_c_comments(text):
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    text = re.sub(r"//.*", "", text)
    return text


def header_files():
    return posix_header_files()


# Regex fragments shared by the POSIX_NAMES scanners.  `_DECL_PREFIX` matches the
# return-type slop in front of a header-style declaration (`extern` optional);
# `_DEF_PREFIX` matches the return-type slop in front of a definition body.
_DECL_PREFIX = r"(?:^|[;\n}])\s*(?:extern\s+)?[A-Za-z_][\w\s\*]*"
_DEF_PREFIX = r"(?:^|[;\n}])\s*(?:__attribute__\s*\(\([^)]*\)\)\s*)*[A-Za-z_][\w\s\*]*"


def _compile_name_patterns(prefix, name, terminator):
    """Build the (plain, wrapped-with-parens, function-pointer) patterns that
    detect `name` as a declaration (terminator=`;`) or definition (`{`).
    `wrapped` matches libc's `int(foo)(...)` idiom; harmless for header scans."""
    esc = re.escape(name)
    return [
        re.compile(prefix + r"\b" + esc + r"\s*\([^;{}]*\)\s*" + terminator, re.S),
        re.compile(prefix + r"\(" + esc + r"\)\s*\([^;{}]*\)\s*" + terminator, re.S),
        re.compile(prefix + r"\(\*\s*" + esc + r"\s*\([^;{}]*\)\s*\)", re.S),
    ]


def _scan_posix_names(paths, prefix, terminator, preprocess):
    """For each `path` in `paths`, search `preprocess(text)` for every name in
    POSIX_NAMES and collect the paths that match."""
    name_patterns = {name: _compile_name_patterns(prefix, name, terminator)
                     for name in POSIX_NAMES}
    out = collections.defaultdict(set)
    for path in paths:
        text = preprocess(path.read_text(encoding="utf-8", errors="ignore"))
        rel = path.relative_to(ROOT).as_posix()
        for name, patterns in name_patterns.items():
            if any(p.search(text) for p in patterns):
                out[name].add(rel)
    return {name: sorted(paths) for name, paths in out.items()}


def scan_exported_functions():
    out = _scan_posix_names(header_files(), _DECL_PREFIX, ";", strip_c_comments)
    return {name: sorted(set(paths)) for name, paths in out.items()}


# Every function these headers declare must be a tracked matrix interface.
# They carry the non-prefixed names (open/read/fpathconf/...) that the
# prefix-based POSIX_PATTERNS drift check cannot see, which is exactly how such
# interfaces once slipped the matrix.  chroot is a legacy non-POSIX call RTMux
# declares but does not present as a POSIX interface.
COVERAGE_HEADERS = [
    "include/unistd.h",
    "include/fcntl.h",
    "include/dirent.h",
    "include/sys/stat.h",
    "include/sys/wait.h",
    "include/sys/utsname.h",
]
COVERAGE_EXCLUDE = {"chroot", "issetugid"}  # issetugid: BSD, not POSIX


def validate_header_coverage():
    """Return sorted (name, header) for a function declared in a coverage
    header that is neither tracked in POSIX_NAMES nor explicitly excluded."""
    decl_re = re.compile(_DECL_PREFIX + r"\b([A-Za-z_]\w*)\s*\([^;{}]*\)\s*;", re.S)
    missing = set()
    for rel in COVERAGE_HEADERS:
        text = strip_c_comments(read(rel))
        for m in decl_re.finditer(text):
            name = m.group(1)
            if name not in POSIX_NAMES and name not in COVERAGE_EXCLUDE:
                missing.add((name, rel))
    return sorted(missing)


def scan_drift_candidates():
    """Find POSIX-shaped declarations in include/ not in POSIX_NAMES."""
    decl_re = re.compile(
        _DECL_PREFIX + r"\b([A-Za-z_]\w*)\s*\([^;{}]*\)\s*;",
        re.S,
    )
    seen = collections.defaultdict(set)
    for path in header_files():
        text = strip_c_comments(path.read_text(encoding="utf-8", errors="ignore"))
        rel = path.relative_to(ROOT).as_posix()
        for m in decl_re.finditer(text):
            name = m.group(1)
            if name in POSIX_NAMES:
                continue
            if any(p.match(name) for p in POSIX_PATTERNS):
                seen[name].add(rel)
    return {name: sorted(paths) for name, paths in seen.items()}


def scan_libc_defs():
    c_sources = list((ROOT / "lib/libc").rglob("*.c"))
    out = _scan_posix_names(c_sources, _DEF_PREFIX, r"\{", strip_c_comments)
    for path in c_sources:
        text = strip_c_comments(path.read_text(encoding="utf-8", errors="ignore"))
        rel = path.relative_to(ROOT).as_posix()
        for name in POSIX_NAMES:
            if name not in out and re.search(
                r"\b__weak_reference\s*\([^,()]+,\s*" +
                re.escape(name) + r"\s*\)",
                text,
            ):
                out.setdefault(name, []).append(rel)
    for path in header_files():
        text = path.read_text(encoding="utf-8", errors="ignore")
        rel = path.relative_to(ROOT).as_posix()
        for name in POSIX_NAMES:
            if name not in out and re.search(
                r"^\s*#\s*define\s+" + re.escape(name) + r"\s*\(", text, re.M
            ):
                out.setdefault(name, []).append(rel)
    # Assembly stubs reference POSIX names without C syntax; a bare word match
    # is enough to flag the file as carrying an implementation.
    name_res = {name: re.compile(r"\b" + re.escape(name) + r"\b")
                for name in POSIX_NAMES}
    asm_hits = collections.defaultdict(set)
    for path in (ROOT / "lib/libc").rglob("*.S"):
        text = path.read_text(encoding="utf-8", errors="ignore")
        rel = path.relative_to(ROOT).as_posix()
        for name, name_re in name_res.items():
            if name_re.search(text):
                asm_hits[name].add(rel)
    for name, paths in asm_hits.items():
        out[name] = sorted(set(out.get(name, [])) | paths)
    return out


def scan_stub_reasons():
    text = read("lib/libc/rtmux/posix-stubs.c")
    out = {}
    fn_re = re.compile(r"^\s*[A-Za-z_][\w\s\*]*\s+([A-Za-z_]\w*)\s*\([^;{}]*\)\s*\{", re.M)
    matches = list(fn_re.finditer(text))
    for i, m in enumerate(matches):
        name = m.group(1)
        if name not in POSIX_NAMES:
            continue
        comment_start = text.rfind("/*", 0, m.start())
        prev_end = matches[i - 1].end() if i else 0
        if comment_start < prev_end:
            comment_start = m.start()
        chunk_end = matches[i + 1].start() if i + 1 < len(matches) else len(text)
        chunk = text[comment_start:chunk_end]
        reason = []
        if "TODO:" in chunk:
            todo = re.search(r"TODO:\s*((?:.|\n)*?)(?:\n\s*\*/|\n\s*For now:)", chunk)
            if todo:
                note = re.sub(r"\n\s*\*\s*", " ", todo.group(1))
                note = re.sub(r"\s+", " ", note).strip(" .")
                note = note.split(". For now", 1)[0]
                reason.append("TODO: " + note)
        fail_closed = "ENOSYS" in chunk
        if fail_closed:
            reason.append("returns ENOSYS")
        if not fail_closed and ("no-op" in chunk or "For now" in chunk):
            reason.append("placeholder semantics")
        out[name] = "; ".join(reason) or "placeholder in posix-stubs.c"
    return out


def scan_opts_pse51_manifest():
    out = {}
    if not OPTS_PSE51_MANIFEST.is_file():
        return out
    rel = OPTS_PSE51_MANIFEST.relative_to(ROOT).as_posix()
    for lineno, line in enumerate(
        OPTS_PSE51_MANIFEST.read_text(encoding="utf-8").splitlines(), 1
    ):
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        fields = line.split()
        if len(fields) >= 3:
            out.setdefault(fields[0], []).append((fields[1], f"{rel}:{fields[1]}"))
    return out


def scan_opts_pse51_exclusions():
    out = {}
    if not OPTS_PSE51_EXCLUSIONS.is_file():
        return out
    rel = OPTS_PSE51_EXCLUSIONS.relative_to(ROOT).as_posix()
    for lineno, line in enumerate(
        OPTS_PSE51_EXCLUSIONS.read_text(encoding="utf-8").splitlines(), 1
    ):
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        fields = line.split(None, 2)
        if len(fields) != 3:
            out.setdefault("", []).append(
                (f"line-{lineno}", f"{rel}:{lineno}: malformed row")
            )
            continue
        iface, tag, reason = fields
        out.setdefault(iface, []).append((tag, reason))
    return out


def scan_tests():
    out = {}
    for path in (ROOT / "tests/userland").rglob("*.c"):
        text = path.read_text(encoding="utf-8", errors="ignore")
        names = set()
        for name in POSIX_NAMES:
            if re.search(r"\b" + re.escape(name) + r"\s*\(", text):
                names.add(name)
        for name in names:
            out.setdefault(name, []).append(path.relative_to(ROOT).as_posix())
    for name, rows in scan_opts_pse51_manifest().items():
        for _, cite in rows:
            out.setdefault(name, []).append(cite)
    rel = OPTS_PSE51_EXCLUSIONS.relative_to(ROOT).as_posix()
    for name, rows in scan_opts_pse51_exclusions().items():
        for tag, _ in rows:
            out.setdefault(name, []).append(f"{rel}:{tag}")
    return out


def scan_sysconf_options():
    text = read("lib/libc/posix/sysconf.c")
    advertised = advertised_posix_options()
    rows = []
    for m in re.finditer(r"_SC_([A-Z0-9_]+).*?,\s*_POSIX_([A-Z0-9_]+)", text):
        if "_POSIX_" + m.group(2) in advertised:
            rows.append((m.group(1), m.group(2)))
    return sorted(set(rows))


OPTIONS_PROBE = "tests/userland/posix-options.c"

# Realtime mutex-protocol options are evidenced by measurement plus the PI/PC
# design, not a runtime boolean (§12): a symbol being present says nothing about
# whether priority inheritance/ceiling actually bounds inversion.  Each maps the
# option (without the `_POSIX_` prefix) to the artifacts that evidence it, as
# (path, anchor) pairs — the anchor is a substring that must still be present, so
# a citation to a renamed row or deleted test fails the gate instead of rotting.
RT_EVIDENCED = {
    "THREAD_PRIO_INHERIT": [
        ("docs/pi-pc-design.md", None),
        ("docs/rt-budgets.md", "Contended PRIO_INHERIT mutex wake"),
        ("tests/userland/rt-latency/pi-inversion-xfail.c", None),
    ],
    "THREAD_PRIO_PROTECT": [
        ("docs/pi-pc-design.md", None),
        ("docs/rt-budgets.md", "Priority-inheritance boost span"),
        ("tests/userland/sync.c", "prio_protect_required_pass"),
    ],
}

SYS_CONF_EVIDENCED = {
    "FSYNC": [
        ("userland/init", "/bin/sync /fs/root/copy.txt"),
        ("userland/utils/toolbox.c", "fdatasync(fd)"),
        ("servers/lkl-fsd/main.c", "lkl_sys_fsync"),
    ],
    "SYNCHRONIZED_IO": [
        ("userland/init", "/bin/sync /fs/root/copy.txt"),
        ("userland/utils/toolbox.c", "fdatasync(fd)"),
        ("servers/lkl-fsd/main.c", "lkl_sys_fsync"),
    ],
}


def rt_citation(option):
    """The `evidenced (...)` status string for an RT-evidenced option."""
    return "evidenced (" + ", ".join(rel for rel, _ in RT_EVIDENCED[option]) + ")"


def validate_rt_evidence():
    """RT-evidenced options whose cited artifact is missing or no longer holds
    its anchor text; keeps an `evidenced` row from surviving a rename/delete."""
    errors = []
    for option, cites in sorted(RT_EVIDENCED.items()):
        for rel, anchor in cites:
            path = ROOT / rel
            if not path.exists():
                errors.append(f"_POSIX_{option}: cited {rel} is missing")
            elif anchor and anchor not in path.read_text(
                encoding="utf-8", errors="ignore"
            ):
                errors.append(f"_POSIX_{option}: {rel} lost anchor '{anchor}'")
    return errors


def sysconf_citation(option):
    return "evidenced (" + ", ".join(rel for rel, _ in SYS_CONF_EVIDENCED[option]) + ")"


def validate_sysconf_evidence():
    errors = []
    for option, cites in sorted(SYS_CONF_EVIDENCED.items()):
        for rel, anchor in cites:
            path = ROOT / rel
            if not path.exists():
                errors.append(f"_POSIX_{option}: cited {rel} is missing")
            elif anchor not in path.read_text(encoding="utf-8", errors="ignore"):
                errors.append(f"_POSIX_{option}: {rel} lost anchor '{anchor}'")
    return errors


def probed_options():
    """Map each `_SC_*` the runtime probe verifies to the `_POSIX_*` it checks
    it against.  Only a real `sysconf(_SC_X) == _POSIX_Y` assertion counts —
    comments and string literals are stripped first — so a bare mention in a
    comment or a `check()` label cannot over-claim verification.  The probe runs
    under syscall_testing, so a broken assertion fails the gate."""
    path = ROOT / OPTIONS_PROBE
    if not path.exists():
        return {}
    text = strip_c_comments(path.read_text(encoding="utf-8"))
    text = re.sub(r'"(?:\\.|[^"\\])*"', '""', text)  # drop string literals
    out = {}
    for m in re.finditer(
        r"sysconf\(\s*_SC_([A-Z0-9_]+)\s*\)\s*==\s*_POSIX_([A-Z0-9_]+)", text
    ):
        out[m.group(1)] = m.group(2)
    return out


def parse_porting_omissions(text):
    """Rows of the "Omitted interfaces" table in docs/porting.md, keyed by name.

    scripts/check-porting-contract.sh reads the same table to prove each
    interface stays absent from libc."""
    rows = {}
    in_section = False
    for line in text.splitlines():
        if line.startswith("## "):
            in_section = line.strip() == "## Omitted interfaces"
            continue
        if not in_section or not line.startswith("|"):
            continue
        cells = [c.strip() for c in line.strip().strip("|").split("|")]
        m = re.fullmatch(r"`([A-Za-z_][A-Za-z_0-9]*)`", cells[0])
        if len(cells) != 4 or not m:
            continue
        rows[m.group(1)] = {
            "alternative": cells[1],
            "outcome": cells[2],
            "fixture": cells[3].strip("`"),
        }
    return rows


def scan_porting_omissions():
    return parse_porting_omissions(PORTING_DOC.read_text(encoding="utf-8"))


def status_for(name, exported, defs, stubs, tests=None, omissions=()):
    if name in omissions:
        return "not-supported"
    if name in STUB_OVERRIDES:
        status = STUB_OVERRIDES[name][0]
        if status == "implemented" and tests is not None and not tests.get(name):
            return "untested"
        return status
    if name in stubs:
        return "stub"
    if name in defs:
        if tests is not None and not tests.get(name):
            return "untested"
        return "implemented"
    if name not in exported:
        return "missing"
    return "declared-only"


# "not-supported" (an interface omitted by design, see docs/porting.md) is
# deliberately absent: it is a declared outcome, not an incomplete row.
BAD_CLOSURE_STATUS = {"declared-only", "missing", "partial", "stub", "untested"}


def profile_closure(rows):
    bad_by_opt = collections.defaultdict(list)
    for row in rows:
        for opt in row["opts"]:
            if row["status"] in BAD_CLOSURE_STATUS:
                bad_by_opt[opt].append(f"{row['name']}:{row['status']}")

    closure = {}
    for profile, opts in sorted(POSIX_PROFILES.items()):
        claimed = sorted(opt for opt in opts if not bad_by_opt.get(opt))
        blocked = {
            opt: sorted(bad_by_opt[opt])
            for opt in sorted(opts)
            if bad_by_opt.get(opt)
        }
        closure[profile] = (claimed, blocked)
    return closure


def option_index():
    """Map each function to the sorted `_POSIX_*` option(s) that require it,
    inverted from OPTION_REQUIREMENTS.  Only the mapped option groups appear
    here; a function that belongs to an unmapped option (a tracked gap) or to
    no option is simply absent, and the matrix shows `-` rather than guessing
    `base`."""
    idx = collections.defaultdict(set)
    for opt, syms in OPTION_REQUIREMENTS.items():
        for s in syms:
            idx[s].add(opt)
    return {name: sorted(opts) for name, opts in idx.items()}


def min_profile(name, opt_index):
    """The smallest build profile(s) that include `name`: the minimal elements,
    in the partial order minimal < {realtime, lkl} < full, of the profiles that
    advertise an option requiring the interface.  Returns `-` when the interface
    is not gated by any profiled option (always available, or its option is not
    advertised in any profile)."""
    opts = set(opt_index.get(name, []))
    if not opts:
        return "-"
    including = [p for p in ("minimal", "realtime", "lkl", "full")
                if POSIX_PROFILES[p] & opts]
    if not including:
        return "-"
    if "minimal" in including:
        return "minimal"
    mids = [p for p in ("realtime", "lkl") if p in including]
    return ", ".join(mids) if mids else "full"


def generate(drift):
    exported = scan_exported_functions()
    defs = scan_libc_defs()
    stubs = scan_stub_reasons()
    tests = scan_tests()
    opts_exclusions = scan_opts_pse51_exclusions()
    sysconf_rows = scan_sysconf_options()
    omissions = scan_porting_omissions()
    profile_opts = set().union(*POSIX_PROFILES.values())
    required = {
        s
        for opt, syms in OPTION_REQUIREMENTS.items()
        if opt in profile_opts
        for s in syms
    }
    names = sorted(set(POSIX_NAMES) | required | set(exported) | set(defs) |
                   set(stubs) | set(omissions))

    lines = [
        "# RTMux POSIX Compliance Matrix",
        "",
        "<!-- Generated by scripts/gen-posix-matrix.py; edit the generator, not this table. -->",
        "",
        "This matrix is a drift-detection snapshot.  It distinguishes implemented",
        "interfaces, untested libc bodies, partial implementations (libc surface",
        "exists but the kernel contract is incomplete), temporary stubs, and",
        "interfaces RTMux intentionally removed from the POSIX surface.  Signal",
        "constants and POSIX signal data",
        "types remain in `include/signal.h`, while process notification uses notifications.",
        "",
        "## Function Surface",
        "",
        "The Option group column attributes each interface to the POSIX option "
        "that requires it (`-` for base interfaces or ones whose option is not "
        "yet mapped in the ledger).  The Min profile column names the smallest "
        "build profile that includes it (see Build Profiles below); `realtime, "
        "lkl` marks an interface reachable from either of those incomparable "
        "profiles, and `-` an interface no profiled option gates.",
        "",
        "| Interface | Option group | Min profile | Status | Exported from |"
        " Implementation | Tests | Notes |",
        "| --- | --- | --- | --- | --- | --- | --- | --- |",
    ]

    opt_index = option_index()
    function_rows = []
    for name in names:
        opts = opt_index.get(name, [])
        group = "<br>".join(opts) or "-"
        profile = min_profile(name, opt_index)
        status = status_for(name, exported, defs, stubs, tests, omissions)
        hdr = "<br>".join(sorted(exported.get(name, []))) or "-"
        impl = "<br>".join(sorted(defs.get(name, []))) or "-"
        test = "<br>".join(sorted(tests.get(name, []))) or "-"
        if name in omissions:
            test = omissions[name]["fixture"]
        function_rows.append({
            "name": name,
            "opts": opts,
            "status": status,
            "test": test,
        })
        notes = []
        if name in DESIGN_NOTES:
            notes.append(DESIGN_NOTES[name])
            # Cite the PSE51-docs semantic page only on these divergence rows
            # (notification-backed signals), not every row.  The path is built
            # statically so generation stays deterministic without the external
            # oracle checked out; validate_pse51_docs() enforces it really exists.
            if name in PSE51_REQUIRED_SYMBOLS and name not in PSE51_NO_PAGE:
                notes.append(f"semantics: {pse51_citation_path(name)}")
        if name in STUB_OVERRIDES:
            notes.append(STUB_OVERRIDES[name][1])
        if name in stubs:
            notes.append(stubs[name])
        if name in opts_exclusions:
            reason = "; ".join(reason for _, reason in opts_exclusions[name])
            notes.append(f"OPTS PSE51 exclusion: {reason}")
        if name in omissions:
            om = omissions[name]
            notes.append(f"omitted by design; alternative: {om['alternative']}; "
                         f"expected: {om['outcome']}")
        if status == "untested":
            notes.append("libc body exists, but no conformance test cites this interface")
        note = "; ".join(notes)
        if status in {"stub", "partial"} and test == "-":
            note = (note + "; " if note else "") + "missing expected-fail/skip coverage"
        lines.append(
            f"| `{name}` | {group} | {profile} | {status} | {hdr} | {impl} | "
            f"{test} | {note or '-'} |")

    lines += [
        "",
        "## `sysconf()` POSIX Options",
        "",
        "`lib/libc/posix/sysconf.c` currently mirrors compile-time `_POSIX_*` macros",
        "into runtime `_SC_*` values for the following option groups.  A `verified`",
        f"row has a runtime probe in `{OPTIONS_PROBE}` asserting both the reported",
        "value and the option's behavior; an `evidenced` row is backed by a",
        "specific runtime gate outside the single-process options probe;",
        "`needs-verification` rows are still self-declared claims awaiting a probe,",
        "a measurement, or an explicit downgrade.",
        "",
        "| `_SC_*` option | Compile-time macro | Matrix status |",
        "| --- | --- | --- |",
    ]
    probed = probed_options()
    for sc, posix in sysconf_rows:
        if probed.get(sc) == posix:
            status = f"verified ({OPTIONS_PROBE})"
        elif posix in RT_EVIDENCED:
            status = rt_citation(posix)
        elif posix in SYS_CONF_EVIDENCED:
            status = sysconf_citation(posix)
        elif posix in {"REALTIME_SIGNALS"}:
            status = "verify-notification-backed-signal-semantics"
        else:
            status = "needs-verification"
        lines.append(f"| `_SC_{sc}` | `_POSIX_{posix}` | {status} |")

    if drift:
        lines += [
            "",
            "## Drift Detected",
            "",
            "The following POSIX-shaped function declarations were found in",
            "`include/` but are not classified by this matrix.  Add them to",
            "`POSIX_NAMES` (and `STUB_OVERRIDES` if the libc body does not yet",
            "match the kernel contract).  This section disappears when no drift",
            "remains.",
            "",
            "| Symbol | Header(s) |",
            "| --- | --- |",
        ]
        for name in sorted(drift):
            paths = "<br>".join(drift[name])
            lines.append(f"| `{name}` | {paths} |")

    lines += [
        "",
        "## External Conformance Runner",
        "",
        "`scripts/run-posix-conformance.sh --summary build/posix-summary.json`",
        "wraps the adopted out-of-tree Open POSIX Test Suite PSE51 subset.  The",
        "GPL suite checkout stays outside the Apache tree under",
        "`externals/open-posix-test-suite`; the wrapper records only summarized",
        "JSON evidence: suite name/version, command, manifest, pass/fail counts,",
        "unsupported profile exclusions, and log location.",
        "",
        "## Native Filesystem Divergences",
        "",
        "Several POSIX regular-file behaviors do not apply to RTMux's native",
        "objects and are covered by the LKL-backed (ext4) path instead, not by a",
        "native regression:",
        "",
        "- The only native writable object is `/dev/shmem` (a fixed-extent",
        "  memory object).  A write past the end returns `ENOSYS` rather than",
        "  extending the object, so `O_APPEND`-grows-the-file, sparse-hole",
        "  zero-fill, and `ftruncate`-grow are not native semantics.",
        "- No native directory-mutating API is provided: `mkdir`, `rmdir`, and",
        "  `rename`/`renameat` are absent, and `unlinkat(AT_REMOVEDIR)` returns",
        "  `ENOSYS`, so `rmdir`-non-empty and rename-into-own-descendant have no",
        "  native form (plain `unlink`/`unlinkat` file removal does exist).",
        "  `ftruncate` and `dup3` are also absent,",
        "  and `O_DIRECTORY`/`O_NOFOLLOW` are not defined, so none are advertised.",
        "- `fsync`/`fdatasync` now succeed on native objects: every native",
        "  filesystem is RAM-backed (read-only romfs/cpiofs/capfs, mmap-backed",
        "  shmem) with no slower durable tier, so their sync handlers return 0.",
        "- `fcntl` record locking is native for `/dev/shmem`; read-only image",
        "  filesystems intentionally keep no advisory-lock state and fail closed",
        "  with `ENOSYS`.",
        "",
        "## Signal Model Divergences",
        "",
        "RTMux keeps signal state process-global: a single shared mask, handler",
        "table, and pending set (`lib/libc/rtmux/signal-state.c`), delivered on the",
        "per-process system channel.  There is no per-thread signal state, so the",
        "advertised `_POSIX_THREADS` per-thread signal functions diverge:",
        "",
        "- `pthread_sigmask` operates on the one process-wide mask.  For a",
        "  single-threaded process this equals `sigprocmask`; with multiple threads",
        "  the mask is shared rather than per-thread.",
        "- `pthread_kill` cannot target an individual thread: it delivers the",
        "  signal process-wide (as `kill(getpid(), sig)` would) and runs the",
        "  handler in the caller.  A non-positive thread id is rejected with",
        "  `ESRCH`; there is no live-thread registry, so any positive id is",
        "  accepted (POSIX makes an invalid handle undefined behavior).",
        "",
        "## LKL Domain libc Floor",
        "",
        f"The LKL servers (`lkl-fsd`, `lkl-netd`) pull {len(LKL_FLOOR)} symbols from",
        "libc, measured from their built objects plus the prebuilt",
        "`liblkl-rtmux.a` (which links before `libc.so`).  This is the basis for",
        "the `lkl` build profile: filesystem I/O (`open`,",
        "`close`, `pread`, `pwrite`, `fstat`, `fsync`, `fcntl`), allocation",
        "(`malloc`/`calloc`/`free`, `mmap`/`munmap`), pthreads and TLS",
        "(`pthread_*`, `pthread_key_*`, `pthread_getspecific`), a little stdio",
        "(`fprintf`, `fputs`, `puts`, `strerror`), and the RTMux runtime glue",
        "(`dispatch_*`, `ioserv_*`, `interrupt_*`, `thread_*`).  Notably it needs",
        f"no POSIX sockets and no message queues.  Of the {len(LKL_FLOOR)},",
        f"{len(LKL_FLOOR & set(POSIX_NAMES))} are tracked POSIX interfaces above.",
        "The authoritative list is `posix_profile.LKL_FLOOR`; derive and",
        "drift-check it with `scripts/lkl-floor.py [--check]` (needs the LKL",
        "artifacts built).",
        "",
        "## Build Profiles",
        "",
        "Advertised POSIX options are grouped into build profiles; the Kconfig",
        "`choice` that selects one is still to come.",
        "`posix_profile.POSIX_PROFILES` defines four, as a partial order",
        "`minimal` ⊆ {`realtime`, `lkl`} ⊆ `full` -- deliberately NOT a total",
        "order:",
        "",
        "| Profile | Options | Basis |",
        "| --- | --- | --- |",
        f"| `minimal` | {len(POSIX_PROFILES['minimal'])} | PSE51 mandatory "
        "realtime set |",
        f"| `realtime` | {len(POSIX_PROFILES['realtime'])} | minimal + PSE52 "
        "filesystem/memory + PSE53 process (posix_spawn) |",
        f"| `lkl` | {len(POSIX_PROFILES['lkl'])} | minimal + the option groups "
        "the measured LKL floor pulls |",
        f"| `full` | {len(POSIX_PROFILES['full'])} | every advertised standard "
        "POSIX option |",
        "",
        "`lkl` and `realtime` are incomparable, as the roadmap predicted: `lkl`",
        "adds " + ", ".join(
            "`" + o + "`" for o in
            sorted(POSIX_PROFILES["lkl"] - POSIX_PROFILES["realtime"])) +
        " (from the measured floor) that `realtime` omits, while `realtime`",
        "adds process/IPC options `lkl` never pulls.  `validate_profiles()`",
        "enforces the order: a profile that advertises an option absent from",
        "`full`, or drops a `minimal` option, fails `make check-posix-matrix`.",
        "",
        "## Profile Closure Claims",
        "",
        "These claims are computed from the function rows above.  An option group",
        "is claimed for a profile only when every mapped interface row in that",
        "group is implemented and tested; incomplete groups emit matrix row IDs",
        "as the generated POSIX closure backlog.",
        "",
        "| Profile | Claimed clean option groups | Blocked matrix row IDs |",
        "| --- | --- | --- |",
    ]
    for profile, (claimed, blocked) in profile_closure(function_rows).items():
        claim = "<br>".join(f"`{opt}`" for opt in claimed) or "-"
        block = "<br>".join(
            f"`{opt}`: " + ", ".join(f"`{row}`" for row in rows)
            for opt, rows in blocked.items()
        ) or "-"
        lines.append(f"| `{profile}` | {claim} | {block} |")

    lines += [
        "",
        "## Maintenance Rule",
        "",
        "Run `make check-posix-matrix` in CI.  Any new exported POSIX-like",
        "interface, new stub, or test movement must update this generated file.",
        "Every stub or removed-by-design row must either have a userland test that",
        "documents the expected behavior or carry the generated missing-coverage",
        "note until the test exists.  Drift-detected rows and declared-only",
        "matrix rows must be classified before `--check` will pass.",
        "",
    ]
    return "\n".join(lines)


# Advertised options whose libc function set is incomplete -- real
# advertised-ahead-of-behavior gaps (implement the
# missing functions or de-advertise before they can join OPTION_REQUIREMENTS).
# This bucket is now empty: all four advertised-ahead gaps are resolved.
#   _POSIX_THREAD_SAFE_FUNCTIONS graduated to OPTION_REQUIREMENTS once stdio was
#     internally locked (the flockfile family + _unlocked accessors) and the
#     reentrant set (rand_r/readdir_r/getlogin_r/ttyname_r) landed.
#   _POSIX_THREADS graduated once pthread_kill/pthread_sigmask landed and its
#     base [THR] set was audited.
#   _POSIX_REGEXP and _POSIX_SHELL were de-advertised to -1 in include/unistd.h
#     (no regex engine; system/popen need fork), so they no longer appear in
#     advertised_posix_options() and need no bucket here.
# Keep it a set (not {}, which is a dict) so the set-union in
# validate_option_coverage() still works.
OPTION_KNOWN_INCOMPLETE = set()


def _libc_defined_symbols(names):
    found = set()
    for path in (ROOT / "lib/libc").rglob("*.c"):
        text = strip_c_comments(path.read_text(encoding="utf-8", errors="ignore"))
        for name in names:
            if name in found:
                continue
            n = re.escape(name)
            # A real definition: `T name(args){`, or the `T (name)(args){` idiom
            # that lets a function coexist with a same-named macro (e.g.
            # pthread_mutex_trylock).  A weak-reference alias also counts:
            # `__weak_reference(__impl, name);` is how libc exports a public name
            # for an internal implementation (e.g. strtok_r -> __strtok_r).
            if re.search(r"\b" + n + r"\s*\)?\s*\([^;{}]*\)\s*\{", text) or \
               re.search(r"\b__weak_reference\s*\([^,()]+,\s*" + n + r"\s*\)",
                         text):
                found.add(name)
    return found


def validate_option_coverage():
    """Return advertised _POSIX_* options that are neither mapped in
    OPTION_REQUIREMENTS, classified capability-only, nor a tracked gap.  Keeps
    the option-group ledger exhaustive: a newly advertised option must be
    accounted for in exactly one bucket."""
    advertised = set(advertised_posix_options())
    classified = (set(OPTION_REQUIREMENTS) | OPTION_CAPABILITY_ONLY |
                  OPTION_NONSTANDARD | OPTION_KNOWN_INCOMPLETE)
    return sorted(advertised - classified)


def validate_advertised_options():
    """Return [(option, [missing symbols])] for advertised options whose group
    is not fully implemented in libc."""
    advertised = advertised_posix_options()
    names = sorted({s for syms in OPTION_REQUIREMENTS.values() for s in syms})
    defined = _libc_defined_symbols(names)
    bad = []
    for opt, syms in sorted(OPTION_REQUIREMENTS.items()):
        if opt in advertised:
            missing = [s for s in syms if s not in defined]
            if missing:
                bad.append((opt, missing))
    return bad


def _drop_inactive_posix_blocks(text, advertised):
    lines = []
    skipping = 0
    for line in text.splitlines():
        m = re.match(r"\s*#\s*if\s+defined\((_POSIX_[A-Z0-9_]+)\)", line)
        if m and m.group(1) not in advertised:
            skipping += 1
            continue
        if skipping:
            if re.match(r"\s*#\s*if\b", line):
                skipping += 1
            elif re.match(r"\s*#\s*endif\b", line):
                skipping -= 1
            continue
        lines.append(line)
    return "\n".join(lines)


def _sysconf_posix_claims(rel):
    advertised = advertised_posix_options()
    text = _drop_inactive_posix_blocks(strip_c_comments(read(rel)), advertised)
    claims = []
    for m in re.finditer(r"_SC_([A-Z0-9_]+).*?,\s*_POSIX_([A-Z0-9_]+)", text):
        claims.append((rel, "_SC_" + m.group(1), "_POSIX_" + m.group(2)))
    for m in re.finditer(
        r"roottask_sysconf_set\([^;]*?(_SC_[A-Z0-9_]+)\s*,\s*(-1|_POSIX_[A-Z0-9_]+)",
        text,
        re.S,
    ):
        sc, value = m.groups()
        claims.append((rel, sc, value))
    return claims


def validate_sysconf_claims():
    advertised = advertised_posix_options()
    claims = []
    for rel in [
        "lib/libc/posix/sysconf.c",
        "servers/roottask/sys/sysconf.c",
        "servers/roottask/path/namedsem.c",
    ]:
        claims.extend(_sysconf_posix_claims(rel))

    bad = []
    for rel, sc, value in claims:
        opt = "_POSIX_" + sc.removeprefix("_SC_")
        if value == "-1" and opt in advertised:
            bad.append((rel, sc, "runtime -1 for advertised " + opt))
        elif value.startswith("_POSIX_") and value not in advertised:
            bad.append((rel, sc, "uses unsupported " + value))
    return bad


# The two static sysconf tables are independent sources of truth for the same
# runtime answer: libc resolves an `_SC_*` in-process, roottask answers the
# forwarded query for names libc does not hold.  Where both name the same
# `_SC_*`, they must return the same value -- the roottask table already flags
# this drift risk in prose ("Stay in lockstep if kernel ever changes them").
STATIC_SYSCONF_TABLES = [
    ("lib/libc/posix/sysconf.c", "_sysconf_table"),
    ("servers/roottask/sys/sysconf.c", "roottask_sysconf_table"),
]


def _sysconf_table_values(rel, table):
    """Map each `_SC_*` entry in the named static initializer to its value
    token (the initializer after the `_SC_*` name, up to the next comma).  The
    array is a flat brace-free initializer terminated by `};`, so slicing from
    the last declaration (past any `#ifdef` PIC/non-PIC pair) to that
    terminator isolates the body without a full C parse.  Inactive
    `#if defined(_POSIX_*)` option rows are dropped first, mirroring
    `_sysconf_posix_claims`, so a row compiled out in this configuration cannot
    inject a spurious value.  A single leading C cast is stripped from the
    value so `(long) ULONG_MAX` and `ULONG_MAX` compare equal."""
    text = _drop_inactive_posix_blocks(
        strip_c_comments(read(rel)), advertised_posix_options())
    decls = list(re.finditer(re.escape(table) + r"\s*\[\s*\]\s*=\s*\{", text))
    if not decls:
        return {}
    start = decls[-1].end()
    term = re.search(r"\}\s*;", text[start:])
    body = text[start : start + term.start()] if term else text[start:]
    out = {}
    for row in re.finditer(r"_SC_([A-Z0-9_]+)\s*,\s*([^,}]+?)\s*(?=[,}])", body):
        value = re.sub(r"^\([^)]*\)\s*", "", row.group(2))
        out.setdefault("_SC_" + row.group(1), re.sub(r"\s+", " ", value).strip())
    return out


def validate_sysconf_table_agreement():
    """Return [(sc, libc_value, roottask_value)] for every `_SC_*` defined in
    BOTH static sysconf tables with a differing value.  A divergence is a real
    correctness bug (the two tables would answer the same query differently),
    not a documentation nit."""
    libc, roottask = (
        _sysconf_table_values(rel, table) for rel, table in STATIC_SYSCONF_TABLES
    )
    return [
        (sc, libc[sc], roottask[sc])
        for sc in sorted(set(libc) & set(roottask))
        if libc[sc] != roottask[sc]
    ]


def pse51_citation_path(name):
    """Repo-relative PSE51-docs README path for `name`, built not probed, so
    callers stay deterministic without the external oracle checked out."""
    return (PSE51_DOC_ROOT / name / "README.md").relative_to(ROOT).as_posix()


def pse51_has_doc_page(name):
    """True when a PSE51-docs README for `name` is checked out."""
    return (PSE51_DOC_ROOT / name / "README.md").is_file()


def validate_pse51_docs():
    """PSE51-mandatory interfaces that lack both a PSE51-docs page and a
    documented no-page reason, plus stale/redundant PSE51_NO_PAGE entries.

    The per-symbol page check needs the external oracle checked out; when it is
    absent it is skipped so the default gate still runs, but the map-consistency
    checks (no stale key, no reason shadowing a real page) run unconditionally."""
    errors = []
    for name in sorted(PSE51_NO_PAGE):
        if name not in PSE51_REQUIRED_SYMBOLS:
            errors.append(f"{name}: PSE51_NO_PAGE names a non-mandatory symbol")
    if not PSE51_DOC_ROOT.is_dir():
        return errors
    for name in sorted(PSE51_REQUIRED_SYMBOLS):
        if not pse51_has_doc_page(name) and name not in PSE51_NO_PAGE:
            errors.append(f"{name}: no PSE51-docs page and no PSE51_NO_PAGE reason")
    for name in sorted(PSE51_NO_PAGE):
        if pse51_has_doc_page(name):
            errors.append(f"{name}: PSE51_NO_PAGE reason but a doc page exists")
    return errors


def validate_opts_pse51_coverage():
    manifest = set(scan_opts_pse51_manifest())
    exclusions = scan_opts_pse51_exclusions()
    errors = []
    for name, rows in sorted(exclusions.items()):
        if not name:
            errors.extend(reason for _, reason in rows)
            continue
        if name not in PSE51_REQUIRED_SYMBOLS:
            errors.append(f"{name}: OPTS PSE51 exclusion names a non-mandatory symbol")
        if name in manifest:
            errors.append(f"{name}: OPTS PSE51 has both a passing case and an exclusion")
        for tag, reason in rows:
            if not tag or not re.match(r"^[A-Za-z0-9_.-]+$", tag):
                errors.append(f"{name}: invalid OPTS PSE51 exclusion tag {tag!r}")
            if not reason:
                errors.append(f"{name}: empty OPTS PSE51 exclusion reason")
    for name in sorted(PSE51_REQUIRED_SYMBOLS - manifest - set(exclusions)):
        errors.append(f"{name}: no OPTS PSE51 manifest row or exclusion")
    return errors


def validate_matrix_classification(content):
    """Rows that made it into the generated matrix but still have no concrete
    status.  This covers the mandatory rows without maintaining a second POSIX
    symbol list: if the matrix publishes a row, it must be implemented, partial
    with a reason, stubbed/unsupported with a reason, untested, missing, or
    removed by design -- never just declared."""
    errors = []
    for line in content.splitlines():
        m = re.match(r"\| `([^`]+)` \| .* \| declared-only \|", line)
        if m:
            errors.append(f"{m.group(1)}: matrix row is declared-only")
    return errors


def validate_porting_omissions(content, porting_text):
    """The matrix and the omission table in docs/porting.md must agree: every
    table entry has exactly one not-supported row naming its fixture, no
    not-supported row lacks a table entry, and no omitted name is defined or
    declared by libc."""
    table = parse_porting_omissions(porting_text)
    rows = collections.defaultdict(list)
    for row in parse_function_rows(content):
        if row["status"] == "not-supported":
            rows[row["name"]].append(row)

    errors = []
    if not table:
        errors.append("docs/porting.md: no rows in the omitted-interfaces table")
    for name, om in sorted(table.items()):
        found = rows.get(name, [])
        if len(found) != 1:
            errors.append(f"{name}: {len(found)} not-supported matrix row(s), want 1")
        elif found[0]["test"] != om["fixture"]:
            errors.append(f"{name}: matrix fixture {found[0]['test']} != {om['fixture']}")
    for name in sorted(set(rows) - set(table)):
        errors.append(f"{name}: not-supported row has no omitted-interfaces table entry")
    live = set(scan_exported_functions()) | set(scan_libc_defs())
    for name in sorted(live & set(table)):
        errors.append(f"{name}: omitted by design but declared or defined in libc")
    return errors


def parse_function_rows(content):
    rows = []
    for line in content.splitlines():
        if not line.startswith("| `"):
            continue
        parts = [part.strip() for part in line.split("|")[1:-1]]
        if len(parts) != 8 or parts[0] == "`_SC_*` option":
            continue
        opts = [] if parts[1] == "-" else parts[1].split("<br>")
        rows.append({
            "name": parts[0].strip("`"),
            "opts": opts,
            "status": parts[3],
            "test": parts[6],
        })
    return rows


def validate_profile_closure(content):
    errors = []
    rows = parse_function_rows(content)
    closure = profile_closure(rows)
    for profile, (claimed, _) in closure.items():
        claimed = set(claimed)
        for row in rows:
            if row["status"] in BAD_CLOSURE_STATUS and claimed & set(row["opts"]):
                errors.append(
                    f"{profile}: claims {sorted(claimed & set(row['opts']))} "
                    f"but {row['name']} is {row['status']}"
                )
    return errors


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--check", action="store_true")
    parser.add_argument("--strict-options", action="store_true")
    args = parser.parse_args()
    drift = scan_drift_candidates()
    content = generate(drift)
    if args.check:
        old = OUT.read_text(encoding="utf-8") if OUT.exists() else ""
        rc = 0
        if old != content:
            print(f"{OUT.relative_to(ROOT)} is out of date", file=sys.stderr)
            rc = 1
        if drift:
            print(
                f"{OUT.relative_to(ROOT)}: drift — {len(drift)} unclassified "
                f"POSIX-shaped symbol(s); regenerate and classify:",
                file=sys.stderr,
            )
            for name in sorted(drift):
                print(f"  {name}: {', '.join(drift[name])}", file=sys.stderr)
            rc = 1
        if not PSE51_DOC_ROOT.is_dir():
            print(
                f"{PSE51_DOC_ROOT.relative_to(ROOT)}: absent — PSE51 per-symbol "
                "semantic-citation check skipped (map consistency still enforced)",
                file=sys.stderr,
            )
        rt_errors = validate_rt_evidence()
        if rt_errors:
            print(
                f"{OUT.relative_to(ROOT)}: {len(rt_errors)} RT-evidence "
                "citation problem(s):",
                file=sys.stderr,
            )
            for err in rt_errors:
                print(f"  {err}", file=sys.stderr)
            rc = 1
        sysconf_errors = validate_sysconf_evidence()
        if sysconf_errors:
            print(
                f"{OUT.relative_to(ROOT)}: {len(sysconf_errors)} sysconf-evidence "
                "citation problem(s):",
                file=sys.stderr,
            )
            for err in sysconf_errors:
                print(f"  {err}", file=sys.stderr)
            rc = 1
        unclassified = validate_option_coverage()
        if unclassified:
            print(
                f"POSIX option coverage: {len(unclassified)} advertised option(s) "
                "not classified -- add each to OPTION_REQUIREMENTS, "
                "OPTION_CAPABILITY_ONLY, OPTION_NONSTANDARD, or "
                "OPTION_KNOWN_INCOMPLETE:",
                file=sys.stderr,
            )
            for opt in unclassified:
                print(f"  {opt}", file=sys.stderr)
            rc = 1
        coverage = validate_header_coverage()
        if coverage:
            print(
                f"POSIX header coverage: {len(coverage)} declared function(s) "
                "not tracked in POSIX_NAMES (add to POSIX_NAMES or "
                "COVERAGE_EXCLUDE):",
                file=sys.stderr,
            )
            for name, rel in coverage:
                print(f"  {name}: {rel}", file=sys.stderr)
            rc = 1
        agree_errors = validate_sysconf_table_agreement()
        if agree_errors:
            print(
                "sysconf: libc and roottask _SC_* tables disagree "
                f"({len(agree_errors)} entr(ies)):",
                file=sys.stderr,
            )
            for sc, libc_v, root_v in agree_errors:
                print(f"  {sc}: libc={libc_v} roottask={root_v}", file=sys.stderr)
            rc = 1
        doc_errors = validate_pse51_docs()
        if doc_errors:
            print(
                f"{PSE51_DOC_ROOT.relative_to(ROOT)}: {len(doc_errors)} PSE51 "
                "semantic-citation problem(s):",
                file=sys.stderr,
            )
            for err in doc_errors:
                print(f"  {err}", file=sys.stderr)
            rc = 1
        opts_errors = validate_opts_pse51_coverage()
        if opts_errors:
            print(
                f"{OPTS_PSE51_MANIFEST.relative_to(ROOT)}: "
                f"{len(opts_errors)} OPTS PSE51 coverage problem(s):",
                file=sys.stderr,
            )
            for err in opts_errors:
                print(f"  {err}", file=sys.stderr)
            rc = 1
        row_errors = validate_matrix_classification(content)
        if row_errors:
            print(
                f"{OUT.relative_to(ROOT)}: {len(row_errors)} POSIX matrix "
                "classification problem(s):",
                file=sys.stderr,
            )
            for err in row_errors:
                print(f"  {err}", file=sys.stderr)
            rc = 1
        omission_errors = validate_porting_omissions(
            OUT.read_text(encoding="utf-8") if OUT.exists() else "",
            PORTING_DOC.read_text(encoding="utf-8"))
        if omission_errors:
            print(
                f"{OUT.relative_to(ROOT)}: {len(omission_errors)} porting "
                "omission problem(s):",
                file=sys.stderr,
            )
            for err in omission_errors:
                print(f"  {err}", file=sys.stderr)
            rc = 1
        closure_errors = validate_profile_closure(content)
        if closure_errors:
            print(
                f"{OUT.relative_to(ROOT)}: {len(closure_errors)} POSIX profile "
                "closure problem(s):",
                file=sys.stderr,
            )
            for err in closure_errors:
                print(f"  {err}", file=sys.stderr)
            rc = 1
        profile_errors = validate_profiles()
        if profile_errors:
            print(
                f"POSIX profiles: {len(profile_errors)} partial-order "
                "violation(s) (minimal must be a subset of {realtime, lkl}, all "
                "a subset of full):",
                file=sys.stderr,
            )
            for err in profile_errors:
                print(f"  {err}", file=sys.stderr)
            rc = 1
        if args.strict_options:
            bad = validate_advertised_options()
            if bad:
                print(
                    f"include/unistd.h: {len(bad)} advertised _POSIX_* option(s) "
                    "lack a complete libc implementation:",
                    file=sys.stderr,
                )
                for opt, missing in bad:
                    print(f"  {opt}: missing {', '.join(missing)}", file=sys.stderr)
                rc = 1
            bad_sysconf = validate_sysconf_claims()
            if bad_sysconf:
                print(
                    "sysconf: advertised _POSIX_* options disagree with "
                    "runtime _SC_* claims:",
                    file=sys.stderr,
                )
                for rel, sc, reason in bad_sysconf:
                    print(f"  {rel}: {sc}: {reason}", file=sys.stderr)
                rc = 1
        return rc
    OUT.parent.mkdir(parents=True, exist_ok=True)
    OUT.write_text(content, encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
