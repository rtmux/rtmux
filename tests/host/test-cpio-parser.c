/*
 * Host-side malformed CPIO parser tests.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/cpio.h>

struct newc_hdr {
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
} __attribute__((packed));

static void put_hex8(char out[8], uint32_t value)
{
    static const char hex[] = "0123456789abcdef";

    for (int i = 7; i >= 0; i--) {
        out[i] = hex[value & 0xf];
        value >>= 4;
    }
}

static size_t align4(size_t off)
{
    return (off + 3u) & ~(size_t) 3u;
}

static size_t put_entry(uint8_t *buf,
                        size_t off,
                        const char *name,
                        uint32_t namesize,
                        const void *data,
                        uint32_t filesize)
{
    struct newc_hdr *h = (struct newc_hdr *) (void *) (buf + off);

    memset(h, '0', sizeof(*h));
    memcpy(h->magic, CPIO_NEWC_MAGIC, 6);
    put_hex8(h->mode, 0100644);
    put_hex8(h->filesize, filesize);
    put_hex8(h->namesize, namesize);
    off += sizeof(*h);
    memcpy(buf + off, name, namesize);
    off = align4(off + namesize);
    if (data && filesize != 0)
        memcpy(buf + off, data, filesize);
    return align4(off + filesize);
}

static void count_cb(const cpio_file_info_t *info, void *arg)
{
    int *count = arg;

    assert(!strcmp(info->filename, "file"));
    assert(info->filesize == 3);
    assert(memcmp(info->data, "abc", 3) == 0);
    (*count)++;
}

int main(void)
{
    uint8_t buf[512];
    cpio_file_info_t info;
    size_t off;
    int count = 0;

    memset(buf, 0, sizeof(buf));
    off = put_entry(buf, 0, "file", sizeof("file"), "abc", 3);
    off = put_entry(buf, off, "TRAILER!!!", sizeof("TRAILER!!!"), NULL, 0);
    assert(cpio_check_valid(buf, off));
    assert(cpio_find_file(buf, off, "file", &info));
    assert(info.filesize == 3 && memcmp(info.data, "abc", 3) == 0);
    cpio_iterate(buf, off, count_cb, &count);
    assert(count == 1);

    memset(buf, 0, sizeof(buf));
    off = put_entry(buf, 0, "bad!", 4, "x", 1);
    assert(!cpio_find_file(buf, off, "bad", &info));
    count = 0;
    cpio_iterate(buf, off, count_cb, &count);
    assert(count == 0);

    memset(buf, 0, sizeof(buf));
    (void) put_entry(buf, 0, "huge", sizeof("huge"), NULL, 0x1000);
    off = align4(sizeof(struct newc_hdr) + sizeof("huge"));
    assert(!cpio_find_file(buf, off, "huge", &info));

    puts("cpio-parser-test: ok");
    return 0;
}
