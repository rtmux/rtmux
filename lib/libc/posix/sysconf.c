/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <unistd.h>
#include <errno.h>
#include <stdbool.h>
#include <sys/conf.h>
#include <sys/sysmsg.h>
#include <sys/rtmux-core.h>
#include <inttypes.h>

static int _sysconf_get(int name, long *value, char *str)
{
    sys_conf_t msg;

    msg.i.type = _SYS_CONF;
    msg.i.subtype = _SYS_SUB_GET;
    msg.i.cmd = _CONF_NUM;
    msg.i.name = name;
    msg.i.value = *value;

    if (msg_sendnc_r(ROOTTASK_COID, &msg.i, sizeof msg.i, &msg.o,
                     sizeof msg.o)) {
        return -1;
    }
    *value = msg.o.value;
    return msg.o.match;
}

#ifdef __PIC__ /* PIC bug in GCC where casting to (intptr_t) causes a rdonly \
                  section fixup */
static long _sysconf_table[] = {
#else
static const long _sysconf_table[] = {
#endif
    _CONF_CALL_PTR, (intptr_t) &_sysconf_list,
    _CONF_VALUE_MIN | _SC_NGROUPS_MAX, NGROUPS_MAX,
    _CONF_VALUE_NUM | _SC_THREAD_DESTRUCTOR_ITERATIONS,
    PTHREAD_DESTRUCTOR_ITERATIONS, _CONF_VALUE_NUM | _SC_MB_LEN_MAX, MB_LEN_MAX,
    _CONF_VALUE_NUM | _SC_CHAR_BIT, CHAR_BIT, _CONF_VALUE_NUM | _SC_CHAR_MAX,
    CHAR_MAX, _CONF_VALUE_NUM | _SC_CHAR_MIN, CHAR_MIN,
    _CONF_VALUE_NUM | _SC_INT_MAX, INT_MAX, _CONF_VALUE_NUM | _SC_INT_MIN,
    INT_MIN, _CONF_VALUE_NUM | _SC_LONG_BIT, __LONG_BITS__,
    _CONF_VALUE_NUM | _SC_SCHAR_MAX, SCHAR_MAX, _CONF_VALUE_NUM | _SC_SCHAR_MIN,
    SCHAR_MIN, _CONF_VALUE_NUM | _SC_SHRT_MAX, SHRT_MAX,
    _CONF_VALUE_NUM | _SC_SHRT_MIN, SHRT_MIN, _CONF_VALUE_NUM | _SC_SSIZE_MAX,
    LONG_MAX, _CONF_VALUE_NUM | _SC_UCHAR_MAX, UCHAR_MAX,
    _CONF_VALUE_NUM | _SC_UINT_MAX, UINT_MAX, _CONF_VALUE_NUM | _SC_ULONG_MAX,
    (long) ULONG_MAX, _CONF_VALUE_NUM | _SC_USHRT_MAX, USHRT_MAX,
    _CONF_VALUE_NUM | _SC_WORD_BIT, __INT_BITS__,

    /* Implement sysconf() support for all defined POSIX option groups;
     * basically this mirrors the presence and value of anything defined
     * compile-time in <unistd.h> to a corresponding run-time value. Let
     * kernel/roottask limit run-time _SC_VERSION Let mqueue/mq handle run-time
     * _SC_MESSAGE_PASSING Let mqueue/roottask handle run-time _SC_SEMAPHORES
     */
    _CONF_VALUE_MIN | _SC_VERSION, _POSIX_VERSION,
#if defined(_POSIX_ADVISORY_INFO) && defined(_SC_ADVISORY_INFO)
    _CONF_VALUE_NUM | _SC_ADVISORY_INFO, _POSIX_ADVISORY_INFO,
#endif
#if defined(_POSIX_ASYNCHRONOUS_IO) && defined(_SC_ASYNCHRONOUS_IO)
    _CONF_VALUE_NUM | _SC_ASYNCHRONOUS_IO, _POSIX_ASYNCHRONOUS_IO,
#endif
#if defined(_POSIX_BARRIERS) && defined(_SC_BARRIERS)
    _CONF_VALUE_NUM | _SC_BARRIERS, _POSIX_BARRIERS,
#endif
#if defined(_POSIX_CPUTIME) && defined(_SC_CPUTIME)
    _CONF_VALUE_NUM | _SC_CPUTIME, _POSIX_CPUTIME,
#endif
#if defined(_POSIX_CLOCK_SELECTION) && defined(_SC_CLOCK_SELECTION)
    _CONF_VALUE_NUM | _SC_CLOCK_SELECTION, _POSIX_CLOCK_SELECTION,
#endif
    _CONF_VALUE_NUM | _SC_FSYNC, _POSIX_FSYNC,
#if defined(_POSIX_MAPPED_FILES) && defined(_SC_MAPPED_FILES)
    _CONF_VALUE_NUM | _SC_MAPPED_FILES, _POSIX_MAPPED_FILES,
#endif
    _CONF_VALUE_NUM | _SC_MEMLOCK, _POSIX_MEMLOCK,
    _CONF_VALUE_NUM | _SC_MEMLOCK_RANGE, _POSIX_MEMLOCK_RANGE,
    _CONF_VALUE_NUM | _SC_MONOTONIC_CLOCK, _POSIX_MONOTONIC_CLOCK,
#if defined(_POSIX_MEMORY_PROTECTION) && defined(_SC_MEMORY_PROTECTION)
    _CONF_VALUE_NUM | _SC_MEMORY_PROTECTION, _POSIX_MEMORY_PROTECTION,
#endif
#if defined(_POSIX_MESSAGE_PASSING) && defined(_SC_MESSAGE_PASSING)
    _CONF_VALUE_NUM | _SC_MESSAGE_PASSING, _POSIX_MESSAGE_PASSING,
#endif
#if defined(_POSIX_PRIORITIZED_IO) && defined(_SC_PRIORITIZED_IO)
    _CONF_VALUE_NUM | _SC_PRIORITIZED_IO, _POSIX_PRIORITIZED_IO,
#endif
#if defined(_POSIX_PRIORITY_SCHEDULING) && defined(_SC_PRIORITY_SCHEDULING)
    _CONF_VALUE_NUM | _SC_PRIORITY_SCHEDULING, _POSIX_PRIORITY_SCHEDULING,
#endif
#if defined(_POSIX_READER_WRITER_LOCKS) && defined(_SC_READER_WRITER_LOCKS)
    _CONF_VALUE_NUM | _SC_READER_WRITER_LOCKS, _POSIX_READER_WRITER_LOCKS,
#endif
#if defined(_POSIX_REALTIME_SIGNALS) && defined(_SC_REALTIME_SIGNALS)
    _CONF_VALUE_NUM | _SC_REALTIME_SIGNALS, _POSIX_REALTIME_SIGNALS,
#endif
    _CONF_VALUE_NUM | _SC_SHARED_MEMORY_OBJECTS, _POSIX_SHARED_MEMORY_OBJECTS,
    _CONF_VALUE_NUM | _SC_SYNCHRONIZED_IO, _POSIX_SYNCHRONIZED_IO,
#if defined(_POSIX_SPIN_LOCKS) && defined(_SC_SPIN_LOCKS)
    _CONF_VALUE_NUM | _SC_SPIN_LOCKS, _POSIX_SPIN_LOCKS,
#endif
#if defined(_POSIX_SPAWN) && defined(_SC_SPAWN)
    _CONF_VALUE_NUM | _SC_SPAWN, _POSIX_SPAWN,
#endif
    _CONF_VALUE_NUM | _SC_SPORADIC_SERVER, _POSIX_SPORADIC_SERVER,
    _CONF_VALUE_NUM | _SC_THREAD_CPUTIME, _POSIX_THREAD_CPUTIME,
    _CONF_VALUE_NUM | _SC_THREADS, _POSIX_THREADS,
#if defined(_POSIX_TIMEOUTS) && defined(_SC_TIMEOUTS)
    _CONF_VALUE_NUM | _SC_TIMEOUTS, _POSIX_TIMEOUTS,
#endif
    _CONF_VALUE_NUM | _SC_TIMERS, _POSIX_TIMERS,
    _CONF_VALUE_NUM | _SC_THREAD_PRIO_INHERIT, _POSIX_THREAD_PRIO_INHERIT,
    _CONF_VALUE_NUM | _SC_THREAD_PRIO_PROTECT, _POSIX_THREAD_PRIO_PROTECT,
    _CONF_VALUE_NUM | _SC_THREAD_PRIORITY_SCHEDULING,
    _POSIX_THREAD_PRIORITY_SCHEDULING,
    _CONF_VALUE_NUM | _SC_THREAD_ATTR_STACKADDR, _POSIX_THREAD_ATTR_STACKADDR,
#if defined(_POSIX_THREAD_SAFE_FUNCTIONS) && defined(_SC_THREAD_SAFE_FUNCTIONS)
    _CONF_VALUE_NUM | _SC_THREAD_SAFE_FUNCTIONS, _POSIX_THREAD_SAFE_FUNCTIONS,
#endif
#if defined(_POSIX_THREAD_PROCESS_SHARED) && defined(_SC_THREAD_PROCESS_SHARED)
    _CONF_VALUE_NUM | _SC_THREAD_PROCESS_SHARED, _POSIX_THREAD_PROCESS_SHARED,
#endif
    _CONF_VALUE_NUM | _SC_THREAD_SPORADIC_SERVER, _POSIX_THREAD_SPORADIC_SERVER,
    _CONF_VALUE_NUM | _SC_THREAD_ATTR_STACKSIZE, _POSIX_THREAD_ATTR_STACKSIZE,
#if defined(_POSIX_TYPED_MEMORY_OBJECTS) && defined(_SC_TYPED_MEMORY_OBJECTS)
    _CONF_VALUE_NUM | _SC_TYPED_MEMORY_OBJECTS, _POSIX_TYPED_MEMORY_OBJECTS,
#endif
#if defined(TZNAME_MAX) && defined(_SC_TZNAME_MAX)
    _CONF_VALUE_NUM | _SC_TZNAME_MAX, TZNAME_MAX,
#endif
    _CONF_LINK_FCN, (intptr_t) _sysconf_get};

long *_sysconf_list;

/* Every _SC_* name <confname.h> defines (an alias of another name is covered by
 * its target). scripts/check-sysconf-names.py keeps this list in step with the
 * header; add a new name to both.
 */
static bool sysconf_name_valid(int name)
{
    switch (name) {
    case _SC_ARG_MAX:
    case _SC_CHILD_MAX:
    case _SC_CLK_TCK:
    case _SC_NGROUPS_MAX:
    case _SC_OPEN_MAX:
    case _SC_JOB_CONTROL:
    case _SC_SAVED_IDS:
    case _SC_VERSION:
    case _SC_LOGNAME_MAX:
    case _SC_PAGESIZE:
    case _SC_STREAM_MAX:
    case _SC_TZNAME_MAX:
    case _SC_AIO_LISTIO_MAX:
    case _SC_AIO_MAX:
    case _SC_AIO_PRIO_DELTA_MAX:
    case _SC_DELAYTIMER_MAX:
    case _SC_MQ_OPEN_MAX:
    case _SC_MQ_PRIO_MAX:
    case _SC_RTSIG_MAX:
    case _SC_SEM_NSEMS_MAX:
    case _SC_SEM_VALUE_MAX:
    case _SC_SIGQUEUE_MAX:
    case _SC_TIMER_MAX:
    case _SC_ASYNCHRONOUS_IO:
    case _SC_FSYNC:
    case _SC_MAPPED_FILES:
    case _SC_MEMLOCK:
    case _SC_MEMLOCK_RANGE:
    case _SC_MEMORY_PROTECTION:
    case _SC_MESSAGE_PASSING:
    case _SC_PRIORITIZED_IO:
    case _SC_PRIORITY_SCHEDULING:
    case _SC_REALTIME_SIGNALS:
    case _SC_SEMAPHORES:
    case _SC_SHARED_MEMORY_OBJECTS:
    case _SC_SYNCHRONIZED_IO:
    case _SC_TIMERS:
    case _SC_GETGR_R_SIZE_MAX:
    case _SC_GETPW_R_SIZE_MAX:
    case _SC_THREAD_DESTRUCTOR_ITERATIONS:
    case _SC_THREAD_KEYS_MAX:
    case _SC_THREAD_STACK_MIN:
    case _SC_THREAD_THREADS_MAX:
    case _SC_THREADS:
    case _SC_THREAD_ATTR_STACKADDR:
    case _SC_THREAD_ATTR_STACKSIZE:
    case _SC_THREAD_PRIORITY_SCHEDULING:
    case _SC_THREAD_PRIO_INHERIT:
    case _SC_THREAD_PRIO_PROTECT:
    case _SC_THREAD_PROCESS_SHARED:
    case _SC_THREAD_SAFE_FUNCTIONS:
    case _SC_2_CHAR_TERM:
    case _SC_2_C_BIND:
    case _SC_2_C_DEV:
    case _SC_2_C_VERSION:
    case _SC_2_LOCALEDEF:
    case _SC_2_SW_DEV:
    case _SC_2_UPE:
    case _SC_2_VERSION:
    case _SC_ATEXIT_MAX:
    case _SC_AVPHYS_PAGES:
    case _SC_BC_BASE_MAX:
    case _SC_BC_DIM_MAX:
    case _SC_BC_SCALE_MAX:
    case _SC_BC_STRING_MAX:
    case _SC_CHARCLASS_NAME_MAX:
    case _SC_CHAR_BIT:
    case _SC_CHAR_MAX:
    case _SC_CHAR_MIN:
    case _SC_COLL_WEIGHTS_MAX:
    case _SC_EQUIV_CLASS_MAX:
    case _SC_EXPR_NEST_MAX:
    case _SC_INT_MAX:
    case _SC_INT_MIN:
    case _SC_LINE_MAX:
    case _SC_LONG_BIT:
    case _SC_MB_LEN_MAX:
    case _SC_NL_ARGMAX:
    case _SC_NL_LANGMAX:
    case _SC_NL_MSGMAX:
    case _SC_NL_NMAX:
    case _SC_NL_SETMAX:
    case _SC_NL_TEXTMAX:
    case _SC_NPROCESSORS_CONF:
    case _SC_NPROCESSORS_ONLN:
    case _SC_NZERO:
    case _SC_PHYS_PAGES:
    case _SC_POLL:
    case _SC_RE_DUP_MAX:
    case _SC_SCHAR_MAX:
    case _SC_SCHAR_MIN:
    case _SC_SELECT:
    case _SC_SHRT_MAX:
    case _SC_SHRT_MIN:
    case _SC_SSIZE_MAX:
    case _SC_T_IOV_MAX:
    case _SC_UCHAR_MAX:
    case _SC_UINT_MAX:
    case _SC_UIO_MAXIOV:
    case _SC_ULONG_MAX:
    case _SC_USHRT_MAX:
    case _SC_WORD_BIT:
    case _SC_ADVISORY_INFO:
    case _SC_CPUTIME:
    case _SC_SPAWN:
    case _SC_SPORADIC_SERVER:
    case _SC_THREAD_CPUTIME:
    case _SC_THREAD_SPORADIC_SERVER:
    case _SC_TIMEOUTS:
    case _SC_BARRIERS:
    case _SC_CLOCK_SELECTION:
    case _SC_MONOTONIC_CLOCK:
    case _SC_READER_WRITER_LOCKS:
    case _SC_SPIN_LOCKS:
    case _SC_TYPED_MEMORY_OBJECTS:
    case _SC_2_PBS:
    case _SC_2_PBS_ACCOUNTING:
    case _SC_2_PBS_CHECKPOINT:
    case _SC_2_PBS_LOCATE:
    case _SC_2_PBS_MESSAGE:
    case _SC_2_PBS_TRACK:
    case _SC_HOST_NAME_MAX:
    case _SC_IOV_MAX:
    case _SC_IPV6:
    case _SC_RAW_SOCKETS:
    case _SC_REGEXP:
    case _SC_SHELL:
    case _SC_SYMLOOP_MAX:
        return true;
    default:
        return false;
    }
}

static long sysconf_value(int name)
{
    long value = -1;
    int saved_errno = errno;

    /* A name with no limit leaves value at -1 with errno unchanged; only a name
     * <confname.h> does not define is EINVAL.
     */
    if (_conf_get(_sysconf_table, name, &value, 0) == -1 && value == -1) {
        errno = saved_errno;
        if (!sysconf_name_valid(name))
            errno = EINVAL;
    }
    return value;
}

long sysconf(int name)
{
    return sysconf_value(name);
}
