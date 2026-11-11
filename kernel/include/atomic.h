/*
 * RTMux kernel/private atomic helpers.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef _RTMUX_PRIVATE_ATOMIC_H
#define _RTMUX_PRIVATE_ATOMIC_H

#ifndef __PLATFORM_H_INCLUDED
#include <sys/platform.h>
#endif

/* Naming note: these names match the real semantics emitted by "amoor.w" /
 * "amoor.d" (atomic OR), not atomic store: bit-set (OR), bit-clear (AND-NOT),
 * bit-toggle (XOR), add, and sub.
 */
__BEGIN_DECLS
extern void atomic_add(volatile unsigned *__loc, unsigned __incr);
extern void atomic_clr(volatile unsigned *__loc, unsigned __bits);
extern void atomic_or(volatile unsigned *__loc, unsigned __bits);
extern void atomic_sub(volatile unsigned *__loc, unsigned __decr);
extern void atomic_toggle(volatile unsigned *__loc, unsigned __bits);

extern unsigned atomic_add_value(volatile unsigned *__loc, unsigned __incr);
extern unsigned atomic_clr_value(volatile unsigned *__loc, unsigned __bits);
extern unsigned atomic_or_value(volatile unsigned *__loc, unsigned __bits);
extern unsigned atomic_sub_value(volatile unsigned *__loc, unsigned __decr);
extern unsigned atomic_toggle_value(volatile unsigned *__loc, unsigned __bits);

extern void atomic_add_64(volatile unsigned long *__loc, unsigned long __incr);
extern void atomic_clr_64(volatile unsigned long *__loc, unsigned long __bits);
extern void atomic_or_64(volatile unsigned long *__loc, unsigned long __bits);
extern void atomic_sub_64(volatile unsigned long *__loc, unsigned long __decr);
extern void atomic_toggle_64(volatile unsigned long *__loc,
                             unsigned long __bits);

extern unsigned long atomic_add_value_64(volatile unsigned long *__loc,
                                         unsigned long __incr);
extern unsigned long atomic_clr_value_64(volatile unsigned long *__loc,
                                         unsigned long __bits);
extern unsigned long atomic_or_value_64(volatile unsigned long *__loc,
                                        unsigned long __bits);
extern unsigned long atomic_sub_value_64(volatile unsigned long *__loc,
                                         unsigned long __decr);
extern unsigned long atomic_toggle_value_64(volatile unsigned long *__loc,
                                            unsigned long __bits);
__END_DECLS

#endif
