/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef _KERNEL_OBJECTS_H
#define _KERNEL_OBJECTS_H

#include <sys/resource.h>
#include <sys/types.h>
#include <sys/fault.h>
#include <kdebug.h>
#include <sys/debug.h>
#include <stdatomic.h>
#include <stdbool.h>
#include "memclass.h"
#include "macros.h"
#ifdef KERNEL_INTERNAL

/* lock.h is kernel-only: kspinlock_t uses S-mode infrastructure (get_cpunum and
 * per-CPU state) that U-mode roottask cannot reach. lock.h is only needed for
 * the struct bodies below (which are themselves KERNEL_INTERNAL-gated), so the
 * include sits under the same gate.
 */
#include "lock.h" /* kspinlock_t for per-object lock fields */
#include <cap-table.h>
#endif
#include <posix-types.h>


/** Forward declarations of structure types. */
typedef struct channel_entry channel_t;
typedef struct mm_as address_t;
typedef struct debug_entry debug_t;
typedef struct process_entry process_t;
typedef struct limits_entry limits_t;
typedef struct timer_entry ktimer_t;
typedef struct session_entry session_t;
typedef struct dispatch_entry kdispatch_t;
typedef struct thread_entry thread_t;
typedef struct thread_entry vthread_t; /* Same as thread_t, but smaller */
typedef struct notification_entry notification_t;
typedef struct syncevent_entry sync_event_t;
typedef struct sync_entry ksync_t;
typedef struct turnstile_entry turnstile_t;

/* Opaque identity token for a sync object's wait queue. Today it is the user
 * sync_t's virtual address; process-shared syncs will key on a physical address
 * instead (bit 0 reserved as the shared discriminator). It is only ever hashed
 * and compared, never dereferenced. See docs/pshared-sync-design.md.
 */
typedef uintptr_t sync_key_t;
typedef struct interrupt_entry interrupt_t;
typedef struct interrupt_level intr_level_t;
typedef struct breakpt_entry breakpoint_t;

/* Path namespace object types */
typedef struct node_entry node_t;
typedef union path_object path_object_t;

/* Kernel-call argument union */
typedef union kcall_args kcall_args_t;

/* This will define the size of a short message. It should be at least 16 bytes
 * as this is aways available in the thread args union.
 */
#ifndef _SHORT_MSG_LEN
#define _SHORT_MSG_LEN 256
#endif

/* This defines the number of incoming send and reply iov_t cached by the
 * message pass code. This should be the same size as the SHORT_MSG_LEN buffer.
 */

#ifndef _NUM_CACHED_SEND_IOV
#define _NUM_CACHED_SEND_IOV (_SHORT_MSG_LEN / (sizeof(iov_t)))
#endif

#ifndef _NUM_CACHED_REPLY_IOV
#define _NUM_CACHED_REPLY_IOV (_SHORT_MSG_LEN / (sizeof(iov_t)))
#endif

#define RTMUX_PROCESS_ENDPOINT_CAP_SLOTS 128

#define THREAD_DESTROY_CLAIM_ACTIVE 1U
#define THREAD_DESTROY_CLAIM_RESERVED_BASE 2U

/* Types (used by notification_t, thread_t, vthread_t, connect_t, channel_t) */
enum {
    TYPE_THREAD,
    TYPE_NOTIFICATION,
    TYPE_VTHREAD = 3,
    TYPE_CONNECTION,
    TYPE_CHANNEL,
    TYPE_VNOTIFICATION,
    TYPE_SYNCEVENT,
    TYPE_TYPE_MASK = 0x0f,
    TYPE_FLAG_FIRST = 0x10
};

#define TYPE_MASK(t) ((t) & TYPE_TYPE_MASK)

typedef struct vector_entry {
    uint16_t nentries;
    uint16_t nfree;
    void *free;
    void **vector;
} vector_t;

/* A list node and list header for use with the LINK3_* macros. */
typedef struct link3_node {
    struct link3_node *next, **prev;
} LINK3_NODE;

typedef struct link3_hdr {
    LINK3_NODE *head, *tail;
} LINK3_HDR;

/* a list node and list header for use with the LINK4_* macros. */
typedef LINK3_NODE LINK4_NODE;

typedef struct link4_hdr {
    LINK4_NODE *head, *tail;
    int count;
} LINK4_HDR;


/* Macro to define the fields needed for the pril routines - done this way so
 * that all the structures that use them always have the same layout at the
 * begining.
 */
#define PRIL_ENTRY_FIELDS              \
    union {                            \
        struct pril_entry *pril;       \
        thread_t *thread;              \
        notification_t *notification;  \
        sync_event_t *sev;             \
    } next;                            \
    union {                            \
        struct pril_entry *prio_tail;  \
        struct pril_entry **pril;      \
        thread_t **thread;             \
        notification_t **notification; \
        sync_event_t **sev;            \
    } prev;                            \
    uint8_t type;                      \
    uint8_t priority;

typedef struct pril_entry {
    PRIL_ENTRY_FIELDS
} pril_t;

struct pril_update {
    struct pril_update *next;
    struct pril_update *prev;
    pril_t *pril;
};

typedef struct {
    pril_t *data;
} pril_head_t;



/* KERNEL-PRIVATE STRUCT BODIES
 *
 * The bodies below are gated behind KERNEL_INTERNAL. Kernel sources
 * (kernel/Makefile defines -DKERNEL_INTERNAL) get the full struct definitions
 * and can dereference fields freely.
 *
 * Roottask (and any other user-mode consumer) does NOT define KERNEL_INTERNAL —
 * it sees only the typedef pointer-to-incomplete (the "typedef struct foo
 * *Foo;" declarations near the top of this file). Any attempt by roottask code
 * to dereference a "process_t *" / "thread_t *" / "channel_t *" / etc. is a
 * compile error, by design: those pointers come from the kernel via
 * query_object and similar kernel extensions and refer to kernel-heap memory
 * that U-mode roottask cannot read (heap is PTE_U=0).
 *
 * Any field roottask genuinely needs to read or write must move to a public,
 * roottask-owned home (typically root_process_t in
 * servers/roottask/include/root-process.h, allocated in roottask's user VA).
 */
#ifdef KERNEL_INTERNAL

typedef struct credential_entry {
    struct credential_entry *next;

    /* Shared refcount. Owner lock: cred_slock; mutate via cred_set. (The one
     * unlocked bump at roottask install is single-threaded early boot.)
     */
    uint32_t links;
    struct _cred_info info;
} credential_t;

typedef struct process_entry {
    process_t *next;
    pid_t pid;
    uint32_t flags;
    uint64_t priv_caps;
    rtmux_cap_table_t endpoint_caps;
    rtmux_cap_slot_t endpoint_cap_slots[RTMUX_PROCESS_ENDPOINT_CAP_SLOTS];
    address_t *memory;
    uintptr_t boundary_addr;
    vector_t chancons; /* Channels and non-fd connect vector. */
    vector_t fdcons;   /* Fd connection vector. */
    debug_t *debugger;
    struct _process_local_storage *pls;
    volatile uint64_t running_time;
    volatile uint64_t system_time;

    /* The roottask-side process mux-lock pointer used to live here as "struct
     * proc_mux_lock *lock". It is purely roottask bookkeeping (the kernel never
     * observed it beyond a NULL-on-destroy assertion) and now lives in
     * root_process_t.lock — see servers/roottask/include/root-process.h.
     */
    struct process_entry *parent;
    vector_t threads;
    vector_t timers;

    /* "pgrp", "session", "siginfo" were POSIX policy fields that the kernel had
     * no business knowing about. They have moved to "root_process_t"
     * (roottask-side) — see servers/roottask/include/root-process.h. The kernel
     * does NOT touch sessions or process groups any more; "_client_info.sid" /
     * "BKGND_PGRP" were dropped from the ABI for the same reason
     * (connect.c::ksys_connect_client_info).
     */
    credential_t *cred;
    limits_t *limits;
    ktimer_t *alarm;
    void (*canstub)(void);
    process_t *child;
    process_t *sibling;
    thread_t *join_queue;
    kdispatch_t *default_dpp;

    /* "uint32_t umask" was here — moved to root_process_t.umask. Kernel never
     * read it beyond inheritance (now done roottask-side in process_create
     * wrapper) and the nano_debug snapshot.
     */
    uint32_t num_active_threads;
    uint32_t num_processes; /* # of processes created */
    channel_t *death_chp;
    channel_t *sys_chp; /* system channel for notification delivery */
    /* The fields root, cwd, events, wap, and debug_waiting used to live here.
     * They were never read or written by the kernel — pure roottask- side
     * per-process state — and have been moved into roottask's own
     * root_process_t (see servers/roottask/include/root-process.h). Removing
     * them from process_t is part of the heap-PTE_U=0 redesign: with the kernel
     * heap unreadable from U-mode, every roottask field deref off a kernel-
     * allocated process_t would fault. Keep this comment around as a landmark
     * for the remaining process_t split.
     *
     * "debug_name" (executable path) also lived here. Moved to
     * root_process_t.debug_name — /proc readers consume it via /proc/<pid>/name
     * and DEVCTL_PROC_MAPDEBUG_BASE. Kernel diagnostic readers (shutdown.c /
     * event.c / stall-detector.c) gracefully degrade to "?" without it.
     */
    uintptr_t initial_sp;
    uintptr_t initial_ip; /* set by loader before SPAWN_DONE */
    uintptr_t base_addr;
    struct loader_context *lcp;
    struct procfs_ocb *ocb_list;
    struct kdebug_entry kdebug;
    thread_t *valid_thp;

    /* conf_table and rsrc_list previously lived here but were never read by the
     * kernel — roottask-side bookkeeping (sysconf and hwres respectively).
     * Moved into root_process_t; see servers/roottask/include/root-process.h.
     * pgrp/session/siginfo went the same way for the same reason — POSIX policy
     * isn't the kernel's job.
     */
    uint64_t start_time;
    uint64_t kids_running_time;
    uint64_t kids_system_time;
    uint64_t max_cpu_time; /* in nano-seconds */
    uint32_t seq;

    /* Lifetime pins for process_t pointers promoted from process_rcu lookups.
     * The destroy path removes the process from process_vector, drains
     * process_rcu, then waits for this counter before freeing process-owned
     * vectors/credentials/object pools.
     */
    volatile unsigned pin_count;
    ulong_t rlimit_vals_soft[RLIM_NLIMITS];
    ulong_t rlimit_vals_hard[RLIM_NLIMITS];

    /* Open-connection count. Owner lock: this process's vec_slock (16-bit, so
     * no sub-word AMO; locked rather than atomic). Bumped at connect_attach,
     * dropped via process_nfds_dec.
     */
    uint16_t nfds;
    uint8_t terming_priority;
    uint8_t process_priority;
    siginfo_t fault_siginfo;
    bool querying;
    unsigned pending_interrupts;

    /* Per-process vector spinlock (LOCK_CLASS_PROCVEC). Initialized by
     * kext_process_create. Serializes mutating operations on the per-process
     * vector_t structures ("fdcons", "chancons") against concurrent
     * searches/lookups — closing the TOCTOU between a ksys_connect_attach
     * reuse-search and a ksys_connect_detach vector_rem. Used in serial fashion
     * (acquire, vec op, release) — never nested under cop->lock or chp->lock.
     */
    kspinlock_t vec_slock;
} process_t;

#endif /* KERNEL_INTERNAL — close credential_t / process_t gate */

/* Channel descriptor */
#ifdef KERNEL_INTERNAL
typedef struct channel_entry {
    channel_t *next;
    int32_t chid;
    uint8_t type;
    uint8_t zero;
    uint16_t flags; /* RTMUX_CHF_* */
    process_t *process;
    thread_t *destroyer; /* thread that began ChannelDestroy; only it resumes
                          * a flagged channel on a KCALL_RESTART'd call
                          */
    pril_head_t send_queue;
    thread_t *receive_queue;
    thread_t *reply_queue;
    mode_t mode; /* permissions */
    bool destroying;

    /* Per-channel spinlock (LOCK_CLASS_CHANNEL). Initialized at channel-create
     * time (channel.c). Protects the per-channel scalar state
     * (flags/process/chid/destroying) AND the send/receive/reply queues — see
     * doc/tex/KerFuncRef/ch03-channels-connections.tex.
     *
     * Lifetime: a chp->lock acquirer must keep chp alive — the message paths
     * hold a connobj_rcu read section (lookup_rcvid / sendv / sendnotification
     * / receivev open it), and _channel_destroy does rcu_sched_synchronize()
     * before freeing chp. chp->lock < cop->lock by class rank; always drop
     * chp->lock before force_ready / ready / block (SCHED ranks below CHANNEL).
     */
    kspinlock_t lock;
} channel_t;

struct channel_async_entry {
    struct channel_entry ch; /* basic channel_entry */
    struct sigevent event;   /* the event to be sent for notification */
    process_t *ev_prp;       /* the process who registers the event */
};

struct global_msg_entry {
    struct global_msg_entry *next;
    unsigned priority;
    size_t size;
};

typedef struct connect_entry connect_t;
typedef struct channel_gbl_entry {
    channel_t ch;            /* basic channel_entry */
    struct sigevent event;   /* the event to be sent for notification */
    process_t *ev_prp;       /* the process who registers the event */
    int ev_coid;             /* handle to identify the owner of the event */
    size_t buffer_size;      /* size of kernel buffer */
    unsigned max_num_buffer; /* maximum number of buffer allowed */
    unsigned num_curr_msg;   /* how many msgs in the channel send queue */
    struct _cred_info cred;  /* credential */
    void *free;              /* free buffer list */
    struct global_msg_entry *tail; /* points to the tail of the queue */
    connect_t *cons; /* connection list for cleanup when channel is destroyed */
    /* Capability gate: 0 for a public channel (connect by chid alone allowed),
     * otherwise the gating cap id minted over this channel in the creator's
     * endpoint_caps at RTMUX_FLG_CHF_CAP_GATED create time. A connect is
     * admitted only if the caller holds a live cap naming this channel object.
     */
    rtmux_cap_t required_cap;
} global_channel_t;

typedef struct connect_entry {
    connect_t *next;
    uint16_t scoid;
    int16_t infoscoid;
    uint8_t type;
    uint8_t zero;
    uint16_t flags; /* COF_* */
    /* Non-zero while this connection holds one notification_souls reservation
     * for its disconnect notice (object_reserve at attach on a channel with
     * RTMUX_FLG_CHF_DISCONNECT; released by connect_detach once the notice is
     * delivered, or by connect_soul_free). Placed after "flags" so it uses the
     * padding before "process".
     */
    uint8_t nresv;
    process_t *process;
    channel_t *channel;

    /* Atomic refcount shared across the send/reply/channel/detach paths (no
     * single natural lock). Mutate via atomic_add_value or connect_links_drop
     * (globals.h), NOT under cop->lock; on a drop old==0 is a fatal underflow.
     */
    uint32_t links;
    uint32_t rights;
    rtmux_cap_t cap;
    union {
        struct {
            uint32_t nd;
            pid_t pid;
            uint32_t chid;
            uint32_t coid;
            uint32_t seq;
            connect_t *cop;
            connect_t *next;
        } lcl;
        struct {
            credential_t *cred;
            uint32_t nd;
            pid_t pid;
            pid_t sid;
            uint32_t flags; /* RTMUX_CI_* */
            uint32_t coid;
        } remote;
    } un;

    /* Per-connection spinlock (LOCK_CLASS_CONNECT). Initialized at
     * ksys_connect_attach time. Covers "flags" (CLOEXEC/NONBLOCK mutations from
     * ksys_connect_flags) and the "next" chain when adding/removing VCONNECT
     * siblings. It does NOT cover the "links" refcount, which is atomic (see
     * the field comment above); do not reintroduce a cop->lock around links.
     * The kernel-pointer fields process/channel are read under the lock when
     * they could race with destroy.
     *
     * Lock order: chp->lock (LOCK_CLASS_CHANNEL) before cop->lock
     * (LOCK_CLASS_CONNECT).
     */
    kspinlock_t lock;
} connect_t;

#endif /* KERNEL_INTERNAL — close channel_t / connect_t / global_channel_t \
        * gate                                                               \
        */

#ifdef KERNEL_INTERNAL
typedef struct remote_ipc_state {
    process_t *prp;
    channel_t *chp;
} remote_ipc_state_t;


typedef struct notification_entry {
    PRIL_ENTRY_FIELDS
    uint8_t spare;
    int8_t code;
    uintptr_t value;
    intptr_t id; /* scoid if TYPE_NOTIFICATION */

    unsigned count;
} notification_t;

/* Entries for the sporadic scheduling policy */

/* Single replenishment object per schedinfo, used in a list */
typedef struct _ss_replenish {
    struct _ss_replenish *next; /* Next item to replenish */
    thread_t *thp;              /* Thread to replenish */
    uint64_t amount;            /* Amount to replenish by (nsec) */
    uint64_t repl_time;         /* When to replenish this item (nsec) */
} sched_replenish_t;

/* Incorporated into the schedinfo below //This information is shared between
 * threads with the same policy struct _ss_attr { struct _ss_attr *next; uint8_t
 * low_priority; //Low priority to drop to when empty uint8_t refcount; //Number
 * of people sharing this attribute uint16_t max_repl; //Maximum numer of
 * replenishments uint64_t repl_period; //Period of the replenisment (nsec)
 * uint64_t init_budget; //Initial budget for the thread (nsec) };
 */

/* This information must be maintained per thread */
typedef struct _ss_schedinfo {
    uint8_t low_priority;
    uint8_t org_priority; /* 0 if not dropped, pre-drop priority otherwise */
    uint8_t sched_locked; /* set while the caller holds the dispatch lock */
    uint8_t reserved;
    uint16_t max_repl;        /* Maximum number of replenishments */
    uint16_t repl_count;      /* Number of replenishments performed */
    uint64_t repl_period;     /* Period of the replenishments (nsec) */
    uint64_t init_budget;     /* Initial budget for the thread (nsec) */
    uint64_t curr_budget;     /* Current budget for the thread (nsec) */
    uint64_t activation_time; /* When this thread was "activated" */
    uint64_t consumed;        /* Amount consumed on this run */
    struct _ss_replenish replenishment;
} sched_info_t;

#endif /* KERNEL_INTERNAL — close remote_ipc_state_t / notification_t / \
          sched_info_t gate */

/* End entries for the sporadic scheduling algorithm */

typedef uint32_t int_fl_t;

#ifdef KERNEL_INTERNAL
typedef struct thread_entry { /* Also used for vthread_entry */
    PRIL_ENTRY_FIELDS uint8_t real_priority;
    uint8_t policy;
    uint8_t state;
    uint32_t reserved_word;
    uint32_t runcpu;
    int_fl_t internal_flags;
    uint64_t timestamp_last_block; /* set to copy of snap_time() anytime this
                                    * thread blocks. not valid if state is
                                    * STATE_READY or STATE_RUNNING
                                    */
    process_t *as_prp;
    atomic_uint async_flags;
    kdispatch_t *dpp;
    uint32_t timeout_flags;
    uint32_t flags;
    uint32_t sched_flags; /* for aps scheduling */
    int32_t syscall;
    process_t *process;
    uint32_t tid;
    kdispatch_t *orig_dpp; /* after being a guest of another aps partition, it's
                            returned here
                            */
    union {
        struct _ss_schedinfo *ss_info;
        uint32_t rr_ticks;
    } schedinfo;
    union {
        struct { /* type = thread_t */
            struct _thread_local_storage *tls;
            void *stackaddr;
            size_t stacksize;
        } lcl;
        struct {            /* type = vthread_t */
            uint32_t srcnd; /* remote view of this node (Overlays
                               thread_local_storage pointer)
                               */
            void *reserved;
            uint32_t vtid; /* virtual thread id (index in thread_vector) */
        } remote;
    } un;
    thread_t *client; /* the client that the server thread works on behalf */
    void *blocked_on; /* blocked wait object */
    turnstile_t *blocked_on_turnstile; /* PI/PC wait-chain object, not
                                        * blocked_on channel state
                                        */
    turnstile_t
        *owned_turnstiles; /* turnstiles currently boosting this owner */
    thread_t *restart;

    /* The following is used to hold arguments when a thread blocks to make them
     * easy to access in another threads context. Only a very small number of
     * arguments need to be saved.
     */
    union {
        struct {             /* Used by send/read/write for general messages */
            iov_t *rmsg;     /* Must be 1st arg */
            int32_t rparts;  /* Must be 2nd arg */
            uint32_t coid;   /* Must be 3rd arg */
            uint32_t msglen; /* Must be 4th arg */
            uint32_t dstmsglen; /* Must be 5th arg */
            thread_t *server; /* the thread in server to serve this msg. Must be
                                6th arg
                                */
            uint32_t srcmsglen;
            iov_t *smsg;
            int32_t sparts;
            thread_t *dthp; /* Dest thread for message restarts */
            iov_t siov[_NUM_CACHED_SEND_IOV];
            iov_t riov[_NUM_CACHED_REPLY_IOV];
        } ms;

        struct {                /* Used by send for short messages */
            iov_t *rmsg;        /* Must be 1st arg */
            int32_t rparts;     /* Must be 2nd arg */
            uint32_t coid;      /* Must be 3rd arg */
            uint32_t msglen;    /* Must be 4th arg */
            uint32_t dstmsglen; /* Must be 5th arg */
            thread_t *server; /* the thread in server to serve this msg. Must be
                                6th arg
                                */
            void *fill[4];
            uint8_t buff[_SHORT_MSG_LEN];
            iov_t riov[_NUM_CACHED_REPLY_IOV];
        } msbuff;

        struct { /* Used by nano_specret for get rcvinfo && short messages */
            iov_t *rmsg;    /* Must be 1st arg */
            int32_t rparts; /* Must be 2nd arg */
            struct _msg_info *info;
            thread_t *thp;
            connect_t *cop;
            uintptr_t value;
            intptr_t id;
            int8_t code;
        } ri;

#define RTMUX_NOIOV \
    (1UL << (8 * sizeof(uint32_t) - 1)) /* Must agree with sparts/rparts */
        struct { /* Used by ksys_timer_timeout (nanosleep only) */
            struct _itimer left;
            uint64_t *timeptr;
            uint32_t spare;
        } to;
        struct { /* Used by ksys_thread_join */
            void **statusptr;
            void *status;
        } jo;
        struct { /* Used while acquiring a mutex or waiting on a condvar */
            sync_t *mutex;
            sync_key_t
                wait_key; /* opaque wait-queue key (VA today); not a ptr */
            uint32_t saved_timeout_flags;
            unsigned long owner; /* owner of *mutex */
            ksync_t *next;       /* next mutex in the thread mutex hold list */
            ksync_t **prev; /* owner, next and prev only meaningful when the
                             thread is the first one in a mutex waiting list
                             */
            long incr;      /* saved sync_t.count (same type) */
        } mu;
        struct { /* Used by async_flags processing in an SMP machine */
            uint32_t save_ip;
            uint32_t save_type;
            uint32_t code;
            uint32_t fault_type;
            uint32_t fault_addr;
        } async;
        struct { /* Used to hold stuff during thread creation */
            void *arg;
            const struct _thread_attr *attr;
            void (*exitfunc)(void *status);
            const struct _thread_attr *real_attr;
            int not_sigev_thread;
            /* Caller's attr, copied at create. Like the rest of wa it lives
             * in the args union and is valid only until thread_specret
             * finishes; the new thread does not run other kcalls before that.
             */
            struct _thread_attr kattr;
        } wa;

    } args;
    uint32_t runmask;
    uint32_t default_runmask;
    char *name;
    uint64_t key; /* msg_key_data secret; 0 = unset, never verifies */
    uint32_t key_fail; /* failed VERIFYs against key since it was minted */
    int32_t last_chid;
    void *status;
#define SIZEOF_VTHREAD offsetof(struct thread_entry, timeout)
    /* Items below are not needed for type=vthread_t */
    ktimer_t *timeout;
    thread_t *join;
    ksync_t *mutex_holdlist; /* list of holding mutexes whose waiting queue is
                              not empty
                              */
    uint64_t start_time;
    FPU_REGISTERS *fpudata;
    volatile unsigned ticker_using;
    volatile uint64_t running_time;

    /* Dispatch ready-queue linkage. Distinct from PRIL_ENTRY_FIELDS
     * (next.thread/prev.thread at the head of this struct) so the
     * sync_slock-protected PRIL wait lists and the sched_slock-protected
     * dispatch ready queues never write to overlapping bytes — closing a
     * cross-protocol corruption race. FreeBSD's struct thread separates td_runq
     * from td_slpq for the same reason. Manipulated only via LINK3_BEG /
     * LINK3_END / LINK3_REM under sched_slock; back-converted to thread_t* via
     * link3_to_thread().
     */
    LINK3_NODE link3_link;
    struct cpu_thread_entry cpu; /* Must be just before 'reg' field. */
    CPU_REGISTERS reg;

    /* Trap auxiliary fields — populated by kernel_exit before sret to U-mode.
     * Trap entry reads these at fixed offsets from sscratch (which holds
     * &thp->reg) to bootstrap the kernel stack and tp. Must be immediately
     * after reg (accessed as reg + FRAME_SIZE + N).
     */
    struct {
        uint64_t kernel_sp; /* per-CPU kernel stack top */
        uint64_t kernel_tp; /* per-CPU cpupage pointer (tp in S-mode) */
    } trap_aux;

    /* Destroy-claim — single-writer barrier for thread_destroy().
     *
     * Set 0 at thread_create (post-object_alloc reset). thread_destroy()
     * atomically CASes 0→THREAD_DESTROY_CLAIM_ACTIVE; dead join/detach may
     * reserve 0→THREAD_DESTROY_CLAIM_RESERVED_BASE + cpu while holding
     * vec_slock, then consume that reservation in thread_destroy() on the same
     * CPU. A caller that fails the CAS bails (another path owns destruction).
     * Deferred-return paths (running on another CPU, force_ready failed, FPU on
     * another CPU, alloc'd stack proc-time teardown, zombie return) release the
     * claim with an atomic store-0 before returning, so the next caller can
     * pick up. The committed path past the zombie check holds the claim through
     * object_free(); the soul's next allocator resets the field via
     * thread_create.
     *
     * Closes the lifecycle.c:701 double-remove crash by ensuring at most one
     * caller reaches vector_rem(&prp->threads, tid).
     */
    volatile unsigned destroy_claim;

    /* Lifetime pins for pointers promoted from tthread_rcu read sections.
     *
     * A caller may pin only while the thread is still protected by RCU or by
     * another subsystem lock that proves the object cannot be recycled. The
     * destroy path removes the thread from prp->threads, drains tthread_rcu,
     * then waits for this counter to reach zero before object_free().
     */
    volatile unsigned pin_count;

    /* Saved syscall argument registers (a0..a5), captured at every kcall entry.
     * KCALL_RESTART restores these into the saved register frame before the
     * user PC is rewound to the ecall, so a re-executed syscall sees its
     * ORIGINAL arguments.
     *
     * Without this, a blocking syscall whose a0 was overwritten with a
     * return/status value (SETKSTATUS == write reg.gpr[A0]) and then
     * KCALL_RESTART'd would re-issue the ecall with a0 = status, not the
     * original arg — e.g. a replied msg_send(coid=3) restarted as
     * msg_send(coid=0) and went to the wrong server. RTMux restores the saved
     * argument registers on restart so a re-executed syscall always sees its
     * original arguments.
     */
    uint64_t kcall_c_args[6];

    /* PRIO_PROTECT (priority ceiling) bookkeeping. held_ceilings[] is the
     * unordered multiset of ceilings for the PRIO_PROTECT mutexes this thread
     * currently holds; held_ceiling_floor caches their maximum (the effective
     * ceiling boost). Releasing a mutex removes its ceiling and recomputes the
     * max, so an out-of-order release drops the boost to the next-highest held
     * ceiling instead of staying pinned at the historical max.
     *
     * The store is bounded: past HELD_CEILING_MAX simultaneously-held ceilings
     * the exact set no longer fits, so held_ceiling_saturated latches and the
     * floor is kept conservatively high (never lowered until the thread fully
     * unwinds to zero holds). That path only over-elevates priority briefly and
     * never under-protects (no inversion); realistic nesting stays well under
     * the cap and is tracked exactly.
     *
     * Placed at the tail of thread_t on purpose: earlier fields (the args
     * union, kcall_c_args, the SIZEOF_VTHREAD boundary) sit at offsets other
     * code depends on, so this bookkeeping is appended rather than inserted.
     */
    uint8_t held_ceilings[HELD_CEILING_MAX];
    unsigned held_ceiling_n; /* wide enough that no realizable nesting wraps */
    uint8_t held_ceiling_floor;
    bool held_ceiling_saturated;
    /* timestamp_last_block value the stall detector has already reported for
     * this thread (stall_scan_roottask_waiters, CONFIG_STALL_DETECTOR). Written
     * only by that scan, which runs on the boot hart alone; 0 = never reported.
     * Appended at the tail like the fields above.
     */
    uint64_t stall_reported_ts;
} thread_t;

/* Map a dispatch-list LINK3_NODE pointer back to its enclosing thread_t. The
 * dispatch ready queue (kdispatch_t.ready[pri].head/tail) stores LINK3_NODE*
 * values that point at &thp->link3_link; this helper does the container_of()
 * back-conversion.
 *
 * Returns NULL for a NULL node.
 */
static inline thread_t *link3_to_thread(LINK3_NODE *node)
{
    return node ? (thread_t *) ((char *) node -
                                __builtin_offsetof(thread_t, link3_link))
                : (thread_t *) 0;
}

#endif /* KERNEL_INTERNAL — close thread_t gate */

enum {
    SYNCEVENT_SUBTYPE_EVENT,
    SYNCEVENT_SUBTYPE_PRIORITYCEILING,
    SYNCEVENT_SUBTYPE_CLOCKID
};

#ifdef KERNEL_INTERNAL
typedef struct syncevent_entry {
    PRIL_ENTRY_FIELDS uint8_t subtype;
    uint8_t reserved;
    union {
        int clockid;
        struct {
            process_t *process;
            uint32_t tid;
            struct sigevent event;
        } ev;
    } un;
} sync_event_t;

typedef struct sync_entry {
    ksync_t *next;
    path_object_t *obj;
    paddr_t addr;
    pril_head_t waiting;
} ksync_t;


typedef struct timer_entry {
    struct timer_link {
        ktimer_t *next;
        ktimer_t *prev;
    } link;
    ktimer_t *pending;
    struct timer_queue *queue;
    thread_t *thread;
    uint32_t overruns;
    int8_t unused;

    /* Non-zero when this timer holds one notification_souls reservation for its
     * SIGEV_NOTIFICATION delivery (object_reserve at create, object_unreserve
     * at destroy). Drives allocation-free timer-expiry delivery.
     */
    uint8_t nresv;
    uint8_t clockid;
    uint8_t flags;
    struct sigevent event;
    struct _itimer itime;

    /* Per-object spinlock (LOCK_CLASS_TIMER). Initialized by timer_alloc() in
     * time/timer-core.c; covers the per-tip state fields
     * (flags/itime/event/thread/overruns/clockid). Acquired before
     * timer_wheel_slock when both are needed (see
     * doc/tex/KerFuncRef/ch06-timers-clocks.tex).
     */
    kspinlock_t lock;
} ktimer_t;

/* WARNING: it is very important that this structure be kept a power of 2
 *          for performance reasons. You have to change the definition
 *          of LOG2_SIZEOF_INTRLEVEL if this structure changes.
 */
struct interrupt_level {
    struct interrupt_entry *queue;
    struct intrinfo_entry *info;
    signed short cascade_level;
    unsigned short config;
    unsigned short level_base;
    unsigned short mask_count;
    uint64_t reserved;
#define LOG2_SIZEOF_INTRLEVEL 5
};

/* interrupt_t.kind discriminator for the direct IST wake model. Records which
 * of the three attach paths produced this entry, so the trap handler can
 * dispatch delivery correctly:
 *
 *   INTR_KIND_THREAD  — interrupt_attach_thread().  Trap delivers a
 *                       synthetic SIGEV_INTR to "thread" inline:
 *                       ready() if STATE_INTR, atomic_or otherwise.
 *
 *   INTR_KIND_EVENT   — __interrupt_attach_event().  Trap calls
 *                       sigevent_deliver_inline(&event, NULL) which
 *                       dispatches on event.sigev_notify
 *                       (SIGEV_NOTIFICATION/SIGEV_INTR/SIGEV_UNBLOCK/...).
 *
 *   INTR_KIND_HANDLER — internal-only attach with a function-
 *                       pointer callback that runs in trap context.
 *                       Used by the timer-side STIMER path
 *                       (clock_handler) and the trace subsystem
 *                       (core.c em_event / em_st).  The public
 *                       interrupt_attach() form is removed; only
 *                       kernel-internal callers reach this case.
 */
enum interrupt_kind {
    INTR_KIND_THREAD = 0,
    INTR_KIND_EVENT = 1,
    INTR_KIND_HANDLER = 2,
};

struct interrupt_entry {
    interrupt_t *next;
    thread_t *thread;
    const struct sigevent *(*handler)(void *area, int id);
    void *area;
    unsigned flags;
    unsigned level;
    unsigned mask_count;
    int id;
    uint64_t fire_count; /* total fires since interrupt_attach */
    struct cpu_intrsave cpu;

    /* The registered sigevent for INTR_KIND_EVENT, and the kind discriminator
     * above. For INTR_KIND_THREAD / INTR_KIND_HANDLER, "event" is unused
     * (zeroed at attach time).
     */
    struct sigevent event;
    uint8_t kind;

    /* Non-zero when this INTR_KIND_EVENT source holds one notification_souls
     * reservation for allocation-free SIGEV_NOTIFICATION delivery from the trap
     * handler (object_reserve at attach, object_unreserve at detach).
     */
    uint8_t nresv;

    /* Hart this IRQ source is pinned to. 0xFF = no owner (broadcast). Set by
     * ksys_interrupt_attach_thread / ksys_interrupt_attach_event right after
     * interrupt_attach{,_event} returns, BEFORE the explicit unmask the
     * kcall_exits perform. The PLIC unmask callback consults plic_owner_hart[]
     * (which mirrors this for fast lookup without walking interrupt_vector).
     */
    uint8_t owner_hart;
};

#endif /* KERNEL_INTERNAL — close sync_event_t / ksync_t / ktimer_t / \
          interrupt_entry gate */

/* struct interrupt_query_entry — copy-out target for query_object
 * (_QUERY_INTERRUPT). Public so roottask /proc readers can inspect fields. The
 * embedded "struct interrupt_entry info" carries the kernel-private body;
 * roottask accesses its scalar fields freely (handler is a function pointer
 * that's only useful on the kernel side, but roottask shovels it through to
 * userspace /proc readers).
 */
struct interrupt_entry_pub {
    void *next;   /* interrupt_t * — opaque */
    void *thread; /* thread_t *    — opaque */
    const struct sigevent *(*handler)(void *area, int id);
    void *area;
    unsigned flags;
    unsigned level;
    unsigned mask_count;
    int id;
    uint64_t fire_count;
    struct cpu_intrsave cpu;
};

struct interrupt_query_entry {
#ifdef KERNEL_INTERNAL
    struct interrupt_entry info;
#else
    struct interrupt_entry_pub info;
#endif
    pid_t pid;
    int tid;
    unsigned vector;
    struct sigevent event;
};

#ifdef KERNEL_INTERNAL
typedef struct dispatch_entry {
    kdispatch_t *next; /* per-CPU runqueue state */
    kdispatch_t **prev;
    int id;          /* Dispatch grou id */
    unsigned sindex; /* sort index */
    unsigned __pad;  /* Align ready array on 8 bytes */
    void *bumper;    /* Must be zero to stop smp_resched() */
    /* Per-CPU sched lock. Each pcpu_array[hart].pc_dispatch carries its own
     * kspinlock_t — same-CPU dispatch mutations (ready, unready, mark_running,
     * select) hold only the local instance's lock. Cross-CPU operations
     * (ready_default routing thp to another hart, force_ready on a remote thp,
     * select_thread's steal scan) take the target's lock, acquiring in
     * ascending hart-id order if more than one is needed.
     *
     * Rank class is LOCK_CLASS_SCHED. lock_debug treats multiple
     * LOCK_CLASS_SCHED locks as an "instance-aware" rank, ordered by lock
     * address — equivalent to ordering by hart_id since
     * pcpu_array[hart].pc_dispatch addresses are stable.
     */
    kspinlock_t lock;
    LINK3_HDR ready[NUM_PRI]; /* priority runqueue arrays */
#if NUM_PRI <= 8
    uint8_t lo;
#elif NUM_PRI <= 8 * 8
    uint8_t hi;
    uint8_t lo[(NUM_PRI + 7) / 8];
#elif NUM_PRI <= 8 * 8 * 8
    uint8_t hi;
    uint8_t mid[(NUM_PRI + (8 * 8 - 1)) / (8 * 8)];
    uint8_t lo[(NUM_PRI + (8 * 8 - 1)) / (8 * 8)][8];
#else
#error NUM_PRI too high
#endif
} kdispatch_t; /* per-CPU runqueue */
#endif         /* KERNEL_INTERNAL — kdispatch_t body */


typedef struct session_entry {
    uint32_t links;
    pid_t leader;
    pid_t pgrp;
    int fd;
    unsigned reserved[10];
} session_t;


typedef struct soul_entry {
    void *next;
    int32_t type;    /* Type of soul */
    uint32_t total;  /* Total allocated (free or in use). */
    uint32_t used;   /* Number in use. */
    uint32_t size;   /* Size of object in the list. */
    uint32_t newmin; /* Current min needed. */
    uint32_t min;    /* Lowest min allowed. */
    uint32_t highwater;

    /* Free souls held back for allocation-free realtime delivery. Non-realtime
     * object_alloc keeps (total - used) > reserved, growing rather than dipping
     * below the floor; object_alloc_reserved bypasses the floor. Grown by
     * object_reserve at attach/create of a SIGEV_NOTIFICATION IRQ/timer source,
     * released by object_unreserve at detach/destroy. Mutated under soul_slock.
     */
    uint32_t reserved;
    uint16_t counter;
    uint8_t align; /* Alignement requirement on soul */
    uint8_t flags; /* Soul flags */
} soul_t;

/* There is no tIntrEvent type and no intrevent_pending queue. All sigevent
 * producers deliver inline via sigevent_deliver_inline() (kernel/ipc/event.c).
 */


typedef struct hash_entry {
    uint32_t mask;
    void **table;
} hash_t;

#ifdef KERNEL_INTERNAL
typedef struct limits_entry {
    struct limits_entry *next;

    /* Shared per-uid refcount. Owner lock: limits_slock, because the last drop
     * must unlink from limits_list under that same lock (so it is locked, not
     * atomic, even though it would fit an AMO).
     */
    uint32_t links;
    uint32_t uid;
    uint32_t max[LIMITS_NUM];
    uint32_t cur[LIMITS_NUM];
} limits_t;

struct fault_handlers {
    void (*fault)(thread_t *thp, CPU_REGISTERS *regs, unsigned flags);
    void (*restart)(thread_t *thp, CPU_REGISTERS *regs);
};

typedef struct breakpt_entry {
    breakpoint_t *next;
    debug_break_t brk;
    unsigned planted;
#ifdef CPU_DEBUG_BRKPT
    struct cpu_debug_brkpt {
        CPU_DEBUG_BRKPT
    } cpu;
#endif
} breakpoint_t;

typedef struct debug_entry {
    process_t *process;
    unsigned flags;
    int what;
    int why;
    fltset_t faults;
    int tid; /* current tid */
    breakpoint_t *brk;
    breakpoint_t *skip_brk; /* Skip this breakpoint if not null */
#ifdef CPU_DEBUG
    struct cpu_debug {
        CPU_DEBUG
    } cpu;
#endif
} debug_t;

struct fault_info {
    process_t *prp;
    uintptr_t vaddr;
    unsigned sigcode;
    struct cpu_fault_info cpu;
};
#endif /* KERNEL_INTERNAL — close limits_t / breakpoint_t / debug_t gate */

/* struct fault_info — passed by pointer to vm_ops fault callbacks (kernel
 * mm_fault as well as roottask as_fault). The body is not gated because
 * roottask's as_fault touches info->vaddr / info->sigcode; info->prp is
 * declared as "void *" here (it's a kernel-allocated process_t that roottask
 * cannot deref anyway). The kernel side has its own typedef-via-process_t
 * earlier — this is the shared-layout mirror. See the gated declaration higher
 * up.
 */
#ifndef KERNEL_INTERNAL
struct fault_info {
    void *prp;
    uintptr_t vaddr;
    unsigned sigcode;
    struct cpu_fault_info cpu;
};
#endif

#endif /* _KERNEL_OBJECTS_H */
