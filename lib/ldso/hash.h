/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef LDSO_HASH_H
#define LDSO_HASH_H

#include <stdint.h>

uint32_t elf_hash(const char *name) __attribute__((visibility("hidden")));
uint32_t ldso_gnu_hash(const char *name) __attribute__((visibility("hidden")));

#endif /* LDSO_HASH_H */
