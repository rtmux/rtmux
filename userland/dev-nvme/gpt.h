/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * dev-nvme GPT parser
 *
 * Pure parser with no I/O of its own: the caller supplies a callback that reads
 * raw LBAs from the underlying block device. GPT only: the parser rejects plain
 * MBR layouts and hybrid GPT configurations.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef DEV_NVME_GPT_H
#define DEV_NVME_GPT_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

/* Return codes for gpt_parse(). */
#define GPT_OK 0
#define GPT_ERR_READ -1          /* read callback failed */
#define GPT_ERR_NO_PMBR -2       /* LBA 0 isn't a protective MBR */
#define GPT_ERR_HYBRID_MBR -3    /* MBR carries non-EE partitions */
#define GPT_ERR_BAD_SIG -4       /* GPT header signature wrong */
#define GPT_ERR_BAD_REV -5       /* GPT header revision unsupported */
#define GPT_ERR_BAD_HDR_CRC -6   /* header CRC32 mismatch */
#define GPT_ERR_BAD_ENTRY_CRC -7 /* partition-array CRC32 mismatch */
#define GPT_ERR_BAD_GEOMETRY -8  /* entry size or count out of range */
#define GPT_ERR_NOMEM -9         /* out of memory */

/* Read callback: return the number of LBAs actually read (must equal n_lba on
 * success), or a negative value on I/O error. buf is at least n_lba * lba_size
 * bytes.
 */
typedef ssize_t (*gpt_read_fn_t)(void *ctx,
                                 uint64_t lba,
                                 void *buf,
                                 size_t n_lba);

typedef struct {
    uint64_t start_lba;
    uint64_t end_lba; /* inclusive */
    uint8_t type_guid[16];
    uint8_t part_guid[16];
    uint64_t attrs;
    char name[37]; /* UTF-8, NUL-terminated */
} gpt_part_t;

typedef struct {
    uint8_t disk_guid[16];
    uint32_t lba_size;
    uint64_t first_usable_lba;
    uint64_t last_usable_lba;
    unsigned n_parts;  /* number of populated entries in parts[] */
    gpt_part_t *parts; /* malloc'd, freed by gpt_free() */
} gpt_table_t;

/* gpt_parse — read and validate a GPT from an open block device.
 *
 * read_fn caller-provided read callback (must read whole LBAs) ctx opaque
 * context passed to read_fn lba_size sector size in bytes (512, 4096, …) out
 * table populated on success; *out is zero-initialized first
 *
 * Returns GPT_OK (0) on success, or a negative GPT_ERR_* code. On success, call
 * gpt_free(out) when done.
 */
int gpt_parse(gpt_read_fn_t read_fn,
              void *ctx,
              uint32_t lba_size,
              gpt_table_t *out);

void gpt_free(gpt_table_t *tbl);

#endif /* DEV_NVME_GPT_H */
