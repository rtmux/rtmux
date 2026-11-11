/*
 * RTMux endian helpers.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef _ENDIAN_H
#define _ENDIAN_H

#include <stdint.h>

static inline uint16_t rtmux_bswap16(uint16_t x)
{
    return (uint16_t) ((x >> 8) | (x << 8));
}

static inline uint32_t rtmux_bswap32(uint32_t x)
{
    return ((x & 0x000000ffu) << 24) | ((x & 0x0000ff00u) << 8) |
           ((x & 0x00ff0000u) >> 8) | ((x & 0xff000000u) >> 24);
}

static inline uint64_t rtmux_bswap64(uint64_t x)
{
    return ((x & 0x00000000000000ffull) << 56) |
           ((x & 0x000000000000ff00ull) << 40) |
           ((x & 0x0000000000ff0000ull) << 24) |
           ((x & 0x00000000ff000000ull) << 8) |
           ((x & 0x000000ff00000000ull) >> 8) |
           ((x & 0x0000ff0000000000ull) >> 24) |
           ((x & 0x00ff000000000000ull) >> 40) |
           ((x & 0xff00000000000000ull) >> 56);
}

#define ENDIAN_RET16(x) rtmux_bswap16((uint16_t) (x))
#define ENDIAN_RET32(x) rtmux_bswap32((uint32_t) (x))
#define ENDIAN_RET64(x) rtmux_bswap64((uint64_t) (x))

#define ENDIAN_SWAP16(p) (*(uint16_t *) (p) = ENDIAN_RET16(*(uint16_t *) (p)))
#define ENDIAN_SWAP32(p) (*(uint32_t *) (p) = ENDIAN_RET32(*(uint32_t *) (p)))
#define ENDIAN_SWAP64(p) (*(uint64_t *) (p) = ENDIAN_RET64(*(uint64_t *) (p)))

#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
#define ENDIAN_LE16(x) ENDIAN_RET16(x)
#define ENDIAN_LE32(x) ENDIAN_RET32(x)
#define ENDIAN_LE64(x) ENDIAN_RET64(x)
#else
#define ENDIAN_LE16(x) ((uint16_t) (x))
#define ENDIAN_LE32(x) ((uint32_t) (x))
#define ENDIAN_LE64(x) ((uint64_t) (x))
#endif

#endif /* _ENDIAN_H */
