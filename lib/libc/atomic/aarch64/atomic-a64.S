/* SPDX-License-Identifier: Apache-2.0 */
/* Armv8.0 LL/SC atomics; return the value before the operation. */
.text
.global atomic_add
.type atomic_add, %function
atomic_add:
.global atomic_add_value
.type atomic_add_value, %function
atomic_add_value:
1: ldaxr w2, [x0]
   add w3, w2, w1
   stlxr w4, w3, [x0]
   cbnz w4, 1b
   mov w0, w2
   ret
.global atomic_sub
.type atomic_sub, %function
atomic_sub:
.global atomic_sub_value
.type atomic_sub_value, %function
atomic_sub_value:
1: ldaxr w2, [x0]
   sub w3, w2, w1
   stlxr w4, w3, [x0]
   cbnz w4, 1b
   mov w0, w2
   ret
.global atomic_or
.type atomic_or, %function
atomic_or:
.global atomic_or_value
.type atomic_or_value, %function
atomic_or_value:
1: ldaxr w2, [x0]
   orr w3, w2, w1
   stlxr w4, w3, [x0]
   cbnz w4, 1b
   mov w0, w2
   ret
.global atomic_clr
.type atomic_clr, %function
atomic_clr:
.global atomic_clr_value
.type atomic_clr_value, %function
atomic_clr_value:
1: ldaxr w2, [x0]
   bic w3, w2, w1
   stlxr w4, w3, [x0]
   cbnz w4, 1b
   mov w0, w2
   ret
.global atomic_toggle
.type atomic_toggle, %function
atomic_toggle:
.global atomic_toggle_value
.type atomic_toggle_value, %function
atomic_toggle_value:
1: ldaxr w2, [x0]
   eor w3, w2, w1
   stlxr w4, w3, [x0]
   cbnz w4, 1b
   mov w0, w2
   ret
.global atomic_add_64
.type atomic_add_64, %function
atomic_add_64:
.global atomic_add_value_64
.type atomic_add_value_64, %function
atomic_add_value_64:
1: ldaxr x2, [x0]
   add x3, x2, x1
   stlxr w4, x3, [x0]
   cbnz w4, 1b
   mov x0, x2
   ret
.global atomic_sub_64
.type atomic_sub_64, %function
atomic_sub_64:
.global atomic_sub_value_64
.type atomic_sub_value_64, %function
atomic_sub_value_64:
1: ldaxr x2, [x0]
   sub x3, x2, x1
   stlxr w4, x3, [x0]
   cbnz w4, 1b
   mov x0, x2
   ret
.global atomic_or_64
.type atomic_or_64, %function
atomic_or_64:
.global atomic_or_value_64
.type atomic_or_value_64, %function
atomic_or_value_64:
1: ldaxr x2, [x0]
   orr x3, x2, x1
   stlxr w4, x3, [x0]
   cbnz w4, 1b
   mov x0, x2
   ret
.global atomic_clr_64
.type atomic_clr_64, %function
atomic_clr_64:
.global atomic_clr_value_64
.type atomic_clr_value_64, %function
atomic_clr_value_64:
1: ldaxr x2, [x0]
   bic x3, x2, x1
   stlxr w4, x3, [x0]
   cbnz w4, 1b
   mov x0, x2
   ret
.global atomic_toggle_64
.type atomic_toggle_64, %function
atomic_toggle_64:
.global atomic_toggle_value_64
.type atomic_toggle_value_64, %function
atomic_toggle_value_64:
1: ldaxr x2, [x0]
   eor x3, x2, x1
   stlxr w4, x3, [x0]
   cbnz w4, 1b
   mov x0, x2
   ret
.global _smp_xchg
.type _smp_xchg, %function
_smp_xchg:
1: ldaxr w2, [x0]
   stlxr w3, w1, [x0]
   cbnz w3, 1b
   mov w0, w2
   ret
.global _smp_xchg_ul
.type _smp_xchg_ul, %function
_smp_xchg_ul:
1: ldaxr x2, [x0]
   stlxr w3, x1, [x0]
   cbnz w3, 1b
   mov x0, x2
   ret
.global _smp_cmpxchg
.type _smp_cmpxchg, %function
_smp_cmpxchg:
1: ldaxr w3, [x0]
   cmp w3, w1
   b.ne 2f
   stlxr w4, w2, [x0]
   cbnz w4, 1b
2: clrex
   mov w0, w3
   ret
.global _mux_smp_cmpxchg_ul
.type _mux_smp_cmpxchg_ul, %function
_mux_smp_cmpxchg_ul:
1: ldaxr x3, [x0]
   cmp x3, x1
   b.ne 2f
   stlxr w4, x2, [x0]
   cbnz w4, 1b
2: clrex
   mov x0, x3
   ret
.global _mux_smp_cmpxchg
.set _mux_smp_cmpxchg, _smp_cmpxchg
