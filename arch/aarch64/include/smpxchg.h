/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef __SMPXCHG_H_INCLUDED
#define __SMPXCHG_H_INCLUDED

#ifndef __PLATFORM_H_INCLUDED
#include <sys/platform.h>
#endif

__BEGIN_DECLS

extern unsigned _smp_cmpxchg(volatile unsigned *__dst,
                             unsigned __cmp,
                             unsigned __src);
extern unsigned _smp_xchg(volatile unsigned *__dst, unsigned __src);

/* _mux_smp_cmpxchg / _smp_xchg_ul operate on unsigned long for LP64 mutex owner
 * fields (owner is tid, fits in 32 bits, but the field is unsigned long for
 * alignment).
 */
extern unsigned long _mux_smp_cmpxchg_ul(volatile unsigned long *__dst,
                                         unsigned long __cmp,
                                         unsigned long __src);
extern unsigned long _smp_xchg_ul(volatile unsigned long *__dst,
                                  unsigned long __src);

#define __mutex_smp_cmpxchg(d, c, s) \
    _mux_smp_cmpxchg_ul((d), (unsigned long) (c), (unsigned long) (s))
#define __mutex_smp_xchg(d, n) _smp_xchg_ul((d), (unsigned long) (n))

__END_DECLS

#endif
