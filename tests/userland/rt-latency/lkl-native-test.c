/*
 * lkl-native-test - native-capability regressions for LKL filesystem opens.
 *
 * Runs inside the lkl-fsd-tree scenario after ext4 is mounted at /fs/root. It
 * opens files through roottask and drives the RTMux native file-message subset
 * (_IOMSG_RTMUX_NATIVE / _IO_NATIVE_*) straight against lkl-fsd's returned
 * channel, asserting that native STAT/READ/WRITE honor the same
 * open-control-block authority as ordinary I/O and that every malformed request
 * fails closed. Prints one PASS/FAIL line and exits non-zero on any failure so
 * the init chain gates on it.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/iomsg.h>
#include <sys/rtmux-core.h>
#include <sys/stat.h>
#include <unistd.h>

#define RO_PATH "/fs/root/hello.txt"
#define RW_PATH "/fs/root/native-test.txt"

static int fails;

static void check(int ok, const char *name)
{
    if (ok) {
        printf("  ok   %s\n", name);
    } else {
        printf("  FAIL %s (errno=%d)\n", name, errno);
        fails++;
    }
}

static void init_native_hdr(struct _io_native_hdr *hdr, uint16_t subtype)
{
    memset(hdr, 0, sizeof(*hdr));
    hdr->msg.type = _IO_MSG;
    hdr->msg.combine_len = sizeof(*hdr);
    hdr->msg.protocol = _IOMSG_RTMUX_NATIVE;
    hdr->msg.subtype = subtype;
    hdr->version = _IO_NATIVE_VERSION;
}

/* Zero a whole native request, stamp its header, and set combine_len to the
 * full message size. init_native_hdr() clears only the header, but fid/offset/
 * flags ride over IPC too and must not carry stack junk, so the memset spans
 * the entire object. Each check then tweaks only the fields it exercises.
 */
static void reset_native_msg(void *msg, uint16_t len, uint16_t subtype)
{
    memset(msg, 0, len);
    init_native_hdr(msg, subtype);
    ((struct _io_native_hdr *) msg)->msg.combine_len = len;
}

/* A native WRITE carries its payload inline after the fixed header. lkl-fsd
 * validates nbytes against `ctp->size - sizeof(*wr)`, so send sizeof(*wr) + n
 * bytes: that guarantees exactly n bytes of headroom past the struct, which the
 * server accepts (the struct's own inline data[] bytes are extra slack).
 */
static int native_write(int fd, uint64_t offset, const void *data, uint32_t n)
{
    unsigned char buf[sizeof(struct _io_native_write) + 256];
    struct _io_native_write *wr = (struct _io_native_write *) (void *) buf;
    size_t total = sizeof(*wr) + n;

    if (n > sizeof(buf) - sizeof(*wr)) {
        errno = EINVAL;
        return -1;
    }
    init_native_hdr(&wr->hdr, _IO_NATIVE_WRITE);
    wr->hdr.msg.combine_len = (uint16_t) total;
    wr->offset = offset;
    wr->nbytes = n;
    memcpy(wr->data, data, n);
    return msg_send(fd, buf, total, NULL, 0);
}

int main(void)
{
    struct _io_native_stat stmsg;
    struct _io_native_read rdmsg;
    struct stat st;
    char buf[128];
    int fd;
    int ret;

    printf("lkl-native-test: BEGIN\n");

    /* 1. native STAT on a read-only regular file. */
    fd = open(RO_PATH, O_RDONLY);
    if (fd < 0) {
        printf("  FAIL open %s (errno=%d)\nlkl-native-test: FAIL\n", RO_PATH,
               errno);
        return 1;
    }
    reset_native_msg(&stmsg, sizeof(stmsg), _IO_NATIVE_STAT);
    ret = msg_send(fd, &stmsg, sizeof(stmsg), &st, sizeof(st));
    check(ret == EOK && S_ISREG(st.st_mode) && st.st_size > 0,
          "native STAT describes the regular file");

    /* 2. native READ returns the file bytes without a path lookup. */
    memset(buf, 0, sizeof(buf));
    reset_native_msg(&rdmsg, sizeof(rdmsg), _IO_NATIVE_READ);
    rdmsg.offset = 0;
    rdmsg.nbytes = sizeof(buf) - 1;
    ret = msg_send(fd, &rdmsg, sizeof(rdmsg), buf, sizeof(buf) - 1);
    check(ret == EOK && strstr(buf, "hello from lkl-fsd") != NULL,
          "native READ returns file content");

    /* 3. native WRITE on an O_RDONLY handle is refused (fd authority, not
     * path): the open control block carries no write right.
     */
    errno = 0;
    ret = native_write(fd, 0, "x", 1);
    check(ret == -1 && errno == EBADF, "native WRITE on O_RDONLY rejected");

    /* 4. unsupported protocol version fails closed. */
    reset_native_msg(&rdmsg, sizeof(rdmsg), _IO_NATIVE_READ);
    rdmsg.hdr.version = _IO_NATIVE_VERSION + 1;
    rdmsg.nbytes = sizeof(buf) - 1;
    errno = 0;
    ret = msg_send(fd, &rdmsg, sizeof(rdmsg), buf, sizeof(buf) - 1);
    check(ret == -1 && errno == EINVAL, "native bad version rejected");

    /* 5. oversized transfer metadata fails closed (E2BIG, rejected by the
     * kernel native-cap prep before delivery; no integer overflow).
     */
    reset_native_msg(&rdmsg, sizeof(rdmsg), _IO_NATIVE_READ);
    rdmsg.hdr.nxfers = _IO_NATIVE_CAP_TRANSFER_MAX + 1;
    errno = 0;
    ret = msg_send(fd, &rdmsg, sizeof(rdmsg), buf, sizeof(buf) - 1);
    check(ret == -1 && errno == E2BIG, "native oversized nxfers rejected");

    /* 6. a native message shorter than the request struct fails closed. */
    reset_native_msg(&rdmsg, sizeof(rdmsg), _IO_NATIVE_STAT);
    rdmsg.hdr.msg.combine_len = sizeof(struct _io_native_hdr);
    errno = 0;
    ret = msg_send(fd, &rdmsg, sizeof(struct _io_native_hdr), &st, sizeof(st));
    check(ret == -1 && errno == EINVAL, "native short STAT payload rejected");

    /* 7. stale handle: a closed fd no longer authorizes native messages. */
    close(fd);
    reset_native_msg(&stmsg, sizeof(stmsg), _IO_NATIVE_STAT);
    errno = 0;
    ret = msg_send(fd, &stmsg, sizeof(stmsg), &st, sizeof(st));
    check(ret == -1 && errno == EBADF, "native message on closed fd rejected");

    /* 8. writable open: native WRITE succeeds and refreshes the stat size. */
    fd = open(RW_PATH, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        printf("  FAIL open %s (errno=%d)\nlkl-native-test: FAIL\n", RW_PATH,
               errno);
        return 1;
    }
    /* A successful native WRITE replies with the byte count (the
     * _IO_SET_WRITE_NBYTES convention), not EOK.
     */
    ret = native_write(fd, 0, "hello", 5);
    check(ret == 5, "native WRITE on O_WRONLY accepted");
    reset_native_msg(&stmsg, sizeof(stmsg), _IO_NATIVE_STAT);
    ret = msg_send(fd, &stmsg, sizeof(stmsg), &st, sizeof(st));
    check(ret == EOK && st.st_size == 5,
          "native WRITE refreshes the stat size");

    /* 9. oversized write nbytes (larger than the delivered body) fails closed
     * without reading past the message.
     */
    {
        struct _io_native_write wr;
        reset_native_msg(&wr, sizeof(wr), _IO_NATIVE_WRITE);
        wr.offset = 0;
        wr.nbytes = 0xffffffffU;
        errno = 0;
        ret = msg_send(fd, &wr, sizeof(wr), NULL, 0);
        check(ret == -1 && errno == EINVAL, "native oversized WRITE rejected");
    }

    close(fd);
    (void) unlink(RW_PATH);

    if (fails == 0) {
        printf("lkl-native-test: PASS\n");
        return 0;
    }
    printf("lkl-native-test: FAIL (%d)\n", fails);
    return 1;
}
