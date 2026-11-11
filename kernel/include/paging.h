/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef _KERNEL_PAGING_H
#define _KERNEL_PAGING_H

#include <arch/cpu-paging.h>

#if VM_USER_SPACE_BOUNDARY < VM_KERN_SPACE_BOUNDARY
/* kernel/ProcNto is in high memory */
#define WITHIN_BOUNDARY(first, last, boundary) \
    ((first) <= (boundary) && (last) <= (boundary))
#else
/* kernel/ProcNto is in low memory */
#define WITHIN_BOUNDARY(first, last, boundary) \
    ((first) >= (boundary) && (last) >= (boundary))
#endif

#endif
