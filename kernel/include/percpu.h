/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef _KERNEL_PERCPU_H
#define _KERNEL_PERCPU_H

/* Per-CPU array abstraction.
 *
 * RTMux stores per-CPU state in plain C arrays sized PROCESSORS_MAX, indexed by
 * RISC-V hart ID (the value returned by get_cpunum()). Hart IDs are not
 * necessarily 0..N-1 — a 4-CPU SoC may report harts {0,1,2,3} or {1,2,3,4},
 * depending on what U-Boot/SBI assigned.
 *
 * The macros below are the only place that hart-ID semantics live. Call sites
 * should use this_cpu_*() / per_cpu*() rather than indexing arrays directly, so
 * a future migration to sequential CPU indexing only has to change one file.
 *
 * Storage layout is unchanged: DEFINE_PER_CPU(int, foo) emits exactly "int
 * foo[PROCESSORS_MAX]", so existing assembly references keep working. The
 * macros are usable for any new per-CPU variable.
 *
 * Caller's translation unit must already have:
 *     PROCESSORS_MAX     — from <config.h>  (kernel-side:
 * CONFIG_PROCESSORS_MAX) get_cpunum() — from <arch/runtime.h> on RISC-V
 *
 * Both are pulled in via "core/globals.h" before this header in normal kernel
 * builds, so most call sites need only include "percpu.h" alongside the headers
 * they already use.
 */

#ifndef __ASSEMBLY__

/* PROCESSORS_MAX is an alias for CONFIG_PROCESSORS_MAX (see
 * kernel/include/smpswitch.h:23). Use the Kconfig-derived macro directly here
 * so this header is self-contained — roottask, libc, and any future out-of-tree
 * consumer that already pulls <generated/autoconf.h> can include lock.h (and
 * therefore objects.h) without first including smpswitch.h. Without this
 * fallback, adding lock.h to objects.h breaks roottask.
 */
#ifndef PROCESSORS_MAX
#define PROCESSORS_MAX CONFIG_PROCESSORS_MAX
#endif

#define DECLARE_PER_CPU(type, name) extern type name[PROCESSORS_MAX]
#define DEFINE_PER_CPU(type, name) type name[PROCESSORS_MAX]

#define this_cpu_ptr(name) (&(name)[get_cpunum()])
#define this_cpu_read(name) ((name)[get_cpunum()])
#define this_cpu_write(name, val)     \
    do {                              \
        (name)[get_cpunum()] = (val); \
    } while (0)
#define this_cpu_inc(name)      \
    do {                        \
        ++(name)[get_cpunum()]; \
    } while (0)
#define this_cpu_dec(name)      \
    do {                        \
        --(name)[get_cpunum()]; \
    } while (0)

#define per_cpu(name, cpu) ((name)[cpu])
#define per_cpu_ptr(name, cpu) (&(name)[cpu])

#endif /* __ASSEMBLY__ */

#endif /* _KERNEL_PERCPU_H */
