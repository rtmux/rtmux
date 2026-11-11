/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* sys/syspage.h */

#ifndef __SYSPAGE_H_INCLUDED
#define __SYSPAGE_H_INCLUDED

#include <sys/rtmux-core.h>

__BEGIN_DECLS
#if defined(__CLOCKADJUST)
struct _clockadjust __CLOCKADJUST;
#undef __CLOCKADJUST
#endif

struct _thread_local_storage;
struct _process_local_storage;
struct syspage_entry;
struct tracebuf;

extern struct syspage_entry *_syspage_ptr;

typedef struct {
    uint16_t entry_off;
    uint16_t entry_size;
} syspage_entry_info;

/* Include CPU specific definition for network queries. */
#include <arch/syspage.h>

/* The CPUPAGE_ENTRY structure and the INDEXES must be matched. */
struct cpupage_entry {
    struct _thread_local_storage *tls;  /* offset 0  */
    struct _process_local_storage *pls; /* offset 8  */
    struct syspage_entry *syspage;      /* offset 16 */
    unsigned long cpu;                  /* offset 24 */
    unsigned long state;                /* offset 32 */
    void *trapframe;                    /* offset 40: per-CPU trapframe VA */
    unsigned long spare[7];
};

enum {
    CPUPAGE_ADDR = -1, /* R */
    CPUPAGE_TLS = 0,   /* R */
    CPUPAGE_PLS,       /* RW */
    CPUPAGE_SYSPAGE,   /* R */
    CPUPAGE_CPU,       /* R */
    CPUPAGE_STATE,     /* R */
    CPUPAGE_MAX
};

/* Architecture independent system page definitions */

/* System page types */
enum { SYSPAGE_RISCV };

#define QTIME_FLAG_TIMER_ON_CPU0 1u
#define QTIME_FLAG_CHECK_STABLE 2u
#define QTIME_FLAG_TICKLESS 4u
#define QTIME_FLAG_GLOBAL_CLOCKCYCLES \
    8u /* clockcycles is synchronized between all processors */

typedef struct qtime_entry {
    /* Tick rate of the architecture's free-running TIME counter (RISC-V rdtime,
     * from the FDT cpus/timebase-frequency), NOT the CPU clock rate. The name is
     * ABI and predates the distinction. Scale a duration by this only if the
     * duration was measured with that same counter: on RISC-V that is rdtime in
     * userland and cpu_clock_ticks() in the kernel. clock_cycles() reads the
     * separate CYCLE counter, whose rate this is not, and under QEMU is
     * host-derived rather than guest-timer-derived.
     */
    uint64_t cycles_per_sec;
    uint64_t volatile nsec_tod_adjust;
    uint64_t volatile nsec;
    unsigned long nsec_inc;
    unsigned long boot_time; /* UTC seconds when machine booted */

    struct _clockadjust adjust;
    unsigned long timer_rate; /* times 10^timer_scale */
    long timer_scale;
    unsigned long timer_load;
    long intr;
    unsigned long epoch;
    unsigned long flags;
    unsigned int rr_interval_mul;
    unsigned long spare0;
    uint64_t volatile nsec_stable;
    unsigned long spare[4];
} systime_t;

struct intrspin;

typedef enum {
    DEBUG_WATCHDOG_STOP = 0,
    DEBUG_WATCHDOG_CONTINUE,
    DEBUG_WATCHDOG_FEED
} debug_watchdog_cmd;

#define AS_NULL_OFF ((uint16_t) -1)

/* Address space flags */
#define AS_ATTR_READABLE 0x0001
#define AS_ATTR_WRITABLE 0x0002
#define AS_ATTR_CACHABLE 0x0004
#define AS_ATTR_VISIBLE 0x0008
#define AS_ATTR_KIDS 0x0010
#define AS_ATTR_CONTINUED 0x0020

#define AS_PRIORITY_DEFAULT 100

struct asinfo_entry {
    uint64_t start;
    uint64_t end;
    uint16_t owner;
    uint16_t name;
    uint16_t attr;
    uint16_t priority;
    int (*alloc_checker)(struct syspage_entry *__sp,
                         uint64_t *__base,
                         uint64_t *__len,
                         size_t __size,
                         size_t __align);
    uint32_t spare;
};

struct hwinfo_entry {
    unsigned long data[1]; /* variable sized, see <hw/sysinfo.h> */
};

#define SYSTEM_PRIVATE_FLAG_ABNORMAL_REBOOT 1UL

struct system_private_entry {
    struct cpupage_entry *user_cpupageptr;
    struct syspage_entry *user_syspageptr;
    struct cpupage_entry *kern_cpupageptr;
    struct syspage_entry *kern_syspageptr;
    void *debug_info;
    void *debug_call;
    void *boot_program[4];
    unsigned long boot_idx;
    unsigned long cpupage_spacing;
    unsigned long private_flags;
    uint32_t pagesize;
    struct tracebuf *tracebuf;
    uint64_t kdump_info;
    uint32_t spare[3];
    SYSPAGE_KERNEL_ENTRY;
};

/* System independent CPU flags (system dependent grow from bottom up) */
#define CPU_FLAG_FPU (1U << 31) /* CPU has floating point support */
#define CPU_FLAG_MMU (1U << 30) /* MMU is active */

struct cpuinfo_entry {
    uint32_t cpu;
    uint32_t speed;
    uint32_t flags;
    uint32_t spare0;
    uint64_t idle_history;
    uint32_t spare1;
    uint16_t name;
    uint8_t ins_cache;
    uint8_t data_cache;
};

#define CACHE_LIST_END 0xff /* terminate a cache hierarchy list */

/* Cache flags */
#define CACHE_FLAG_INSTR 0x0001   /* cache holds instructions */
#define CACHE_FLAG_DATA 0x0002    /* cache holds data */
#define CACHE_FLAG_UNIFIED 0x0003 /* cache holds both ins & data */
#define CACHE_FLAG_SHARED \
    0x0004 /* cache is shared between multiple CPUs in an SMP system */
#define CACHE_FLAG_SNOOPED                                                     \
    0x0008                         /* cache implements a bus snooping protocol \
                                    */
#define CACHE_FLAG_VIRT_TAG 0x0010 /* cache is virtually tagged */
#define CACHE_FLAG_WRITEBACK 0x0020 /* cache does writeback, not writethru */
#define CACHE_FLAG_CTRL_PHYS 0x0040 /* control function takes 32-bit paddrs */
#define CACHE_FLAG_SUBSET 0x0080    /* this cache obeys the 'subset' property */
#define CACHE_FLAG_NONCOHERENT 0x0100 /* cache is non-coherent on SMP */
#define CACHE_FLAG_NONISA 0x0200      /* cache doesn't obey ISA cache instr */
#define CACHE_FLAG_NOBROADCAST \
    0x0400 /* cache ops aren't broadcast on bus for SMP */
#define CACHE_FLAG_VIRT_IDX 0x0800 /* cache is virtually indexed */
#define CACHE_FLAG_CTRL_PHYS64                     \
    0x1000 /* control function takes 64-bit paddrs \
            */

struct cacheattr_entry {
    uint32_t next;
    uint32_t line_size;
    uint32_t num_lines;
    uint32_t flags;
    unsigned (*control)(uint64_t,
                        unsigned,
                        int,
                        struct cacheattr_entry *,
                        volatile struct syspage_entry *);
    uint16_t ways;
    uint16_t spare0[1];
    uint32_t spare1[2];
};

struct typed_strings_entry {
    char data[4]; /* variable size */
};

struct strings_entry {
    char data[4]; /* variable size */
};

struct cmdline_entry {
    char data[4]; /* variable size — kernel boot command line */
};

struct board_entry {
    char data[4]; /* variable size — FDT /compatible string */
};

/* Kernel identity, filled by the kernel at startup.  The section is sized by
 * its entry_size, so a later field appended here is detectable by consumers.
 */
struct build_info_entry {
    char release[64]; /* RTMUX_VERSION_STRING */
    char build_id[16]; /* RTMUX_BUILD_ID: git short hash, "-d" when dirty */
};

/* System independent interrupt flags (system dependent grow from bottom up). */
#define INTR_FLAG_NMI 0x8000U
#define INTR_FLAG_CASCADE_IMPLICIT_EOI 0x4000U
#define INTR_FLAG_CPU_FAULT 0x2000U
#define INTR_FLAG_SMP_BROADCAST_MASK 0x1000U
#define INTR_FLAG_SMP_BROADCAST_UNMASK 0x0800U

/* System independent interrupt code gen flags (system dependent grow from
 * bottom up).
 */
#define INTR_GENFLAG_LOAD_SYSPAGE 0x8000U
#define INTR_GENFLAG_LOAD_INTRINFO 0x4000U
#define INTR_GENFLAG_LOAD_LEVEL 0x2000U
#define INTR_GENFLAG_LOAD_INTRMASK 0x1000U
#define INTR_GENFLAG_NOGLITCH 0x0800U
#define INTR_GENFLAG_LOAD_CPUNUM 0x0400U
#define INTR_GENFLAG_ID_LOOP 0x0200U

struct __intrgen_data {
    uint16_t genflags;
    uint16_t size;
    void (*rtn)(void);
};

#define INTR_CONFIG_FLAG_PREATTACH 0x0001
#define INTR_CONFIG_FLAG_DISALLOWED 0x0002
#define INTR_CONFIG_FLAG_IPI 0x0004

struct intrinfo_entry {
    uint32_t vector_base;
    uint32_t num_vectors;
    uint32_t cascade_vector;
    uint32_t cpu_intr_base;
    uint16_t cpu_intr_stride;
    uint16_t flags;
    struct __intrgen_data id;
    struct __intrgen_data eoi;
    int (*mask)(struct syspage_entry *, int);
    int (*unmask)(struct syspage_entry *, int);
    unsigned (*config)(struct syspage_entry *, struct intrinfo_entry *, int);
    uint32_t spare[4];
};

struct smp_entry {
    void (*send_ipi)(struct syspage_entry *, unsigned, int, unsigned *);
    void *start_addr;
    uint32_t pending;
    uint32_t cpu;
};

struct pminfo_entry {
    uint32_t wakeup_pending;
    uint32_t wakeup_condition;
    uint32_t spare[4];
    uint32_t managed_storage[1]; /* Variable sized */
};

struct syspage_entry {
    uint16_t size;       /* size of syspage_entry */
    uint16_t total_size; /* size of system page */
    uint16_t type;
    uint16_t num_cpu;
    syspage_entry_info system_private;
    syspage_entry_info asinfo;
    syspage_entry_info hwinfo;
    syspage_entry_info cpuinfo;
    syspage_entry_info cacheattr;
    syspage_entry_info qtime;
    syspage_entry_info typed_strings;
    syspage_entry_info strings;
    syspage_entry_info intrinfo;
    syspage_entry_info smp;
    syspage_entry_info pminfo;
    syspage_entry_info cmdline; /* kernel boot command line (NUL-terminated) */
    syspage_entry_info board;   /* FDT /compatible (NUL-terminated) */
    syspage_entry_info build_info; /* kernel release and build id */
    long spare[1];
    ARCH_SYSPAGE_ENTRY;
};

#define _SYSPAGE_ENTRY_SIZE(b, f) ((b)->f.entry_size)
#define SYSPAGE_ENTRY_SIZE(f) _SYSPAGE_ENTRY_SIZE(_syspage_ptr, f)

#define _SYSPAGE_ENTRY(__base, __field)                       \
    ((struct __field##_entry *) (void *) ((char *) (__base) + \
                                          (__base)->__field.entry_off))

#define _SYSPAGE_CPU_ENTRY(__base, __cpu, __field) \
    ((struct __cpu##_##__field##_entry             \
          *) (void *) ((char *) (__base) + (__base)->__cpu.__field.entry_off))

#define SYSPAGE_ENTRY(__field) _SYSPAGE_ENTRY(_syspage_ptr, __field)
#define SYSPAGE_CPU_ENTRY(__cpu, __field) \
    _SYSPAGE_CPU_ENTRY(_syspage_ptr, __cpu, __field)

/* The build_info record of SP, or NULL when the section is absent, shorter than
 * this consumer expects, or outside the syspage.  Uses no libc, so the dynamic
 * linker can call it.  The strings are NOT guaranteed to terminate: bound them.
 */
static inline const struct build_info_entry *
syspage_build_info_of(const struct syspage_entry *sp)
{
    if (!sp || sp->build_info.entry_size < sizeof(struct build_info_entry) ||
        (size_t) sp->build_info.entry_off + sizeof(struct build_info_entry) >
            sp->total_size)
        return NULL;
    return _SYSPAGE_ENTRY(sp, build_info);
}

__END_DECLS

#endif
