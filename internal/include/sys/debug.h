/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* sys/debug.h - private procfs/debug ABI.
 *
 * This header describes kernel/roottask debug records used by private procfs
 * plumbing. It is not part of the POSIX public header surface.
 */
#ifndef __DEBUG_H_INCLUDED
#define __DEBUG_H_INCLUDED

#ifndef __PLATFORM_H_INCLUDED
#include <sys/platform.h>
#endif

#include <arch/context.h>

#ifndef __FAULT_H_INCLUDED
#include <sys/fault.h>
#endif

#ifndef _SIGNAL_H_INCLUDED
#include <signal.h>
#endif

__BEGIN_DECLS

#define _DEBUG_FLAG_STOPPED 0x00000001    /* Thread is not running */
#define _DEBUG_FLAG_ISTOP 0x00000002      /* Stopped at point of interest */
#define _DEBUG_FLAG_IPINVAL 0x00000010    /* IP is not valid */
#define _DEBUG_FLAG_ISSYS 0x00000020      /* System process */
#define _DEBUG_FLAG_SSTEP 0x00000040      /* Stopped because of single step */
#define _DEBUG_FLAG_CURTID 0x00000080     /* Thread is current thread */
#define _DEBUG_FLAG_TRACE_EXEC 0x00000100 /* Stopped because of breakpoint */
#define _DEBUG_FLAG_TRACE_RD 0x00000200   /* Stopped because of read access */
#define _DEBUG_FLAG_TRACE_WR 0x00000400   /* Stopped because of write access */
#define _DEBUG_FLAG_TRACE_MODIFY \
    0x00000800                     /* Stopped because of modified memory */
#define _DEBUG_FLAG_RLC 0x00010000 /* Run-on-Last-Close flag is set */
#define _DEBUG_FLAG_KLC 0x00020000 /* Kill-on-Last-Close flag is set */
#define _DEBUG_FLAG_MASK (_DEBUG_FLAG_RLC | _DEBUG_FLAG_KLC)

enum {
    _DEBUG_WHY_REQUESTED = 0,
    _DEBUG_WHY_SIGNALLED = 1,
    _DEBUG_WHY_FAULTED = 2,
    _DEBUG_WHY_TERMINATED = 4
};

#define _DEBUG_RUN_CLRSIG 0x00000001 /* Clear pending signal */
#define _DEBUG_RUN_CLRFLT 0x00000002 /* Clear pending fault */
#define _DEBUG_RUN_FAULT 0x00000010  /* Fault mask flags interesting faults */
#define _DEBUG_RUN_VADDR 0x00000020  /* Change ip before running */
#define _DEBUG_RUN_STEP 0x00000040   /* Single step only one thread */
#define _DEBUG_RUN_STEP_ALL \
    0x00000080 /* Single step one thread, other threads run */
#define _DEBUG_RUN_CURTID                                                  \
    0x00000100                    /* Change current thread (target thread) \
                                   */
#define _DEBUG_RUN_ARM 0x00000200 /* Deliver event at point of interest */

typedef struct _debug_process_info {
    pid_t pid;
    pid_t parent;
    uint32_t flags;
    uint32_t umask;
    pid_t child;
    pid_t sibling;
    pid_t pgrp;
    pid_t sid;
    uint64_t base_address;
    uint64_t initial_stack;
    uid_t uid;
    gid_t gid;
    uid_t euid;
    gid_t egid;
    uid_t suid;
    gid_t sgid;
    sigset_t sig_ignore;
    sigset_t sig_queue;
    sigset_t sig_pending;
    uint32_t num_chancons;
    uint32_t num_fdcons;
    uint32_t num_threads;
    uint32_t num_timers;
    uint64_t start_time; /* Start time in nsec */
    uint64_t utime;      /* User running time in nsec */
    uint64_t stime;      /* System running time in nsec */
    uint64_t cutime;     /* terminated children user time in nsec */
    uint64_t cstime;     /* terminated children user time in nsec */
    uint8_t priority;    /* process base priority */
    uint8_t reserved2[7];
    uint8_t extsched[8];
    uint64_t pls;     /* Address of process local storage */
    uint64_t sigstub; /* Address of process signal trampoline */
    uint64_t canstub; /* Address of process thread cancellation trampoline */
    uint64_t reserved[10];
} debug_process_t;

typedef struct _debug_thread_info {
    pid_t pid;
    pthread_t tid;
    uint32_t flags;
    uint16_t why;
    uint16_t what;
    uint64_t ip;
    uint64_t sp;
    uint64_t stkbase;
    uint64_t tls;
    size_t stksize;
    uint32_t tid_flags;
    uint8_t priority;
    uint8_t real_priority;
    uint8_t policy;
    uint8_t state;
    int16_t syscall;
    uint16_t last_cpu;
    uint32_t timeout;
    int32_t last_chid;
    sigset_t sig_blocked;
    sigset_t sig_pending;
    siginfo_t info;
    union {
        struct {
            pthread_t tid;
        } join;
        struct {
            int32_t id;
            uintptr_t sync;
        } sync;
        struct {
            uint32_t nd;
            pid_t pid;
            int32_t coid;
            int32_t chid;
            int32_t scoid;
        } connect;
        struct {
            int32_t chid;
        } channel;
        struct {
            pid_t pid;
            uintptr_t vaddr;
            uint32_t flags;
        } waitpage;
        struct {
            size_t size;
        } stack;
        uint64_t filler[4];
    } blocked;
    uint64_t start_time; /* thread start time in nsec */
    uint64_t sutime;     /* thread system + user running time in nsec */
    uint8_t extsched[8];
    uint64_t
        nsec_since_block; /*how long thread has been blocked. 0 for STATE_READY
                             or STATE_RUNNING. in nsec, but ms resolution.
                             */
    uint64_t reserved2[4];
} debug_thread_t;

typedef struct _debug_run {
    uint32_t flags;
    pthread_t tid;
    fltset_t fault;
    uintptr_t ip;
} debug_run_t;

typedef struct _debug_break {
    uint16_t type;
    int16_t size; /* 1 to 8 for modify, otherwise zero, -1 to remove */
    uintptr_t addr;
} debug_break_t;

enum _debug_break_type {
    _DEBUG_BREAK_EXEC = 0x0001, /* execution breakpoint */
    _DEBUG_BREAK_MASK =
        0x000f, /* mask for matching for replacing/removing breakpoints */
    _DEBUG_BREAK_SOFT =
        0x8000 /* breakpoint inserted/removed during context switch */
};

typedef struct _debug_irq {
    pid_t pid;
    pthread_t tid;
    const struct sigevent *(*handler)(void *area, int id);
    void *area;
    unsigned flags;
    unsigned level;
    unsigned mask_count;
    int id;
    unsigned vector;
    struct sigevent event;
    uint64_t fire_count; /* total IRQ fires since attach */
} debug_irq_t;

typedef struct _debug_timer {
    timer_t id;
    unsigned spare;
    struct _timer_info info;
} debug_timer_t;

typedef union _debug_gregs {
#if defined(__aarch64__)
    AARCH64_CPU_REGISTERS aarch64;
#else
    RISCV_CPU_REGISTERS riscv;
#endif
    uint64_t padding[1024];
} debug_greg_t;

typedef union _debug_fpregs {
    uint64_t padding[1024];
    /* RISC-V: f0-f31 then fcsr, as saved by the kernel (see cpu_fpu_save). */
    struct {
        uint64_t fpr[32];
        uint64_t fcsr;
    } riscv;
} debug_fpreg_t;

typedef union _debug_altregs {
    uint64_t padding[1024];
} debug_altreg_t;

typedef union _debug_perfregs {
#if defined(__aarch64__)
    AARCH64_PERF_REGISTERS aarch64;
#else
    RISCV_PERF_REGISTERS riscv;
#endif
    uint64_t padding[1024];
} debug_perfreg_t;

__END_DECLS
#endif
