/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <unistd.h>
#include <errno.h>
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

static long sysconf_value(int name)
{
    long value = -1;

    if (_conf_get(_sysconf_table, name, &value, 0) == -1 && value == -1) {
        /* POSIX says that sysconf returns -1 and does not set errno when the
         * 'name' is valid and has no limit. If 'name' is invalid, it returns -1
         * and sets errno to EINVAL. What, exactly, is an invalid 'name' isn't
         * specified. For right now, we assume that all 'name' values are OK,
         * and _conf_get returning -1 indicates that 'name' is unset. Assume,
         * for PCTS, that -1 is never valid.
         */
        if (name == -1)
            errno = EINVAL;
    }
    return value;
}

long sysconf(int name)
{
    return sysconf_value(name);
}
