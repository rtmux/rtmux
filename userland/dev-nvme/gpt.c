/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * dev-nvme GPT parser
 *
 * Reads the primary GPT at LBA 1 only (no backup-header fallback), verifies the
 * protective MBR, the header signature/revision, and both CRC32s, then extracts
 * populated partition entries.
 *
 * GPT only. A plain MBR or hybrid GPT (MBR carrying partitions other than a
 * single 0xEE protective entry) is rejected outright.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdlib.h>
#include <string.h>
#include "crc32.h"
#include "gpt.h"

/* On-disk GPT header (UEFI 2.x). All fields little-endian. We work on a raw
 * buffer using this layout plus static_asserts so we don't accidentally depend
 * on host alignment or packing rules.
 */
struct gpt_header_raw {
    uint8_t signature[8]; /* "EFI PART" */
    uint32_t revision;    /* 0x00010000 for v1.0 */
    uint32_t header_size; /* usually 92 */
    uint32_t header_crc32;
    uint32_t reserved;
    uint64_t current_lba;
    uint64_t backup_lba;
    uint64_t first_usable_lba;
    uint64_t last_usable_lba;
    uint8_t disk_guid[16];
    uint64_t entries_lba;
    uint32_t num_entries;
    uint32_t entry_size;
    uint32_t entries_crc32;
} __attribute__((packed));

_Static_assert(sizeof(struct gpt_header_raw) == 92, "gpt_header size");

/* On-disk GPT partition entry. Fields up to/including attributes are 56 bytes;
 * the remaining 72 bytes hold the UTF-16LE name (36 chars).
 */
struct gpt_entry_raw {
    uint8_t type_guid[16];
    uint8_t part_guid[16];
    uint64_t starting_lba;
    uint64_t ending_lba;
    uint64_t attributes;
    uint16_t name_utf16le[36];
} __attribute__((packed));

_Static_assert(sizeof(struct gpt_entry_raw) == 128, "gpt_entry size");

/* Signature bytes spelled out to avoid endian assumptions on the literal. */
static const uint8_t gpt_signature[8] = {
    [0] = 'E', [1] = 'F', [2] = 'I', [3] = ' ',
    [4] = 'P', [5] = 'A', [6] = 'R', [7] = 'T',
};

/* Sane bounds for entry count × size so we never allocate absurd buffers. */
#define GPT_MAX_ENTRIES 128
#define GPT_MAX_ENTRY_BYTES (GPT_MAX_ENTRIES * 128u)

/* Decode a UCS-2 / UTF-16LE partition name to UTF-8. Takes a raw byte pointer
 * (rather than uint16_t*) so the caller doesn't have to worry about alignment
 * coming out of a packed on-disk entry. GPT spec allows any Unicode; we accept
 * ASCII and replace non-ASCII with '?'.
 */
static void decode_name(const uint8_t *utf16le, char *utf8)
{
    for (unsigned i = 0; i < 36; i++) {
        uint16_t c =
            (uint16_t) utf16le[i * 2] | ((uint16_t) utf16le[i * 2 + 1] << 8);
        if (c == 0) {
            utf8[i] = '\0';
            return;
        }
        utf8[i] = (c < 0x80) ? (char) c : '?';
    }
    utf8[36] = '\0';
}

/* Parse and validate the protective MBR at LBA 0. Spec: exactly one partition
 * of type 0xEE spanning (almost) the whole disk; the other three entries
 * zeroed. A plain MBR or a hybrid GPT (which has non-EE entries alongside the
 * 0xEE) is rejected.
 */
static int check_pmbr(const uint8_t *lba0)
{
    /* Boot signature must be 0x55 0xAA at offset 510. */
    if (lba0[510] != 0x55 || lba0[511] != 0xAA)
        return GPT_ERR_NO_PMBR;

    int saw_ee = 0;
    for (unsigned slot = 0; slot < 4; slot++) {
        const uint8_t *e = &lba0[446 + slot * 16];
        uint8_t type = e[4];

        if (type == 0x00)
            continue;
        if (type == 0xEE) {
            saw_ee = 1;
            continue;
        }
        /* Anything else means plain MBR or hybrid — refuse. */
        return GPT_ERR_HYBRID_MBR;
    }
    return saw_ee ? GPT_OK : GPT_ERR_NO_PMBR;
}

/* Parse and validate the primary GPT and extract its partitions.
 *
 * Reads the protective MBR, the GPT header, and the partition-entry array via
 * the caller's read callback; verifies signatures, revision, geometry, and both
 * CRC32s; then copies populated entries into <out>. Parameters:
 *   read_fn  : LBA read callback (returns LBAs read, negative on error)
 *   ctx      : opaque context passed to read_fn
 *   lba_size : logical block size in bytes (512..4096, power of two)
 *   out      : table to populate (zeroed on entry; parts must be freed
 *              with gpt_free on GPT_OK)
 * Return value: GPT_OK on success, or a GPT_ERR_* code on failure.
 */
int gpt_parse(gpt_read_fn_t read_fn,
              void *ctx,
              uint32_t lba_size,
              gpt_table_t *out)
{
    uint8_t *lba_buf = NULL;
    uint8_t *entries_buf = NULL;
    gpt_part_t *parts = NULL;
    int rc;

    memset(out, 0, sizeof(*out));

    if (lba_size < 512 || lba_size > 4096 || (lba_size & (lba_size - 1)) != 0)
        return GPT_ERR_BAD_GEOMETRY;

    /* One scratch LBA buffer — reused for LBA 0 (PMBR) and LBA 1 (header). */
    lba_buf = malloc(lba_size);
    if (!lba_buf) {
        rc = GPT_ERR_NOMEM;
        goto fail;
    }

    /* Protective MBR. */
    if (read_fn(ctx, 0, lba_buf, 1) != 1) {
        rc = GPT_ERR_READ;
        goto fail;
    }
    rc = check_pmbr(lba_buf);
    if (rc != GPT_OK)
        goto fail;

    /* Primary GPT header. */
    if (read_fn(ctx, 1, lba_buf, 1) != 1) {
        rc = GPT_ERR_READ;
        goto fail;
    }

    struct gpt_header_raw hdr;
    memcpy(&hdr, lba_buf, sizeof(hdr));

    if (memcmp(hdr.signature, gpt_signature, 8) != 0) {
        rc = GPT_ERR_BAD_SIG;
        goto fail;
    }
    if (hdr.revision != 0x00010000u) {
        rc = GPT_ERR_BAD_REV;
        goto fail;
    }
    if (hdr.header_size < sizeof(hdr) || hdr.header_size > lba_size) {
        rc = GPT_ERR_BAD_GEOMETRY;
        goto fail;
    }

    /* Header CRC: copy the claimed header_size bytes from the LBA, zero the crc
     * field at offset 16, recompute, compare.
     */
    uint8_t hdr_copy[512];
    if (hdr.header_size > sizeof(hdr_copy)) {
        rc = GPT_ERR_BAD_GEOMETRY;
        goto fail;
    }
    memcpy(hdr_copy, lba_buf, hdr.header_size);
    memset(hdr_copy + 16, 0, 4);
    if (nvme_crc32_ieee(hdr_copy, hdr.header_size) != hdr.header_crc32) {
        rc = GPT_ERR_BAD_HDR_CRC;
        goto fail;
    }

    if (hdr.num_entries == 0 || hdr.num_entries > GPT_MAX_ENTRIES ||
        hdr.entry_size < sizeof(struct gpt_entry_raw) || hdr.entry_size > 512 ||
        (hdr.entry_size & (hdr.entry_size - 1)) != 0 ||
        (uint64_t) hdr.num_entries * hdr.entry_size > GPT_MAX_ENTRY_BYTES) {
        rc = GPT_ERR_BAD_GEOMETRY;
        goto fail;
    }

    /* Partition entry array. */
    uint32_t entries_bytes = hdr.num_entries * hdr.entry_size;
    uint32_t entries_lbas = (entries_bytes + lba_size - 1) / lba_size;

    entries_buf = malloc((size_t) entries_lbas * lba_size);
    if (!entries_buf) {
        rc = GPT_ERR_NOMEM;
        goto fail;
    }
    if (read_fn(ctx, hdr.entries_lba, entries_buf, entries_lbas) !=
        (ssize_t) entries_lbas) {
        rc = GPT_ERR_READ;
        goto fail;
    }
    if (nvme_crc32_ieee(entries_buf, entries_bytes) != hdr.entries_crc32) {
        rc = GPT_ERR_BAD_ENTRY_CRC;
        goto fail;
    }

    /* Count populated entries (non-zero type GUID). */
    unsigned count = 0;
    for (unsigned i = 0; i < hdr.num_entries; i++) {
        const uint8_t *raw = entries_buf + (size_t) i * hdr.entry_size;
        int used = 0;
        for (unsigned b = 0; b < 16; b++)
            if (raw[b]) {
                used = 1;
                break;
            }
        if (used)
            count++;
    }

    if (count) {
        parts = calloc(count, sizeof(*parts));
        if (!parts) {
            rc = GPT_ERR_NOMEM;
            goto fail;
        }

        unsigned j = 0;
        for (unsigned i = 0; i < hdr.num_entries; i++) {
            const uint8_t *raw = entries_buf + (size_t) i * hdr.entry_size;
            struct gpt_entry_raw e;
            int used = 0;

            for (unsigned b = 0; b < 16; b++)
                if (raw[b]) {
                    used = 1;
                    break;
                }
            if (!used)
                continue;

            memcpy(&e, raw, sizeof(e));
            memcpy(parts[j].type_guid, e.type_guid, 16);
            memcpy(parts[j].part_guid, e.part_guid, 16);
            parts[j].start_lba = e.starting_lba;
            parts[j].end_lba = e.ending_lba;
            parts[j].attrs = e.attributes;
            decode_name(raw + 56, parts[j].name);
            j++;
        }
    }

    memcpy(out->disk_guid, hdr.disk_guid, 16);
    out->lba_size = lba_size;
    out->first_usable_lba = hdr.first_usable_lba;
    out->last_usable_lba = hdr.last_usable_lba;
    out->n_parts = count;
    out->parts = parts;

    free(lba_buf);
    free(entries_buf);
    return GPT_OK;

fail:
    free(lba_buf);
    free(entries_buf);
    free(parts);
    return rc;
}

/* Release the partition array attached to a parsed table.
 *
 * Parameters:
 *   tbl : table previously filled by gpt_parse (NULL is tolerated)
 */
void gpt_free(gpt_table_t *tbl)
{
    if (!tbl)
        return;
    free(tbl->parts);
    tbl->parts = NULL;
    tbl->n_parts = 0;
}
