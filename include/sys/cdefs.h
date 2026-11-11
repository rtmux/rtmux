/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * GCC/Clang-family compilers, C99+ with compatibility shims for selected C11
 * spellings, ELF.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef _SYS_CDEFS_H_
#define _SYS_CDEFS_H_

#include <sys/features.h>
#include <stddef.h>

/* Testing against Clang-specific extensions. */
#ifndef __has_attribute
#define __has_attribute(x) 0
#endif
#ifndef __has_extension
#define __has_extension __has_feature
#endif
#ifndef __has_feature
#define __has_feature(x) 0
#endif
#ifndef __has_include
#define __has_include(x) 0
#endif
#ifndef __has_builtin
#define __has_builtin(x) 0
#endif

/* C++ name mangling guards. */
#if defined(__cplusplus)
#define __BEGIN_DECLS extern "C" {
#define __END_DECLS }
#else
#define __BEGIN_DECLS
#define __END_DECLS
#endif

/* Compiler memory barrier. */
#define __compiler_membar() __asm __volatile(" " : : : "memory")

/* Token pasting and stringification. */
#define __P(protos) protos
#define __CONCAT1(x, y) x##y
#define __CONCAT(x, y) __CONCAT1(x, y)
#define __STRING(x) #x
#define __XSTRING(x) __STRING(x)

#define __const const
#define __signed signed
#define __volatile volatile
#if !defined(__cplusplus)
#define __inline inline
#endif

/* Function and variable attributes. */
#define __dead2 __attribute__((__noreturn__))
#define __unused __attribute__((__unused__))
#define __aligned(x) __attribute__((__aligned__(x)))
#define __malloc_like __attribute__((__malloc__))
#define __pure __attribute__((__pure__))
#define __noinline __attribute__((__noinline__))
#define __result_use_check __attribute__((__warn_unused_result__))
#define __exported __attribute__((__visibility__("default")))
#define __hidden __attribute__((__visibility__("hidden")))

/* Branch prediction hints. */
#define __predict_true(exp) __builtin_expect((exp), 1)
#define __predict_false(exp) __builtin_expect((exp), 0)

/* C99 restrict qualifier. */
#if !defined(__STDC_VERSION__) || __STDC_VERSION__ < 199901
#define __restrict
#else
#define __restrict restrict
#endif

/* C99 static array indices in function parameter declarations. */
#if !defined(__cplusplus) && (defined(__clang__) || __GNUC_PREREQ(4, 6)) && \
    (!defined(__STDC_VERSION__) || (__STDC_VERSION__ >= 199901))
#define __min_size(x) static(x)
#else
#define __min_size(x) (x)
#endif

/* C11 keyword support. GCC and Clang-family compilers support these natively
 * when compiling as C11 or later, so these are mostly no-ops.
 */
#if !defined(__STDC_VERSION__) || __STDC_VERSION__ < 201112L
#define _Static_assert(e, msg) \
    typedef char __CONCAT(__static_assert_, __LINE__)[(e) ? 1 : -1]
#define _Alignas(x) __aligned(x)
#define _Alignof(x) __alignof(x)
#define _Noreturn __dead2
#define _Thread_local __thread
#if !defined(__cplusplus)
#define _Atomic(T)        \
    struct {              \
        T volatile __val; \
    }
#endif
#endif

/* _Generic() support. */
#if (defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L) || \
    __has_extension(c_generic_selections)
#define __generic(expr, t, yes, no) _Generic(expr, t: yes, default: no)
#elif __GNUC_PREREQ(3, 1) && !defined(__cplusplus)
#define __generic(expr, t, yes, no)                                        \
    __builtin_choose_expr(__builtin_types_compatible_p(__typeof(expr), t), \
                          yes, no)
#endif

/* printf/scanf format checking. */
#define __printflike(fmtarg, firstvararg) \
    __attribute__((__format__(__printf__, fmtarg, firstvararg)))

/* ELF symbol references. */
#define __weak_reference(sym, alias) \
    __asm__(".weak " #alias);        \
    __asm__(".equ " #alias ", " #sym)

/* offsetof and containerof. */
#define __offsetof(type, field) offsetof(type, field)

#define __containerof(x, s, m)                                            \
    ({                                                                    \
        const volatile __typeof(((s *) 0)->m) *__x = (x);                 \
        __DEQUALIFY(s *, (const volatile char *) __x - __offsetof(s, m)); \
    })

/* Qualifier stripping casts. */
#define __DECONST(type, var) ((type) (__UINTPTR_TYPE__) (const void *) (var))
#define __DEQUALIFY(type, var) \
    ((type) (__UINTPTR_TYPE__) (const volatile void *) (var))

/* Clang nullability qualifiers (no-ops on GCC). */
#if !(defined(__clang__) && __has_feature(nullability))
#define _Nonnull
#define _Nullable
#define _Null_unspecified
#define __NULLABILITY_PRAGMA_PUSH
#define __NULLABILITY_PRAGMA_POP
#else
#define __NULLABILITY_PRAGMA_PUSH    \
    _Pragma("clang diagnostic push") \
        _Pragma("clang diagnostic ignored \"-Wnullability-completeness\"")
#define __NULLABILITY_PRAGMA_POP _Pragma("clang diagnostic pop")
#endif

/* Clang thread safety annotations (no-ops on GCC). */
#if __has_extension(c_thread_safety_attributes)
#define __lock_annotate(x) __attribute__((x))
#else
#define __lock_annotate(x)
#endif

#define __lockable __lock_annotate(lockable)
#define __locks_exclusive(...) \
    __lock_annotate(exclusive_lock_function(__VA_ARGS__))
#define __locks_shared(...) __lock_annotate(shared_lock_function(__VA_ARGS__))
#define __trylocks_exclusive(...) \
    __lock_annotate(exclusive_trylock_function(__VA_ARGS__))
#define __trylocks_shared(...) \
    __lock_annotate(shared_trylock_function(__VA_ARGS__))
#define __unlocks(...) __lock_annotate(unlock_function(__VA_ARGS__))
#define __asserts_exclusive(...) \
    __lock_annotate(assert_exclusive_lock(__VA_ARGS__))
#define __asserts_shared(...) __lock_annotate(assert_shared_lock(__VA_ARGS__))
#define __requires_exclusive(...) \
    __lock_annotate(exclusive_locks_required(__VA_ARGS__))
#define __requires_shared(...) \
    __lock_annotate(shared_locks_required(__VA_ARGS__))
#define __requires_unlocked(...) __lock_annotate(locks_excluded(__VA_ARGS__))
#define __no_lock_analysis __lock_annotate(no_thread_safety_analysis)
#define __guarded_by(x) __lock_annotate(guarded_by(x))
#define __pt_guarded_by(x) __lock_annotate(pt_guarded_by(x))

#endif /* _SYS_CDEFS_H_ */
