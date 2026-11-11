/*
 * RTMux pathname normalization.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef PATH_NORMALIZE_H
#define PATH_NORMALIZE_H

#include <stddef.h>

int path_normalize(const char *path, char *out, size_t outsz, int *dir_out);

#endif
