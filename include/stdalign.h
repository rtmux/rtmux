/*
 * C11 alignment macros with C99 compiler fallback.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef _STDALIGN_H
#define _STDALIGN_H

#include <sys/cdefs.h>

#ifndef __cplusplus
#ifndef alignas
#define alignas _Alignas
#endif
#ifndef alignof
#define alignof _Alignof
#endif
#endif

#define __alignas_is_defined 1
#define __alignof_is_defined 1

#endif /* _STDALIGN_H */
