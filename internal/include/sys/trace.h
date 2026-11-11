/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* sys/trace.h - private kernel trace-control ABI.
 *
 * Trace masks, buffer layouts, and event codes are shared by the kernel and
 * roottask diagnostics; they are not a POSIX user header.
 */

#ifndef __TRACE_H_INCLUDED
#define __TRACE_H_INCLUDED

#ifndef __PLATFORM_H_INCLUDED
#include <sys/platform.h>
#endif

#ifndef __STATES_H_INCLUDED
#include <sys/states.h>
#endif

#include <sys/spinlock.h>

/* Trace event modes */

/* Pointer-carrying commands pass a user pointer as two consecutive command
 * words, low half first (TRACE_PTR_LO / TRACE_PTR_HI); the kernel rebuilds it
 * with TRACE_UNPACK_PTR. One word would truncate to the low 4 GiB on LP64.
 *   INSERTCUSEREVENT : { op, code, ptr_lo, ptr_hi, nwords }
 *   INSERTUSRSTREVENT: { op, code, str_lo, str_hi }
 *   INSERTCCLASSEVENT: { op, <unused>, code, ptr_lo, ptr_hi, nwords }
 * The kernel copies the payload into kernel memory before use; a payload that
 * is unmapped or outside the process fails with EFAULT, an unterminated string
 * or a payload over the kernel's TRACE_USRSTR_MAX bytes (256, kernel/include/trace/events.h) with EINVAL.
 */
#define TRACE_PTR_LO(p) ((uint32_t) (uint64_t) (uintptr_t) (p))
#define TRACE_PTR_HI(p) ((uint32_t) ((uint64_t) (uintptr_t) (p) >> 32))
#define TRACE_UNPACK_PTR(type, lo, hi) \
    ((type) (uintptr_t) (((uint64_t) (hi) << 32) | (uint64_t) (uint32_t) (lo)))

#define RTMUX_TRACE_ADDALLCLASSES ((0x00000001 << 28) | 1)
#define RTMUX_TRACE_DELALLCLASSES ((0x00000001 << 28) | 2)
#define RTMUX_TRACE_ADDCLASS ((0x00000002 << 28) | 3)
#define RTMUX_TRACE_DELCLASS ((0x00000002 << 28) | 4)
#define RTMUX_TRACE_ADDEVENT ((0x00000003 << 28) | 5)
#define RTMUX_TRACE_DELEVENT ((0x00000003 << 28) | 6)
#define RTMUX_TRACE_ADDEVENTHANDLER ((0x00000005 << 28) | 7)
#define RTMUX_TRACE_DELEVENTHANDLER ((0x00000003 << 28) | 8)
#define RTMUX_TRACE_INSERTEVENT ((0x00000005 << 28) | 9)
#define RTMUX_TRACE_START ((0x00000001 << 28) | 10)
#define RTMUX_TRACE_STOP ((0x00000001 << 28) | 11)
#define RTMUX_TRACE_ALLOCBUFFER ((0x00000003 << 28) | 12)
#define RTMUX_TRACE_DEALLOCBUFFER ((0x00000001 << 28) | 13)
#define RTMUX_TRACE_FLUSHBUFFER ((0x00000001 << 28) | 14)
#define RTMUX_TRACE_QUERYEVENTS ((0x00000001 << 28) | 15)
#define RTMUX_TRACE_SETALLCLASSESFAST ((0x00000001 << 28) | 16)
#define RTMUX_TRACE_SETALLCLASSESWIDE ((0x00000001 << 28) | 17)
#define RTMUX_TRACE_SETCLASSFAST ((0x00000002 << 28) | 18)
#define RTMUX_TRACE_SETCLASSWIDE ((0x00000002 << 28) | 19)
#define RTMUX_TRACE_SETEVENTFAST ((0x00000003 << 28) | 20)
#define RTMUX_TRACE_SETEVENTWIDE ((0x00000003 << 28) | 21)
#define RTMUX_TRACE_SETCLASSPID ((0x00000003 << 28) | 22)
#define RTMUX_TRACE_SETCLASSTID ((0x00000004 << 28) | 23)
#define RTMUX_TRACE_SETEVENTPID ((0x00000004 << 28) | 24)
#define RTMUX_TRACE_SETEVENTTID ((0x00000005 << 28) | 25)
#define RTMUX_TRACE_CLRCLASSPID ((0x00000002 << 28) | 26)
#define RTMUX_TRACE_CLRCLASSTID ((0x00000002 << 28) | 27)
#define RTMUX_TRACE_CLREVENTPID ((0x00000003 << 28) | 28)
#define RTMUX_TRACE_CLREVENTTID ((0x00000003 << 28) | 29)
#define RTMUX_TRACE_INSERTSUSEREVENT ((0x00000004 << 28) | 30)
#define RTMUX_TRACE_INSERTCUSEREVENT ((0x00000005 << 28) | 31)
#define RTMUX_TRACE_INSERTUSRSTREVENT ((0x00000004 << 28) | 32)
#define RTMUX_TRACE_STARTNOSTATE ((0x00000001 << 28) | 33)
#define RTMUX_TRACE_SETRINGMODE ((0x00000001 << 28) | 34)
#define RTMUX_TRACE_SETLINEARMODE ((0x00000001 << 28) | 35)
#define RTMUX_TRACE_ADDCLASSEVHANDLER ((0x00000004 << 28) | 36)
#define RTMUX_TRACE_DELCLASSEVHANDLER ((0x00000002 << 28) | 37)
#define RTMUX_TRACE_SKIPBUFFER ((0x00000001 << 28) | 38)
#define RTMUX_TRACE_OVERWRITEBUFFER ((0x00000001 << 28) | 39)
#define RTMUX_TRACE_QUERYVERSION ((0x00000001 << 28) | 40)
#define RTMUX_TRACE_QUERYSUPPORT ((0x00000001 << 28) | 41)
#define RTMUX_TRACE_INSERTSCLASSEVENT ((0x00000005 << 28) | 42)
#define RTMUX_TRACE_INSERTCCLASSEVENT ((0x00000006 << 28) | 43)

/* Trace event classes */
enum {
    RTMUX_TRACE_EMPTY,
    RTMUX_TRACE_CONTROL,
    RTMUX_TRACE_KCALL,
    RTMUX_TRACE_KCALLENTER,
    RTMUX_TRACE_KCALLEXIT,
    RTMUX_TRACE_KCALLINT,
    RTMUX_TRACE_INT,
    RTMUX_TRACE_INTENTER,
    RTMUX_TRACE_INTEXIT,
    RTMUX_TRACE_PROCESS,
    RTMUX_TRACE_THREAD,
    RTMUX_TRACE_VTHREAD,
    RTMUX_TRACE_USER,
    RTMUX_TRACE_SYSTEM,
    RTMUX_TRACE_COMM,
    RTMUX_TRACE_INT_HANDLER_ENTER,
    RTMUX_TRACE_INT_HANDLER_EXIT,
};

/* Trace event selectors */
#define RTMUX_TRACE_EMPTYEVENT (0x00000000)

#define RTMUX_TRACE_CONTROLTIME (0x00000001 << 0)
#define RTMUX_TRACE_CONTROLBUFFER (0x00000001 << 1)

#define RTMUX_TRACE_KCALLFIRST (0x00000000)
#define RTMUX_TRACE_KCALLLAST (__KSYS_BAD)

#define RTMUX_TRACE_INTFIRST (0x00000000)
#define RTMUX_TRACE_INTLAST (0xffffffff)

#define RTMUX_TRACE_PROCCREATE (0x00000001 << 0)
#define RTMUX_TRACE_PROCDESTROY (0x00000001 << 1)
#define RTMUX_TRACE_PROCCREATE_NAME (0x00000001 << 2)
#define RTMUX_TRACE_PROCDESTROY_NAME (0x00000001 << 3)
#define RTMUX_TRACE_PROCTHREAD_NAME (0x00000001 << 4)

#define RTMUX_TRACE_THDEAD (0x00000001 << STATE_DEAD)
#define RTMUX_TRACE_THRUNNING (0x00000001 << STATE_RUNNING)
#define RTMUX_TRACE_THREADY (0x00000001 << STATE_READY)
#define RTMUX_TRACE_THSTOPPED (0x00000001 << STATE_STOPPED)
#define RTMUX_TRACE_THSEND (0x00000001 << STATE_SEND)
#define RTMUX_TRACE_THRECEIVE (0x00000001 << STATE_RECEIVE)
#define RTMUX_TRACE_THREPLY (0x00000001 << STATE_REPLY)
#define RTMUX_TRACE_THSTACK (0x00000001 << STATE_STACK)
#define RTMUX_TRACE_THWAITTHREAD (0x00000001 << STATE_WAITTHREAD)
#define RTMUX_TRACE_THWAITPAGE (0x00000001 << STATE_WAITPAGE)
#define RTMUX_TRACE_THSIGSUSPEND (0x00000001 << STATE_SIGSUSPEND)
#define RTMUX_TRACE_THSIGWAITINFO (0x00000001 << STATE_SIGWAITINFO)
#define RTMUX_TRACE_THNANOSLEEP (0x00000001 << STATE_NANOSLEEP)
#define RTMUX_TRACE_THMUTEX (0x00000001 << STATE_MUTEX)
#define RTMUX_TRACE_THCONDVAR (0x00000001 << STATE_CONDVAR)
#define RTMUX_TRACE_THJOIN (0x00000001 << STATE_JOIN)
#define RTMUX_TRACE_THINTR (0x00000001 << STATE_INTR)
#define RTMUX_TRACE_THSEM (0x00000001 << STATE_SEM)
#define RTMUX_TRACE_THWAITCTX (0x00000001 << STATE_WAITCTX)
#define RTMUX_TRACE_THNET_SEND (0x00000001 << STATE_NET_SEND)
#define RTMUX_TRACE_THNET_REPLY (0x00000001 << STATE_NET_REPLY)
#define RTMUX_TRACE_THCREATE (0x00000001 << _TRACE_THREAD_CREATE)
#define RTMUX_TRACE_THDESTROY (0x00000001 << _TRACE_THREAD_DESTROY)

#define RTMUX_TRACE_VTHDEAD (0x00000001 << STATE_DEAD)
#define RTMUX_TRACE_VTHRUNNING (0x00000001 << STATE_RUNNING)
#define RTMUX_TRACE_VTHREADY (0x00000001 << STATE_READY)
#define RTMUX_TRACE_VTHSTOPPED (0x00000001 << STATE_STOPPED)
#define RTMUX_TRACE_VTHSEND (0x00000001 << STATE_SEND)
#define RTMUX_TRACE_VTHRECEIVE (0x00000001 << STATE_RECEIVE)
#define RTMUX_TRACE_VTHREPLY (0x00000001 << STATE_REPLY)
#define RTMUX_TRACE_VTHSTACK (0x00000001 << STATE_STACK)
#define RTMUX_TRACE_VTHWAITTHREAD (0x00000001 << STATE_WAITTHREAD)
#define RTMUX_TRACE_VTHWAITPAGE (0x00000001 << STATE_WAITPAGE)
#define RTMUX_TRACE_VTHSIGSUSPEND (0x00000001 << STATE_SIGSUSPEND)
#define RTMUX_TRACE_VTHSIGWAITINFO (0x00000001 << STATE_SIGWAITINFO)
#define RTMUX_TRACE_VTHNANOSLEEP (0x00000001 << STATE_NANOSLEEP)
#define RTMUX_TRACE_VTHMUTEX (0x00000001 << STATE_MUTEX)
#define RTMUX_TRACE_VTHCONDVAR (0x00000001 << STATE_CONDVAR)
#define RTMUX_TRACE_VTHJOIN (0x00000001 << STATE_JOIN)
#define RTMUX_TRACE_VTHINTR (0x00000001 << STATE_INTR)
#define RTMUX_TRACE_VTHSEM (0x00000001 << STATE_SEM)
#define RTMUX_TRACE_VTHWAITCTX (0x00000001 << STATE_WAITCTX)
#define RTMUX_TRACE_VTHNET_SEND (0x00000001 << STATE_NET_SEND)
#define RTMUX_TRACE_VTHNET_REPLY (0x00000001 << STATE_NET_REPLY)
#define RTMUX_TRACE_VTHCREATE (0x00000001 << _TRACE_THREAD_CREATE)
#define RTMUX_TRACE_VTHDESTROY (0x00000001 << _TRACE_THREAD_DESTROY)

#define RTMUX_TRACE_USERFIRST (0x00000000)
#define RTMUX_TRACE_USERLAST (0x000003ff)
#define RTMUX_TRACE_USER_LKL_HOST_ENTER (0x00000200)
#define RTMUX_TRACE_USER_LKL_HOST_EXIT (0x00000201)
#define RTMUX_TRACE_USER_LKL_MEASURE_BEGIN (0x00000202)
#define RTMUX_TRACE_USER_LKL_MEASURE_END (0x00000203)
#define RTMUX_TRACE_USER_LKL_MEASURE_WAKE (0x00000204)
#define RTMUX_TRACE_USER_LKL_MEASURE_MONO_LOW (0x00000205)
#define RTMUX_TRACE_USER_LKL_MEASURE_MONO_HIGH (0x00000206)

#define RTMUX_TRACE_LKL_HOST_PAGE_ALLOC (0x00000001)
#define RTMUX_TRACE_LKL_HOST_PAGE_FREE (0x00000002)
#define RTMUX_TRACE_LKL_HOST_SEM_DOWN (0x00000003)
#define RTMUX_TRACE_LKL_HOST_MUTEX_LOCK (0x00000004)
#define RTMUX_TRACE_LKL_HOST_THREAD_JOIN (0x00000005)
#define RTMUX_TRACE_LKL_HOST_TIMER_SET_ONESHOT (0x00000006)
#define RTMUX_TRACE_LKL_HOST_MMAP (0x00000007)
#define RTMUX_TRACE_LKL_HOST_MUNMAP (0x00000008)

#define RTMUX_TRACE_LKL_MEASURE_FS (0x00000001)
#define RTMUX_TRACE_LKL_MEASURE_NET (0x00000002)
#define RTMUX_TRACE_LKL_MEASURE_NET_BLOCKED (0x00000003)
#define RTMUX_TRACE_LKL_MEASURE_NET_REVOKE (0x00000004)
#define RTMUX_TRACE_LKL_MEASURE_FS_RT (0x00000005)
#define RTMUX_TRACE_LKL_MEASURE_NET_RT (0x00000006)

#define RTMUX_TRACE_SYS_RESERVED (0x00000001)
#define RTMUX_TRACE_SYS_PATH (0x00000002)
#define RTMUX_TRACE_SYS_MMAP (0x00000006)
#define RTMUX_TRACE_SYS_MUNMAP (0x00000007)
#define RTMUX_TRACE_SYS_MAPNAME (0x00000008)
#define RTMUX_TRACE_SYS_ADDRESS (0x00000009)
#define RTMUX_TRACE_SYS_FUNC_ENTER (0x0000000a)
#define RTMUX_TRACE_SYS_FUNC_EXIT (0x0000000b)
#define RTMUX_TRACE_SYS_SLOG (0x0000000c)
#define RTMUX_TRACE_SYS_PI_BOOST (0x0000000d)
#define RTMUX_TRACE_SYS_LAST RTMUX_TRACE_SYS_PI_BOOST

#define RTMUX_TRACE_COMM_SMSG (0x00000000)
#define RTMUX_TRACE_COMM_SNOTIFICATION (0x00000001)
#define RTMUX_TRACE_COMM_RMSG (0x00000002)
#define RTMUX_TRACE_COMM_RNOTIFICATION (0x00000003)
#define RTMUX_TRACE_COMM_SNOTIFICATION_EXE (0x00000004)
#define RTMUX_TRACE_COMM_SNOTIFICATION_DIS (0x00000005)
#define RTMUX_TRACE_COMM_SNOTIFICATION_DEA (0x00000006)
#define RTMUX_TRACE_COMM_SNOTIFICATION_UN (0x00000007)
#define RTMUX_TRACE_COMM_SNOTIFICATION_QUN (0x00000008)
#define RTMUX_TRACE_COMM_SIGNAL (0x00000009)
#define RTMUX_TRACE_COMM_REPLY (0x0000000a)
#define RTMUX_TRACE_COMM_ERROR (0x0000000b)
#define RTMUX_TRACE_COMM_LAST RTMUX_TRACE_COMM_ERROR

#define RTMUX_TRACE_MAX_CLASS (0x0000001f)
#define RTMUX_TRACE_MAX_CODES (0x000003ff)

#define RTMUX_TRACE_SETEVENT_C(c, cl) (c &= ~(0x1f << 10), c |= (cl))
#define RTMUX_TRACE_GETEVENT_C(c) ((c) & (0x1f << 10))
#define RTMUX_TRACE_SETEVENT(c, e) (c &= ~(0x3ff), c |= (e))
#define RTMUX_TRACE_GETEVENT(c) ((c) & 0x3ff)
#define RTMUX_TRACE_GETCPU(h) (((h) & 0x3f000000) >> 24)

/* Query support flags */
#define RTMUX_TRACE_NOINSTRSUPP (0x00000000)
#define RTMUX_TRACE_SUPPINSTR (0x00000001 << 0)

/* feature index parameters */
enum { RTMUX_TRACE_FIPID, RTMUX_TRACE_FITID, RTMUX_TRACE_FI_NUM };

/* feature parameter masks */
#define RTMUX_TRACE_FMEMPTY (0x00000000)
#define RTMUX_TRACE_FMPID (1 << RTMUX_TRACE_FIPID)
#define RTMUX_TRACE_FMTID (1 << RTMUX_TRACE_FITID)

#define _TRACE_MAX_EVENT_NUM (0x00000400)

enum _TRACE_THREAD_STATE { /* Two add. thread-states */
                           _TRACE_THREAD_CREATE = STATE_MAX,
                           _TRACE_THREAD_DESTROY,
                           _TRACE_MAX_TH_STATE_NUM
};

typedef uint32_t __traceentry;

typedef struct traceevent {
    __traceentry header;  /* CPU, event, format */
    __traceentry data[3]; /* event data         */
} traceevent_t;

/* tracefile header keywords */
#define _TRACE_MAX_HEADER_LEN (512)
#define _TRACE_HEADER_PREFIX "TRACE_"
#define _TRACE_HEADER_POSTFIX "::"

#define _TRACE_HEADER_KEYWORDS()                                              \
    _TRACE_MK_HK(HEADER_BEGIN), _TRACE_MK_HK(FILE_NAME), _TRACE_MK_HK(DATE),  \
        _TRACE_MK_HK(VER_MAJOR), _TRACE_MK_HK(VER_MINOR),                     \
        _TRACE_MK_HK(LITTLE_ENDIAN), _TRACE_MK_HK(BIG_ENDIAN),                \
        _TRACE_MK_HK(MIDDLE_ENDIAN), _TRACE_MK_HK(ENCODING),                  \
        _TRACE_MK_HK(BOOT_DATE), _TRACE_MK_HK(CYCLES_PER_SEC),                \
        _TRACE_MK_HK(CPU_NUM), _TRACE_MK_HK(SYSNAME), _TRACE_MK_HK(NODENAME), \
        _TRACE_MK_HK(SYS_RELEASE), _TRACE_MK_HK(SYS_VERSION),                 \
        _TRACE_MK_HK(MACHINE), _TRACE_MK_HK(SYSPAGE_LEN),                     \
        _TRACE_MK_HK(HEADER_END)

/* buffer/sequence extraction macros */
#define _TRACE_GET_BUFFNUM(b) (b >> 13)
#define _TRACE_GET_BUFFSEQ(b) (b & 0x000007FF)

#define _TRACE_GET_FLAG(c) ((c) & 0x0fffffff)

/* Number of 32-bit words the opcode's command array occupies, carried in the
 * top nibble of every RTMUX_TRACE_* opcode. ksys_trace_event bounds-checks and
 * probes exactly this many words before reading past the opcode.
 */
#define _TRACE_GET_WORDS(c) (((c) >> 28) & 0xfU)
#define _TRACE_FLAGS_RING (0x00010000)
#define _TRACE_FLAGS_WRITING (0x00020000)
#define _TRACE_FLAGS_WRITTING _TRACE_FLAGS_WRITING
#define _TRACE_FLAGS_FLUSH (0x00040000)

#define _TRACE_MAX_KER_CALL_NUM (128)
#define _TRACE_MAX_INT_NUM (256)
#define _TRACE_MAX_COMM_NUM (RTMUX_TRACE_COMM_LAST + 1)
#define _TRACE_MAX_SYSTEM_NUM (RTMUX_TRACE_SYS_LAST + 1)
#define _TRACE_MAX_USER_NUM (RTMUX_TRACE_USERLAST + 1)

/* event data filled by an event handler */
typedef struct {
    __traceentry header;                  /* same as traceevent header       */
    uint32_t *data_array;                 /* initialized by the user         */
    uint32_t el_num;                      /* number of elements returned     */
    void *area;                           /* user data                       */
    uint32_t feature_mask;                /* bits indicate valid features    */
    uint32_t feature[RTMUX_TRACE_FI_NUM]; /* feature array - additional data */
} event_data_t;

/* S-simple, C-comb., CC-comb.-cont, B-begin, E-end */
#define _TRACE_STRUCT_S (0x00000000U)
#define _TRACE_STRUCT_CB (0x00000001U << 30)
#define _TRACE_STRUCT_CC (0x00000002U << 30)
#define _TRACE_STRUCT_CE (0x00000003U << 30)
#define _TRACE_GET_STRUCT(c) ((c) & (0x3U << 30))

/* Internal classes (C-class). */
#define _TRACE_EMPTY_C (0x00000000 << 10)
#define _TRACE_CONTROL_C (0x00000001 << 10)
#define _TRACE_KER_CALL_C (0x00000002 << 10)
#define _TRACE_INT_C (0x00000003 << 10)
#define _TRACE_PR_TH_C (0x00000004 << 10)
#define _TRACE_SYSTEM_C (0x00000005 << 10)
#define _TRACE_CONTAINER_C _TRACE_SYSTEM_C /* Container class never defined */
#define _TRACE_USER_C (0x00000006 << 10)
#define _TRACE_COMM_C (0x00000007 << 10)
#define _TRACE_TOT_CLASS_NUM (8)

/* pseudo-events since we can't use direct int. num */
#define _TRACE_INT_ENTRY (1)
#define _TRACE_INT_EXIT (2)
#define _TRACE_INT_HANDLER_ENTRY (3)
#define _TRACE_INT_HANDLER_EXIT (4)
#define _TRACE_IRQ_CLAIM_EVENT (5)
#define _TRACE_IST_DISPATCH_EVENT (6)

#define _TRACE_CONTROL_TIME (0x00000001)
#define _TRACE_CONTROL_BUFFER (0x00000002)

/* thread create/destroy */
#define _TRACE_PR_TH_CREATE_T (_TRACE_THREAD_CREATE)
#define _TRACE_PR_TH_DESTROY_T (_TRACE_THREAD_DESTROY)
#define _TRACE_PR_TH_CREATE_VT (_TRACE_PR_TH_CREATE_T + _TRACE_MAX_TH_STATE_NUM)
#define _TRACE_PR_TH_DESTROY_VT \
    (_TRACE_PR_TH_DESTROY_T + _TRACE_MAX_TH_STATE_NUM)

/* process create/destroy => (event<64) */
#define _TRACE_PR_TH_CREATE_P (0x00000001 << 6)
#define _TRACE_PR_TH_DESTROY_P (0x00000002 << 6)
#define _TRACE_PR_TH_CREATE_P_NAME (0x00000003 << 6)
#define _TRACE_PR_TH_DESTROY_P_NAME (0x00000004 << 6)
/* additional thread information */
#define _TRACE_PR_TH_NAME_T (0x00000005 << 6)

/* size of individual trace buffers */
#define _TRACEBUFSIZE (16 * 1024)
#define _TRACEBUFEMPTY (300)
#define _TRACELEMENTS \
    ((_TRACEBUFSIZE - sizeof(struct traceheader)) / sizeof(struct traceevent))

/* kernel <=> tracelogger interface */

#ifndef _SYS_TIME_H
#include <sys/time.h>
#endif

#ifndef _STDARG_H_INCLUDED
#include <stdarg.h>
#endif

typedef struct tracebuf {
    struct traceheader {
        struct tracebuf *baseaddr; /* base address of the trace buffer  */
        struct tracebuf *next;     /* pointer to the next trace buffer  */
        uint32_t flags;            /* event mask, locking states, ...   */
        uint32_t num_events;       /* number of events in the buffer    */
        uint32_t seq_buff_num;     /* buffer number in the sequence     */
        uint32_t ls13_rb_num;      /* 13 pos left-shifted ring buff num */
        struct intrspin spin;      /* spin lock (assume 32-bit length)  */
        traceevent_t volatile *begin_ptr; /* begin of the traceevent chain  */
        traceevent_t volatile *tail_ptr;  /* end of the traceevent chain    */
        uint32_t reserved[31]; /* reserved for future expansions    */
    } h;
    struct traceevent data[_TRACELEMENTS];
} tracebuf_t;

/* Public trace control syscall (RTMUX_TRACE_* opcode arrays); used by the
 * tracelogger to allocate/start the kernel trace ring.
 */
extern int trace_event(unsigned *data);

#endif
