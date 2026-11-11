/*
 * Fuzz harness for the NVMe GPT partition-table parser.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* gpt_parse validates a GPT read off an attacker-controllable disk (header,
 * revision, CRCs, entry geometry). It pulls sectors through a read callback, so
 * the harness backs that callback with the fuzz bytes: reads inside the buffer
 * return real bytes, reads past the end return zeroes, and every read reports
 * success so the parser proceeds into the data it is meant to validate.
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/types.h>

#include "gpt.h"

#define FUZZ_LBA_SIZE 512

struct membuf {
    const uint8_t *data;
    size_t size;
};

static ssize_t mem_read(void *ctx, uint64_t lba, void *buf, size_t n_lba)
{
    struct membuf *m = ctx;
    size_t want = n_lba * FUZZ_LBA_SIZE;

    /* A fuzzed GPT header can drive lba arbitrarily high. Guard the byte-offset
     * multiply so it cannot wrap and alias a valid in-buffer offset: an
     * out-of-range disk read just returns the zero-filled buffer and reports
     * success, which is what a real short device would not do but keeps the
     * parser walking its own validation path rather than the harness
     * misfeeding.
     */
    memset(buf, 0, want);
    if (lba <= SIZE_MAX / FUZZ_LBA_SIZE) {
        size_t off = (size_t) lba * FUZZ_LBA_SIZE;

        if (off < m->size) {
            size_t avail = m->size - off;
            memcpy(buf, m->data + off, avail < want ? avail : want);
        }
    }
    return (ssize_t) n_lba;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    struct membuf m = {data, size};
    gpt_table_t tbl;

    if (gpt_parse(mem_read, &m, FUZZ_LBA_SIZE, &tbl) == 0)
        gpt_free(&tbl);
    return 0;
}
