/*
 * RISC-V 64-bit atomic operations using AMO instructions.
 *
 * All operations work on 32-bit word values through volatile unsigned pointers.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

.text

/*
 * void atomic_add(volatile unsigned *loc, unsigned val)
 */
    .globl  atomic_add
    .type   atomic_add, @function
atomic_add:
    amoadd.w    zero, a1, (a0)
    ret
    .size   atomic_add, .-atomic_add

/*
 * unsigned atomic_add_value(volatile unsigned *loc, unsigned val)
 *   Returns the old value.
 */
    .globl  atomic_add_value
    .type   atomic_add_value, @function
atomic_add_value:
    amoadd.w    a0, a1, (a0)
    ret
    .size   atomic_add_value, .-atomic_add_value

/*
 * void atomic_sub(volatile unsigned *loc, unsigned val)
 */
    .globl  atomic_sub
    .type   atomic_sub, @function
atomic_sub:
    neg     a1, a1
    amoadd.w    zero, a1, (a0)
    ret
    .size   atomic_sub, .-atomic_sub

/*
 * unsigned atomic_sub_value(volatile unsigned *loc, unsigned val)
 *   Returns the old value.
 */
    .globl  atomic_sub_value
    .type   atomic_sub_value, @function
atomic_sub_value:
    neg     a1, a1
    amoadd.w    a0, a1, (a0)
    ret
    .size   atomic_sub_value, .-atomic_sub_value

/*
 * void atomic_or(volatile unsigned *loc, unsigned bits)
 *   Emits amoor.w (atomic OR), NOT atomic store.
 */
    .globl  atomic_or
    .type   atomic_or, @function
atomic_or:
    amoor.w     zero, a1, (a0)
    ret
    .size   atomic_or, .-atomic_or

/*
 * unsigned atomic_or_value(volatile unsigned *loc, unsigned bits)
 *   Emits amoor.w (atomic OR), NOT atomic store.
 *   Returns the old value.
 */
    .globl  atomic_or_value
    .type   atomic_or_value, @function
atomic_or_value:
    amoor.w     a0, a1, (a0)
    ret
    .size   atomic_or_value, .-atomic_or_value

/*
 * void atomic_clr(volatile unsigned *loc, unsigned bits)
 */
    .globl  atomic_clr
    .type   atomic_clr, @function
atomic_clr:
    not     a1, a1
    amoand.w    zero, a1, (a0)
    ret
    .size   atomic_clr, .-atomic_clr

/*
 * unsigned atomic_clr_value(volatile unsigned *loc, unsigned bits)
 *   Returns the old value.
 */
    .globl  atomic_clr_value
    .type   atomic_clr_value, @function
atomic_clr_value:
    not     a1, a1
    amoand.w    a0, a1, (a0)
    ret
    .size   atomic_clr_value, .-atomic_clr_value

/*
 * void atomic_toggle(volatile unsigned *loc, unsigned bits)
 */
    .globl  atomic_toggle
    .type   atomic_toggle, @function
atomic_toggle:
    amoxor.w    zero, a1, (a0)
    ret
    .size   atomic_toggle, .-atomic_toggle

/*
 * unsigned atomic_toggle_value(volatile unsigned *loc, unsigned bits)
 *   Returns the old value.
 */
    .globl  atomic_toggle_value
    .type   atomic_toggle_value, @function
atomic_toggle_value:
    amoxor.w    a0, a1, (a0)
    ret
    .size   atomic_toggle_value, .-atomic_toggle_value

/* ================================================================
 * 64-bit (doubleword) variants using .d AMO instructions.
 * ================================================================ */

/*
 * void atomic_add_64(volatile unsigned long *loc, unsigned long val)
 */
    .globl  atomic_add_64
    .type   atomic_add_64, @function
atomic_add_64:
    amoadd.d    zero, a1, (a0)
    ret
    .size   atomic_add_64, .-atomic_add_64

/*
 * unsigned long atomic_add_value_64(volatile unsigned long *loc, unsigned long val)
 */
    .globl  atomic_add_value_64
    .type   atomic_add_value_64, @function
atomic_add_value_64:
    amoadd.d    a0, a1, (a0)
    ret
    .size   atomic_add_value_64, .-atomic_add_value_64

/*
 * void atomic_sub_64(volatile unsigned long *loc, unsigned long val)
 */
    .globl  atomic_sub_64
    .type   atomic_sub_64, @function
atomic_sub_64:
    neg     a1, a1
    amoadd.d    zero, a1, (a0)
    ret
    .size   atomic_sub_64, .-atomic_sub_64

/*
 * unsigned long atomic_sub_value_64(volatile unsigned long *loc, unsigned long val)
 */
    .globl  atomic_sub_value_64
    .type   atomic_sub_value_64, @function
atomic_sub_value_64:
    neg     a1, a1
    amoadd.d    a0, a1, (a0)
    ret
    .size   atomic_sub_value_64, .-atomic_sub_value_64

/*
 * void atomic_or_64(volatile unsigned long *loc, unsigned long bits)
 *   Emits amoor.d (atomic OR), NOT atomic store.
 */
    .globl  atomic_or_64
    .type   atomic_or_64, @function
atomic_or_64:
    amoor.d     zero, a1, (a0)
    ret
    .size   atomic_or_64, .-atomic_or_64

/*
 * unsigned long atomic_or_value_64(volatile unsigned long *loc, unsigned long bits)
 *   Emits amoor.d (atomic OR), NOT atomic store.  Returns the old value.
 */
    .globl  atomic_or_value_64
    .type   atomic_or_value_64, @function
atomic_or_value_64:
    amoor.d     a0, a1, (a0)
    ret
    .size   atomic_or_value_64, .-atomic_or_value_64

/*
 * void atomic_clr_64(volatile unsigned long *loc, unsigned long bits)
 */
    .globl  atomic_clr_64
    .type   atomic_clr_64, @function
atomic_clr_64:
    not     a1, a1
    amoand.d    zero, a1, (a0)
    ret
    .size   atomic_clr_64, .-atomic_clr_64

/*
 * unsigned long atomic_clr_value_64(volatile unsigned long *loc, unsigned long bits)
 */
    .globl  atomic_clr_value_64
    .type   atomic_clr_value_64, @function
atomic_clr_value_64:
    not     a1, a1
    amoand.d    a0, a1, (a0)
    ret
    .size   atomic_clr_value_64, .-atomic_clr_value_64

/*
 * void atomic_toggle_64(volatile unsigned long *loc, unsigned long bits)
 */
    .globl  atomic_toggle_64
    .type   atomic_toggle_64, @function
atomic_toggle_64:
    amoxor.d    zero, a1, (a0)
    ret
    .size   atomic_toggle_64, .-atomic_toggle_64

/*
 * unsigned long atomic_toggle_value_64(volatile unsigned long *loc, unsigned long bits)
 */
    .globl  atomic_toggle_value_64
    .type   atomic_toggle_value_64, @function
atomic_toggle_value_64:
    amoxor.d    a0, a1, (a0)
    ret
    .size   atomic_toggle_value_64, .-atomic_toggle_value_64

/* ================================================================
 * SMP compare-and-swap / exchange primitives.
 * Used by pthread_mutex_{lock,unlock,trylock} fast paths.
 * ================================================================ */

/*
 * unsigned _smp_cmpxchg(volatile unsigned *dst, unsigned cmp, unsigned src)
 *   Atomically: if (*dst == cmp) *dst = src; return old *dst.
 */
    .globl  _smp_cmpxchg
    .type   _smp_cmpxchg, @function
_smp_cmpxchg:
1:  lr.w.aq a3, (a0)
    bne     a3, a1, 2f
    sc.w.rl a4, a2, (a0)
    bnez    a4, 1b
2:  mv      a0, a3
    ret
    .size   _smp_cmpxchg, .-_smp_cmpxchg

/*
 * unsigned _smp_xchg(volatile unsigned *dst, unsigned src)
 *   Atomically: old = *dst; *dst = src; return old.
 */
    .globl  _smp_xchg
    .type   _smp_xchg, @function
_smp_xchg:
    amoswap.w.aqrl  a0, a1, (a0)
    ret
    .size   _smp_xchg, .-_smp_xchg

/*
 * unsigned long _mux_smp_cmpxchg_ul(volatile unsigned long *dst,
 *                                    unsigned long cmp, unsigned long src)
 *   64-bit compare-and-swap for LP64 mutex owner fields.
 */
    .globl  _mux_smp_cmpxchg_ul
    .type   _mux_smp_cmpxchg_ul, @function
_mux_smp_cmpxchg_ul:
1:  lr.d.aq a3, (a0)
    bne     a3, a1, 2f
    sc.d.rl a4, a2, (a0)
    bnez    a4, 1b
2:  mv      a0, a3
    ret
    .size   _mux_smp_cmpxchg_ul, .-_mux_smp_cmpxchg_ul

/*
 * unsigned long _smp_xchg_ul(volatile unsigned long *dst, unsigned long src)
 *   64-bit atomic exchange.
 */
    .globl  _smp_xchg_ul
    .type   _smp_xchg_ul, @function
_smp_xchg_ul:
    amoswap.d.aqrl  a0, a1, (a0)
    ret
    .size   _smp_xchg_ul, .-_smp_xchg_ul
