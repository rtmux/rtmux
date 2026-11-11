/**
 * \file cpio.c
 *
 * CPIO newc format archive parser.
 *
 * Adapted from gRouvr hypervisor CPIO parser. Drops EFI types (UINTN ->
 * size_t).
 *
 * \license SPDX-License-Identifier: BSD-3-Clause
 */

#include <stdbool.h>
#include <sys/cpio.h>
#include <string.h>

/* CPIO newc header structure (all fields are 8-byte ASCII hex) */
typedef struct {
    char magic[6];
    char ino[8];
    char mode[8];
    char uid[8];
    char gid[8];
    char nlink[8];
    char mtime[8];
    char filesize[8];
    char devmajor[8];
    char devminor[8];
    char rdevmajor[8];
    char rdevminor[8];
    char namesize[8];
    char check[8];
} __attribute__((packed)) cpio_newc_header_t;

/* Parse 8-character hex string to uint32_t */
static uint32_t parse_hex8(const char *hex, int *ok)
{
    uint32_t val = 0;

    *ok = 1;
    for (int i = 0; i < 8; i++) {
        val <<= 4;
        if (hex[i] >= '0' && hex[i] <= '9')
            val |= (uint32_t) (hex[i] - '0');
        else if (hex[i] >= 'a' && hex[i] <= 'f')
            val |= (uint32_t) (hex[i] - 'a' + 10);
        else if (hex[i] >= 'A' && hex[i] <= 'F')
            val |= (uint32_t) (hex[i] - 'A' + 10);
        else {
            *ok = 0;
            return 0;
        }
    }
    return val;
}

static int add_overflows_size(size_t a, size_t b, size_t *out)
{
    if (b > SIZE_MAX - a)
        return 1;
    *out = a + b;
    return 0;
}

static int align_4(size_t off, size_t *out)
{
    if (add_overflows_size(off, 3, out))
        return 1;
    *out &= ~(size_t) 3;
    return 0;
}

int cpio_check_valid(const uint8_t *data, size_t size)
{
    if (size < sizeof(cpio_newc_header_t))
        return 0;

    const cpio_newc_header_t *hdr = (const cpio_newc_header_t *) data;
    if (memcmp(hdr->magic, CPIO_NEWC_MAGIC, 6) != 0)
        return 0;

    return 1;
}

void cpio_iterate(const uint8_t *data,
                  size_t size,
                  cpio_callback_t callback,
                  void *user_data)
{
    size_t off = 0;

    while (off < size) {
        size_t next;
        if (sizeof(cpio_newc_header_t) > size - off)
            break;

        const cpio_newc_header_t *hdr =
            (const cpio_newc_header_t *) (const void *) (data + off);

        if (memcmp(hdr->magic, CPIO_NEWC_MAGIC, 6) != 0)
            break;

        int ok;
        uint32_t namesize = parse_hex8(hdr->namesize, &ok);
        if (!ok || namesize == 0)
            break;
        uint32_t filesize = parse_hex8(hdr->filesize, &ok);
        if (!ok)
            break;
        uint32_t mode = parse_hex8(hdr->mode, &ok);
        if (!ok)
            break;

        off += sizeof(cpio_newc_header_t);

        if ((size_t) namesize > size - off)
            break;

        const char *filename = (const char *) (const void *) (data + off);
        if (filename[namesize - 1] != '\0' ||
            namesize > sizeof(((cpio_file_info_t *) 0)->filename))
            break;

        off += namesize;
        if (align_4(off, &off) || off > size)
            break;

        /* End of archive marker */
        if (namesize == sizeof("TRAILER!!!") &&
            memcmp(filename, "TRAILER!!!", sizeof("TRAILER!!!")) == 0)
            break;

        if ((size_t) filesize > size - off)
            break;

        cpio_file_info_t file_info;
        strncpy(file_info.filename, filename, sizeof(file_info.filename) - 1);
        file_info.filename[sizeof(file_info.filename) - 1] = '\0';
        file_info.filesize = filesize;
        file_info.mode = mode;
        file_info.data = data + off;

        if (callback)
            callback(&file_info, user_data);

        if (add_overflows_size(off, filesize, &next) || align_4(next, &off) ||
            off > size)
            break;
    }
}

/* Internal context for cpio_find_file */
typedef struct {
    const char *target_name;
    cpio_file_info_t *result;
    bool found;
} cpio_find_ctx_t;

static const char *cpio_normalize_name(const char *name)
{
    /* Strip any interleaved leading "/" and "./" so paths like "/.//foo" and
     * "./foo" both compare equal to "foo". A single pass each would leave a
     * stray separator behind.
     */
    for (;;) {
        if (name[0] == '/')
            name++;
        else if (name[0] == '.' && name[1] == '/')
            name += 2;
        else
            break;
    }
    return name;
}

static void cpio_find_callback(const cpio_file_info_t *file_info,
                               void *user_data)
{
    cpio_find_ctx_t *ctx = (cpio_find_ctx_t *) user_data;

    if (ctx->found)
        return;

    if (strcmp(cpio_normalize_name(file_info->filename), ctx->target_name) ==
        0) {
        *ctx->result = *file_info;
        ctx->found = true;
    }
}

int cpio_find_file(const uint8_t *data,
                   size_t size,
                   const char *filename,
                   cpio_file_info_t *info)
{
    cpio_find_ctx_t ctx;
    ctx.target_name = cpio_normalize_name(filename);
    ctx.result = info;
    ctx.found = false;

    cpio_iterate(data, size, cpio_find_callback, &ctx);

    return ctx.found;
}
