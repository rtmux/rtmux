/*
 * Untrusted-path join for the RTMux LKL filesystem server.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef RTMUX_LKL_FSD_PATHBUILD_H
#define RTMUX_LKL_FSD_PATHBUILD_H

#include <stddef.h>

/* Join a trusted mount prefix and an untrusted relative path into dst. Rejects
 * an absolute rel and any ".." path component so the result cannot escape the
 * mount. Parameters:
 *   dst   : destination buffer.
 *   dstsz : size of dst in bytes.
 *   mount : trusted mount-point prefix.
 *   rel   : untrusted relative path (a client-supplied connect path).
 * Return value: 0 on success, -1 if rel is absolute, contains a ".." component,
 * or the joined path would not fit in dst.
 */
int build_lkl_path(char *dst, size_t dstsz, const char *mount, const char *rel);

#endif /* RTMUX_LKL_FSD_PATHBUILD_H */
