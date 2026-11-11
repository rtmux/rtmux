/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef __HWRES_TYPES_H_INCLUDED
#define __HWRES_TYPES_H_INCLUDED

#include <stdint.h>

#include <sys/types.h>

/* Hardware resource classes delegated as ranges or interrupt lines. */
#define HWRES_TYPE_MASK 0xff
enum hwres_type {
    HWRES_MEMORY = 0,
    HWRES_IRQ,
    HWRES_IO_PORT,
    HWRES_DMA_CHANNEL,
    HWRES_PCI_MEMORY,
    HWRES_RESERVED_5,
    HWRES_RESERVED_6,
    HWRES_RESERVED_7,
    HWRES_TYPE_COUNT = 8
};

#define HWRES_FLAG_MASK 0xffffff00
#define HWRES_FLAG_USED 0x00000100
#define HWRES_FLAG_ALIGN 0x00000200
#define HWRES_FLAG_RANGE 0x00000400
#define HWRES_FLAG_SHARE 0x00000800
#define HWRES_FLAG_TOPDOWN 0x00001000
#define HWRES_FLAG_NAME 0x00002000
#define HWRES_FLAG_LIST 0x00004000
#define HWRES_FLAG_RSVP 0x00004000
#define HWRES_FLAG_NOREMOVE 0x00008000

typedef struct hwres_range {
    uint64_t start;
    uint64_t end;
    uint32_t flags;
    const char *name;
} hwres_range_t;

typedef struct hwres_claim {
    uint64_t length;
    uint64_t align;
    uint64_t start;
    uint64_t end;
    uint32_t flags;
    uint32_t zero[2];
    const char *name;
} hwres_claim_t;

#endif
