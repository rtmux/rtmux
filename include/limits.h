/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* limits.h Machine and OS limits */
#ifndef _LIMITS_H_INCLUDED
#define _LIMITS_H_INCLUDED

#ifndef __PLATFORM_H_INCLUDED
#include <sys/platform.h>
#endif

/* ANSI required limits */
#define CHAR_BIT 8           /*  number of bits in a byte        */
#define SCHAR_MIN (-127 - 1) /*  minimum value of a signed char      */
#define SCHAR_MAX 127        /*  maximum value of a signed char      */
#define UCHAR_MAX 255        /*  maximum value of an unsigned char   */
#if defined(__CHAR_SIGNED__)
#define CHAR_MIN SCHAR_MIN /*  minimum value of a char         */
#define CHAR_MAX SCHAR_MAX /*  maximum value of a char         */
#elif defined(__CHAR_UNSIGNED__)
#define CHAR_MIN 0         /*  minimum value of a char         */
#define CHAR_MAX UCHAR_MAX /*  maximum value of a char         */
#else
#error CHAR sign not defined
#endif
#define MB_LEN_MAX 8 /*  maximum length of multibyte char    */

#define SHRT_MIN (-32767 - 1) /*  minimum value of a short int        */
#define SHRT_MAX 32767        /*  maximum value of a short int        */
#if __INT_BITS__ - 0 <= 16
#define USHRT_MAX 65535 /*  maximum value of an unsigned short  */
#else
#define USHRT_MAX 65535U /*  maximum value of an unsigned short  */
#endif

#if __LONG_BITS__ - 0 == 32
#define LONG_MAX 2147483647L        /*  maximum value of a long int     */
#define LONG_MIN (-2147483647L - 1) /* minimum value of a long int     */
#define ULONG_MAX 4294967295UL      /* maximum value of a unsigned long    */
#elif __LONG_BITS__ - 0 == 64
#define LONG_MIN \
    (-9223372036854775807L - 1)       /*  minimum value of a long        */
#define LONG_MAX 9223372036854775807L /*  maximum value of a long        */
#define ULONG_MAX \
    18446744073709551615UL /*  maximum value of an unsigned long */
#else
#error __LONG_BITS__ Not valid
#endif

#if __INT_BITS__ - 0 == 64
#define LLONG_MIN \
    (-9223372036854775807 - 1)        /*  minimum value of a long long        */
#define LLONG_MAX 9223372036854775807 /*  maximum value of a long long */
#define ULLONG_MAX \
    18446744073709551615U /*  maximum value of an unsigned long long */
#else
#define LLONG_MIN \
    (-9223372036854775807LL - 1) /*  minimum value of a long long        */
#define LLONG_MAX \
    9223372036854775807LL /*  maximum value of a long long        */
#define ULLONG_MAX \
    18446744073709551615ULL /*  maximum value of an unsigned long long */
#endif

#if __INT_BITS__ - 0 == 16
#define INT_MAX 32767        /*  maximum value of a int     */
#define INT_MIN (-32767 - 1) /* minimum value of a int     */
#define UINT_MAX 65535U      /* maximum value of a unsigned int   */
#elif __INT_BITS__ - 0 == 32
#define INT_MAX 2147483647        /*  maximum value of a int     */
#define INT_MIN (-2147483647 - 1) /* minimum value of a int     */
#define UINT_MAX 4294967295U      /* maximum value of a unsigned int   */
#elif __INT_BITS__ - 0 == 64
#define INT_MIN (-9223372036854775807 - 1) /*  minimum value of a int  */
#define INT_MAX 9223372036854775807        /*  maximum value of a int   */
#define UINT_MAX 18446744073709551615U /*  maximum value of an unsigned int */
#else
#error __INT_BITS__ Not valid
#endif


/* POSIX required limits. These are minimums values, not absolute limits, and
 * the real value may be greater.
 */

#define _POSIX_ARG_MAX 4096    /*  Length of args for exec             */
                               /*  functions including environment data */
#define _POSIX_CHILD_MAX 6     /*  Number of simultaneous              */
                               /*  processes per real user ID.         */
#define _POSIX_LINK_MAX 8      /*  Max. links per file                 */
#define _POSIX_MAX_CANON 255   /*  No. bytes in terminal               */
                               /*  cannonical input queue.             */
#define _POSIX_MAX_INPUT 255   /*  Size of terminal input queue buffer */
#define _POSIX_NAME_MAX 14     /*  Max bytes in a filename             */
#define _POSIX_NGROUPS_MAX 0   /*  Num. simultaneous supplementary     */
                               /*  group IDs per process               */
#define _POSIX_OPEN_MAX 16     /*  Min num. files open per process.    */
#define _POSIX_PATH_MAX 256    /*  Num. bytes in pathname (excl. NULL) */
#define _POSIX_PIPE_BUF 512    /*  Num. bytes written atomically to a  */
                               /*  pipe.                               */
#define _POSIX_SSIZE_MAX 32767 /*  The value that can be stored in an  */
                               /*  object of type ssize_t.             */
#define _POSIX_STREAM_MAX 8    /*  The number of streams that one      */
                               /*  process can have open at one time.  */
#define _POSIX_TZNAME_MAX 3    /*  The maximum number of bytes         */

/* supported for the name of a time zone (not of the TZ variable). */

/* 1003.1-2004 changed the following */
#undef _POSIX_CHILD_MAX
#define _POSIX_CHILD_MAX 25

#undef _POSIX_NGROUPS_MAX
#define _POSIX_NGROUPS_MAX 8

#undef _POSIX_OPEN_MAX
#define _POSIX_OPEN_MAX 20

#undef _POSIX_TZNAME_MAX
#define _POSIX_TZNAME_MAX 6

#undef _POSIX_HOST_NAME_MAX
#define _POSIX_HOST_NAME_MAX 255

#define _POSIX_AIO_LISTIO_MAX 2
#define _POSIX_AIO_MAX 1
#define _POSIX_DELAYTIMER_MAX 32
#define _POSIX_MQ_OPEN_MAX 8
#define _POSIX_MQ_PRIO_MAX 32
#define _POSIX_RTSIG_MAX 8
#define _POSIX_SEM_NSEMS_MAX 256
#define _POSIX_SEM_VALUE_MAX 32767
#define _POSIX_SIGQUEUE_MAX 32
#define _POSIX_TIMER_MAX 32

#define _POSIX_THREAD_DESTRUCTOR_ITERATIONS 4
#define _POSIX_THREAD_KEYS_MAX 128
#define _POSIX_THREAD_THREADS_MAX 64

#define _POSIX_SS_REPL_MAX 4
#define SS_REPL_MAX USHRT_MAX

#define _POSIX_INTERRUPT_OVERRUN_MAX 32

#define _POSIX_SYMLOOP_MAX 8 /*  The number of loops to traverse     */

/* while resolving symbolic links or prefix aliases. */
#define _POSIX_SYMLINK_MAX 255
#define _POSIX_UID_MAX 2

/* Run-time increasable values */

#define __NGROUPS_MAX 8
#define NGROUPS_MAX __NGROUPS_MAX /*  Num. simultaneous supplementary */
                                  /*  group IDs per process           */

/* Run-time invariant values (possible indeterminate).
 *
 * CHILD_MAX and OPEN_MAX are indeterminate. If not defined, use sysconf(_SC_?)
 * to determine their values.
 */

#define ARG_MAX 61440
#undef CHILD_MAX      /* no child max             */
#undef OPEN_MAX       /* no open max              */
#undef STREAM_MAX     /* no stream max            */
#define TZNAME_MAX 30 /* Implementation maximum   */

#undef AIO_LISTIO_MAX
#undef AIO_MAX
#undef AIO_PRIO_DELTA_MAX
#define DELAYTIMER_MAX (1 << 20)
#undef MQ_OPEN_MAX
#define MQ_PRIO_MAX _POSIX_MQ_PRIO_MAX
#undef PAGESIZE
#undef RTSIG_MAX
#undef SEM_NSEMS_MAX
#define SEM_VALUE_MAX (INT_MAX / 2)
#undef SIGQUEUE_MAX
#undef TIMER_MAX

#define PTHREAD_DESTRUCTOR_ITERATIONS _POSIX_THREAD_DESTRUCTOR_ITERATIONS
#define PTHREAD_KEYS_MAX _POSIX_THREAD_KEYS_MAX
#define PTHREAD_STACK_MIN 256
#undef PTHREAD_THREADS_MAX



/* Pathname variable values (may vary by pathname). If not defined, use
 * fpathconf(fd, _PC_?) where the fd-based query is meaningful.
 */
#define LINK_MAX 65535u
#define PIPE_BUF 5120
#undef MAX_CANNON
#undef MAX_INPUT
#undef NAME_MAX
#undef PATH_MAX
/* Posix .2/9 limits. */
#define BC_BASE_MAX 99
#define BC_DIM_MAX 2048
#define BC_SCALE_MAX 99
#define COLL_ELEM_MAX 4
#define EXPR_NEST_MAX 32
#define LINE_MAX 2048
#define PASTE_FILES_MAX 12
#define RE_DUP_MAX 255
#define SED_PATTERN_MAX 20480
#define SENDTO_MAX 90000L
#define SORT_LINE_MAX 20480

#define NAME_MAX 255
#define PATH_MAX 1024

#undef MIN_ALLOC_SIZE
#undef REC_MIN_XFER_SIZE
#undef REC_MAX_XFER_SIZE
#undef REC_INCR_XFER_SIZE
#undef REC_XFER_ALIGN
#undef MAX_ATOMIC_SIZE
#undef POSIX_REC_INCR_XFER_SIZE
#undef POSIX_ALLOC_SIZE_MIN
#undef POSIX_REC_MAX_XFER_SIZE
#undef POSIX_REC_MIN_XFER_SIZE
#undef POSIX_REC_XFER_ALIGN

/* Invariant values */

/* ssize_t is the signed counterpart of size_t (see <sys/_sizetypes.h>), which
 * is __SIZE_TYPE__ — "unsigned long" on RV64 LP64. SSIZE_MAX therefore tracks
 * LONG_MAX, not INT_MAX.
 */
#define SSIZE_MAX LONG_MAX



/* POSIX required limits. These are maximum values, not absolute limits, and the
 * real value may be less.
 */

#define _POSIX_CLOCKRES_MIN 20000000

#define _POSIX2_BC_BASE_MAX 99
#define _POSIX2_BC_DIM_MAX 2048
#define _POSIX2_BC_SCALE_MAX 99
#define _POSIX2_BC_STRING_MAX 1000
#define _POSIX2_COLL_WEIGHTS_MAX 2
#define _POSIX2_EXPR_NEST_MAX 32
#define _POSIX2_LINE_MAX 2048
#define _POSIX2_RE_DUP_MAX 255

/* Unix98 requirement limits */
#define ATEXIT_MAX \
    32 /* Maximum number of functions that may be registered with atexit */
#define _XOPEN_IOV_MAX 16
#undef IOV_MAX  /*  _XOPEN_IOV_MAX */
#undef PASS_MAX /*  20 */

#define BC_STRING_MAX 1000
#define COLL_WEIGHTS_MAX 2

#define WORD_BIT __INT_BITS__
#define CHARCLASS_NAME_MAX 14
#define NL_ARGMAX 9
#define NL_LANGMAX 14
#define NL_MSGMAX 32767
#undef NL_NMAX
#define NL_SETMAX 255
#define NL_TEXTMAX _POSIX2_LINE_MAX
#define NZERO 20
#ifndef TMP_MAX /* Also defined in stdio.h */
#define TMP_MAX (26 * 26 * 26)
#endif

#define LONGLONG_MIN LLONG_MIN
#define LONGLONG_MAX LLONG_MAX
#define ULONGLONG_MAX ULLONG_MAX

/* RTMux values */
#define SYMLOOP_MAX 16 /* This is bigger than _POSIX_SYMLOOP_MAX */
#undef SYMLINK_MAX     /* no fixed compile-time symlink target maximum */
#define _MALLOC_ALIGN 8U
#define RTMUX_MSG_ALIGN 8U

#endif
