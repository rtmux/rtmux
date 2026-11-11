/*-
 * crypt(3) — SHA-512 modular form ($6$salt$hash) only.
 *
 * Adapted from FreeBSD's lib/libcrypt/crypt-sha512.c (BSD-2-Clause) with the
 * "$6$" algorithm-prefix dispatcher and the b64_from_24bit helper inlined. No
 * DES, no MD5, no bcrypt. If a salt doesn't begin with "$6$" we still treat it
 * as a SHA-512 salt — there is no other algorithm to fall through to.
 *
 * The work-factor field "rounds=N$" is honored (clamped to [ROUNDS_MIN,
 * ROUNDS_MAX]); default is 5000.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 * Original: 2011 The FreeBSD Project.
 */

#include <crypt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>

#include "sha512.h"

static const char sha512_salt_prefix[] = "$6$";
static const char sha512_rounds_prefix[] = "rounds=";

#define SALT_LEN_MAX 16
#define ROUNDS_DEFAULT 5000
#define ROUNDS_MIN 1000
#define ROUNDS_MAX 999999999

/* Output buffer for crypt() — POSIX says crypt() may return a pointer to static
 * storage that subsequent calls overwrite.
 */
#define CRYPT_BUFSZ 128
static char crypt_static_buf[CRYPT_BUFSZ];

/* Base-64 encoding alphabet used by crypt(3) (note: NOT standard base64; this
 * is the historical Unix-crypt order).
 */
static const char itoa64[] =
    "./0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";

/* Pack three input bytes into n base-64 chars, advancing **cp. */
static void b64_from_24bit(uint8_t B2, uint8_t B1, uint8_t B0, int n, char **cp)
{
    uint32_t w = ((uint32_t) B2 << 16) | ((uint32_t) B1 << 8) | B0;
    int i;

    for (i = 0; i < n; i++) {
        **cp = itoa64[w & 0x3f];
        (*cp)++;
        w >>= 6;
    }
}

#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif
#ifndef MAX
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#endif

/* crypt_sha512: writes "$6$[rounds=N$]salt$hash" to "buffer". Caller guarantees
 * buffer is at least CRYPT_BUFSZ bytes.
 */
static int crypt_sha512(const char *key, const char *salt, char *buffer)
{
    unsigned long srounds;
    uint8_t alt_result[64], temp_result[64];
    SHA512_CTX ctx, alt_ctx;
    size_t salt_len, key_len, cnt, rounds;
    char *cp;
    char *p_bytes, *s_bytes, *endp;
    const char *num;
    bool rounds_custom;

    rounds = ROUNDS_DEFAULT;
    rounds_custom = false;

    if (strncmp(sha512_salt_prefix, salt, sizeof(sha512_salt_prefix) - 1) == 0)
        salt += sizeof(sha512_salt_prefix) - 1;

    if (strncmp(salt, sha512_rounds_prefix, sizeof(sha512_rounds_prefix) - 1) ==
        0) {
        num = salt + sizeof(sha512_rounds_prefix) - 1;
        srounds = strtoul(num, &endp, 10);
        if (*endp == '$') {
            salt = endp + 1;
            rounds = MAX(ROUNDS_MIN, MIN(srounds, ROUNDS_MAX));
            rounds_custom = true;
        }
    }

    salt_len = MIN(strcspn(salt, "$"), SALT_LEN_MAX);
    key_len = strlen(key);

    /* Step 1-3: digest A = SHA512(key || salt || ALT) where ALT is
     * SHA512(key || salt || key), then key-bit-by-bit either ALT or key.
     */
    SHA512_Init(&ctx);
    SHA512_Update(&ctx, key, key_len);
    SHA512_Update(&ctx, salt, salt_len);

    SHA512_Init(&alt_ctx);
    SHA512_Update(&alt_ctx, key, key_len);
    SHA512_Update(&alt_ctx, salt, salt_len);
    SHA512_Update(&alt_ctx, key, key_len);
    SHA512_Final(alt_result, &alt_ctx);

    for (cnt = key_len; cnt > 64; cnt -= 64)
        SHA512_Update(&ctx, alt_result, 64);
    SHA512_Update(&ctx, alt_result, cnt);

    for (cnt = key_len; cnt > 0; cnt >>= 1)
        if ((cnt & 1) != 0)
            SHA512_Update(&ctx, alt_result, 64);
        else
            SHA512_Update(&ctx, key, key_len);

    SHA512_Final(alt_result, &ctx);

    /* Step 4: P-byte sequence (key-length bytes derived from SHA512(key)
     * repeated key_len times).
     */
    SHA512_Init(&alt_ctx);
    for (cnt = 0; cnt < key_len; ++cnt)
        SHA512_Update(&alt_ctx, key, key_len);
    SHA512_Final(temp_result, &alt_ctx);

    /* Use a pair of small malloc()'d scratch buffers for P and S so we don't
     * depend on alloca (which may be unavailable on some libc configurations).
     * freed at end of function.
     */
    p_bytes = key_len ? malloc(key_len) : NULL;
    s_bytes = salt_len ? malloc(salt_len) : NULL;
    if ((key_len && !p_bytes) || (salt_len && !s_bytes)) {
        free(p_bytes);
        free(s_bytes);
        return -1;
    }

    cp = p_bytes;
    for (cnt = key_len; cnt >= 64; cnt -= 64) {
        memcpy(cp, temp_result, 64);
        cp += 64;
    }
    memcpy(cp, temp_result, cnt);

    /* Step 5: S-byte sequence. */
    SHA512_Init(&alt_ctx);
    for (cnt = 0; cnt < (size_t) 16 + alt_result[0]; ++cnt)
        SHA512_Update(&alt_ctx, salt, salt_len);
    SHA512_Final(temp_result, &alt_ctx);

    cp = s_bytes;
    for (cnt = salt_len; cnt >= 64; cnt -= 64) {
        memcpy(cp, temp_result, 64);
        cp += 64;
    }
    memcpy(cp, temp_result, cnt);

    /* Step 6: the rounds loop. This is the work factor. */
    for (cnt = 0; cnt < rounds; ++cnt) {
        SHA512_Init(&ctx);

        if ((cnt & 1) != 0)
            SHA512_Update(&ctx, p_bytes, key_len);
        else
            SHA512_Update(&ctx, alt_result, 64);

        if (cnt % 3 != 0)
            SHA512_Update(&ctx, s_bytes, salt_len);

        if (cnt % 7 != 0)
            SHA512_Update(&ctx, p_bytes, key_len);

        if ((cnt & 1) != 0)
            SHA512_Update(&ctx, alt_result, 64);
        else
            SHA512_Update(&ctx, p_bytes, key_len);

        SHA512_Final(alt_result, &ctx);
    }

    /* Step 7: build output string "$6$[rounds=N$]salt$base64hash". */
    cp = buffer;
    memcpy(cp, sha512_salt_prefix, sizeof(sha512_salt_prefix) - 1);
    cp += sizeof(sha512_salt_prefix) - 1;

    if (rounds_custom)
        cp += sprintf(cp, "%s%zu$", sha512_rounds_prefix, rounds);

    memcpy(cp, salt, salt_len);
    cp += salt_len;
    *cp++ = '$';

    /* Drepper-spec base-64 permutation of the 64-byte digest into 86 chars. */
    b64_from_24bit(alt_result[0], alt_result[21], alt_result[42], 4, &cp);
    b64_from_24bit(alt_result[22], alt_result[43], alt_result[1], 4, &cp);
    b64_from_24bit(alt_result[44], alt_result[2], alt_result[23], 4, &cp);
    b64_from_24bit(alt_result[3], alt_result[24], alt_result[45], 4, &cp);
    b64_from_24bit(alt_result[25], alt_result[46], alt_result[4], 4, &cp);
    b64_from_24bit(alt_result[47], alt_result[5], alt_result[26], 4, &cp);
    b64_from_24bit(alt_result[6], alt_result[27], alt_result[48], 4, &cp);
    b64_from_24bit(alt_result[28], alt_result[49], alt_result[7], 4, &cp);
    b64_from_24bit(alt_result[50], alt_result[8], alt_result[29], 4, &cp);
    b64_from_24bit(alt_result[9], alt_result[30], alt_result[51], 4, &cp);
    b64_from_24bit(alt_result[31], alt_result[52], alt_result[10], 4, &cp);
    b64_from_24bit(alt_result[53], alt_result[11], alt_result[32], 4, &cp);
    b64_from_24bit(alt_result[12], alt_result[33], alt_result[54], 4, &cp);
    b64_from_24bit(alt_result[34], alt_result[55], alt_result[13], 4, &cp);
    b64_from_24bit(alt_result[56], alt_result[14], alt_result[35], 4, &cp);
    b64_from_24bit(alt_result[15], alt_result[36], alt_result[57], 4, &cp);
    b64_from_24bit(alt_result[37], alt_result[58], alt_result[16], 4, &cp);
    b64_from_24bit(alt_result[59], alt_result[17], alt_result[38], 4, &cp);
    b64_from_24bit(alt_result[18], alt_result[39], alt_result[60], 4, &cp);
    b64_from_24bit(alt_result[40], alt_result[61], alt_result[19], 4, &cp);
    b64_from_24bit(alt_result[62], alt_result[20], alt_result[41], 4, &cp);
    b64_from_24bit(0, 0, alt_result[63], 2, &cp);

    *cp = '\0';

    /* Wipe intermediates: defense in depth. */
    memset(temp_result, 0, sizeof(temp_result));
    memset(alt_result, 0, sizeof(alt_result));
    if (p_bytes)
        memset(p_bytes, 0, key_len);
    if (s_bytes)
        memset(s_bytes, 0, salt_len);
    free(p_bytes);
    free(s_bytes);
    return 0;
}

/* Public crypt(3).
 *
 * Returns a pointer into a static buffer; subsequent calls overwrite it.
 *
 * Returns NULL on any error (out of memory).
 */
char *crypt(const char *key, const char *salt)
{
    if (!key || !salt)
        return NULL;
    if (crypt_sha512(key, salt, crypt_static_buf) != 0)
        return NULL;
    return crypt_static_buf;
}
