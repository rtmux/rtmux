/*
 * Frama-C 33 rejects C11 _Atomic. Model atomics as plain objects with
 * sequential semantics: correct for single-threaded functional proofs, blind to
 * races by construction. Any concurrency claim needs the litmus gates
 * (scripts/check-rcu-litmus.sh, check-ipc-litmus.sh), not WP.
 */
#ifndef RTMUX_FRAMAC_STDATOMIC_H
#define RTMUX_FRAMAC_STDATOMIC_H

#define _Atomic

typedef enum {
    memory_order_relaxed,
    memory_order_consume,
    memory_order_acquire,
    memory_order_release,
    memory_order_acq_rel,
    memory_order_seq_cst
} memory_order;

typedef _Bool atomic_bool;
typedef char atomic_char;
typedef signed char atomic_schar;
typedef unsigned char atomic_uchar;
typedef short atomic_short;
typedef unsigned short atomic_ushort;
typedef int atomic_int;
typedef unsigned int atomic_uint;
typedef long atomic_long;
typedef unsigned long atomic_ulong;
typedef long long atomic_llong;
typedef unsigned long long atomic_ullong;
typedef unsigned long atomic_size_t;
typedef long atomic_ptrdiff_t;
typedef long atomic_intptr_t;
typedef unsigned long atomic_uintptr_t;

#define ATOMIC_VAR_INIT(v) (v)
#define atomic_init(p, v) (void) (*(p) = (v))

#define atomic_thread_fence(o) ((void) 0)
#define atomic_signal_fence(o) ((void) 0)
#define atomic_is_lock_free(p) 1

#define atomic_load_explicit(p, o) (*(p))
#define atomic_load(p) (*(p))
#define atomic_store_explicit(p, v, o) (void) (*(p) = (v))
#define atomic_store(p, v) (void) (*(p) = (v))

#define atomic_exchange_explicit(p, v, o) __rtmux_fc_xchg((p), (v))
#define atomic_exchange(p, v) __rtmux_fc_xchg((p), (v))
#define __rtmux_fc_xchg(p, v) \
    ({ __typeof__(*(p)) __o = *(p); *(p) = (v); __o; })

#define atomic_fetch_add_explicit(p, v, o) __rtmux_fc_rmw(p, +, v)
#define atomic_fetch_add(p, v) __rtmux_fc_rmw(p, +, v)
#define atomic_fetch_sub_explicit(p, v, o) __rtmux_fc_rmw(p, -, v)
#define atomic_fetch_sub(p, v) __rtmux_fc_rmw(p, -, v)
#define atomic_fetch_or_explicit(p, v, o) __rtmux_fc_rmw(p, |, v)
#define atomic_fetch_or(p, v) __rtmux_fc_rmw(p, |, v)
#define atomic_fetch_and_explicit(p, v, o) __rtmux_fc_rmw(p, &, v)
#define atomic_fetch_and(p, v) __rtmux_fc_rmw(p, &, v)
#define atomic_fetch_xor_explicit(p, v, o) __rtmux_fc_rmw(p, ^, v)
#define atomic_fetch_xor(p, v) __rtmux_fc_rmw(p, ^, v)
#define __rtmux_fc_rmw(p, op, v) \
    ({ __typeof__(*(p)) __o = *(p); *(p) = (__typeof__(*(p))) (__o op (v)); __o; })

/* A faithful sequential CAS: it fails whenever *p differs from *e.  What
 * collapses a retry loop is the absence of interference, not the CAS.  Note
 * the weak form maps here too, so spurious failure is not modeled.
 */
#define atomic_compare_exchange_strong_explicit(p, e, d, s, f) \
    __rtmux_fc_cas((p), (e), (d))
#define atomic_compare_exchange_weak_explicit(p, e, d, s, f) \
    __rtmux_fc_cas((p), (e), (d))
#define atomic_compare_exchange_strong(p, e, d) __rtmux_fc_cas((p), (e), (d))
#define atomic_compare_exchange_weak(p, e, d) __rtmux_fc_cas((p), (e), (d))
#define __rtmux_fc_cas(p, e, d) \
    ({ int __ok = (*(p) == *(e)); if (__ok) *(p) = (d); else *(e) = *(p); __ok; })

#endif /* RTMUX_FRAMAC_STDATOMIC_H */
