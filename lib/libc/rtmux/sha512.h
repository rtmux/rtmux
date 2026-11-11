/*-
 * SHA-512 streaming hash interface (internal to libc/rtmux).
 *
 * Used by lib/libc/rtmux/crypt.c. Not in the public include path — if userland
 * needs SHA-512 separately later, copy this header to include/sha512.h and add
 * file/fd helpers as needed.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 * Original: 2005 Colin Percival.
 */

#ifndef _RTMUX_SHA512_H_
#define _RTMUX_SHA512_H_

#include <stddef.h>
#include <stdint.h>
#include <sys/cdefs.h>

#define SHA512_BLOCK_LENGTH 128
#define SHA512_DIGEST_LENGTH 64
#define SHA512_DIGEST_STRING_LENGTH (SHA512_DIGEST_LENGTH * 2 + 1)

typedef struct SHA512Context {
    uint64_t state[8];
    uint64_t count[2];
    uint8_t buf[SHA512_BLOCK_LENGTH];
} SHA512_CTX;

void SHA512_Init(SHA512_CTX *) __hidden;
void SHA512_Update(SHA512_CTX *, const void *, size_t) __hidden;
void SHA512_Final(unsigned char[SHA512_DIGEST_LENGTH], SHA512_CTX *) __hidden;

#endif /* !_RTMUX_SHA512_H_ */
