/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* time.h Time functions */
#ifndef _TIME_H_INCLUDED

#ifndef __PLATFORM_H_INCLUDED
#include <sys/platform.h>
#endif

#include <sys/_timetypes.h>
#include <sys/_clocktypes.h>
#include <sys/_idtypes.h>

#define _TIME_H_INCLUDED

#ifndef _TIME_H_DECLARED
#define _TIME_H_DECLARED

/* Unix98 says that the value of CLOCKS_PER_SEC is required to be 1 million on
 * all XSI-conformant systems, and it may be variable on other systems and it
 * should not be assumed that CLOCKS_PER_SEC is a compile-time constant.
 */
#define CLOCKS_PER_SEC 1000000
struct itimerspec {
    struct timespec it_value, it_interval;
};

/*  Clock types */
#define CLOCK_REALTIME 0
#define CLOCK_SOFTTIME 1
#define CLOCK_MONOTONIC 2
#define CLOCK_PROCESS_CPUTIME_ID 3
#define CLOCK_THREAD_CPUTIME_ID 4


/* Timer settime flags */
#define TIMER_ABSTIME ((int) 0x80000000)

__BEGIN_DECLS
struct sigevent; /* for C++ */

extern int clock_getres(clockid_t __clock_id, struct timespec *__res);
extern int clock_gettime(clockid_t __clock_id, struct timespec *__tp);
extern int clock_settime(clockid_t __clock_id, const struct timespec *__tp);

extern int clock_getcpuclockid(pid_t __pid, clockid_t *__clock_id);

extern int nanosleep(const struct timespec *__rqtp, struct timespec *__rmtp);
extern int timer_create(clockid_t __clock_id,
                        struct sigevent *__evp,
                        timer_t *__timerid);
extern int timer_delete(timer_t __timerid);
extern int timer_getoverrun(timer_t __timerid);
extern int timer_gettime(timer_t __timerid, struct itimerspec *__value);
extern int timer_settime(timer_t __timerid,
                         int __flags,
                         const struct itimerspec *__value,
                         struct itimerspec *__ovalue);

extern int clock_nanosleep(clockid_t __clock_id,
                           int __flags,
                           const struct timespec *__rqtp,
                           struct timespec *__rmtp);

#define timespec2nsec(__ts) \
    ((uint64_t) (__ts)->tv_sec * 1000000000ULL + (uint64_t) (__ts)->tv_nsec)

#define TIMESPEC_VALID(__ts) \
    ((__ts)->tv_nsec >= 0 && (__ts)->tv_nsec < 1000000000L)

struct tm {
    int tm_sec;          /* seconds after the minute -- [0,61] */
    int tm_min;          /* minutes after the hour   -- [0,59] */
    int tm_hour;         /* hours after midnight     -- [0,23] */
    int tm_mday;         /* day of the month         -- [1,31] */
    int tm_mon;          /* months since January     -- [0,11] */
    int tm_year;         /* years since 1900                   */
    int tm_wday;         /* days since Sunday        -- [0,6]  */
    int tm_yday;         /* days since January 1     -- [0,365] */
    int tm_isdst;        /* Daylight Savings Time flag */
    long int tm_gmtoff;  /* Offset from gmt */
    const char *tm_zone; /* String for zone name */
};

extern struct tm *gmtime(const time_t *__timer);
extern struct tm *localtime(const time_t *__timer);
extern char *asctime(const struct tm *__tp);
extern char *ctime(const time_t *__timer);
extern size_t strftime(char *__s,
                       size_t __maxsiz,
                       const char *__fmt,
                       const struct tm *__tp);
extern time_t time(time_t *__timer);

extern struct tm *gmtime_r(const time_t *__timer, struct tm *__tm);
extern struct tm *localtime_r(const time_t *__timer, struct tm *__tm);
extern char *asctime_r(const struct tm *__tp, char *__buf);
extern char *ctime_r(const time_t *__timer, char *__buf);

__END_DECLS
#endif

#endif
