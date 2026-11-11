/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* resource.h */
#ifndef __RESOURCE_H_INCLUDED
#define __RESOURCE_H_INCLUDED

#ifndef _SYS_TIME_H_
#include <sys/time.h>
#endif

#ifndef __PLATFORM_H_INCLUDED
#include <sys/platform.h>
#endif

#include <sys/_idtypes.h>

#undef NZERO /* Use sysconf(_SC_NZERO) */

#define RLIMIT_CPU 0     /* Limit on CPU time per process. */
#define RLIMIT_FSIZE 1   /* Limit on file size. */
#define RLIMIT_DATA 2    /* Limit on data segment size. */
#define RLIMIT_STACK 3   /* Limit on stack size. */
#define RLIMIT_CORE 4    /* Limit on size of core dump file. */
#define RLIMIT_NOFILE 5  /* Limit on number of open files. */
#define RLIMIT_AS 6      /* Limit on address space size. */
#define RLIMIT_MEMLOCK 7 /* locked-in-memory address space */
#define RLIMIT_NPROC 8   /* number of processes */
#define RLIMIT_NTHR 9    /* number of threads */

#define RLIM_NLIMITS 10 /* number of resource limits */

typedef uint64_t rlim_t;
#define RLIM_INFINITY \
    ((rlim_t) - 3L) /* A value of rlim_t indicating no limit. */
#define RLIM_SAVED_MAX                                                      \
    ((rlim_t) - 2L) /* A value of type rlim_t indicating an unrepresentable \
                       saved hard limit. */
#define RLIM_SAVED_CUR                                                      \
    ((rlim_t) - 1L) /* A value of type rlim_t indicating an unrepresentable \
                       saved soft limit. */

struct rlimit {
    rlim_t rlim_cur; /* the current (soft) limit */
    rlim_t rlim_max; /* hard limit */
};

#define RUSAGE_SELF 0 /* Returns information about the current process. */
#define RUSAGE_CHILDREN \
    -1 /* Returns information about children of the current process. */

struct rusage {
    struct timeval ru_utime; /* user time used */
    struct timeval ru_stime; /* system time used */
    long ru_maxrss;          /* max resident set size */
    long ru_ixrss;           /* integral shared memory size */
#define ru_first ru_ixrss
    long ru_idrss;    /* integral unshared data " */
    long ru_isrss;    /* integral unshared stack " */
    long ru_minflt;   /* page reclaims */
    long ru_majflt;   /* page faults */
    long ru_nswap;    /* swaps */
    long ru_inblock;  /* block input operations */
    long ru_oublock;  /* block output operations */
    long ru_msgsnd;   /* messages sent */
    long ru_msgrcv;   /* messages received */
    long ru_nsignals; /* signals received */
    long ru_nvcsw;    /* voluntary context switches */
    long ru_nivcsw;   /* involuntary " */
#define ru_last ru_nivcsw
};

__BEGIN_DECLS
extern int getrusage(int, struct rusage *);
extern int getrlimit(int, struct rlimit *);
extern int setrlimit(int, const struct rlimit *);

__END_DECLS
#endif
