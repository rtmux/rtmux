#!/usr/bin/env python3
#
# Shared POSIX profile sets used by the conformance gates.

import pathlib
import re


ROOT = pathlib.Path(__file__).resolve().parents[1]

POSIX_HEADER_PATHS = {
    "assert.h",
    "ctype.h",
    "dirent.h",
    "dlfcn.h",
    "errno.h",
    "fcntl.h",
    "fnmatch.h",
    "grp.h",
    "inttypes.h",
    "libgen.h",
    "limits.h",
    "mqueue.h",
    "pthread.h",
    "pwd.h",
    "sched.h",
    "semaphore.h",
    "setjmp.h",
    "signal.h",
    "spawn.h",
    "stdarg.h",
    "stdbool.h",
    "stdint.h",
    "stdio.h",
    "stdlib.h",
    "string.h",
    "strings.h",
    "sys/mman.h",
    "sys/poll.h",
    "sys/resource.h",
    "sys/select.h",
    "sys/stat.h",
    "sys/statvfs.h",
    "sys/time.h",
    "sys/times.h",
    "sys/types.h",
    "sys/uio.h",
    "sys/utsname.h",
    "sys/wait.h",
    "termios.h",
    "time.h",
    "unistd.h",
    "utime.h",
}


def posix_header_files():
    return [ROOT / "include" / path for path in sorted(POSIX_HEADER_PATHS)]

PSE51_REQUIRED_SYMBOLS = {
    "clock_getcpuclockid",
    "clock_getres",
    "clock_gettime",
    "clock_settime",
    "getppid",
    "kill",
    "mlock",
    "mlockall",
    "munlock",
    "munlockall",
    "nanosleep",
    "pthread_cond_broadcast",
    "pthread_cond_signal",
    "pthread_cond_timedwait",
    "pthread_cond_wait",
    "pthread_create",
    "pthread_getcpuclockid",
    "pthread_key_create",
    "pthread_mutex_lock",
    "pthread_mutex_trylock",
    "pthread_mutex_unlock",
    "raise",
    "sched_yield",
    "shm_open",
    "shm_unlink",
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
    "signal",
    "sigprocmask",
    "sysconf",
    "timer_create",
    "timer_delete",
    "timer_getoverrun",
    "timer_gettime",
    "timer_settime",
    "umask",
}

PSE51_REQUIRED_MACROS = {
    "_POSIX_FSYNC",
    "_POSIX_MEMLOCK",
    "_POSIX_MEMLOCK_RANGE",
    "_POSIX_MONOTONIC_CLOCK",
    "_POSIX_SEMAPHORES",
    "_POSIX_SHARED_MEMORY_OBJECTS",
    "_POSIX_SYNCHRONIZED_IO",
    "_POSIX_TIMERS",
    "_POSIX_THREADS",
    "_POSIX_THREAD_ATTR_STACKADDR",
    "_POSIX_THREAD_ATTR_STACKSIZE",
    "_POSIX_THREAD_CPUTIME",
    "_POSIX_THREAD_PRIO_INHERIT",
    "_POSIX_THREAD_PRIO_PROTECT",
    "_POSIX_THREAD_PRIORITY_SCHEDULING",
}

# The measured libc floor of the LKL domain: the libc symbols the lkl-fsd and
# lkl-netd servers pull in, scanned from their built objects together with their
# LKL archives (which link before libc.so).  This is the basis for
# the `lkl` build profile -- filesystem I/O, alloc, TLS, and
# the RTMux runtime glue, with no POSIX sockets or message queues.  Derived and
# drift-checked by scripts/lkl-floor.py; regenerate with `lkl-floor.py` and
# verify with `lkl-floor.py --check` after touching the LKL servers.
LKL_FLOOR = {
    "__clock_time",
    "__get_errno_ptr",
    "__sched_set",
    "__thread_cancel",
    "_setjmp",
    "abort",
    "calloc",
    "close",
    "dispatch_block",
    "dispatch_context_alloc",
    "dispatch_context_free",
    "dispatch_create",
    "dispatch_handler",
    "environ",
    "fcntl",
    "fprintf",
    "fputs",
    "free",
    "fstat",
    "fsync",
    "getpid",
    "interrupt_attach_thread",
    "interrupt_detach",
    "interrupt_wait",
    "ioserv_attach",
    "ioserv_detach",
    "ioserv_open_bind",
    "kill",
    "longjmp",
    "malloc",
    "mem_offset",
    "memchr",
    "memcmp",
    "memcpy",
    "memmove",
    "memset",
    "mmap",
    "msg_send",
    "munmap",
    "nanosleep",
    "open",
    "pause",
    "posix_spawn",
    "pread",
    "pthread_attr_init",
    "pthread_attr_setstacksize",
    "pthread_cond_broadcast",
    "pthread_cond_destroy",
    "pthread_cond_init",
    "pthread_cond_signal",
    "pthread_cond_timedwait",
    "pthread_cond_wait",
    "pthread_condattr_destroy",
    "pthread_condattr_init",
    "pthread_condattr_setclock",
    "pthread_create",
    "pthread_detach",
    "pthread_exit",
    "pthread_getspecific",
    "pthread_key_create",
    "pthread_key_delete",
    "pthread_mutex_destroy",
    "pthread_mutex_init",
    "pthread_mutex_lock",
    "pthread_mutex_trylock",
    "pthread_mutex_unlock",
    "pthread_mutexattr_destroy",
    "pthread_mutexattr_init",
    "pthread_once",
    "pthread_self",
    "pthread_setspecific",
    "puts",
    "pwrite",
    "rtmux_detach",
    "rtmux_hwres_release_range",
    "rtmux_phys_contain",
    "siglongjmp",
    "snprintf",
    "stderr",
    "strchr",
    "strcmp",
    "strcpy",
    "strcspn",
    "strerror",
    "strlen",
    "strncat",
    "strncmp",
    "strncpy",
    "strstr",
    "strtok",
    "strtok_r",
    "strtol",
    "strtoul",
    "thread_ctl",
    "thread_join",
    "thread_join_r",
    "thread_pool_create",
    "thread_pool_start",
    "vsnprintf",
    "waitpid",
    "write",
}

# POSIX option groups: the single-source-of-truth registry the matrix and
# (upcoming) build-profile machinery both consume.

# Advertising an option macro != -1 is a conformance claim for the whole group.
# If unistd.h advertises one of these, every listed symbol must have a real libc
# definition or the claim is a lie (header says yes, link/run says no).
OPTION_REQUIREMENTS = {
    # POSIX.1-2001 Thread-Safe Functions option [TSF]: the reentrant *_r
    # functions plus the stdio locking primitives and _unlocked accessors.  All
    # 21 are implemented -- stdio is now internally locked (see the flockfile
    # family) and the reentrant set is complete.
    "_POSIX_THREAD_SAFE_FUNCTIONS": [
        "asctime_r", "ctime_r", "gmtime_r", "localtime_r",
        "flockfile", "ftrylockfile", "funlockfile",
        "getc_unlocked", "getchar_unlocked",
        "putc_unlocked", "putchar_unlocked",
        "getgrgid_r", "getgrnam_r", "getpwnam_r", "getpwuid_r",
        "getlogin_r", "readdir_r", "rand_r",
        "strerror_r", "strtok_r", "ttyname_r",
    ],
    # Base POSIX.1-2001 Threads option [THR].  Excludes: pthread_atfork (N/A
    # without fork); pthread_cleanup_push/pop (provided as paired macros in
    # <pthread.h>, so there is no function body to gate); and every function
    # owned by a thread SUB-option below (stack/sched/prio/pshared/cputime,
    # barriers, spin, rwlocks), which carries its own _POSIX_* macro.
    # pthread_kill/pthread_sigmask deliver over RTMux's process-global signal
    # model (see the Signal Model Divergence note in the generated matrix).
    "_POSIX_THREADS": [
        "pthread_create", "pthread_detach", "pthread_equal", "pthread_exit",
        "pthread_join", "pthread_self",
        "pthread_cancel", "pthread_setcancelstate", "pthread_setcanceltype",
        "pthread_testcancel",
        "pthread_cond_init", "pthread_cond_destroy", "pthread_cond_wait",
        "pthread_cond_timedwait", "pthread_cond_signal", "pthread_cond_broadcast",
        "pthread_condattr_init", "pthread_condattr_destroy",
        "pthread_mutex_init", "pthread_mutex_destroy", "pthread_mutex_lock",
        "pthread_mutex_trylock", "pthread_mutex_unlock",
        "pthread_mutexattr_init", "pthread_mutexattr_destroy",
        "pthread_once", "pthread_key_create", "pthread_key_delete",
        "pthread_getspecific", "pthread_setspecific",
        "pthread_attr_init", "pthread_attr_destroy",
        "pthread_attr_getdetachstate", "pthread_attr_setdetachstate",
        "pthread_attr_getguardsize", "pthread_attr_setguardsize",
        "pthread_kill", "pthread_sigmask",
    ],
    "_POSIX_BARRIERS": [
        "pthread_barrier_init", "pthread_barrier_destroy", "pthread_barrier_wait",
        "pthread_barrierattr_init", "pthread_barrierattr_destroy",
        "pthread_barrierattr_getpshared", "pthread_barrierattr_setpshared",
    ],
    "_POSIX_SPIN_LOCKS": [
        "pthread_spin_init", "pthread_spin_destroy", "pthread_spin_lock",
        "pthread_spin_trylock", "pthread_spin_unlock",
    ],
    "_POSIX_READER_WRITER_LOCKS": [
        "pthread_rwlock_init", "pthread_rwlock_destroy", "pthread_rwlock_rdlock",
        "pthread_rwlock_tryrdlock", "pthread_rwlock_timedrdlock",
        "pthread_rwlock_wrlock", "pthread_rwlock_trywrlock",
        "pthread_rwlock_timedwrlock", "pthread_rwlock_unlock",
        "pthread_rwlockattr_init", "pthread_rwlockattr_destroy",
        "pthread_rwlockattr_getpshared", "pthread_rwlockattr_setpshared",
    ],
    "_POSIX_CLOCK_SELECTION": [
        "clock_nanosleep",
        "pthread_condattr_getclock", "pthread_condattr_setclock",
    ],
    "_POSIX_TYPED_MEMORY_OBJECTS": [
        "posix_typed_mem_open", "posix_typed_mem_get_info",
    ],
    "_POSIX_SEMAPHORES": [
        "sem_init", "sem_destroy", "sem_open", "sem_close", "sem_unlink",
        "sem_wait", "sem_trywait", "sem_timedwait", "sem_post", "sem_getvalue",
    ],
    "_POSIX_TIMERS": [
        "timer_create", "timer_delete", "timer_settime", "timer_gettime",
        "timer_getoverrun",
    ],
    "_POSIX_MESSAGE_PASSING": [
        "mq_open", "mq_close", "mq_unlink", "mq_send", "mq_receive",
        "mq_timedsend", "mq_timedreceive", "mq_getattr", "mq_setattr",
    ],
    "_POSIX_REALTIME_SIGNALS": [
        "sigqueue", "sigwaitinfo", "sigtimedwait",
    ],
    "_POSIX_MEMLOCK": [
        "mlockall", "munlockall",
    ],
    "_POSIX_MEMLOCK_RANGE": [
        "mlock", "munlock",
    ],
    "_POSIX_SHARED_MEMORY_OBJECTS": [
        "shm_open", "shm_unlink",
    ],
    "_POSIX_THREAD_ATTR_STACKSIZE": [
        "pthread_attr_getstacksize", "pthread_attr_setstacksize",
        "pthread_attr_getstack", "pthread_attr_setstack",
    ],
    "_POSIX_THREAD_ATTR_STACKADDR": [
        "pthread_attr_getstack", "pthread_attr_setstack",
    ],
    "_POSIX_THREAD_CPUTIME": [
        "pthread_getcpuclockid",
    ],
    "_POSIX_SPAWN": [
        "posix_spawn", "posix_spawnp",
        "posix_spawn_file_actions_init", "posix_spawn_file_actions_destroy",
        "posix_spawn_file_actions_addopen", "posix_spawn_file_actions_addclose",
        "posix_spawn_file_actions_adddup2",
        "posix_spawnattr_init", "posix_spawnattr_destroy",
        "posix_spawnattr_getflags", "posix_spawnattr_setflags",
        "posix_spawnattr_getpgroup", "posix_spawnattr_setpgroup",
        "posix_spawnattr_getsigdefault", "posix_spawnattr_setsigdefault",
        "posix_spawnattr_getsigmask", "posix_spawnattr_setsigmask",
    ],
    "_POSIX_PRIORITY_SCHEDULING": [
        "sched_setparam", "sched_getparam", "sched_setscheduler",
        "sched_getscheduler", "sched_yield", "sched_get_priority_max",
        "sched_get_priority_min", "sched_rr_get_interval",
    ],
    "_POSIX_THREAD_PRIO_INHERIT": [
        "pthread_mutexattr_getprotocol", "pthread_mutexattr_setprotocol",
    ],
    "_POSIX_THREAD_PRIO_PROTECT": [
        "pthread_mutexattr_getprotocol", "pthread_mutexattr_setprotocol",
        "pthread_mutexattr_getprioceiling", "pthread_mutexattr_setprioceiling",
        "pthread_mutex_getprioceiling", "pthread_mutex_setprioceiling",
    ],
    "_POSIX_THREAD_PROCESS_SHARED": [
        "pthread_mutexattr_getpshared", "pthread_mutexattr_setpshared",
        "pthread_condattr_getpshared", "pthread_condattr_setpshared",
    ],
    "_POSIX_THREAD_PRIORITY_SCHEDULING": [
        "pthread_attr_getinheritsched", "pthread_attr_setinheritsched",
        "pthread_attr_getschedpolicy", "pthread_attr_setschedpolicy",
        "pthread_attr_getscope", "pthread_attr_setscope",
        "pthread_attr_getschedparam", "pthread_attr_setschedparam",
        "pthread_getschedparam", "pthread_setschedparam",
        "pthread_setschedprio",
    ],
    "_POSIX_TIMEOUTS": [
        "pthread_mutex_timedlock", "pthread_rwlock_timedrdlock",
        "pthread_rwlock_timedwrlock",
    ],
    "_POSIX_MAPPED_FILES": [
        "mmap", "munmap", "msync",
    ],
    "_POSIX_MEMORY_PROTECTION": [
        "mprotect",
    ],
    "_POSIX_FSYNC": [
        "fsync",
    ],
    "_POSIX_SYNCHRONIZED_IO": [
        "fdatasync",
    ],
    "_POSIX_JOB_CONTROL": [
        "tcsetpgrp",
    ],
}

# Advertised options with no dedicated function interface to gate -- capability
# values, behavior flags, and a scheduling-policy extension.  Note the three
# *_IO names here are the fpathconf `_PC_ASYNC_IO`/`_PC_PRIO_IO`/`_PC_SYNC_IO`
# per-file capability values, NOT the asynchronous-I/O function option
# (`_POSIX_ASYNCHRONOUS_IO`, which is #undef'd in include/unistd.h because RTMux
# has no aio_* implementation).
OPTION_CAPABILITY_ONLY = {
    "_POSIX_ASYNC_IO", "_POSIX_PRIO_IO", "_POSIX_SYNC_IO",   # fpathconf _PC_* values
    "_POSIX_NO_TRUNC", "_POSIX_VDISABLE",                    # pathconf/terminal caps
    "_POSIX_MONOTONIC_CLOCK",                                # CLOCK_MONOTONIC id
    "_POSIX_SAVED_IDS",                                      # saved-set-uid behavior
    "_POSIX_VERSION",                                        # the version macro itself
    "_POSIX_SPORADIC_SERVER", "_POSIX_THREAD_SPORADIC_SERVER",   # SCHED_SPORADIC policy
}

# Non-standard options from the POSIX 1003.1d/1j drafts (not POSIX.1-2001), so
# they fall outside the standard option-group ledger.  Their interfaces are
# RTMux-shaped (devctl(), not posix_devctl()) or absent; they are tracked here
# only so the coverage check stays exhaustive.
OPTION_NONSTANDARD = {
    "_POSIX_DEVICE_CONTROL", "_POSIX_DEVCTL_DIRECTION",
    "_POSIX_INTERRUPT_CONTROL", "_POSIX_THREAD_ASYNC_ABORT",
}


def advertised_posix_options():
    """Map each `_POSIX_*` option macro advertised (value != -1) in
    include/unistd.h to its value token.  A `#undef`-d or `-1` option is not
    advertised and does not appear."""
    out = {}
    text = (ROOT / "include" / "unistd.h").read_text(encoding="utf-8")
    for line in text.splitlines():
        m = re.match(r"\s*#\s*define\s+(_POSIX_[A-Z0-9_]+)\b(.*)", line)
        if m and m.group(2).strip() != "-1":
            out[m.group(1)] = m.group(2).strip()
    return out


# Build-selectable POSIX profiles, as advertised-option-macro
# sets.  minimal, lkl, and full derive from the data above; realtime applies the
# profile rule (minimal + PSE52 filesystem/realtime-I/O + PSE53 process
# primitives reachable via posix_spawn, restricted to options RTMux advertises).
# They form the partial order minimal in {realtime, lkl} in full -- NOT a total
# order (lkl is not a superset of realtime): the LKL floor needs no sockets or
# mqueue, while realtime needs no filesystem mmap.  validate_profiles() enforces
# the order.
# The advertised standard POSIX options: the ones unistd.h actually advertises
# (value != -1) that are also a mapped group or a capability-only value.  This
# excludes options that are mapped-but-#undef'd (e.g. _POSIX_REALTIME_SIGNALS,
# whose functions exist but whose macro is not advertised) and the draft-standard
# OPTION_NONSTANDARD macros -- neither is an advertised POSIX.1 profile option.
_ADVERTISED = frozenset(advertised_posix_options())
_STANDARD_OPTIONS = frozenset(OPTION_REQUIREMENTS) | frozenset(OPTION_CAPABILITY_ONLY)

_PROFILE_MINIMAL = frozenset(PSE51_REQUIRED_MACROS)

# lkl: minimal + every advertised option group whose functions the LKL servers
# actually pull (its function set intersects the measured LKL_FLOOR).
_PROFILE_LKL = _PROFILE_MINIMAL | frozenset(
    opt for opt, fns in OPTION_REQUIREMENTS.items()
    if opt in _ADVERTISED and set(fns) & LKL_FLOOR)

# full: every advertised standard POSIX option.
_PROFILE_FULL = _ADVERTISED & _STANDARD_OPTIONS

# realtime: minimal + PSE52 (filesystem/memory) + PSE53 (process primitives
# reachable via posix_spawn), only where RTMux advertises them.  _POSIX_TIMEOUTS
# and the thread-safe-functions option round out the realtime feature set; job
# control and saved-ids are PSE54/interactive scope and stay in full only.
_PROFILE_REALTIME = _PROFILE_MINIMAL | frozenset({
    "_POSIX_MAPPED_FILES", "_POSIX_MEMORY_PROTECTION",      # PSE52 filesystem/memory
    "_POSIX_TIMEOUTS",                                      # realtime timeouts
    "_POSIX_MESSAGE_PASSING",                               # realtime IPC (mqueue)
    "_POSIX_SPAWN", "_POSIX_PRIORITY_SCHEDULING",           # PSE53 process (posix_spawn)
    "_POSIX_THREAD_SAFE_FUNCTIONS",                         # required by PSE51/52/53
})

POSIX_PROFILES = {
    "minimal": _PROFILE_MINIMAL,
    "realtime": _PROFILE_REALTIME,
    "lkl": _PROFILE_LKL,
    "full": _PROFILE_FULL,
}


def validate_profiles():
    """Return partial-order violations, empty when the profiles satisfy
    minimal subset-of {realtime, lkl} subset-of full and every profile option is
    advertised (present in full)."""
    errors = []
    minimal, full = POSIX_PROFILES["minimal"], POSIX_PROFILES["full"]
    for name in ("realtime", "lkl"):
        dropped = minimal - POSIX_PROFILES[name]
        if dropped:
            errors.append(f"{name} drops minimal options: {sorted(dropped)}")
    for name in ("minimal", "realtime", "lkl"):
        extra = POSIX_PROFILES[name] - full
        if extra:
            errors.append(
                f"{name} advertises options absent from full: {sorted(extra)}")
    return errors
