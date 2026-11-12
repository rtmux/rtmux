/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef _RTMUX_CORE_H
#define _RTMUX_CORE_H

#include <sys/platform.h>
#include <sys/types.h>
#include <sys/states.h>
#include <sys/sched-ops.h>
#include <limits.h>

typedef uint64_t paddr_t;
typedef uint32_t paddr32_t;
typedef uint64_t paddr64_t;

#ifndef _NULL
#define _NULL 0
#endif

#ifndef _WCMAX
#define _WCMAX 0x7fffffff
#endif

#if _WCMAX == 0x7fffffff
#define _WCMIN 0
#else
#define _WCMIN (-_WCMAX - 1)
#endif

typedef struct iovec {
    void *iov_base;
    size_t iov_len;
} iov_t;

/* Signal structures */
typedef struct _stack {
    void *ss_sp;
    size_t ss_size;
    int ss_flags;
} stack_t;

struct _clockadjust {
    unsigned long tick_count;
    long tick_nsec_inc;
};

struct _itimer {
    uint64_t nsec;
    uint64_t interval_nsec;
};

struct _cred_info {
    uid_t ruid;
    uid_t euid;
    uid_t suid;
    gid_t rgid;
    gid_t egid;
    gid_t sgid;
    uint32_t ngroups;
    gid_t grouplist[__NGROUPS_MAX];
};

struct _timer_info {
    struct _itimer itime;
    struct _itimer otime;
    uint32_t flags;
    int32_t tid;
    int32_t notify;
    clockid_t clockid;
    uint32_t overruns;
    struct sigevent event;
};

#if defined(__GNUC__) || defined(__INTEL_COMPILER)
#define __SECSTR(__sec, __s) \
    __asm__(".section " #__sec ";.asciz \"" __s "\";.previous")
#endif

__BEGIN_DECLS

#define RTMUX_TIMEOUT_RECEIVE (1 << STATE_RECEIVE)
#define RTMUX_TIMEOUT_SEND (1 << STATE_SEND)
#define RTMUX_TIMEOUT_REPLY (1 << STATE_REPLY)
#define RTMUX_TIMEOUT_SIGSUSPEND (1 << STATE_SIGSUSPEND)
#define RTMUX_TIMEOUT_SIGWAITINFO (1 << STATE_SIGWAITINFO)
#define RTMUX_TIMEOUT_NANOSLEEP (1 << STATE_NANOSLEEP)
#define RTMUX_TIMEOUT_MUTEX (1 << STATE_MUTEX)
#define RTMUX_TIMEOUT_CONDVAR (1 << STATE_CONDVAR)
#define RTMUX_TIMEOUT_JOIN (1 << STATE_JOIN)
#define RTMUX_TIMEOUT_INTR (1 << STATE_INTR)
#define RTMUX_TIMEOUT_SEM (1 << STATE_SEM)

struct _msg_info {      /* _msg_info    _server_info */
    uint32_t nd;        /*  client      server */
    uint32_t srcnd;     /*  server      n/a */
    pid_t pid;          /*  client      server */
    int32_t tid;        /*  thread      n/a */
    int32_t chid;       /*  server      server */
    int32_t scoid;      /*  server      server */
    int32_t coid;       /*  client      client */
    uint32_t msglen;    /*  msg         n/a */
    uint32_t srcmsglen; /*  thread      n/a */
    uint32_t dstmsglen; /*  thread      n/a */
    int16_t priority;   /*  thread      n/a */
    uint16_t flags;     /*  n/a         client */
    uint32_t endpoint_rights;
    uint64_t endpoint_cap;
};

#define _server_info _msg_info
#define RTMUX_MI_ENDIAN_BIG 0x00000001 /* Same as RTMUX_CI_ENDIAN_BIG */
#define RTMUX_MI_ENDIAN_DIFF 0x00000002
#define RTMUX_MI_NET_CRED_DIRTY 0x00000200
#define RTMUX_MI_UNBLOCK_REQ 0x00000100

typedef struct _client_info {
    uint32_t nd;
    pid_t pid;
    pid_t sid;
    uint32_t flags;
    struct _cred_info cred;
} client_info_t;

#define RTMUX_FLG_CI_ENDIAN_BIG 0x00000001 /* Same as RTMUX_MI_ENDIAN_BIG */
#define RTMUX_FLG_CI_STOPPED 0x00000080

struct _vtid_info {
    int32_t tid;
    uint32_t coid;
    uint32_t priority;
    uint32_t srcmsglen;
    int32_t keydata;
    uint32_t srcnd;
    uint32_t dstmsglen;
    int32_t zero;
};

#define RTMUX_TIMER_SEARCH 0x00000001
#define RTMUX_TIMER_RESET_OVERRUNS 0x00000002

/* struct _sighandler_info removed — signal trampoline eliminated */

struct _sched_info {
    int priority_min;
    int priority_max;
    uint64_t interval;
    int priority_priv;
    int reserved[11];
};

#define RTMUX_TI_ACTIVE 0x00000001
#define RTMUX_TI_ABSOLUTE 0x00000002
#define RTMUX_TI_EXPIRED 0x00000004
#define RTMUX_TI_TOD_BASED 0x00000008
#define RTMUX_TI_TARGET_PROCESS 0x00000010

/* RTMux notification = the single async-delivery primitive unifying what Linux
 * splits across threaded-IRQ wakeups, eventfd, and completions. It is
 * value-carrying and queued, pulled by the receiver via MsgReceive; it is not a
 * POSIX signal and not a lossy badge.
 *
 * Definitions for type and subtype of any notification. We keep the codes away
 * from the defined SI_ signal codes to prevent confusion.
 */
#define _NOTIFICATION_TYPE 0
#define _NOTIFICATION_SUBTYPE 0
#define _NOTIFICATION_CODE_UNBLOCK (-32)     /* value - rcvid */
#define _NOTIFICATION_CODE_DISCONNECT (-33)  /* value - server connection */
#define _NOTIFICATION_CODE_THREADDEATH (-34) /* value - thread id */
#define _NOTIFICATION_CODE_COIDDEATH (-35)   /* value - coid */
#define _NOTIFICATION_CODE_NET_ACK            \
    (-36) /* value - vtid; not in use anymore \
           */
#define _NOTIFICATION_CODE_NET_UNBLOCK (-37) /* value - vtid */
#define _NOTIFICATION_CODE_NET_DETACH (-38)  /* value - coid */
#define _NOTIFICATION_CODE_PROC_TERMINATED \
    (-39) /* value - terminated process pid */
#define _NOTIFICATION_CODE_SIGNAL               \
    (-40) /* value - signo | (sender_pid << 16) \
           */

#define _NOTIFICATION_CODE_MINAVAIL 0 /* RTMux never uses this range */
#define _NOTIFICATION_CODE_MAXAVAIL 127

struct _notification {
    uint16_t type;
    uint16_t subtype;
    int8_t code;
    uint8_t zero[3];
    /* value is the payload; scoid is the source connection cookie. */
    union sigval value;
    int32_t scoid;
};


/* Define interrupt flags. */
#define RTMUX_HARD_FLAGS_END 0x01

/* Define flags which can be applied to a notification type. */
#define RTMUX_NOTIFICATION_IF_UNIQUE 0x1000
#define RTMUX_NOTIFICATION_REPLACE 0x2000

/* Define the flags of a process (process_t.flags) */
#define RTMUX_FLG_PROC_NOCLDSTOP 1U
#define RTMUX_FLG_PROC_LOADING 2U
#define RTMUX_FLG_PROC_TERMING 4U
#define RTMUX_FLG_PROC_ZOMBIE 8U
#define RTMUX_FLG_PROC_NOZOMBIE 0x10U
#define RTMUX_FLG_PROC_FORKED 0x20U
#define RTMUX_FLG_PROC_ORPHAN_PGRP 0x40U
#define RTMUX_FLG_PROC_STOPPED 0x80U
#define RTMUX_FLG_PROC_DEBUG_STOPPED 0x100U
#define RTMUX_FLG_PROC_BKGND_PGRP 0x200U
#define RTMUX_FLG_PROC_NO_LIMITS 0x400U
#define RTMUX_FLG_PROC_CONTINUED 0x800U
#define RTMUX_FLG_PROC_CHECK_INTR 0x1000U
#define RTMUX_FLG_PROC_COREDUMP 0x2000U
#define RTMUX_FLG_PROC_PTRACED 0x4000U
#define RTMUX_FLG_PROC_SLEADER 0x10000U
#define RTMUX_FLG_PROC_WAITINFO 0x20000U
#define RTMUX_FLG_PROC_DESTROYALL 0x80000U
#define RTMUX_FLG_PROC_NOCOREDUMP 0x100000U
#define RTMUX_FLG_PROC_NOCTTY 0x200000U
#define RTMUX_FLG_PROC_WAITDONE 0x400000U
#define RTMUX_FLG_PROC_TERM_WAITING 0x800000U
#define RTMUX_FLG_PROC_THREADWATCH 0x80000000U

/* Define the flags of a thread thread_t.flags */
#define RTMUX_FLG_THR_INTR_PENDING 0x00010000U
#define RTMUX_FLG_THR_DETACHED 0x00020000U
#define RTMUX_FLG_THR_THREADS_HOLD 0x00100000U
#define RTMUX_FLG_THR_UNBLOCK_REQ 0x00400000U
#define RTMUX_FLG_THR_ALIGN_FAULT 0x01000000U
#define RTMUX_FLG_THR_SSTEP 0x02000000U
#define RTMUX_FLG_THR_ALLOCED_STACK 0x04000000U
#define RTMUX_FLG_THR_NOMULTISIG 0x08000000U
#define RTMUX_FLG_THR_FROZEN 0x10000000U
#define RTMUX_FLG_THR_IOPRIV 0x80000000U

/* Thread Control commands */
#define RTMUX_TCTL_IO 1
#define RTMUX_TCTL_THREADS_HOLD 2
#define RTMUX_TCTL_THREADS_CONT 3
#define RTMUX_TCTL_RUNMASK 4
#define RTMUX_TCTL_ALIGN_FAULT 5
#define RTMUX_TCTL_RUNMASK_GET_AND_SET 6
#define RTMUX_TCTL_PERFCOUNT 7
#define RTMUX_TCTL_ONE_THREAD_HOLD 8
#define RTMUX_TCTL_ONE_THREAD_CONT 9
#define RTMUX_TCTL_RUNMASK_GET_AND_SET_INHERIT 10
#define RTMUX_TCTL_NAME 11
#define RTMUX_TCTL_THREAD_DESTROY_STATS 12
/* data = (void *) 1 / (void *) 0: allow / forbid a native capability header in
 * the replies to this thread's own requests (calling thread only).
 */
#define RTMUX_TCTL_NATIVE_REPLY 13
/* data = the 1-based tid of a thread of the calling process (the pthread_t
 * value). Break that thread's interruptible blocked call with EINTR (message
 * send/reply/receive, condvar, semaphore, sleep, join; never a mutex wait).
 * Returns EAGAIN when the thread is not in such a wait, ESRCH for an unknown
 * tid. Same-process only.
 */
#define RTMUX_TCTL_INTERRUPT 14
#define RTMUX_TCTL_RESERVED 0x80000000

struct _thread_destroy_stats {
    uint32_t reset;
    uint32_t reserved;
    uint64_t calls;
    uint64_t rcu_wait_ns;
    uint64_t rcu_enroll_ns;
    uint64_t rcu_scan_ns;
    uint64_t rcu_post_scan_ns;
    uint64_t pin_wait_ns;
    uint64_t active_scan_ns;
    uint64_t object_free_ns;
    uint64_t object_free_heap;
    uint64_t object_free_cache;
    uint64_t object_free_heap_ns;
    uint64_t object_free_cache_ns;
    uint64_t object_free_cache_clear_ns;
    uint64_t object_free_cache_link_ns;
    uint64_t pin_waited;
    uint64_t pin_skipped;
    uint64_t rcu_waited;
    uint64_t rcu_skipped;
    uint64_t rcu_waited_ns;
    uint64_t rcu_skipped_ns;
    uint64_t rcu_busy_peer_sum;
    uint64_t rcu_busy_peer_max;
    uint64_t rcu_busy_thread;
    uint64_t rcu_busy_msg;
    uint64_t rcu_busy_sys;
    uint64_t rcu_busy_channel;
    uint64_t rcu_busy_connect;
    uint64_t rcu_busy_interrupt;
    uint64_t rcu_busy_sync;
    uint64_t rcu_busy_sched;
    uint64_t rcu_busy_time;
    uint64_t rcu_busy_idle;
    uint64_t rcu_busy_dispatch;
    uint64_t rcu_busy_timer;
    uint64_t rcu_busy_fault;
    uint64_t rcu_busy_priv;
    uint64_t rcu_busy_heartbeat;
    uint64_t rcu_busy_priv_system;
    uint64_t rcu_busy_priv_vm;
    uint64_t rcu_busy_priv_ipc;
    uint64_t rcu_busy_priv_process;
    uint64_t rcu_busy_priv_thread;
    uint64_t rcu_busy_priv_pa;
    uint64_t rcu_busy_priv_pa_alloc;
    uint64_t rcu_busy_priv_pa_free;
    uint64_t rcu_busy_priv_pa_zero;
    uint64_t rcu_busy_priv_pa_info;
    uint64_t rcu_busy_priv_pa_xlat;
    uint64_t rcu_busy_priv_pa_pq;
    uint64_t rcu_busy_unstamped;
    uint64_t rcu_busy_other;
    uint64_t destroy_claim_active;
    uint64_t destroy_claim_reserved;
    uint64_t destroy_join_wake;
    uint64_t destroy_join_wake_ns;
    uint64_t destroy_join_post_ready;
    uint64_t destroy_join_post_ready_ns;
    uint64_t destroy_post_vector_ns;
    uint64_t destroy_post_cpu_ns;
    uint64_t destroy_post_name_ns;
    uint64_t destroy_post_notify_ns;
};

/* Limit on the maximum number of characters for a thread name */
#define RTMUX_FLG_THREAD_NAME_MAX 100

struct _thread_name {
    int new_name_len;
    int name_buf_len;
    char name_buf[1];
};

/* Where 'size' must be the precise value required to define the number of cpus
 * currently on the system: eg. RMSK_SIZE(_syspage_ptr->num_cpu).
 */
struct _thread_runmask {
    unsigned int size;
    /*	unsigned	runmask[size];		*/
    /*	unsigned	inherit_mask[size];	*/
};

#define RMSK_SIZE(num_cpu) (((unsigned) (num_cpu) - 1U) / __INT_BITS__ + 1U)

/* Where 'cpu' is zero based */
#define RMSK_SET(cpu, p) \
    ((p)[(cpu) / __INT_BITS__] |= (1U << ((cpu) % __INT_BITS__)))

#define RMSK_CLR(cpu, p) \
    ((p)[(cpu) / __INT_BITS__] &= ~(1U << ((cpu) % __INT_BITS__)))

#define RMSK_ISSET(cpu, p) \
    ((p)[(cpu) / __INT_BITS__] & (1U << ((cpu) % __INT_BITS__)))

/* Define channel flags */
#define RTMUX_FLG_CHF_FIXED_PRIORITY 0x0001U
#define RTMUX_FLG_CHF_UNBLOCK 0x0002U
#define RTMUX_FLG_CHF_THREAD_DEATH 0x0004U
#define RTMUX_FLG_CHF_DISCONNECT 0x0008U
#define RTMUX_FLG_CHF_NET_MSG 0x0010U
#define RTMUX_FLG_CHF_SENDER_LEN 0x0020U
#define RTMUX_FLG_CHF_COID_DISCONNECT 0x0040U
#define RTMUX_FLG_CHF_REPLY_LEN 0x0080U
#define RTMUX_FLG_CHF_STICKY 0x0100U
#define RTMUX_FLG_CHF_ASYNC_NONBLOCK 0x0200U
/* Capability-gate a global channel: connect by chid alone is refused unless the
 * caller holds a live capability over the channel. Only meaningful together
 * with RTMUX_FLG_CHF_GLOBAL. See channel_create_cap().
 */
#define RTMUX_FLG_CHF_CAP_GATED 0x0400U
#define RTMUX_FLG_CHF_GLOBAL 0x0800U


/* Define connect flags */
#define RTMUX_FLG_COF_CLOEXEC 0x0001U
#define RTMUX_FLG_COF_DEAD 0x0002U
#define RTMUX_FLG_COF_NOSHARE 0x0040U
#define RTMUX_FLG_COF_NETCON 0x0080U
#define RTMUX_FLG_COF_NONBLOCK 0x0100U
#define RTMUX_FLG_COF_ASYNC 0x0200U
#define RTMUX_FLG_COF_GLOBAL 0x0400U
/* Connect-flags operation, not a stored flag: when this bit is in the
 * connect_flags mask, the kernel clears RTMUX_CAP_ENDPOINT_SEND from the
 * connection's rights, so the holder can no longer send or arm notifications on
 * it. Revocation is one-way (the bits value is ignored for this bit); the
 * connection itself stays live for its other operations.
 */
#define RTMUX_FLG_COF_REVOKE_SEND 0x0800U

#define RTMUX_CAP_ENDPOINT_SEND 0x00000001U
#define RTMUX_CAP_ENDPOINT_RECEIVE 0x00000002U
#define RTMUX_CAP_ENDPOINT_GRANT 0x00000004U
#define RTMUX_CAP_ENDPOINT_ALL                              \
    (RTMUX_CAP_ENDPOINT_SEND | RTMUX_CAP_ENDPOINT_RECEIVE | \
     RTMUX_CAP_ENDPOINT_GRANT)

#define RTMUX_CAP_MEM_MAP_READ 0x00000001U
#define RTMUX_CAP_MEM_MAP_WRITE 0x00000002U
#define RTMUX_CAP_MEM_MAP_EXEC 0x00000004U
#define RTMUX_CAP_MEM_MAP_ALL \
    (RTMUX_CAP_MEM_MAP_READ | RTMUX_CAP_MEM_MAP_WRITE | RTMUX_CAP_MEM_MAP_EXEC)

/* Rights for a RTMUX_CAP_TYPE_PROCESS capability (an object cap over a
 * process_t). The low bits deliberately alias the endpoint and mem-map rights:
 * rights are only ever compared within one object class, because
 * cap_lookup_pin_typed rejects a type mismatch before the rights mask is
 * tested, so a process-rights value is never interpreted against an endpoint
 * cap. This mirrors how RTMUX_CAP_MEM_MAP_* already reuses the same low bits.
 */
#define RTMUX_CAP_PROCESS_INSPECT 0x00000001U
#define RTMUX_CAP_PROCESS_MAKE_THREAD 0x00000002U
#define RTMUX_CAP_PROCESS_CONTROL 0x00000004U
#define RTMUX_CAP_PROCESS_KILL 0x00000008U
#define RTMUX_CAP_PROCESS_ALL                                    \
    (RTMUX_CAP_PROCESS_INSPECT | RTMUX_CAP_PROCESS_MAKE_THREAD | \
     RTMUX_CAP_PROCESS_CONTROL | RTMUX_CAP_PROCESS_KILL)

/* The second-highest bit marks a side-channel fd outside the connection vector.
 */
#define RTMUX_SIDE_CHANNEL ((~0u ^ (~0u >> 1)) >> 1)

/* The second-highest bit marks a global channel id. */
#define RTMUX_GLOBAL_CHANNEL ((~0u ^ (~0u >> 1)) >> 1)


/* Define flags of a timeout The first n bits are reserved for (1L <<
 * thp->state)
 */
#define RTMUX_FLG_TIMEOUT_MASK ((1 << STATE_MAX) - 1)
#define RTMUX_FLG_TIMEOUT_ACTIVE (1 << STATE_MAX)
#define RTMUX_FLG_TIMEOUT_IMMEDIATE (1 << (STATE_MAX + 1))

/* Flag bits for interrupt_attach_thread / __interrupt_attach_event */
#define RTMUX_INTR_FLAGS_END 0x01
#define RTMUX_INTR_FLAGS_NO_UNMASK 0x02
#define RTMUX_INTR_FLAGS_PROCESS 0x04
#define RTMUX_INTR_FLAGS_TRK_MSK 0x08
#define RTMUX_INTR_FLAGS_EXCLUSIVE 0x10

/* Flag bits for interrupt_wait. RTMux-only identifiers.
 *
 *   RTMUX_INTR_WAIT_FLAGS_UNMASK — re-unmask the IRQ level before the
 *     wait blocks.  Saves an explicit __interrupt_unmask kcall on the
 *     hot path: the IST can loop "for(;;) interrupt_wait(UNMASK,...)"
 *     and skip the separate unmask.  When set, ksys_interrupt_wait
 *     also completes any PLIC claim that plic_record_claim deferred
 *     (so the gateway transitions InService→Idle, not Pending —
 *     otherwise one spurious IRQ per real one).
 *
 *   RTMUX_INTR_WAIT_FLAGS_FAST — skip timeout setup; the kcall body
 *     does not arm a timeout timer and ignores kap->timeout.  Faster
 *     hot-path for ISTs that don't need timed waits.
 */
#define RTMUX_INTR_WAIT_FLAGS_UNMASK 0x01
#define RTMUX_INTR_WAIT_FLAGS_FAST 0x02

/* System independent interrupt classes for interrupt_attach[Event] */
#define RTMUX_INTR_CLASS_EXTERNAL (0x0000UL << 16)
#define RTMUX_INTR_CLASS_SYNTHETIC (0x7fffUL << 16)

#define RTMUX_INTR_SPARE (RTMUX_INTR_CLASS_SYNTHETIC | 0xffff)

#define RTMUX_HOOK_TRACE (RTMUX_INTR_CLASS_SYNTHETIC | 0)
#define RTMUX_HOOK_LAST (RTMUX_INTR_CLASS_SYNTHETIC | 0)

/* Define flags for msg_readiov */
#define RTMUX_READIOV_SEND 0
#define RTMUX_READIOV_REPLY 1

struct _clockperiod {
    unsigned long nsec;
    long fract;
};

/* Limits */
#define LIMITS_PROCESS 0
#define LIMITS_THREAD 1
#define LIMITS_TIMER 2
#define LIMITS_NOTIFICATION 3
#define LIMITS_SYNC 4
#define LIMITS_CONNECT 5
#define LIMITS_CHANNEL 6
#define LIMITS_INTERRUPT 7
#define LIMITS_SYNCEVENT 8
#define LIMITS_NUM 9

/* Operation to tell msg_key_data to verify instead of calculate */
#define RTMUX_KEYDATA_VERIFY 0
#define RTMUX_KEYDATA_CALCULATE 1
/* Wrong VERIFY keys a minted msg_key_data secret survives before it is retired;
 * shared by the kernel (ipc/message.c) and the capgate retire test.
 */
#define KEYDATA_MAX_FAILS 8

/* Sync Control commands */
#define RTMUX_SCTL_SETPRIOCEILING 1 /* mutex    const int *prioceiling */
#define RTMUX_SCTL_GETPRIOCEILING 2 /* mutex    int *prioceiling */
#define RTMUX_SCTL_SETEVENT 3       /* mutex    struct sigevent *event */

struct iovec;

#define ROOTTASK_PID 1
#define ROOTTASK_CHID 1
/* Roottask connection is always the first side channel. */
#define ROOTTASK_COID RTMUX_SIDE_CHANNEL
#define ROOTTASK_HANDLE 0

#define RTMUX_HWRES_MEMORY 0
#define RTMUX_HWRES_PCI_MEMORY 4

extern int rtmux_detach(int __status);
extern int rtmux_phys_contain(void);
extern int rtmux_hwres_claim_range(uint64_t __base,
                                   size_t __size,
                                   unsigned __type);
extern int rtmux_hwres_claim_range_for_pid(uint64_t __base,
                                           size_t __size,
                                           unsigned __type,
                                           pid_t __pid);
extern void rtmux_hwres_release_range(uint64_t __base,
                                      size_t __size,
                                      unsigned __type);
extern void rtmux_hwres_release_range_for_pid(uint64_t __base,
                                             size_t __size,
                                             unsigned __type,
                                             pid_t __pid);
struct rtmux_resource_handle;
/* Pass as rtmux_delegate_device's expected_base to accept any granted base (a
 * real MMIO base can be 0, so this cannot be a value the server may grant: the
 * server rejects any base above INT64_MAX).
 */
#define RTMUX_DELEGATE_ANY_BASE ((uint64_t) -1)
extern int rtmux_delegate_device(uint32_t __device_id,
                                 size_t __min_size,
                                 uint64_t __expected_base,
                                 struct rtmux_resource_handle *__out);
extern int tcsetsid(int __filedes, pid_t __pid);
extern int memobj_handle_from_fd_r(int __fd,
                                   unsigned __rights,
                                   uint64_t *__handle);
extern uint64_t memobj_handle_from_fd(int __fd, unsigned __rights);
extern int memobj_handle_close(uint64_t __handle);
extern void *mmap_handle(void *__addr,
                         size_t __len,
                         int __prot,
                         int __flags,
                         uint64_t __handle,
                         off64_t __off);
extern int mem_offset(__const void *__addr,
                      int __fd,
                      size_t __len,
                      off_t *__off,
                      size_t *__contig_len);

__END_DECLS

/* Function prototypes for all kernel calls. Suppressed by
 * _RTMUX_NO_SYSCALL_PROTOS (used by calls.c which provides generic void*
 * implementations).
 */
#ifndef _RTMUX_NO_SYSCALL_PROTOS
/* Debug print via kernel console (syscall 3, __KSYS_DBGPRINT) */
extern void dbgprint(const char *__str, size_t __len);

/* Formatted debug print (vsnprintf + dbgprint) */
extern void dbgprintf(const char *__fmt, ...)
    __attribute__((__format__(__printf__, 1, 2)));

extern int channel_create(unsigned __flags);
extern int channel_create_r(unsigned __flags);
/* Create a capability-gated global channel (RTMUX_FLG_CHF_CAP_GATED implied).
 * On success the minted gating capability id is written to *__cap_out; the
 * server delegates it to authorized clients via the IPC cap transfer.
 */
extern int channel_create_cap(unsigned __flags, uint64_t *__cap_out);
extern int channel_destroy(int __chid);
extern int channel_destroy_r(int __chid);
extern int connect_attach(uint32_t __nd,
                          pid_t __pid,
                          int __chid,
                          unsigned __index,
                          int __flags);
extern int connect_attach_r(uint32_t __nd,
                            pid_t __pid,
                            int __chid,
                            unsigned __index,
                            int __flags);
extern uint64_t connect_attach_cap(uint32_t __nd,
                                   pid_t __pid,
                                   int __chid,
                                   unsigned __index,
                                   int __flags);
extern int connect_attach_cap_r(uint32_t __nd,
                                pid_t __pid,
                                int __chid,
                                unsigned __index,
                                int __flags,
                                uint64_t *__cap);
extern int __connect_detach(uint64_t __handle);
extern int connect_detach_r(uint64_t __handle);
extern int connect_server_info(pid_t __pid,
                               uint64_t __handle,
                               struct _server_info *__info);
extern int connect_server_info_r(pid_t __pid,
                                 uint64_t __handle,
                                 struct _server_info *__info);
extern int connect_client_info(int __scoid,
                               struct _client_info *__info,
                               int __ngroups);
extern int connect_client_info_r(int __scoid,
                                 struct _client_info *__info,
                                 int __ngroups);
extern int connect_flags(pid_t __pid,
                         uint64_t __handle,
                         unsigned __mask,
                         unsigned __bits);
extern int connect_flags_r(pid_t __pid,
                           uint64_t __handle,
                           unsigned __mask,
                           unsigned __bits);

extern int msg_send(int __coid,
                    const void *__smsg,
                    size_t __sbytes,
                    void *__rmsg,
                    size_t __rbytes);
extern int msg_send_r(int __coid,
                      const void *__smsg,
                      size_t __sbytes,
                      void *__rmsg,
                      size_t __rbytes);
extern int msg_send_cap(uint64_t __cap,
                        const void *__smsg,
                        size_t __sbytes,
                        void *__rmsg,
                        size_t __rbytes);
extern int msg_send_cap_r(uint64_t __cap,
                          const void *__smsg,
                          size_t __sbytes,
                          void *__rmsg,
                          size_t __rbytes);
extern int msg_sendnc(int __coid,
                      const void *__smsg,
                      size_t __sbytes,
                      void *__rmsg,
                      size_t __rbytes);
extern int msg_sendnc_r(int __coid,
                        const void *__smsg,
                        size_t __sbytes,
                        void *__rmsg,
                        size_t __rbytes);
extern int msg_sendv(int __coid,
                     const struct iovec *__siov,
                     int __sparts,
                     const struct iovec *__riov,
                     int __rparts);
extern int msg_sendv_r(int __coid,
                       const struct iovec *__siov,
                       int __sparts,
                       const struct iovec *__riov,
                       int __rparts);
extern int msg_sendv_cap(uint64_t __cap,
                         const struct iovec *__siov,
                         int __sparts,
                         const struct iovec *__riov,
                         int __rparts);
extern int msg_sendv_cap_r(uint64_t __cap,
                           const struct iovec *__siov,
                           int __sparts,
                           const struct iovec *__riov,
                           int __rparts);
extern int msg_sendvnc(int __coid,
                       const struct iovec *__siov,
                       int __sparts,
                       const struct iovec *__riov,
                       int __rparts);
extern int msg_sendvnc_r(int __coid,
                         const struct iovec *__siov,
                         int __sparts,
                         const struct iovec *__riov,
                         int __rparts);
extern int msg_receive(int __chid,
                       void *__msg,
                       size_t __bytes,
                       struct _msg_info *__info);
extern int msg_receive_r(int __chid,
                         void *__msg,
                         size_t __bytes,
                         struct _msg_info *__info);
extern int msg_receivev(int __chid,
                        const struct iovec *__iov,
                        int __parts,
                        struct _msg_info *__info);
extern int msg_receivev_r(int __chid,
                          const struct iovec *__iov,
                          int __parts,
                          struct _msg_info *__info);
extern int msg_receive_notification(int __chid,
                                    void *__notification,
                                    size_t __bytes,
                                    struct _msg_info *__info);
extern int msg_receive_notification_r(int __chid,
                                      void *__notification,
                                      size_t __bytes,
                                      struct _msg_info *__info);
extern int msg_receive_notificationv(int __chid,
                                     const struct iovec *__iov,
                                     int __parts,
                                     struct _msg_info *__info);
extern int msg_receive_notificationv_r(int __chid,
                                       const struct iovec *__iov,
                                       int __parts,
                                       struct _msg_info *__info);
extern int msg_reply(int __rcvid, int __status, const void *__msg, int __bytes);
extern int msg_reply_r(int __rcvid,
                       int __status,
                       const void *__msg,
                       int __bytes);
extern int msg_replyv(int __rcvid,
                      int __status,
                      const struct iovec *__iov,
                      int __parts);
extern int msg_replyv_r(int __rcvid,
                        int __status,
                        const struct iovec *__iov,
                        int __parts);
extern int msg_readiov(int __rcvid,
                       const struct iovec *__iov,
                       int __parts,
                       int __offset,
                       int __flags);
extern int msg_readiov_r(int __rcvid,
                         const struct iovec *__iov,
                         int __parts,
                         int __offset,
                         int __flags);
extern int msg_read(int __rcvid, void *__msg, int __bytes, int __offset);
extern int msg_read_r(int __rcvid, void *__msg, int __bytes, int __offset);
extern int msg_readv(int __rcvid,
                     const struct iovec *__iov,
                     int __parts,
                     int __offset);
extern int msg_readv_r(int __rcvid,
                       const struct iovec *__iov,
                       int __parts,
                       int __offset);
extern int msg_write(int __rcvid, const void *__msg, int __bytes, int __offset);
extern int msg_write_r(int __rcvid,
                       const void *__msg,
                       int __bytes,
                       int __offset);
extern int msg_writev(int __rcvid,
                      const struct iovec *__iov,
                      int __parts,
                      int __offset);
extern int msg_writev_r(int __rcvid,
                        const struct iovec *__iov,
                        int __parts,
                        int __offset);
extern int msg_send_notification(int __coid,
                                 int __priority,
                                 int __code,
                                 int __value);
extern int msg_send_notification_r(int __coid,
                                   int __priority,
                                   int __code,
                                   int __value);
extern int msg_send_notification_cap(uint64_t __cap,
                                     int __priority,
                                     int __code,
                                     int __value);
extern int msg_send_notification_cap_r(uint64_t __cap,
                                       int __priority,
                                       int __code,
                                       int __value);
extern int msg_deliver_event(int __rcvid, const struct sigevent *__event);
extern int msg_deliver_event_r(int __rcvid, const struct sigevent *__event);
extern int msg_verify_event(int __rcvid, const struct sigevent *__event);
extern int msg_verify_event_r(int __rcvid, const struct sigevent *__event);
extern int msg_info(int __rcvid, struct _msg_info *__info);
extern int msg_info_r(int __rcvid, struct _msg_info *__info);
extern int msg_key_data(int __rcvid,
                        int __oper,
                        uint32_t __key,
                        uint32_t *__newkey,
                        const struct iovec *__iov,
                        int __parts);
extern int msg_key_data_r(int __rcvid,
                          int __oper,
                          uint32_t __key,
                          uint32_t *__newkey,
                          const struct iovec *__iov,
                          int __parts);
extern int msg_error(int __rcvid, int __err);
extern int msg_error_r(int __rcvid, int __err);
extern int msg_current(int __rcvid);
extern int msg_current_r(int __rcvid);
/* Signal* kernel calls removed — signal subsystem eliminated. Stubs returning
 * -ENOSYS exist in calls.c for declared-but-unimplemented entry points.
 */

extern int __thread_create(pid_t __pid,
                           void *(*__func)(void *__arg),
                           void *__arg,
                           const struct _thread_attr *__attr);
extern int thread_create_r(pid_t __pid,
                           void *(*__func)(void *__arg),
                           void *__arg,
                           const struct _thread_attr *__attr);
extern int __thread_destroy(int __tid, int __priority, void *__status);
extern int thread_destroy_r(int __tid, int __priority, void *__status);
extern int thread_detach(int __tid);
extern int thread_detach_r(int __tid);
extern int thread_join(int __tid, void **__status);
extern int thread_join_r(int __tid, void **__status);
extern int __thread_cancel(int __tid, void (*__canstub)(void));
extern int thread_cancel_r(int __tid, void (*__canstub)(void));
extern int thread_ctl(int __cmd, void *__data);
extern int thread_ctl_r(int __cmd, void *__data);

extern int interrupt_attach_thread(int __intr, unsigned __flags);
extern int interrupt_attach_thread_r(int __intr, unsigned __flags);
extern int __interrupt_attach_event(int __intr,
                                    const struct sigevent *__event,
                                    unsigned __flags);
extern int interrupt_attach_event_r(int __intr,
                                    const struct sigevent *__event,
                                    unsigned __flags);
extern int interrupt_detach(int __id);
extern int interrupt_detach_r(int __id);
extern int interrupt_wait(int __flags, const uint64_t *__timeout);
extern int interrupt_wait_r(int __flags, const uint64_t *__timeout);
extern int __interrupt_mask(int __intr, int __id);
extern int interrupt_mask_r(int __intr, int __id);
extern int __interrupt_unmask(int __intr, int __id);
extern int interrupt_unmask_r(int __intr, int __id);

extern int __sched_get(pid_t __pid, int __tid, struct sched_param *__param);
extern int __sched_get_r(pid_t __pid, int __tid, struct sched_param *__param);
extern int __sched_set(pid_t __pid,
                       int __tid,
                       int __algorithm,
                       const struct sched_param *__param);
extern int __sched_info(pid_t __pid,
                        int __algorithm,
                        struct _sched_info *__info);
extern int __sched_yield(void);

extern int __timer_create(clockid_t __id, const struct sigevent *__notify);
extern int __timer_destroy(timer_t __id);
extern int __timer_settime(timer_t __id,
                           int __flags,
                           const struct _itimer *__itime,
                           struct _itimer *__oitime);
extern int __timer_info(pid_t __pid,
                        timer_t __id,
                        int __flags,
                        struct _timer_info *__info);
extern int __timer_timeout(clockid_t __id,
                           int __flags,
                           const struct sigevent *__notify,
                           const uint64_t *__ntime,
                           uint64_t *__otime);
extern int __timer_timeout_r(clockid_t __id,
                             int __flags,
                             const struct sigevent *__notify,
                             const uint64_t *__ntime,
                             uint64_t *__otime);

extern int sync_type_create(unsigned __type,
                            sync_t *__sync,
                            const struct _sync_attr *__attr);
extern int sync_type_create_r(unsigned __type,
                              sync_t *__sync,
                              const struct _sync_attr *__attr);
extern int __sync_destroy(sync_t *__sync);
extern int sync_destroy_r(sync_t *__sync);
extern int sync_ctl(int __cmd, sync_t *__sync, void *__data);
extern int sync_ctl_r(int __cmd, sync_t *__sync, void *__data);
extern int __sync_mutex_lock(sync_t *__sync);
extern int sync_mutex_lock_r(sync_t *__sync);
extern int __sync_mutex_unlock(sync_t *__sync);
extern int sync_mutex_unlock_r(sync_t *__sync);
extern int __sync_mutex_revive(sync_t *__sync);
extern int sync_mutex_revive_r(sync_t *__sync);
extern int sync_condvar_wait(sync_t *__sync, sync_t *__mutex);
extern int sync_condvar_wait_r(sync_t *__sync, sync_t *__mutex);
extern int sync_condvar_signal(sync_t *__sync, int __all);
extern int sync_condvar_signal_r(sync_t *__sync, int __all);
extern int sync_sem_post(sync_t *__sync);
extern int sync_sem_post_r(sync_t *__sync);
extern int sync_sem_wait(sync_t *__sync, int __tryto);
extern int sync_sem_wait_r(sync_t *__sync, int __tryto);

/* roottask-privileged kcall (slot __KSYS_PRIV). Reachable only with the
 * matching per-operation priv_caps bit; declaration is here for the libc weak
 * stub at lib/libc/riscv/syscalls.c. Sub-IDs in <private/priv-calls.h> (kernel
 * + roottask builds only).
 */
extern long syscall_priv(int __id, void *__arg);

extern int __clock_time(clockid_t __id, const uint64_t *_new, uint64_t *__old);
extern int __clock_time_r(clockid_t __id,
                          const uint64_t *_new,
                          uint64_t *__old);
extern int __clock_period(clockid_t __id,
                          const struct _clockperiod *_new,
                          struct _clockperiod *__old,
                          int __reserved);
extern int __clock_id(pid_t __pid, int __tid);

#endif /* _RTMUX_NO_SYSCALL_PROTOS */

#endif
