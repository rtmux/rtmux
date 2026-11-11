/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * crypt(3) — password hashing, SHA-512 modular form ($6$salt$hash).
 *
 * RTMux libc only ships SHA-512 ($6$). No DES, no MD5, no bcrypt; salts without
 * a "$6$" prefix are still treated as SHA-512 input.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef _CRYPT_H_
#define _CRYPT_H_

#include <sys/cdefs.h>

__BEGIN_DECLS

/* Hash "key" against "salt".
 *
 * Returns a pointer into a static buffer (overwritten by subsequent calls), or
 * NULL on error. Output form:
 *
 *   $6$[rounds=N$]salt$base64hash
 *
 * If "salt" is itself a previous crypt() output, compare the returned string to
 * the stored hash without leaking the first differing byte.
 */
char *crypt(const char *__key, const char *__salt);

__END_DECLS

#endif /* !_CRYPT_H_ */
