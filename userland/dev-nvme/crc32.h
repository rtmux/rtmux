/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * dev-nvme CRC32 helper
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef DEV_NVME_CRC32_H
#define DEV_NVME_CRC32_H

#include <stddef.h>
#include <stdint.h>

/* CRC32/IEEE 802.3, reflected polynomial 0xEDB88320. This is the variant used
 * by GPT headers and partition-entry arrays.
 */
static inline uint32_t nvme_crc32_ieee(const void *buf, size_t len)
{
    const uint8_t *p = buf;
    uint32_t crc = 0xFFFFFFFFu;

    while (len--) {
        crc ^= *p++;
        for (unsigned bit = 0; bit < 8; bit++)
            crc = (crc & 1) ? (0xEDB88320u ^ (crc >> 1)) : (crc >> 1);
    }

    return crc ^ 0xFFFFFFFFu;
}

#endif /* DEV_NVME_CRC32_H */
