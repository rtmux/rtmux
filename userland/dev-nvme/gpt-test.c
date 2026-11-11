/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * dev-nvme GPT parser host-side self-test
 *
 * Builds a synthetic GPT layout in a memory buffer, hands it to the parser
 * through a memory-backed read callback, and asserts the resulting gpt_table_t
 * matches what we wrote.
 *
 * Also exercises three rejection paths: plain MBR, hybrid GPT, and a GPT with a
 * corrupted header CRC32.
 *
 * Portable C -- compiles with the host gcc and driver-private GPT header.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include "crc32.h"
#include "gpt.h"

#define LBA_SIZE 512
#define N_LBAS 2048
#define ENTRIES_LBA 2
#define ENTRIES 128

static uint8_t disk[N_LBAS * LBA_SIZE];

static ssize_t mem_read(void *ctx, uint64_t lba, void *buf, size_t n)
{
    (void) ctx;
    if (lba + n > N_LBAS)
        return -1;
    memcpy(buf, disk + lba * LBA_SIZE, n * LBA_SIZE);
    return n;
}

/* Two known type GUIDs for test partitions. Stored as raw bytes so we don't
 * depend on host endianness of GUID field rendering.
 */
static const uint8_t TYPE_RTMUX[16] = {
    0x11, 0x4d, 0x54, 0x52, 0x2e, 0x32, 0x17, 0x40,
    0xba, 0xe8, 0xe4, 0xd9, 0xc9, 0xd4, 0xe9, 0x79,
};
static const uint8_t TYPE_LINUXFS[16] = {
    0xaf, 0x3d, 0xc6, 0x0f, 0x83, 0x84, 0x72, 0x47,
    0x8e, 0x79, 0x3d, 0x69, 0xd8, 0x47, 0x7d, 0xe4,
};

/* Write a protective MBR at LBA 0. */
static void write_pmbr(void)
{
    uint8_t *p = disk;
    memset(p, 0, LBA_SIZE);
    /* slot 0: type 0xEE spanning LBAs 1..N-1 */
    p[446 + 4] = 0xEE;

    /* Protective-MBR LBA fields. Spec says set to 0xFFFFFFFF when disk > 2^32
     * LBAs. Small disk: real values.
     */
    uint32_t start = 1, size = N_LBAS - 1;
    memcpy(p + 446 + 8, &start, 4);
    memcpy(p + 446 + 12, &size, 4);
    p[510] = 0x55;
    p[511] = 0xAA;
}

struct hdr {
    uint8_t signature[8];
    uint32_t revision;
    uint32_t header_size;
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

struct ent {
    uint8_t type_guid[16];
    uint8_t part_guid[16];
    uint64_t starting_lba;
    uint64_t ending_lba;
    uint64_t attributes;
    uint16_t name_utf16le[36];
} __attribute__((packed));

/* Write ASCII to a UTF-16LE byte buffer (72 bytes for 36 chars). */
static void put_ascii_name(uint8_t *dst, const char *s)
{
    unsigned i = 0;
    for (; s[i] && i < 36; i++) {
        dst[i * 2] = (uint8_t) s[i];
        dst[i * 2 + 1] = 0;
    }
    for (; i < 36; i++) {
        dst[i * 2] = 0;
        dst[i * 2 + 1] = 0;
    }
}

/* Populate a valid GPT with two partitions.  Returns the header. */
static void write_valid_gpt(void)
{
    memset(disk + LBA_SIZE, 0, (N_LBAS - 1) * LBA_SIZE);
    write_pmbr();

    /* Build entry array at LBA 2 (entries 0..1 populated). */
    struct ent *ea = (struct ent *) (disk + ENTRIES_LBA * LBA_SIZE);

    uint8_t *ea_bytes = (uint8_t *) ea;

    memcpy(ea[0].type_guid, TYPE_RTMUX, 16);
    memset(ea[0].part_guid, 0xAA, 16);
    ea[0].starting_lba = 64;
    ea[0].ending_lba = 1023;
    ea[0].attributes = 0;
    put_ascii_name(ea_bytes + 0 * sizeof(struct ent) + 56, "rtmux-root");

    memcpy(ea[1].type_guid, TYPE_LINUXFS, 16);
    memset(ea[1].part_guid, 0xBB, 16);
    ea[1].starting_lba = 1024;
    ea[1].ending_lba = 2014;
    ea[1].attributes = 0;
    put_ascii_name(ea_bytes + 1 * sizeof(struct ent) + 56, "linux-home");

    uint32_t entries_bytes = ENTRIES * sizeof(struct ent);
    uint32_t entries_crc = nvme_crc32_ieee(ea, entries_bytes);

    /* Build header at LBA 1. */
    struct hdr h;
    memset(&h, 0, sizeof(h));
    memcpy(h.signature, "EFI PART", 8);
    h.revision = 0x00010000;
    h.header_size = 92;
    h.reserved = 0;
    h.current_lba = 1;
    h.backup_lba = N_LBAS - 1;
    h.first_usable_lba = 34;
    h.last_usable_lba = N_LBAS - 34;
    memset(h.disk_guid, 0x55, 16);
    h.entries_lba = ENTRIES_LBA;
    h.num_entries = ENTRIES;
    h.entry_size = sizeof(struct ent);
    h.entries_crc32 = entries_crc;
    h.header_crc32 = nvme_crc32_ieee(&h, h.header_size);

    memcpy(disk + LBA_SIZE, &h, sizeof(h));
}

/* test cases */

static int test_valid(void)
{
    gpt_table_t tbl;

    write_valid_gpt();

    int rc = gpt_parse(mem_read, NULL, LBA_SIZE, &tbl);
    if (rc != GPT_OK) {
        fprintf(stderr, "test_valid: gpt_parse rc=%d\n", rc);
        return 1;
    }
    if (tbl.n_parts != 2) {
        fprintf(stderr, "test_valid: n_parts=%u, expected 2\n", tbl.n_parts);
        return 1;
    }
    if (memcmp(tbl.parts[0].type_guid, TYPE_RTMUX, 16) != 0) {
        fprintf(stderr, "test_valid: p0 type_guid mismatch\n");
        return 1;
    }
    if (tbl.parts[0].start_lba != 64 || tbl.parts[0].end_lba != 1023) {
        fprintf(stderr, "test_valid: p0 lba range wrong\n");
        return 1;
    }
    if (strcmp(tbl.parts[0].name, "rtmux-root") != 0) {
        fprintf(stderr, "test_valid: p0 name=\"%s\"\n", tbl.parts[0].name);
        return 1;
    }
    if (memcmp(tbl.parts[1].type_guid, TYPE_LINUXFS, 16) != 0) {
        fprintf(stderr, "test_valid: p1 type_guid mismatch\n");
        return 1;
    }
    if (strcmp(tbl.parts[1].name, "linux-home") != 0) {
        fprintf(stderr, "test_valid: p1 name=\"%s\"\n", tbl.parts[1].name);
        return 1;
    }
    gpt_free(&tbl);
    return 0;
}

static int test_plain_mbr(void)
{
    gpt_table_t tbl;

    write_valid_gpt();
    /* Overwrite PMBR slot 0 type with linux (0x83) — no longer 0xEE. */
    disk[446 + 4] = 0x83;

    int rc = gpt_parse(mem_read, NULL, LBA_SIZE, &tbl);
    if (rc != GPT_ERR_HYBRID_MBR) {
        fprintf(stderr, "test_plain_mbr: rc=%d, expected HYBRID\n", rc);
        return 1;
    }
    return 0;
}

static int test_hybrid(void)
{
    gpt_table_t tbl;

    write_valid_gpt();
    /* Add a second non-zero slot alongside the 0xEE — classic hybrid. */
    disk[446 + 16 + 4] = 0x07; /* slot 1 type = NTFS */

    int rc = gpt_parse(mem_read, NULL, LBA_SIZE, &tbl);
    if (rc != GPT_ERR_HYBRID_MBR) {
        fprintf(stderr, "test_hybrid: rc=%d, expected HYBRID\n", rc);
        return 1;
    }
    return 0;
}

static int test_bad_header_crc(void)
{
    gpt_table_t tbl;

    write_valid_gpt();
    /* Flip a bit inside the header area — CRC no longer matches. */
    disk[LBA_SIZE + 40] ^= 0x01;

    int rc = gpt_parse(mem_read, NULL, LBA_SIZE, &tbl);
    if (rc != GPT_ERR_BAD_HDR_CRC) {
        fprintf(stderr, "test_bad_header_crc: rc=%d\n", rc);
        return 1;
    }
    return 0;
}

static int test_bad_entries_crc(void)
{
    gpt_table_t tbl;

    write_valid_gpt();
    /* Flip a byte in the entries array (after the header's CRC seal). */
    disk[ENTRIES_LBA * LBA_SIZE + 100] ^= 0x80;

    int rc = gpt_parse(mem_read, NULL, LBA_SIZE, &tbl);
    if (rc != GPT_ERR_BAD_ENTRY_CRC) {
        fprintf(stderr, "test_bad_entries_crc: rc=%d\n", rc);
        return 1;
    }
    return 0;
}

int main(void)
{
    int failed = 0;

    failed += test_valid();
    failed += test_plain_mbr();
    failed += test_hybrid();
    failed += test_bad_header_crc();
    failed += test_bad_entries_crc();

    if (failed) {
        fprintf(stderr, "dev-nvme-gpt: %d test(s) FAILED\n", failed);
        return 1;
    }
    printf("dev-nvme-gpt: all tests passed\n");
    return 0;
}
