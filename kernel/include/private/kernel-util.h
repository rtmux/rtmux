/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * kernel-util.h — public declaration subset of the kernel utility library (the
 * slim link surface that roottask legitimately shares with the kernel: souls,
 * vectors, the small atomic helpers).
 *
 * The full set of kernel-internal declarations lives in kernel/include/decls.h,
 * gated behind KERNEL_INTERNAL. Roottask MUST NOT include
 * kernel/include/decls.h — that header takes pointers to types (process_t,
 * thread_t, connect_t, ...) that are deliberately incomplete from U-mode
 * roottask's point of view (heap is PTE_U=0 — see feedback_heap_pte_u_zero.md).
 *
 * If roottask code thinks it needs another kernel utility, the decision is
 * structural: either (a) the function is a pure helper with no kernel-pointer
 * arguments and it gets a declaration here, or (b) it dereferences kernel-only
 * state and roottask has to reach it via a kernel extension instead. Don't fold
 * opaque kernel pointers into this header.
 */

#ifndef _SYS_RTMUX_KERLIB_H
#define _SYS_RTMUX_KERLIB_H

#include <stddef.h>
#include <stdint.h>

/* Forward declarations. The struct bodies live in kernel/include/objects.h
 * (only visible to kernel sources). process_t is intentionally NOT declared —
 * see the file-level comment. Any "object_alloc(prp, soul)"-style API that
 * requires a process_t argument is kernel-private.
 */
struct soul_entry;
struct vector_entry;

typedef struct soul_entry soul_t;
typedef struct vector_entry vector_t;

/* Souls (object pool allocator) — roottask owns only roottask-private souls. */
extern void *object_alloc(soul_t *soulp);
extern void object_free(soul_t *soulp, void *ptr);

/* Vectors — roottask uses these for its own per-process / per-server tables.
 * The vector itself is plain memory; the kernel just provides the alloc/lookup
 * helpers.
 */
extern void *vector_lookup(vector_t *vec, unsigned id);
extern int vector_add(vector_t *vec, void *object, unsigned index);
extern void *vector_rem(vector_t *vec, unsigned index);
extern void vector_free(vector_t *vec);

/* walk_asinfo — iterate the kernel's syspage asinfo entries. Used by roottask
 * bootstrap (cpiofs ramdisk lookup, etc.).
 */
struct asinfo_entry;
extern int walk_asinfo(const char *name,
                       int (*func)(struct asinfo_entry *, char *, void *),
                       void *data);

/* STRLCPY — bounded string copy macro shared with roottask. Mirrors BSD
 * strlcpy() except it returns nothing. The truncation diagnostic is gated
 * behind NDEBUG; the kprintf() path runs only on the kernel side, so this
 * header is safe to include from roottask code (which pulls kprintf() through
 * libc).
 */

#include <string.h>

/* STR_OVERFLOW — roottask's root-log.h does "#undef STR_OVERFLOW" and redefines
 * it on top of root_warn, so we keep the same macro name (no "_RTMUX_" prefix)
 * to allow that redefinition to win. The default here matches the historical
 * decls.h definition.
 */
#ifndef STR_OVERFLOW
#ifndef NDEBUG
extern int kprintf(const char *fmt, ...);
#define STR_OVERFLOW(f, line, s, d, l)                          \
    kprintf(                                                    \
        "%s:%u - strncpy overflow, src truncated to %d bytes\n" \
        "\tsrc - '%s'\n\tdst - '%s'\n",                         \
        (f), (line), (l) - 1, (s), (d))
#else
#define STR_OVERFLOW(f, line, s, d, l)
#endif
#endif

#ifndef CRASHCHECK
#define CRASHCHECK(cond)                                                     \
    do {                                                                     \
        if (cond) {                                                          \
            extern void crash_halt(const char *fmt, ...);                    \
            crash_halt("CRASHCHECK: " __FILE__ ":%d %s\n", __LINE__, #cond); \
        }                                                                    \
    } while (0)
#endif

#ifndef STRINGIZE
#define _RTMUX_STRINGIZE_(x) #x
#define STRINGIZE(x) _RTMUX_STRINGIZE_(x)
#endif

#define STRLCPY(d, s, l)                                                     \
    ({                                                                       \
        CRASHCHECK((size_t) (l) > 4096);                                     \
        if (!memccpy((d), (s), '\0', (l))) {                                 \
            (d)[((l) > 0) ? (l) - 1 : (l)] = '\0';                           \
            STR_OVERFLOW(STRINGIZE(__BASE_FILE__), __LINE__, (s), (d), (l)); \
        }                                                                    \
    })

#endif /* _SYS_RTMUX_KERLIB_H */
