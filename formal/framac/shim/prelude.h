/*
 * Force-included ahead of every kernel source. Frama-C 33 has no C11 _Atomic
 * and no __atomic_* builtins: left alone it infers a prototype for each builtin
 * from its first call site, which both rejects the second call with a different
 * width (kernel/thread/lifecycle.c) and turns every atomic read into an opaque
 * extern call, so EVA loses the value of most kernel state. Modeling them as
 * ordinary reads and writes keeps the sequential semantics that WP reasons
 * about.
 *
 * What this erases: memory ordering, and interference between CPUs. No result
 * obtained under this prelude says anything about SMP races. Those belong to
 * the litmus gates (scripts/check-rcu-litmus.sh, scripts/check-ipc-litmus.sh),
 * not to Frama-C.
 */
#ifndef RTMUX_FRAMAC_PRELUDE_H
#define RTMUX_FRAMAC_PRELUDE_H

#define _Atomic

#define __ATOMIC_RELAXED 0
#define __ATOMIC_CONSUME 1
#define __ATOMIC_ACQUIRE 2
#define __ATOMIC_RELEASE 3
#define __ATOMIC_ACQ_REL 4
#define __ATOMIC_SEQ_CST 5

#define __atomic_load_n(p, o) (*(p))
#define __atomic_store_n(p, v, o) ((void) (*(p) = (v)))
#define __atomic_thread_fence(o) ((void) 0)
#define __atomic_signal_fence(o) ((void) 0)

#define __atomic_exchange_n(p, v, o) \
    ({ __typeof__(*(p)) __fc_old = *(p); *(p) = (v); __fc_old; })

#define __rtmux_fc_fetch_op(p, op, v)                            \
    ({ __typeof__(*(p)) __fc_old = *(p);                         \
       *(p) = (__typeof__(*(p))) (__fc_old op(v));               \
       __fc_old; })
#define __rtmux_fc_op_fetch(p, op, v)                            \
    ({ *(p) = (__typeof__(*(p))) (*(p) op(v)); *(p); })

#define __atomic_fetch_add(p, v, o) __rtmux_fc_fetch_op(p, +, v)
#define __atomic_fetch_sub(p, v, o) __rtmux_fc_fetch_op(p, -, v)
#define __atomic_fetch_or(p, v, o) __rtmux_fc_fetch_op(p, |, v)
#define __atomic_fetch_and(p, v, o) __rtmux_fc_fetch_op(p, &, v)
#define __atomic_fetch_xor(p, v, o) __rtmux_fc_fetch_op(p, ^, v)
#define __atomic_add_fetch(p, v, o) __rtmux_fc_op_fetch(p, +, v)
#define __atomic_sub_fetch(p, v, o) __rtmux_fc_op_fetch(p, -, v)
#define __atomic_or_fetch(p, v, o) __rtmux_fc_op_fetch(p, |, v)
#define __atomic_and_fetch(p, v, o) __rtmux_fc_op_fetch(p, &, v)
#define __atomic_xor_fetch(p, v, o) __rtmux_fc_op_fetch(p, ^, v)

/* A faithful sequential CAS: it fails whenever *p differs from *e, and then
 * updates *e the way the real builtin does.  What collapses a retry loop is
 * the absence of interference, not the CAS itself.  Two gaps to know about:
 * weak CAS maps here too, so spurious failure is removed from the model, and
 * nothing here can express another CPU changing *p between the load and the
 * exchange.
 */
#define __atomic_compare_exchange_n(p, e, d, weak, s, f)         \
    ({ int __fc_ok = (*(p) == *(e));                             \
       if (__fc_ok) *(p) = (d); else *(e) = *(p);                \
       __fc_ok; })

/* The tree's <errno.h> opens with a bare "extern int errno;", which trips
 * Frama-C's CERT MSC38-C check as a hard error. Defining errno as a macro first
 * rewrites that line into a declaration of __get_errno_ptr, which is what the
 * header itself declares one line further down, so nothing named errno is ever
 * declared as an object.
 */
int *__get_errno_ptr(void) __attribute__((__const__));
#define errno (*__get_errno_ptr())

/* <setjmp.h> trips the same check and has no macro form to hide behind, so
 * rename it.  This rewrites the declaration only: <setjmp.h> redefines setjmp
 * in terms of sigsetjmp further down and that definition wins, so call sites
 * still expand to _setjmp.  Moot in practice, since the kernel reaches
 * _setjmp through xfer_setjmp and never calls setjmp directly.
 */
#define setjmp(e) __rtmux_fc_setjmp(e)

#endif /* RTMUX_FRAMAC_PRELUDE_H */
