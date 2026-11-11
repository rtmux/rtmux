/*
 * Fuzz harness for the LKL server connect-path validator
 * rtmux_lkl_connect_path_ok.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* rtmux_lkl_connect_path_ok (servers/lkl/connectpath.c) is the guard that a
 * client connect path is NUL-terminated within the bytes actually received,
 * before the server runs str*() over it. Both the received length and the
 * message path_len are attacker-controlled, so the harness overlays the fuzz
 * bytes on a struct _io_connect and passes the buffer length as the received
 * size. The bytes are copied into a uint32-aligned buffer so the path_len read
 * lands on an aligned address as a real received message would.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <sys/iomsg.h>

int rtmux_lkl_connect_path_ok(size_t received, const struct _io_connect *c);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    static volatile int sink;
    void *buf = malloc(size != 0 ? size : 1);

    /* An exactly sized allocation (malloc is aligned for the path_len read)
     * keeps the sanitizer bounds tight, so trusting path_len past the received
     * length reads off the end rather than into slack.
     */
    if (!buf)
        return 0;
    memcpy(buf, data, size);
    sink = rtmux_lkl_connect_path_ok(size, (const struct _io_connect *) buf);
    free(buf);
    return 0;
}
