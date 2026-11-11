/*-
 * SHA-512 streaming hash (Init / Update / Final).
 *
 * Adapted for RTMux from FreeBSD's sys/crypto/sha2/sha512c.c (Colin Percival,
 * BSD-2-Clause). We carry only the streaming API — none of the libmd file/fd
 * helpers, no SHA-512/224, /256, /384 variants, no kernel-mode hooks, no ARM64
 * ifunc dispatch.
 *
 * Used by lib/libc/rtmux/crypt.c to implement crypt(3) ($6$...).
 *
 * SPDX-License-Identifier: BSD-2-Clause
 * Original copyright: 2005 Colin Percival
 */

#include "sha512.h"
#include <stdint.h>
#include <string.h>

/* Big-endian byte-stream encode/decode of uint64_t. These are the only endian
 * helpers SHA-512 needs, so inline them here rather than pulling in a system
 * <sys/endian.h>.
 */
static inline void be64enc(void *p, uint64_t v)
{
    uint8_t *b = p;
    b[0] = (uint8_t) (v >> 56);
    b[1] = (uint8_t) (v >> 48);
    b[2] = (uint8_t) (v >> 40);
    b[3] = (uint8_t) (v >> 32);
    b[4] = (uint8_t) (v >> 24);
    b[5] = (uint8_t) (v >> 16);
    b[6] = (uint8_t) (v >> 8);
    b[7] = (uint8_t) v;
}

static inline uint64_t be64dec(const void *p)
{
    const uint8_t *b = p;
    return ((uint64_t) b[0] << 56) | ((uint64_t) b[1] << 48) |
           ((uint64_t) b[2] << 40) | ((uint64_t) b[3] << 32) |
           ((uint64_t) b[4] << 24) | ((uint64_t) b[5] << 16) |
           ((uint64_t) b[6] << 8) | (uint64_t) b[7];
}

static inline void be64enc_vect(unsigned char *dst,
                                const uint64_t *src,
                                size_t len)
{
    size_t i;

    for (i = 0; i < len / 8; i++)
        be64enc(dst + i * 8, src[i]);
}

static inline void be64dec_vect(uint64_t *dst,
                                const unsigned char *src,
                                size_t len)
{
    size_t i;

    for (i = 0; i < len / 8; i++)
        dst[i] = be64dec(src + i * 8);
}

/* SHA-512 round constants. */
static const uint64_t K[80] = {
    0x428a2f98d728ae22ULL, 0x7137449123ef65cdULL, 0xb5c0fbcfec4d3b2fULL,
    0xe9b5dba58189dbbcULL, 0x3956c25bf348b538ULL, 0x59f111f1b605d019ULL,
    0x923f82a4af194f9bULL, 0xab1c5ed5da6d8118ULL, 0xd807aa98a3030242ULL,
    0x12835b0145706fbeULL, 0x243185be4ee4b28cULL, 0x550c7dc3d5ffb4e2ULL,
    0x72be5d74f27b896fULL, 0x80deb1fe3b1696b1ULL, 0x9bdc06a725c71235ULL,
    0xc19bf174cf692694ULL, 0xe49b69c19ef14ad2ULL, 0xefbe4786384f25e3ULL,
    0x0fc19dc68b8cd5b5ULL, 0x240ca1cc77ac9c65ULL, 0x2de92c6f592b0275ULL,
    0x4a7484aa6ea6e483ULL, 0x5cb0a9dcbd41fbd4ULL, 0x76f988da831153b5ULL,
    0x983e5152ee66dfabULL, 0xa831c66d2db43210ULL, 0xb00327c898fb213fULL,
    0xbf597fc7beef0ee4ULL, 0xc6e00bf33da88fc2ULL, 0xd5a79147930aa725ULL,
    0x06ca6351e003826fULL, 0x142929670a0e6e70ULL, 0x27b70a8546d22ffcULL,
    0x2e1b21385c26c926ULL, 0x4d2c6dfc5ac42aedULL, 0x53380d139d95b3dfULL,
    0x650a73548baf63deULL, 0x766a0abb3c77b2a8ULL, 0x81c2c92e47edaee6ULL,
    0x92722c851482353bULL, 0xa2bfe8a14cf10364ULL, 0xa81a664bbc423001ULL,
    0xc24b8b70d0f89791ULL, 0xc76c51a30654be30ULL, 0xd192e819d6ef5218ULL,
    0xd69906245565a910ULL, 0xf40e35855771202aULL, 0x106aa07032bbd1b8ULL,
    0x19a4c116b8d2d0c8ULL, 0x1e376c085141ab53ULL, 0x2748774cdf8eeb99ULL,
    0x34b0bcb5e19b48a8ULL, 0x391c0cb3c5c95a63ULL, 0x4ed8aa4ae3418acbULL,
    0x5b9cca4f7763e373ULL, 0x682e6ff3d6b2b8a3ULL, 0x748f82ee5defb2fcULL,
    0x78a5636f43172f60ULL, 0x84c87814a1f0ab72ULL, 0x8cc702081a6439ecULL,
    0x90befffa23631e28ULL, 0xa4506cebde82bde9ULL, 0xbef9a3f7b2c67915ULL,
    0xc67178f2e372532bULL, 0xca273eceea26619cULL, 0xd186b8c721c0c207ULL,
    0xeada7dd6cde0eb1eULL, 0xf57d4f7fee6ed178ULL, 0x06f067aa72176fbaULL,
    0x0a637dc5a2c898a6ULL, 0x113f9804bef90daeULL, 0x1b710b35131c471bULL,
    0x28db77f523047d84ULL, 0x32caab7b40c72493ULL, 0x3c9ebe0a15c9bebcULL,
    0x431d67c49c100d4cULL, 0x4cc5d4becb3e42b6ULL, 0x597f299cfc657e2aULL,
    0x5fcb6fab3ad6faecULL, 0x6c44198c4a475817ULL};

#define Ch(x, y, z) ((x & (y ^ z)) ^ z)
#define Maj(x, y, z) ((x & (y | z)) | (y & z))
#define SHR(x, n) (x >> n)
#define ROTR(x, n) ((x >> n) | (x << (64 - n)))
#define S0(x) (ROTR(x, 28) ^ ROTR(x, 34) ^ ROTR(x, 39))
#define S1(x) (ROTR(x, 14) ^ ROTR(x, 18) ^ ROTR(x, 41))
#define s0(x) (ROTR(x, 1) ^ ROTR(x, 8) ^ SHR(x, 7))
#define s1(x) (ROTR(x, 19) ^ ROTR(x, 61) ^ SHR(x, 6))

#define RND(a, b, c, d, e, f, g, h, k) \
    h += S1(e) + Ch(e, f, g) + k;      \
    d += h;                            \
    h += S0(a) + Maj(a, b, c);

#define RNDr(S, W, i, ii)                                                   \
    RND(S[(80 - i) % 8], S[(81 - i) % 8], S[(82 - i) % 8], S[(83 - i) % 8], \
        S[(84 - i) % 8], S[(85 - i) % 8], S[(86 - i) % 8], S[(87 - i) % 8], \
        W[i + ii] + K[i + ii])

#define MSCH(W, ii, i) \
    W[i + ii + 16] =   \
        s1(W[i + ii + 14]) + W[i + ii + 9] + s0(W[i + ii + 1]) + W[i + ii]

static void SHA512_Transform(uint64_t *state,
                             const unsigned char block[SHA512_BLOCK_LENGTH])
{
    uint64_t W[80];
    uint64_t S[8];
    int i;

    be64dec_vect(W, block, SHA512_BLOCK_LENGTH);
    memcpy(S, state, SHA512_DIGEST_LENGTH);

    for (i = 0; i < 80; i += 16) {
        RNDr(S, W, 0, i);
        RNDr(S, W, 1, i);
        RNDr(S, W, 2, i);
        RNDr(S, W, 3, i);
        RNDr(S, W, 4, i);
        RNDr(S, W, 5, i);
        RNDr(S, W, 6, i);
        RNDr(S, W, 7, i);
        RNDr(S, W, 8, i);
        RNDr(S, W, 9, i);
        RNDr(S, W, 10, i);
        RNDr(S, W, 11, i);
        RNDr(S, W, 12, i);
        RNDr(S, W, 13, i);
        RNDr(S, W, 14, i);
        RNDr(S, W, 15, i);

        if (i == 64)
            break;
        MSCH(W, 0, i);
        MSCH(W, 1, i);
        MSCH(W, 2, i);
        MSCH(W, 3, i);
        MSCH(W, 4, i);
        MSCH(W, 5, i);
        MSCH(W, 6, i);
        MSCH(W, 7, i);
        MSCH(W, 8, i);
        MSCH(W, 9, i);
        MSCH(W, 10, i);
        MSCH(W, 11, i);
        MSCH(W, 12, i);
        MSCH(W, 13, i);
        MSCH(W, 14, i);
        MSCH(W, 15, i);
    }

    for (i = 0; i < 8; i++)
        state[i] += S[i];
}

static const unsigned char PAD[SHA512_BLOCK_LENGTH] = {
    0x80, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0,    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0,    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0,    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0,    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0,    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};

static void SHA512_Pad(SHA512_CTX *ctx)
{
    size_t r;

    r = (ctx->count[1] >> 3) & 0x7f;
    if (r < 112) {
        memcpy(&ctx->buf[r], PAD, 112 - r);
    } else {
        memcpy(&ctx->buf[r], PAD, 128 - r);
        SHA512_Transform(ctx->state, ctx->buf);
        memset(&ctx->buf[0], 0, 112);
    }
    be64enc_vect(&ctx->buf[112], ctx->count, 16);
    SHA512_Transform(ctx->state, ctx->buf);
}

void SHA512_Init(SHA512_CTX *ctx)
{
    ctx->count[0] = ctx->count[1] = 0;
    ctx->state[0] = 0x6a09e667f3bcc908ULL;
    ctx->state[1] = 0xbb67ae8584caa73bULL;
    ctx->state[2] = 0x3c6ef372fe94f82bULL;
    ctx->state[3] = 0xa54ff53a5f1d36f1ULL;
    ctx->state[4] = 0x510e527fade682d1ULL;
    ctx->state[5] = 0x9b05688c2b3e6c1fULL;
    ctx->state[6] = 0x1f83d9abfb41bd6bULL;
    ctx->state[7] = 0x5be0cd19137e2179ULL;
}

void SHA512_Update(SHA512_CTX *ctx, const void *in, size_t len)
{
    uint64_t bitlen[2];
    uint64_t r;
    const unsigned char *src = in;

    /* A zero-length update adds no bits and buffers nothing, so return before
     * the memcpy: callers such as crypt() pass (NULL, 0) for an empty key or
     * salt, and memcpy(dst, NULL, 0) is undefined behavior the compiler may
     * exploit even though it copies nothing.
     */
    if (len == 0)
        return;

    r = (ctx->count[1] >> 3) & 0x7f;

    bitlen[1] = ((uint64_t) len) << 3;
    bitlen[0] = ((uint64_t) len) >> 61;

    if ((ctx->count[1] += bitlen[1]) < bitlen[1])
        ctx->count[0]++;
    ctx->count[0] += bitlen[0];

    if (len < SHA512_BLOCK_LENGTH - r) {
        memcpy(&ctx->buf[r], src, len);
        return;
    }

    memcpy(&ctx->buf[r], src, SHA512_BLOCK_LENGTH - r);
    SHA512_Transform(ctx->state, ctx->buf);
    src += SHA512_BLOCK_LENGTH - r;
    len -= SHA512_BLOCK_LENGTH - r;

    while (len >= SHA512_BLOCK_LENGTH) {
        SHA512_Transform(ctx->state, src);
        src += SHA512_BLOCK_LENGTH;
        len -= SHA512_BLOCK_LENGTH;
    }

    memcpy(ctx->buf, src, len);
}

void SHA512_Final(unsigned char digest[SHA512_DIGEST_LENGTH], SHA512_CTX *ctx)
{
    SHA512_Pad(ctx);
    be64enc_vect(digest, ctx->state, SHA512_DIGEST_LENGTH);
    memset(ctx, 0, sizeof(*ctx));
}
