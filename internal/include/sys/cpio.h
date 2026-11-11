/**
 * \file cpio.h
 *
 * CPIO newc format parser.
 *
 * Adapted from gRouvr hypervisor CPIO parser.
 *
 * \license SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _SYS_CPIO_H
#define _SYS_CPIO_H

#include <stdint.h>
#include <stddef.h>

/* CPIO newc format magic number */
#define CPIO_NEWC_MAGIC "070701"

/* File information structure returned by the parser */
typedef struct {
    char filename[256];
    uint32_t filesize;
    uint32_t mode;
    const uint8_t *data;
} cpio_file_info_t;

/* Callback function type for iterating through CPIO entries */
typedef void (*cpio_callback_t)(const cpio_file_info_t *file_info,
                                void *user_data);

/**
 * Check if the data is a valid CPIO newc archive.
 * Returns: 1 if valid, 0 if not.
 */
int cpio_check_valid(const uint8_t *data, size_t size);

/**
 * Iterate through all files in the CPIO archive. Calls the callback function
 * for each file found.
 */
void cpio_iterate(const uint8_t *data,
                  size_t size,
                  cpio_callback_t callback,
                  void *user_data);

/**
 * Find a file in CPIO archive by name.
 * Returns 1 if found, 0 if not found.
 */
int cpio_find_file(const uint8_t *data,
                   size_t size,
                   const char *filename,
                   cpio_file_info_t *info);

#endif /* _SYS_CPIO_H */
