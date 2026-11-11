/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* sys/siginfo.h Signal definitions */
#ifndef __SIGINFO_H_INCLUDED

#ifndef __PLATFORM_H_INCLUDED
#include <sys/platform.h>
#endif

#define __SIGINFO_H_INCLUDED

#ifndef __SIGINFO_H_DECLARED
#define __SIGINFO_H_DECLARED

#define SIGEV_NONE 0          /* notify */
#define SIGEV_SIGNAL 1        /* notify, signo, value */
#define SIGEV_SIGNAL_THREAD 3 /* notify, signo, value, code */
#define SIGEV_NOTIFICATION 4  /* notify, coid, priority, code, value */
#define SIGEV_UNBLOCK 5       /* notify */
#define SIGEV_INTR 6          /* notify */
#define SIGEV_THREAD 7        /* notify, notify_function, notify_attributes */



/* Make some nice macros for initializing the sigevent structure */

#define SIGEV_NONE_INIT(__e) ((__e)->sigev_notify = SIGEV_NONE)

/* SIGEV_SIGNAL_THREAD needed by pipe I/O service (SIGPIPE delivery) */
#define SIGEV_SIGNAL_THREAD_INIT(__e, __s, __v, __c)                        \
    ((__e)->sigev_notify = SIGEV_SIGNAL_THREAD, (__e)->sigev_signo = (__s), \
     (__e)->sigev_value.sival_ptr = (void *) (__v), (__e)->sigev_code = (__c))

#define SIGEV_NOTIFICATION_INIT(__e, __f, __p, __c, __v)                  \
    ((__e)->sigev_notify = SIGEV_NOTIFICATION, (__e)->sigev_coid = (__f), \
     (__e)->sigev_priority = (__p),                                       \
     (__e)->sigev_value.sival_ptr = (void *) (__v),                       \
     (__e)->sigev_code = (short) (__c))

#define SIGEV_UNBLOCK_INIT(__e) ((__e)->sigev_notify = SIGEV_UNBLOCK)

#define SIGEV_INTR_INIT(__e) ((__e)->sigev_notify = SIGEV_INTR)

#define SIGEV_THREAD_INIT(__e, __f, __v, __a)                                  \
    ((__e)->sigev_notify = SIGEV_THREAD, (__e)->sigev_notify_function = (__f), \
     (__e)->sigev_value.sival_ptr = (void *) (__v),                            \
     (__e)->sigev_notify_attributes = (__a))


#define SIGEV_TYPE_MASK 0x000000ff
#define SIGEV_FLAG_CRITICAL 0x00000100


/* Also, to test the type of a sigevent, don't compare .SIGEV_NOTIFY, as some
 * hidden bits may be set, instead use SIGEV_GET_TYPE, example if(
 * SIGEV_GET_TYPE(my_event) == SIGEV_NOTIFICATION) ...
 */
#define SIGEV_GET_TYPE(__e) ((__e)->sigev_notify & SIGEV_TYPE_MASK)


/* to make threads use "critical time" as defined by the optional Adaptive
 * Partitioning scheduler, set a sigevent to be critical with:
 */
#define SIGEV_MAKE_CRITICAL(__e) ((__e)->sigev_notify |= SIGEV_FLAG_CRITICAL)



/* For user-originated signal info, these codes use si_pid as the sending pid
 * and si_uid as the real uid of the signaling process.
 */
#define SI_USER 0
#define SI_QUEUE (-2)
#define SI_TIMER (-3)
#define SI_ASYNCIO (-4)
#define SI_MESGQ (-5)
#define SI_NOTIFY (-6)
/* RTMux reserves this range for internal process-state codes. */
#define SI_MINAVAIL (-128)

/* SIGILL codes (si_addr == address of faulting instruction) */
#define ILL_ILLOPC 1 /* Illegal opcode */
#define ILL_ILLOPN 2 /* Illegal operand */
#define ILL_ILLADR 3 /* Illegal addressing mode */
#define ILL_ILLTRP 4 /* Illegal trap */
#define ILL_PRVOPC 5 /* Privileged opcode */
#define ILL_PRVREG 6 /* Privileged register */
#define ILL_COPROC 7 /* Coprocessor error */
#define ILL_BADSTK 8 /* internal stack error */

/* SIGSEGV codes (si_addr == address of faulting memory reference) */
#define SEGV_MAPERR 1 /* Address not mapped */
#define SEGV_ACCERR 2 /* No permissions */

/* SIGBUS codes (si_addr == address of faulting memory reference) */
#define BUS_ADRALN 1 /* Invalid address alignment */
#define BUS_ADRERR 2 /* Non-existant physical address */
#define BUS_OBJERR \
    3 /* Object specific hardware error (e.g. NMI parity error) */

/* SIGCHILD codes (si_pid == child pid, si_status == exit value or signal,
 * si_uid == real uid of signaling process)
 */
#define CLD_EXITED 1    /* Child has exited (si_status == exit value) */
#define CLD_KILLED 2    /* Child was killed */
#define CLD_DUMPED 3    /* Child terminated abnormally */
#define CLD_TRAPPED 4   /* Traced child has trapped */
#define CLD_STOPPED 5   /* Child has stopped */
#define CLD_CONTINUED 6 /* Stopped child had continued */

/* SIGTRAP codes */
#define TRAP_BRKPT 1 /* Break Point */
#define TRAP_TRACE 2 /* Trace */

/* SIGFPE signal codes (si_addr == address of faulting instruction) */
#define FPE_INTDIV 1 /* Integer divide by zero */
#define FPE_INTOVF 2 /* Integer overflow */
#define FPE_FLTDIV 3 /* Floating point divide by zero */
#define FPE_FLTOVF 4 /* Floating point overflow */
#define FPE_FLTUND 5 /* Floating point underflow */
#define FPE_FLTRES 6 /* Floating point inexact result */
#define FPE_FLTINV 7 /* Invalid floating point operation */
#define FPE_FLTSUB 8 /* Subscript out of range */

#define si_pid __proc.__pid
#define si_value __proc.__pdata.__kill.__value
#define si_uid __proc.__pdata.__kill.__uid
#define si_status __proc.__pdata.__chld.__status
#define si_utime __proc.__pdata.__chld.__utime
#define si_stime __proc.__pdata.__chld.__stime
#define si_fltno __fault.__fltno
#define si_trapno si_fltno
#define si_addr __fault.__addr
#define si_fltip __fault.__fltip
#define si_bdslot __fault.__bdslot

#endif

#endif
