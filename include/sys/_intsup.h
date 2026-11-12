/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * sys/_intsup.h — printf format modifiers for fixed-width integer types
 *
 * Used by <inttypes.h> to define PRId32, PRIx64, etc. RTMux: LP64 only.
 *
 * Original: (c) 2004, 2005 Ralf Corsepius, Ulm/Germany. Simplified for RTMux.
 */

#ifndef _SYS__INTSUP_H
#define _SYS__INTSUP_H

/* GCC/Clang-family compilers define __INT32_TYPE__ etc. */
#define __STDINT_EXP(x) __##x##__

/* intptr_t is long on LP64 */
#define _INTPTR_EQ_LONG

/* Printf length modifiers for intN_t types.
 *   ""   = int
 *   "h"  = short
 *   "hh" = char
 *   "l"  = long
 *   "ll" = long long
 */
#define __INT8 "hh"
#define __INT16 "h"
#define __INT32
#define __INT64 "l"

#define __FAST8 "hh"
#define __FAST16 "l"
#define __FAST32 "l"
#define __FAST64 "l"

#define __LEAST8 "hh"
#define __LEAST16 "h"
#define __LEAST32
#define __LEAST64 "l"

#endif /* _SYS__INTSUP_H */
