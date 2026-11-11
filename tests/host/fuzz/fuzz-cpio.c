/*
 * Fuzz harness for the initramfs CPIO newc parser.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* cpio_iterate walks an untrusted initramfs archive supplied at boot, so it is
 * a real attacker-reachable parser. It already takes a data and size pair,
 * which is exactly the fuzz input, so the harness just runs it with a callback
 * that ignores every entry.
 */

#include <stddef.h>
#include <stdint.h>

#include <sys/cpio.h>

static void noop_cb(const cpio_file_info_t *file_info, void *user_data)
{
    (void) file_info;
    (void) user_data;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    cpio_iterate(data, size, noop_cb, NULL);
    return 0;
}
