/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef _BITOPS_H
#define _BITOPS_H

#include <stdint.h>

/* Population count. rv64g has no popcount instruction (no Zbb), and
 * __builtin_popcountll falls back to libgcc's __popcountdi2, which the
 * kernel does not link, so count with SWAR arithmetic instead.
 */
static inline unsigned popcount64(uint64_t x)
{
    x = x - ((x >> 1) & 0x5555555555555555ULL);
    x = (x & 0x3333333333333333ULL) + ((x >> 2) & 0x3333333333333333ULL);
    x = (x + (x >> 4)) & 0x0F0F0F0F0F0F0F0FULL;
    return (unsigned) ((x * 0x0101010101010101ULL) >> 56);
}

#endif
