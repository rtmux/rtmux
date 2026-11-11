/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "hash.h"

/* SysV hash function for symbol table lookup. */
uint32_t elf_hash(const char *name)
{
    const unsigned char *p = (const unsigned char *) name;
    uint32_t h = 0;

    while (*p != '\0') {
        h = (h << 4) + *p++;
        h ^= (h >> 24) & 0xf0;
    }
    return h & 0x0fffffff;
}

/* GNU hash is the Daniel J. Bernstein hash clipped to 32 bits. */
uint32_t ldso_gnu_hash(const char *name)
{
    const unsigned char *p = (const unsigned char *) name;
    uint32_t h = 5381;

    while (*p != '\0')
        h = h * 33 + *p++;
    return h;
}
