/*
 * Connect-path validation shared by RTMux LKL domain servers.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stddef.h>
#include <string.h>

#include <sys/iomsg.h>

/* Declared in server.h (shared with the LKL servers); kept in sync by hand so
 * this leaf does not pull the heavier dispatch and ioserv headers.
 */
int rtmux_lkl_connect_path_ok(size_t received, const struct _io_connect *c);

/* connect.path_len and its bytes are attacker-controlled; confirm the path is
 * NUL-terminated within what was actually received before any str*() walks it.
 * received is the connect message length in bytes.
 */
int rtmux_lkl_connect_path_ok(size_t received, const struct _io_connect *c)
{
    size_t hdr = offsetof(struct _io_connect, path);
    size_t avail;

    if (received <= hdr)
        return 0;
    avail = received - hdr;
    if (c->path_len < avail)
        avail = c->path_len;

    /* memchr over a zero-length avail returns NULL, so no separate empty check.
     */
    return memchr(c->path, '\0', avail) != NULL;
}
