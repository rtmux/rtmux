/*
 * VFS transport tests.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <dirent.h>
#include <grp.h>
#include <limits.h>
#include <mqueue.h>
#include <pwd.h>
#include <devctl.h>
#include <devctl/proc.h>
#include <fcntl.h>
#include <semaphore.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <spawn.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/dispatch.h>
#include <sys/ftype.h>
#include <sys/ioctl.h>
#include <sys/ioserv.h>
#include <sys/iomsg.h>
#include <sys/mman.h>
#include <sys/path-registry.h>
#include <sys/poll.h>
#include <sys/procfs.h>
#include <sys/states.h>
#include <sys/procmsg.h>
#include <sys/select.h>
#include <sys/resource.h>
#include <sys/rtmux-core.h>
#include <sys/statvfs.h>
#include <sys/syspage.h>
#include <sys/sysmsg.h>
#include <sys/times.h>
#include <sys/utsname.h>
#include <sys/wait.h>

#include "test.h"

static bool wait_child_exit(pid_t pid, int *status, int loops)
{
    struct timespec delay = {.tv_nsec = 10000000L};

    for (int i = 0; i < loops; i++) {
        int rc = waitpid(pid, status, WNOHANG);

        if (rc == pid)
            return true;
        if (rc == -1)
            return false;
        (void) nanosleep(&delay, NULL);
    }
    (void) kill(pid, SIGKILL);
    for (int i = 0; i < 100; i++) {
        int rc = waitpid(pid, status, WNOHANG);

        if (rc == pid || rc == -1)
            break;
        (void) nanosleep(&delay, NULL);
    }
    return false;
}

static void test_pipe_read_write_transport(void)
{
    int fd[2] = {
        [0] = -1,
        [1] = -1,
    };
    const char msg[] = "rtmux-vfs";
    char buf[sizeof(msg)] = {0};
    int pending = -1;
    ssize_t n;

    if (pipe(fd) == -1) {
        test_fail("pipe transport setup", strerror(errno));
        return;
    }

    n = write(fd[1], msg, sizeof(msg));
    check(n == (ssize_t) sizeof(msg), "pipe write transport",
          "write must use existing connection transport");
    check(ioctl(fd[0], FIONREAD, &pending) == 0 && pending == (int) sizeof(msg),
          "ioctl FIONREAD pipe bytes",
          "ioctl must delegate supported byte-count queries to devctl");

    n = read(fd[0], buf, sizeof(buf));
    check(n == (ssize_t) sizeof(buf) && memcmp(buf, msg, sizeof(msg)) == 0,
          "pipe read transport", "read must return bytes from existing pipe");

    (void) close(fd[0]);
    (void) close(fd[1]);
}

static void test_native_combined_io(void)
{
    const char path[] = "/dev/shmem/rtmux-combined-io";
    struct {
        struct _io_write write;
        unsigned char data[4];
        struct _io_stat stat;
    } packet = {0};
    struct stat st;
    int fd;

    (void) unlink(path);
    fd = open(path, O_CREAT | O_EXCL | O_RDWR, 0600);
    if (fd == -1) {
        test_fail("native combined fixture", strerror(errno));
        return;
    }
    packet.write.type = _IO_WRITE;
    packet.write.combine_len =
        (uint16_t) offsetof(__typeof__(packet), stat) | _IO_COMBINE_FLAG;
    packet.write.nbytes = 12;
    packet.stat.type = _IO_STAT;
    packet.stat.combine_len = sizeof packet.stat;
    errno = 0;
    check(msg_send(fd, &packet, sizeof packet, &st, sizeof st) == -1 &&
              errno == EBADMSG,
          "native write cannot consume next frame",
          "a write must not use the following stat header as payload");
    check(fstat(fd, &st) == 0 && st.st_size == 0,
          "native rejects oversized frame before resize",
          "invalid payload bounds must not change file size");

    packet.write.nbytes = 0;
    packet.write.combine_len = _IO_COMBINE_FLAG;
    errno = 0;
    check(msg_send(fd, &packet, sizeof packet, &st, sizeof st) == -1 &&
              errno == EBADMSG,
          "native rejects zero frame length", "frame traversal must progress");
    packet.write.combine_len = 0x7fff | _IO_COMBINE_FLAG;
    errno = 0;
    check(msg_send(fd, &packet, sizeof packet, &st, sizeof st) == -1 &&
              errno == EBADMSG,
          "native rejects frame beyond message", "do not silently clip a frame");
    packet.write.combine_len = sizeof packet.write | _IO_COMBINE_FLAG;
    packet.write.xtype = _IO_XTYPE_OFFSET;
    errno = 0;
    check(msg_send(fd, &packet, sizeof packet, &st, sizeof st) == -1 &&
              errno == EBADMSG,
          "native offset cannot consume next frame",
          "the positioned offset must fit the current frame");
    errno = 0;
    check(msg_send(fd, &packet, sizeof(uint16_t), NULL, 0) == -1 &&
              errno == EBADMSG,
          "native rejects truncated common header", "need type and length");

    struct {
        struct _io_write first;
        unsigned char a[4];
        struct _io_write second;
        unsigned char b[4];
        struct _io_read read;
        struct _xtype_offset offset;
    } chain = {0};
    unsigned char received[8] = {0};
    const unsigned char expected[8] = {1, 2, 3, 4, 5, 6, 7, 8};

    struct {
        struct _io_write write;
        unsigned char data[4];
        unsigned char padding[4];
        struct _io_stat stat;
    } padded = {0};
    padded.write.type = _IO_WRITE;
    padded.write.combine_len =
        (uint16_t) offsetof(__typeof__(padded), stat) | _IO_COMBINE_FLAG;
    padded.write.nbytes = sizeof padded.data;
    memcpy(padded.data, expected, sizeof padded.data);
    memset(padded.padding, 0xcc, sizeof padded.padding);
    padded.stat.type = _IO_STAT;
    padded.stat.combine_len = sizeof padded.stat;
    check(msg_send(fd, &padded, sizeof padded, &st, sizeof st) == 0 &&
              st.st_size == 4 && pread(fd, received, 4, 0) == 4 &&
              memcmp(received, expected, 4) == 0,
          "native combined padding is not payload",
          "write only nbytes while skipping the full padded frame");
    check(lseek(fd, 0, SEEK_SET) == 0, "native combined rewind", "reset position");

    chain.first.type = chain.second.type = _IO_WRITE;
    chain.first.combine_len =
        (uint16_t) offsetof(__typeof__(chain), second) | _IO_COMBINE_FLAG;
    chain.second.combine_len =
        (uint16_t) (offsetof(__typeof__(chain), read) -
                     offsetof(__typeof__(chain), second)) | _IO_COMBINE_FLAG;
    chain.first.nbytes = chain.second.nbytes = 4;
    memcpy(chain.a, expected, 4);
    memcpy(chain.b, expected + 4, 4);
    chain.read.type = _IO_READ;
    chain.read.combine_len = sizeof chain.read + sizeof chain.offset;
    chain.read.nbytes = sizeof received;
    chain.read.xtype = _IO_XTYPE_OFFSET;
    check(msg_send(fd, &chain, sizeof chain, received, sizeof received) == 8 &&
              memcmp(received, expected, sizeof received) == 0,
          "native two writes and terminal read",
          "valid combined writes must use their own buffered payloads");
    check(lseek(fd, 0, SEEK_CUR) == 8 && fstat(fd, &st) == 0 && st.st_size == 8,
          "native combined positions and size", "positioned read must not seek");

    chain.second.nbytes = 12;
    errno = 0;
    check(msg_send(fd, &chain, sizeof chain, received, sizeof received) == -1 &&
              errno == EBADMSG,
          "native stops at invalid second frame", "reject before executing it");
    check(fstat(fd, &st) == 0 && st.st_size == 12 &&
              pread(fd, received, 4, 8) == 4 && memcmp(received, expected, 4) == 0,
          "native retains only earlier completed writes",
          "stop on error without rolling back the first frame");

    _Alignas(8) unsigned char unaligned[18 + sizeof(struct _io_read)] = {0};
    struct _io_write first = {0};
    struct _io_read last = {0};
    first.type = _IO_WRITE;
    first.combine_len = 18 | _IO_COMBINE_FLAG;
    last.type = _IO_READ;
    last.combine_len = sizeof last;
    memcpy(unaligned, &first, sizeof first);
    memcpy(unaligned + 18, &last, sizeof last);
    errno = 0;
    check(msg_send(fd, unaligned, sizeof unaligned, NULL, 0) == -1 &&
              errno == EBADMSG,
          "native rejects misaligned read header", "read fields need LP64 alignment");
    (void) close(fd);
    check(unlink(path) == 0, "native combined cleanup", "unlink fixture");

}

/* A write grows the file before copying the client's data. When the copy
 * faults, the file must be left as it was: same size, same bytes, same
 * position, and no bytes from the failed copy past the old end.
 */
static void test_native_failed_write_no_growth(void)
{
    const char path[] = "/dev/shmem/rtmux-failed-write";
    const size_t page = (size_t) __PAGESIZE;
    unsigned char *buf = MAP_FAILED, tail[64];
    struct stat st;
    int fd = -1, zero = 1;

    (void) unlink(path);
    fd = open(path, O_CREAT | O_EXCL | O_RDWR, 0600);
    if (fd == -1) {
        test_fail("native failed write fixture", strerror(errno));
        return;
    }
    check(write(fd, "abcd", 4) == 4, "native failed write fixture data",
          "4 bytes");

    /* Two pages, the second unmapped: the IPC prefix copies, the server's
     * msgreadv faults on the second page after the file has grown.
     */
    buf = mmap(NULL, 2 * page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON,
               NOFD, 0);
    if (buf == MAP_FAILED) {
        test_fail("native failed write buffer", strerror(errno));
        goto done;
    }
    memset(buf, 'x', page);
    if (munmap(buf + page, page) != 0) {
        test_fail("native failed write unmap", strerror(errno));
        goto done;
    }

    errno = 0;
    check(write(fd, buf, 2 * page) == -1 && errno == EFAULT,
          "native write from faulting buffer fails", "expected EFAULT");
    check(fstat(fd, &st) == 0 && st.st_size == 4,
          "native failed write leaves size unchanged",
          "growth must be undone when the copy fails");
    check(lseek(fd, 0, SEEK_CUR) == 4,
          "native failed write leaves position unchanged", "expected 4");

    errno = 0;
    check(
        pwrite(fd, buf, 2 * page, (off_t) (16 * page)) == -1 && errno == EFAULT,
        "native pwrite past end from faulting buffer fails", "expected EFAULT");
    check(fstat(fd, &st) == 0 && st.st_size == 4,
          "native failed pwrite leaves size unchanged",
          "growth must be undone when the copy fails");

    /* Grow again: the bytes the faulted copy wrote past the old end must be
     * gone, so the gap reads as zeros.
     */
    check(pwrite(fd, "z", 1, (off_t) sizeof tail + 4) == 1,
          "native regrow after failed write", "1 byte");
    if (pread(fd, tail, sizeof tail, 4) != (ssize_t) sizeof tail) {
        zero = 0;
    } else {
        for (size_t i = 0; i < sizeof tail; i++)
            zero &= tail[i] == 0;
    }
    check(zero, "native failed write leaves no bytes past old end",
          "the gap after regrowth must read as zeros");
    {
        char head[4];

        check(pread(fd, head, sizeof head, 0) == 4 &&
                  memcmp(head, "abcd", 4) == 0,
              "native failed write keeps existing data", "expected abcd");
    }
done:
    if (buf != MAP_FAILED)
        (void) munmap(buf, page);
    (void) close(fd);
    (void) unlink(path);
}

static void test_native_large_write(void)
{
    const char path[] = "/dev/shmem/rtmux-large-write";
    const size_t length = 65536;
    unsigned char *data = malloc(length), *received = malloc(length);
    int fd = -1;

    if (!data || !received) {
        test_fail("native large write buffers", "allocation failed");
        goto done;
    }
    (void) unlink(path);
    fd = open(path, O_CREAT | O_EXCL | O_RDWR, 0600);
    if (fd == -1) {
        test_fail("native large write fixture", strerror(errno));
        goto done;
    }
    for (size_t i = 0; i < length; ++i)
        data[i] = (unsigned char) (i * 37U + i / 4096U);
    check(write(fd, data, length) == (ssize_t) length,
          "native standalone write exceeds receive buffer",
          "validate against full IPC length, not the initial receive prefix");
    check(pread(fd, received, length, 0) == (ssize_t) length &&
              memcmp(data, received, length) == 0,
          "native large write payload intact", "all 64 KiB must match");
done:
    if (fd != -1) {
        (void) close(fd);
        (void) unlink(path);
    }
    free(data);
    free(received);
}

static void test_native_short_reads(void)
{
    const char path[] = "/dev/shmem/rtmux-short-read";
    const unsigned char data[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    struct {
        unsigned char bytes[3];
        unsigned char guard;
    } received = {{0}, 0xcc};
    struct _io_read read_msg = {0};
    struct {
        struct _io_read read;
        struct _xtype_offset offset;
    } positioned = {0};
    struct stat st;
    int fd;

    (void) unlink(path);
    fd = open(path, O_CREAT | O_EXCL | O_RDWR, 0600);
    if (fd == -1) {
        test_fail("native short read fixture", strerror(errno));
        return;
    }
    check(write(fd, data, sizeof data) == sizeof data &&
              lseek(fd, 1, SEEK_SET) == 1,
          "native short read setup", "create data and set position");
    read_msg.type = _IO_READ;
    read_msg.combine_len = sizeof read_msg;
    read_msg.nbytes = 6;
    check(msg_send(fd, &read_msg, sizeof read_msg, received.bytes,
                    sizeof received.bytes) == sizeof received.bytes,
          "native short read returns transferred count",
          "a reply buffer shorter than nbytes is a valid short transfer");
    check(memcmp(received.bytes, data + 1, sizeof received.bytes) == 0 &&
              received.guard == 0xcc,
          "native short read copies only reply prefix", "preserve reply bounds");
    check(lseek(fd, 0, SEEK_CUR) == 4,
          "native short read advances by transferred count",
          "advance by three bytes, not the requested six");
    check(msg_send(fd, &read_msg, sizeof read_msg, NULL, 0) == 0 &&
              lseek(fd, 0, SEEK_CUR) == 4,
          "native empty reply leaves position unchanged",
          "a zero-byte transfer must return zero without moving the OCB");

    positioned.read = read_msg;
    positioned.read.combine_len = sizeof positioned;
    positioned.read.xtype = _IO_XTYPE_OFFSET;
    positioned.offset.offset = 2;
    check(msg_send(fd, &positioned, sizeof positioned, received.bytes, 2) == 2 &&
              memcmp(received.bytes, data + 2, 2) == 0 &&
              lseek(fd, 0, SEEK_CUR) == 4,
          "native positioned short read preserves position",
          "return the copied prefix without updating the current position");
    check(read(fd, received.bytes, 2) == 2 &&
              memcmp(received.bytes, data + 4, 2) == 0,
          "native read resumes after short transfer", "do not repeat bytes");
    check(fstat(fd, &st) == 0 && st.st_size == sizeof data,
          "native short reads preserve file size", "reads must not resize");
    (void) close(fd);
    check(unlink(path) == 0, "native short read cleanup", "unlink fixture");
}

static void test_native_rw_validation(void)
{
    const char path[] = "/dev/shmem/rtmux-rw-validation";
    struct {
        struct _io_write write;
        struct _xtype_offset offset;
        unsigned char data[4];
    } positioned = {0};
    struct {
        struct _io_read read;
        struct _xtype_offset offset;
    } read_at = {0};
    struct stat st;
    char data[4] = {1, 2, 3, 4};
    int fd;

    (void) unlink(path);
    fd = open(path, O_CREAT | O_EXCL | O_RDWR | O_APPEND, 0600);
    if (fd == -1) {
        test_fail("native validation fixture", strerror(errno));
        return;
    }
    check(write(fd, data, sizeof data) == sizeof data &&
              lseek(fd, 1, SEEK_SET) == 1,
          "native append validation setup", "create data and set position");
    positioned.write.type = _IO_WRITE;
    positioned.write.combine_len = sizeof positioned;
    positioned.write.nbytes = sizeof data;
    positioned.write.xtype = _IO_XTYPE_OFFSET;
    positioned.offset.offset = -1;
    errno = 0;
    check(msg_send(fd, &positioned, sizeof positioned, NULL, 0) == -1 &&
              errno == EINVAL,
          "native rejects negative wire write offset", "validate before append");
    check(lseek(fd, 0, SEEK_CUR) == 1 && fstat(fd, &st) == 0 && st.st_size == 4,
          "native invalid append preserves position and size",
          "invalid requests must not move the OCB to EOF");
    positioned.write.xtype = 3;
    errno = 0;
    check(msg_send(fd, &positioned, sizeof positioned, NULL, 0) == -1 &&
              errno == ENOSYS,
          "native rejects unknown write xtype", "unsupported protocol variant");
    read_at.read.type = _IO_READ;
    read_at.read.combine_len = sizeof read_at;
    read_at.read.nbytes = 1;
    read_at.read.xtype = _IO_XTYPE_OFFSET;
    read_at.offset.offset = -1;
    errno = 0;
    check(msg_send(fd, &read_at, sizeof read_at, data, 1) == -1 &&
              errno == EINVAL,
          "native rejects negative wire read offset", "bypass client-side guard");
    errno = 0;
    check(msg_send(fd, &read_at, 4, data, 1) == -1 && errno == EBADMSG,
          "native rejects truncated read header", "full LP64 header required");
    check(pwrite(fd, data, 1, 0) == 1 && lseek(fd, 0, SEEK_CUR) == 1 &&
              fstat(fd, &st) == 0 && st.st_size == 4,
          "native positioned append preserves position",
          "explicit offsets must not append or change the current position");
    (void) close(fd);
    fd = open(path, O_WRONLY);
    if (fd == -1) {
        test_fail("native write-only fixture", strerror(errno));
        goto done;
    }
    errno = 0;
    check(read(fd, data, 1) == -1 && errno == EBADF,
          "native write-only handle denies read", "enforce read access");
    io_lseek_t seek = {0};
    uint64_t position = 0;
    seek.i.type = _IO_LSEEK;
    seek.i.combine_len = sizeof seek.i;
    seek.i.whence = SEEK_SET;
    seek.i.offset = UINT64_MAX - 1;
    check(msg_send(fd, &seek.i, sizeof seek.i, &position, sizeof position) == 0 &&
              position == UINT64_MAX - 1,
          "native overflow fixture", "set a raw position outside signed off_t");
    errno = 0;
    check(write(fd, data, sizeof data) == -1 && errno == EFBIG,
          "native rejects position plus count overflow", "do not wrap file size");
    check(fstat(fd, &st) == 0 && st.st_size == 4,
          "native overflow leaves size unchanged", "reject before resizing");
done:
    if (fd != -1)
        (void) close(fd);
    check(unlink(path) == 0, "native validation cleanup", "unlink fixture");
}

/* Native memory files use different LP64 read/write wire headers. Exercise
 * actual payloads and offsets, not just open/close or client-side rejection.
 */
void test_native_file_io(void)
{
    const char path[] = "/dev/shmem/rtmux-native-file-io";
    unsigned char data[4096], received[4096];
    const unsigned char marker[] = {0xd3, 0x7a, 0x91};
    struct stat st;
    int fd;
    ssize_t n;

    for (size_t i = 0; i < sizeof data; ++i)
        data[i] = (unsigned char) (i * 37U + 1U);
    (void) unlink(path);
    fd = open(path, O_CREAT | O_EXCL | O_RDWR, 0600);
    if (fd == -1) {
        test_fail("native file create", strerror(errno));
        return;
    }
    check(fstat(fd, &st) == 0 && st.st_size == 0,
          "native file initially empty", "new file must have size zero");
    n = write(fd, data, sizeof data);
    check(n == (ssize_t) sizeof data, "native file write payload",
          "LP64 write header must be decoded as write, not read");
    if (n != (ssize_t) sizeof data)
        goto done;
    check(fstat(fd, &st) == 0 && st.st_size == (off_t) sizeof data,
          "native file write grows size", "write must extend the file");
    check(lseek(fd, 0, SEEK_SET) == 0, "native file rewind", "seek to start");
    n = read(fd, received, sizeof received);
    check(n == (ssize_t) sizeof received &&
              memcmp(data, received, sizeof data) == 0,
          "native file read payload", "copy must start after the actual header");
    check(lseek(fd, 9, SEEK_SET) == 9, "native file seek before positioned I/O",
          "set an independent current position");
    check(pwrite(fd, marker, sizeof marker, 2049) == (ssize_t) sizeof marker,
          "native file pwrite unaligned", "decode write offset after 16-byte header");
    check(lseek(fd, 0, SEEK_CUR) == 9, "native file pwrite preserves position",
          "positioned writes must not move the OCB position");
    memset(received, 0, sizeof received);
    check(pread(fd, received, sizeof marker, 2049) == (ssize_t) sizeof marker &&
              memcmp(received, marker, sizeof marker) == 0,
          "native file pread unaligned", "read the exact positioned-write bytes");
    check(lseek(fd, 0, SEEK_CUR) == 9, "native file pread preserves position",
          "positioned reads must not move the OCB position");
    check(pwrite(fd, marker, sizeof marker, 8193) == (ssize_t) sizeof marker,
          "native file pwrite extends", "positioned write beyond EOF must grow");
    check(fstat(fd, &st) == 0 && st.st_size == 8196,
          "native file sparse size", "new size is offset plus payload length");
    memset(received, 0xff, sizeof received);
    n = pread(fd, received, sizeof received, 4096);
    int zero_hole = n == (ssize_t) sizeof received;
    for (size_t i = 0; zero_hole && i < sizeof received; ++i)
        zero_hole = received[i] == 0;
    check(zero_hole, "native file sparse hole zeroed",
          "the gap before a positioned write must read as zeros");
    check(lseek(fd, 16384, SEEK_SET) == 16384 && write(fd, data, 0) == 0 &&
              fstat(fd, &st) == 0 && st.st_size == 8196,
          "native file zero write does not grow",
          "zero-byte writes beyond EOF must leave the size unchanged");
    check(read(fd, received, 1) == 0, "native file read past EOF", "must return zero");

    /* Reject malformed wire requests before allocating or copying data. */
    struct _io_write wr = {0};
    wr.type = _IO_WRITE;
    wr.combine_len = sizeof wr;
    wr.nbytes = -1;
    errno = 0;
    check(msg_send(fd, &wr, sizeof wr, NULL, 0) == -1 && errno == EINVAL,
          "native file negative wire count", "reject negative signed write count");
    wr.nbytes = 1;
    errno = 0;
    check(msg_send(fd, &wr, sizeof wr, NULL, 0) == -1 && errno == EBADMSG,
          "native file missing wire payload", "advertised payload must exist");
    wr.nbytes = 0;
    wr.xtype = _IO_XTYPE_OFFSET;
    errno = 0;
    check(msg_send(fd, &wr, sizeof wr, NULL, 0) == -1 && errno == EBADMSG,
          "native file truncated write offset", "offset must fit received header");
    wr.xtype = _IO_XTYPE_NONE;
    errno = 0;
    check(msg_send(fd, &wr, 4, NULL, 0) == -1 && errno == EBADMSG,
          "native file truncated write header", "fixed header must fit");
    struct _io_read rd = {0};
    rd.type = _IO_READ;
    rd.combine_len = sizeof rd;
    rd.nbytes = 1;
    rd.xtype = _IO_XTYPE_OFFSET;
    errno = 0;
    check(msg_send(fd, &rd, sizeof rd, received, 1) == -1 && errno == EBADMSG,
          "native file truncated read offset", "read offset must fit received header");
    rd.xtype = _IO_XTYPE_NONE;
    rd.nbytes = SIZE_MAX;
    errno = 0;
    check(msg_send(fd, &rd, sizeof rd, NULL, 0) == -1 && errno == EINVAL,
          "native file oversized wire read", "reply count must be representable");
    check(fstat(fd, &st) == 0 && st.st_size == 8196,
          "native file rejected requests leave size unchanged", "no allocation side effects");
    check(close(fd) == 0, "native file close writer", "close must succeed");
    fd = open(path, O_RDONLY);
    if (fd == -1) {
        test_fail("native file reopen", strerror(errno));
        goto done;
    }
    check(pread(fd, received, sizeof marker, 8193) == (ssize_t) sizeof marker &&
              memcmp(received, marker, sizeof marker) == 0,
          "native file reopen retains bytes", "read the committed object data");
    errno = 0;
    check(write(fd, data, 1) == -1 && errno == EBADF,
          "native file read-only handle denies write", "access mode must remain enforced");
done:
    if (fd != -1)
        (void) close(fd);
    check(unlink(path) == 0, "native file unlink", "fixture cleanup");
    test_native_combined_io();
    test_native_large_write();
    test_native_failed_write_no_growth();
    test_native_short_reads();
    test_native_rw_validation();
    test_native_io_boundaries();
}

static void test_posix_io_size_overflow_guards(void)
{
    char byte = 0;
    FILE *fp;
    int fd;

    fd = open("/dev/zero", O_RDONLY);
    if (fd == -1) {
        test_fail("read size overflow setup", strerror(errno));
        return;
    }
    errno = 0;
    check(read(fd, &byte, (size_t) INT32_MAX + 1U) == -1 && errno == EINVAL,
          "read rejects unrepresentable size", "expected EINVAL");
    (void) close(fd);

    fd = open("/dev/null", O_WRONLY);
    if (fd == -1) {
        test_fail("write size overflow setup", strerror(errno));
        return;
    }
    errno = 0;
    check(write(fd, &byte, (size_t) INT32_MAX + 1U) == -1 && errno == EINVAL,
          "write rejects unrepresentable size", "expected EINVAL");
    (void) close(fd);

    fp = fopen("/dev/zero", "r");
    if (!fp) {
        test_fail("fread overflow setup", strerror(errno));
        return;
    }
    errno = 0;
    check(fread(&byte, SIZE_MAX, 2, fp) == 0 && errno == EOVERFLOW,
          "fread rejects size multiplication overflow", "expected EOVERFLOW");
    (void) fclose(fp);

    fp = fopen("/dev/null", "w");
    if (!fp) {
        test_fail("fwrite overflow setup", strerror(errno));
        return;
    }
    errno = 0;
    check(fwrite(&byte, SIZE_MAX, 2, fp) == 0 && errno == EOVERFLOW,
          "fwrite rejects size multiplication overflow", "expected EOVERFLOW");
    (void) fclose(fp);
}

static void test_posix_memalign_contract(void)
{
    void *ptr = NULL;

    check(posix_memalign(&ptr, sizeof(void *), 64) == EOK && ptr,
          "posix_memalign valid allocation",
          "valid alignment and size must allocate storage");
    free(ptr);

    ptr = (void *) 0x1234;
    check(posix_memalign(&ptr, 3, 64) == EINVAL && ptr == (void *) 0x1234,
          "posix_memalign bad alignment",
          "invalid alignment must fail without changing output");
    check(posix_memalign(NULL, sizeof(void *), 64) == EINVAL,
          "posix_memalign null output",
          "posix_memalign must reject null output pointer");
}

static void test_posix_umask_contract(void)
{
    mode_t old = umask(0022);
    mode_t prev = umask(old);

    check(prev == 0022, "umask returns previous mask",
          "umask must install the new mask and return the old one");
}

static void test_posix_memlock_contract(void)
{
    int value = 0;

    check(mlock(&value, sizeof(value)) == 0, "mlock valid range",
          "wired-memory request must succeed on resident memory");
    check(munlock(&value, sizeof(value)) == 0, "munlock valid range",
          "unlock request must succeed on resident memory");
    check(mlockall(MCL_CURRENT | MCL_FUTURE) == 0, "mlockall valid flags",
          "valid mlockall flags must be accepted");
    check(munlockall() == 0, "munlockall", "expected success");

    errno = 0;
    check(mlock(NULL, 1) == -1 && errno == ENOMEM, "mlock null range",
          "non-empty null range must fail");
    errno = 0;
    check(munlock(NULL, 1) == -1 && errno == ENOMEM, "munlock null range",
          "non-empty null range must fail");
    errno = 0;
    check(mlockall(-1) == -1 && errno == EINVAL, "mlockall bad flags",
          "unknown mlockall flags must fail");
}

static void test_posix_shm_contract(void)
{
    const char name[] = "/rtmux-shm-test";
    int fd;

    (void) shm_unlink(name);
    fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
    if (fd == -1) {
        test_fail("shm_open create", strerror(errno));
        return;
    }
    check(close(fd) == 0, "shm close created fd", "expected success");

    fd = shm_open(name, O_RDWR, 0);
    check(fd != -1, "shm_open existing", "expected fd");
    if (fd != -1)
        (void) close(fd);

    check(shm_unlink(name) == 0, "shm_unlink", "expected success");
    errno = 0;
    check(shm_open(name, O_RDWR, 0) == -1 && errno == ENOENT,
          "shm_open after unlink", "expected ENOENT");
    errno = 0;
    check(shm_unlink("bad/name") == -1 && errno == EINVAL,
          "shm_unlink bad name", "expected EINVAL");
}

static atomic_int pause_signal_seen;

static void pause_signal_handler(int signo)
{
    (void) signo;
    atomic_store(&pause_signal_seen, 1);
}

static void *pause_signal_sender(void *arg)
{
    const struct timespec delay = {.tv_nsec = 10000000L};

    (void) arg;
    (void) nanosleep(&delay, NULL);
    (void) raise(SIGUSR1);
    return NULL;
}

static void test_pause_returns_after_caught_signal(void)
{
    pthread_t tid;
    struct sigaction act = {.sa_handler = pause_signal_handler};
    struct sigaction old;

    sigemptyset(&act.sa_mask);
    if (sigaction(SIGUSR1, &act, &old) == -1) {
        test_fail("pause signal setup", strerror(errno));
        return;
    }
    atomic_store(&pause_signal_seen, 0);
    if (pthread_create(&tid, NULL, pause_signal_sender, NULL) != 0) {
        test_fail("pause signal sender", "pthread_create failed");
        (void) sigaction(SIGUSR1, &old, NULL);
        return;
    }

    errno = 0;
    check(
        pause() == -1 && errno == EINTR && atomic_load(&pause_signal_seen) == 1,
        "pause returns after caught signal", "expected EINTR");
    (void) pthread_join(tid, NULL);
    (void) sigaction(SIGUSR1, &old, NULL);
}

static void test_posix_stubs_fail_closed(void)
{
    struct rusage usage;
    struct tms tm;

    test_pause_returns_after_caught_signal();

    errno = 0;
    check(poll(NULL, 0, 0) == 0, "poll zero fds immediate",
          "nfds==0 with zero timeout must return immediately");
    errno = 0;
    check(poll(NULL, 0, -2) == -1 && errno == EINVAL,
          "poll rejects bad timeout", "timeout less than -1 must fail");

    /* tcflush, tcdrain, tcsendbreak, and tcflow are backed by the terminal
     * drivers. TCOOFF/TCOON are successful no-ops on RTMux's synchronous
     * terminals because there is no queued terminal output to suspend.
     */
    errno = 0;
    check(tcflush(-1, TCIOFLUSH) == -1 && errno == EBADF,
          "tcflush bad fd", "an invalid descriptor must fail EBADF");
    errno = 0;
    check(tcflush(STDIN_FILENO, 0x7fff) == -1 && errno == EINVAL,
          "tcflush bad queue selector",
          "an out-of-range queue selector must fail EINVAL");
    errno = 0;
    check(tcdrain(-1) == -1 && errno == EBADF,
          "tcdrain bad fd", "an invalid descriptor must fail EBADF");
    errno = 0;
    check(tcflow(-1, TCOON) == -1 && errno == EBADF,
          "tcflow bad fd", "an invalid descriptor must fail EBADF");
    errno = 0;
    check(tcflow(STDIN_FILENO, 0x7fff) == -1 && errno == EINVAL,
          "tcflow bad action", "an out-of-range action must fail EINVAL");
    errno = 0;
    check(tcsendbreak(-1, 0) == -1 && errno == EBADF,
          "tcsendbreak bad fd", "an invalid descriptor must fail EBADF");
    errno = 0;
    check(tcsetpgrp(STDIN_FILENO, 0) == -1 && errno == EINVAL,
          "tcsetpgrp rejects zero pgrp",
          "a non-positive process-group id must fail EINVAL");
    errno = 0;
    check(tcsetpgrp(STDIN_FILENO, -1) == -1 && errno == EINVAL,
          "tcsetpgrp rejects negative pgrp",
          "a negative process-group id must fail EINVAL");

    int tc_pipe[2];
    if (pipe(tc_pipe) != 0) {
        test_fail("tc* non-tty setup", strerror(errno));
    } else {
        errno = 0;
        check(tcflush(tc_pipe[0], TCIFLUSH) == -1 && errno == ENOTTY,
              "tcflush non-tty", "a valid non-terminal fd must fail ENOTTY");
        errno = 0;
        check(tcdrain(tc_pipe[0]) == -1 && errno == ENOTTY,
              "tcdrain non-tty", "a valid non-terminal fd must fail ENOTTY");
        errno = 0;
        check(tcflow(tc_pipe[1], TCOON) == -1 && errno == ENOTTY,
              "tcflow non-tty", "a valid non-terminal fd must fail ENOTTY");
        errno = 0;
        check(tcsendbreak(tc_pipe[0], 0) == -1 && errno == ENOTTY,
              "tcsendbreak non-tty", "a valid non-terminal fd must fail ENOTTY");
        close(tc_pipe[0]);
        close(tc_pipe[1]);
    }

    /* On a real terminal tcflush now discards pending input and succeeds for
     * every valid queue selector.
     */
    if (isatty(STDIN_FILENO)) {
        check(tcflush(STDIN_FILENO, TCIFLUSH) == 0 &&
                  tcflush(STDIN_FILENO, TCOFLUSH) == 0 &&
                  tcflush(STDIN_FILENO, TCIOFLUSH) == 0,
              "tcflush on a terminal",
              "a backed terminal must flush and return success");

        /* Terminal output is transmitted synchronously, so a drain has nothing
         * to wait for and must succeed immediately.
         */
        check(tcdrain(STDIN_FILENO) == 0, "tcdrain on a terminal",
              "a drain on a synchronous terminal must return success");

        /* The console is not an async serial line, so POSIX has tcsendbreak
         * take no action and succeed.
         */
        check(tcsendbreak(STDIN_FILENO, 0) == 0, "tcsendbreak on a terminal",
              "tcsendbreak on a non-serial terminal must succeed with no action");
        check(tcflow(STDIN_FILENO, TCIOFF) == 0 &&
                  tcflow(STDIN_FILENO, TCION) == 0,
              "tcflow start-stop chars",
              "TCIOFF/TCION must transmit the terminal stop/start characters");
        check(tcflow(STDIN_FILENO, TCOOFF) == 0 &&
                  tcflow(STDIN_FILENO, TCOON) == 0,
              "tcflow output suspend resume",
              "TCOOFF/TCOON must succeed on a synchronous terminal");
    }

    struct termios tio = {.c_ispeed = B9600, .c_ospeed = B19200};
    check(cfgetispeed(&tio) == B9600 && cfgetospeed(&tio) == B19200,
          "termios speed getters", "expected stored input/output speeds");
    check(cfsetispeed(&tio, B38400) == 0 && cfsetospeed(&tio, B115200) == 0 &&
              cfgetispeed(&tio) == B38400 && cfgetospeed(&tio) == B115200,
          "termios speed setters", "expected valid baud rates to stick");
    errno = 0;
    check(cfsetispeed(&tio, (speed_t) -1) == -1 && errno == EINVAL,
          "termios speed rejects invalid value", "expected EINVAL");

    errno = 0;
    clock_t tick = times(&tm);
    check(tick != (clock_t) -1 && sysconf(_SC_CLK_TCK) > 0 &&
              tm.tms_utime >= 0 && tm.tms_stime >= 0 && tm.tms_cutime >= 0 &&
              tm.tms_cstime >= 0,
          "times reports process ticks",
          "times should use proc-service CPU accounting");
    check(getrusage(RUSAGE_SELF, &usage) == 0 && usage.ru_utime.tv_sec >= 0 &&
              usage.ru_stime.tv_sec >= 0,
          "getrusage self", "expected process CPU accounting");
    check(getrusage(RUSAGE_CHILDREN, &usage) == 0 &&
              usage.ru_utime.tv_sec >= 0 && usage.ru_stime.tv_sec >= 0,
          "getrusage children", "expected child CPU accounting");
    errno = 0;
    check(getrusage(RUSAGE_SELF, NULL) == -1 && errno == EFAULT,
          "getrusage null output", "expected EFAULT");
    errno = 0;
    check(getrusage(42, &usage) == -1 && errno == EINVAL, "getrusage bad who",
          "expected EINVAL");
}

static void test_statvfs_wrappers(void)
{
    struct statvfs stv;
    const char path[] = "/bin/true";
    int fd = open(path, O_RDONLY);

    check(statvfs(path, &stv) == 0 && stv.f_bsize > 0 && stv.f_frsize > 0 &&
              stv.f_namemax > 0,
          "statvfs /bin/true", "expected cpiofs statistics");
    if (fd == -1) {
        test_fail("fstatvfs setup", strerror(errno));
        return;
    }
    check(fstatvfs(fd, &stv) == 0 && !strcmp(stv.f_basetype, "cpiofs"),
          "fstatvfs /bin/true", "expected cpiofs base type");
    (void) close(fd);

    errno = 0;
    check(statvfs("/missing-statvfs", &stv) == -1 && errno == ENOENT,
          "statvfs missing path", "expected ENOENT");
}

/* Child half of nvme_raw_perm: drop to an unprivileged uid and require that
 * every raw NVMe node refuses both read and write opens with EACCES.
 */
int nvme_raw_child(void)
{
    static const char *const nodes[] = {"/dev/nvme0n1", "/dev/nvme0n1p8"};

    if (setgid(1000) != 0 || setuid(1000) != 0)
        return 2;
    for (size_t i = 0; i < sizeof(nodes) / sizeof(nodes[0]); i++) {
        static const int modes[] = {O_RDONLY, O_WRONLY, O_RDWR};

        for (size_t m = 0; m < sizeof(modes) / sizeof(modes[0]); m++) {
            int fd = open(nodes[i], modes[m]);

            if (fd >= 0) {
                (void) close(fd);
                return 3;
            }
            if (errno != EACCES)
                return 4;
        }
    }
    return 0;
}

/* /sys/kstats: every line is key=value, each documented key appears exactly
 * once, and soul.thread.used moves by exactly one while a thread is alive.
 */
static int kstats_read(int fd, char *buf, size_t cap)
{
    ssize_t n, total = 0;

    if (lseek(fd, 0, SEEK_SET) != 0)
        return -1;
    while (total < (ssize_t) cap - 1 &&
           (n = read(fd, buf + total, cap - 1 - (size_t) total)) > 0)
        total += n;
    buf[total < 0 ? 0 : total] = '\0';
    return (int) total;
}

/* Value of "key" in a kstats snapshot, or -1 if absent. */
static long kstats_get(const char *snap, const char *key)
{
    size_t klen = strlen(key);

    for (const char *p = snap; *p; p = strchr(p, '\n') ? strchr(p, '\n') + 1 : p + strlen(p)) {
        if (!strncmp(p, key, klen) && p[klen] == '=')
            return strtol(p + klen + 1, NULL, 10);
    }
    return -1;
}

static void *kstats_thread_body(void *arg)
{
    pthread_barrier_t *b = arg;
    struct timespec nap = {.tv_nsec = 1000000L};

    (void) nanosleep(&nap, NULL); /* a timed wait: allocates this thread's timeout timer */
    (void) pthread_barrier_wait(b);
    (void) pthread_barrier_wait(b);
    return NULL;
}

static void test_sys_kstats(void)
{
    static const char *const pools[] = {
        "notification", "sync", "timer", "thread", "connect", "channel",
        "process", "global_channel", "interrupt", "syncevent", "fpu",
        "credential", "limits", "vthread", "debug", "breakpt", "ssinfo",
        "threadname"};
    static const char *const fields[] = {"used", "reserved", "total"};
    char snap[4096], key[64];
    pthread_barrier_t bar;
    pthread_t th;
    long base, during = -1, after = -1, live_base, live_during = -1;
    long tmo_base, tmo_during = -1, timer_base, timer_during = -1;
    long harts = sysconf(_SC_NPROCESSORS_ONLN);
    int fd, bad = 0, lines = 0;

    fd = open("/sys/kstats", O_RDONLY);
    if (fd < 0) {
        test_fail("sys_kstats open", strerror(errno));
        return;
    }
    if (kstats_read(fd, snap, sizeof snap) <= 0) {
        test_fail("sys_kstats read", "empty snapshot");
        (void) close(fd);
        return;
    }

    for (const char *p = snap; *p; p = strchr(p, '\n') ? strchr(p, '\n') + 1 : p + strlen(p)) {
        const char *eq = strchr(p, '=');
        const char *nl = strchr(p, '\n');

        if (!eq || !nl || eq > nl || eq == p || !(nl - eq > 1))
            bad++;
        lines++;
    }
    check(bad == 0, "sys_kstats_format every line is key=value", snap);
    bad = 0;

    for (size_t i = 0; i < sizeof pools / sizeof pools[0]; i++)
        for (size_t f = 0; f < sizeof fields / sizeof fields[0]; f++) {
            const char *p = snap;
            int hits = 0;

            snprintf(key, sizeof key, "soul.%s.%s=", pools[i], fields[f]);
            while ((p = strstr(p, key))) {
                if (p == snap || p[-1] == '\n')
                    hits++;
                p += strlen(key);
            }
            if (hits != 1)
                bad++;
        }
    /* One timer.<hart>.intr per online hart, each exactly once, none beyond. */
    for (long h = 0; h < harts; h++) {
        const char *p = snap;
        int hits = 0;

        snprintf(key, sizeof key, "timer.%ld.intr=", h);
        while ((p = strstr(p, key))) {
            if (p == snap || p[-1] == '\n')
                hits++;
            p += strlen(key);
        }
        key[strlen(key) - 1] = '\0'; /* drop '=' for kstats_get */
        if (hits != 1 || kstats_get(snap, key) < 0)
            bad++;
    }
    snprintf(key, sizeof key, "timer.%ld.intr", harts);
    if (harts < 1 || kstats_get(snap, key) != -1)
        bad++;
    check(bad == 0 && kstats_get(snap, "pa.free_bytes") > 0 &&
              kstats_get(snap, "roottask.heap_used") > 0 &&
              kstats_get(snap, "ioserv.disconnects") >= 0 &&
              kstats_get(snap, "thread.live") > 0 &&
              kstats_get(snap, "thread.live") <= kstats_get(snap, "soul.thread.used") + 2 &&
              kstats_get(snap, "timeout.live") >= 0 &&
              kstats_get(snap, "timeout.live") <= kstats_get(snap, "thread.live") &&
              kstats_get(snap, "prio.reply_depth") > 0 &&
              kstats_get(snap, "prio.reply_depth_overflow") >= 0 &&
              lines == (int) (3 * (sizeof pools / sizeof pools[0]) + 9 + (size_t) harts),
          "sys_kstats_format documented keys appear exactly once",
          "a documented key is missing, duplicated, or extra");

    /* Two warm-up reads, then +1 thread while one waits on a barrier. */
    (void) kstats_read(fd, snap, sizeof snap);
    (void) kstats_read(fd, snap, sizeof snap);
    base = kstats_get(snap, "soul.thread.used");
    live_base = kstats_get(snap, "thread.live");
    tmo_base = kstats_get(snap, "timeout.live");
    timer_base = kstats_get(snap, "soul.timer.used");
    if (pthread_barrier_init(&bar, NULL, 2) != 0 ||
        pthread_create(&th, NULL, kstats_thread_body, &bar) != 0) {
        test_fail("sys_kstats_thread_delta fixture", strerror(errno));
        (void) close(fd);
        return;
    }
    (void) pthread_barrier_wait(&bar); /* thread is alive and parked */
    (void) kstats_read(fd, snap, sizeof snap);
    during = kstats_get(snap, "soul.thread.used");
    live_during = kstats_get(snap, "thread.live");
    tmo_during = kstats_get(snap, "timeout.live");
    timer_during = kstats_get(snap, "soul.timer.used");
    (void) pthread_barrier_wait(&bar);
    (void) pthread_join(th, NULL);
    for (int i = 0; i < 10; i++) { /* returns to baseline within 100 ms */
        struct timespec ts = {.tv_nsec = 10000000L};

        (void) kstats_read(fd, snap, sizeof snap);
        after = kstats_get(snap, "soul.thread.used");
        if (after == base)
            break;
        (void) nanosleep(&ts, NULL);
    }
    (void) pthread_barrier_destroy(&bar);
    (void) close(fd);
    snprintf(key, sizeof key, "base %ld during %ld after %ld", base, during, after);
    check(base >= 0 && during == base + 1 && after == base,
          "sys_kstats_thread_delta soul.thread.used +1 then baseline", key);

    /* thread.live follows the process thread vectors, so a created thread shows
     * in both gauges and the leak gate's difference (souls no process owns)
     * stays put.
     */
    snprintf(key, sizeof key, "live base %ld during %ld", live_base, live_during);
    check(live_base > 0 && live_during == live_base + 1 && during - live_during == base - live_base,
          "sys_kstats_thread_live +1 with the live thread", key);

    /* The new thread slept once, so it owns a timeout timer: both gauges rise
     * by one and the leak gate's difference (timers nobody owns) stays put.
     */
    check(tmo_base >= 0 && tmo_during == tmo_base + 1 &&
              timer_during - tmo_during == timer_base - tmo_base,
          "sys_kstats_timeout_live +1 with a thread that slept",
          "timeout.live did not track a thread's timeout timer");
}

/* malloc backing: the heap grows past the old 512 KiB static arena, requests of
 * 128 KiB and more are mapped and unmapped directly, and realloc and aligned
 * allocation work across the small/large boundary.
 */
static void test_malloc_growth(void)
{
    enum { BLK = 1024, NBLK = 8192 }; /* 8 MiB in 1 KiB blocks */
    unsigned char **v = calloc(NBLK, sizeof *v);
    int bad = 0, nfail = 0;

    if (!v) {
        test_fail("malloc_8mib_1k fixture", "calloc");
        return;
    }
    for (int pass = 0; pass < 2; pass++) { /* allocate, verify, free, redo */
        for (int i = 0; i < NBLK; i++) {
            v[i] = malloc(BLK);
            if (!v[i]) {
                nfail++;
                continue;
            }
            memset(v[i], i & 0xff, BLK);
        }
        for (int i = 0; i < NBLK; i++) {
            if (!v[i])
                continue;
            for (int k = 0; k < BLK; k += 257)
                if (v[i][k] != (unsigned char) (i & 0xff))
                    bad++;
            free(v[i]);
            v[i] = NULL;
        }
    }
    free(v);
    check(nfail == 0 && bad == 0, "malloc_8mib_1k holds 8 MiB and reuses it",
          "heap did not grow past the old 512 KiB arena or corrupted data");

    /* Large blocks are mapped and unmapped. This repeats a 4 MiB request 64
     * times; it proves large blocks work and that free does not corrupt the
     * heap, but it cannot see a leaked mapping on its own.
     */
    bad = 0;
    for (int i = 0; i < 64; i++) {
        size_t n = (size_t) 4 << 20;
        unsigned char *p = malloc(n);

        if (!p) {
            bad++;
            break;
        }
        p[0] = 1;
        p[n - 1] = 2;
        free(p);
    }
    check(bad == 0, "malloc_large_direct maps and unmaps 4 MiB blocks",
          "a large block was not released by free");

    {
        unsigned char *p = malloc(200 * 1024);
        unsigned char *q;
        int ok = p != NULL;

        if (ok) {
            memset(p, 0x5a, 200 * 1024);
            q = realloc(p, 300 * 1024); /* grow while mapped */
            ok = q && q[0] == 0x5a && q[200 * 1024 - 1] == 0x5a;
            p = q;
        }
        if (ok) {
            q = realloc(p, 100); /* shrink back to the bucketed heap */
            ok = q && q[0] == 0x5a && q[99] == 0x5a;
            p = q;
        }
        if (ok) {
            q = realloc(p, 1 << 20); /* and up again */
            ok = q && q[0] == 0x5a;
            p = q;
        }
        free(p);
        check(ok, "malloc_realloc_crosses_large_boundary",
              "realloc lost data across the 128 KiB mapping threshold");
    }

    {
        void *p = NULL;
        int rc = posix_memalign(&p, 65536, 512 * 1024);

        check(rc == 0 && p && ((uintptr_t) p & 65535) == 0,
              "malloc_aligned_large 64 KiB alignment on a mapped block",
              "aligned allocation from a large block failed");
        free(p);
    }
}

static void test_nvme_raw_perm(void)
{
    static const char *const nodes[] = {"/dev/nvme0n1", "/dev/nvme0n1p8"};
    char *argv[] = {"syscall_testing", "--nvme-raw-child", NULL};
    struct stat st;
    pid_t pid;
    int status = -1;

    if (geteuid() != 0 || stat(nodes[0], &st) != 0)
        return; /* needs root, and NVMe present */

    for (size_t i = 0; i < sizeof(nodes) / sizeof(nodes[0]); i++) {
        int fd = open(nodes[i], O_RDONLY);

        check(fd >= 0, "nvme_raw_perm root open", nodes[i]);
        if (fd >= 0)
            (void) close(fd);
        check(stat(nodes[i], &st) == 0 && (st.st_mode & 07777) == 0600,
              "nvme_raw_perm mode 0600", nodes[i]);
    }

    if (posix_spawn(&pid, "/bin/syscall_testing", NULL, NULL, argv, NULL) != 0) {
        test_fail("nvme_raw_perm spawn", strerror(errno));
        return;
    }
    check(wait_child_exit(pid, &status, 500) && WIFEXITED(status) &&
              WEXITSTATUS(status) == 0,
          "nvme_raw_perm unprivileged open is EACCES",
          "uid 1000 must not open raw NVMe nodes for read or write");
}

static void test_nvme_negative_lseek(void)
{
    struct stat st;
    off_t pos;
    int fd;

    fd = open("/dev/nvme0n1p8", O_RDONLY);
    if (fd == -1) {
        test_fail("nvme lseek setup", strerror(errno));
        return;
    }
    if (fstat(fd, &st) == -1) {
        test_fail("nvme lseek stat", strerror(errno));
        (void) close(fd);
        return;
    }
    if (st.st_size < 1024) {
        test_fail("nvme lseek stat", "device is shorter than test offset");
        (void) close(fd);
        return;
    }

    errno = 0;
    pos = lseek(fd, -512, SEEK_END);
    check(pos == st.st_size - 512, "nvme negative SEEK_END",
          "block devices must accept signed end-relative seeks");

    errno = 0;
    pos = lseek(fd, -512, SEEK_CUR);
    check(pos == st.st_size - 1024, "nvme negative SEEK_CUR",
          "block devices must accept signed current-relative seeks");

    (void) close(fd);
}

/* POSIX error-path corner cases, scoped to interfaces RTMux actually
 * implements. Each case asserts the POSIX-mandated errno on a reliable native
 * fixture (/dev/zero, /dev/null, and the nvme block device the negative-lseek
 * test already relies on).
 */
static void test_posix_errno_corners(void)
{
    char buf[16];
    off_t pos;
    int fd;

    errno = 0;
    check(close(9999) == -1 && errno == EBADF, "close bad fd",
          "closing an unopened descriptor must fail EBADF");

    fd = open("/dev/zero", O_RDWR);
    if (fd == -1) {
        test_fail("errno-corner /dev/zero setup", strerror(errno));
        return;
    }

    errno = 0;
    check(fcntl(fd, 0x7fff) == -1 && errno == EINVAL, "fcntl bad cmd",
          "an unknown fcntl command must fail EINVAL");

    /* pread/pwrite reject a negative offset before touching the server. */
    errno = 0;
    check(pread(fd, buf, sizeof buf, -1) == -1 && errno == EINVAL,
          "pread negative offset", "a negative offset must fail EINVAL");
    errno = 0;
    check(pwrite(fd, buf, sizeof buf, -1) == -1 && errno == EINVAL,
          "pwrite negative offset", "a negative offset must fail EINVAL");

    /* readlink() on a non-symlink is EINVAL — both a leaf device node and a
     * directory-like mount root (a server endpoint with children).
     */
    errno = 0;
    check(readlink("/dev/null", buf, sizeof buf) == -1 && errno == EINVAL,
          "readlink non-symlink", "readlink of a non-symlink must fail EINVAL");
    errno = 0;
    check(readlink("/dev", buf, sizeof buf) == -1 && errno == EINVAL,
          "readlink directory", "readlink of a directory must fail EINVAL");

    (void) close(fd);

    (void) unlink("/dev/shmem/rtmux-fsetfl-mask");
    fd = open("/dev/shmem/rtmux-fsetfl-mask", O_RDWR | O_CREAT | O_TRUNC, 0600);
    if (fd == -1) {
        test_fail("errno-corner F_SETFL fixture", strerror(errno));
        return;
    }
    check((fcntl(fd, F_GETFL) & O_CREAT) == 0, "fcntl F_GETFL status-mask",
          "F_GETFL must not expose open-only creation bits");
    (void) close(fd);
    fd = open("/dev/shmem/rtmux-fsetfl-mask", O_RDWR);
    if (fd == -1) {
        test_fail("errno-corner F_SETFL reopen", strerror(errno));
        (void) unlink("/dev/shmem/rtmux-fsetfl-mask");
        return;
    }
    check(fcntl(fd, F_SETFL, O_APPEND | O_WRONLY | O_CREAT) == 0 &&
              (fcntl(fd, F_GETFL) & (O_APPEND | O_ACCMODE | O_CREAT)) ==
                  (O_APPEND | O_RDWR),
          "fcntl F_SETFL status-mask",
          "F_SETFL must change status flags without changing access/create bits");
    (void) close(fd);
    (void) unlink("/dev/shmem/rtmux-fsetfl-mask");

    /* Seek validation on a real seekable object: a resulting negative offset
     * and an unknown whence are both EINVAL.
     */
    fd = open("/dev/nvme0n1p8", O_RDONLY);
    if (fd == -1) {
        test_fail("errno-corner nvme setup", strerror(errno));
        return;
    }

    errno = 0;
    pos = lseek(fd, -1, SEEK_SET);
    check(pos == (off_t) -1 && errno == EINVAL, "lseek negative absolute",
          "seeking before offset 0 must fail EINVAL");

    errno = 0;
    pos = lseek(fd, 0, 999);
    check(pos == (off_t) -1 && errno == EINVAL, "lseek bad whence",
          "an unknown whence must fail EINVAL");

    (void) close(fd);
}

/* Access-mode and path/open-flag error paths. A descriptor opened one way
 * rejects the opposite I/O with EBADF; a non-directory used as a path prefix
 * component is ENOTDIR; and O_CREAT|O_EXCL on an existing file is EEXIST.
 */
static void test_posix_access_and_path_corners(void)
{
    const char excl[] = "/dev/shmem/rtmux-excl-corner";
    char buf[16];
    int fd;

    /* write() on a read-only descriptor is EBADF. */
    fd = open("/dev/zero", O_RDONLY);
    if (fd == -1) {
        test_fail("access-corner rdonly setup", strerror(errno));
        return;
    }
    errno = 0;
    check(write(fd, "x", 1) == -1 && errno == EBADF, "write on O_RDONLY",
          "writing a read-only descriptor must fail EBADF");
    (void) close(fd);

    /* read() on a write-only descriptor is EBADF. */
    fd = open("/dev/null", O_WRONLY);
    if (fd == -1) {
        test_fail("access-corner wronly setup", strerror(errno));
        return;
    }
    errno = 0;
    check(read(fd, buf, sizeof buf) == -1 && errno == EBADF, "read on O_WRONLY",
          "reading a write-only descriptor must fail EBADF");
    (void) close(fd);

    /* A non-directory used as a path prefix component is ENOTDIR, not ENOENT —
     * for open(), and equally for the stat()/access() resolve paths.
     */
    errno = 0;
    check(open("/dev/null/nope", O_RDONLY) == -1 && errno == ENOTDIR,
          "open non-dir component", "a file path component must fail ENOTDIR");
    {
        struct stat st;
        errno = 0;
        check(stat("/dev/null/nope", &st) == -1 && errno == ENOTDIR,
              "stat non-dir component", "stat through a file must fail ENOTDIR");
    }
    errno = 0;
    check(access("/dev/null/nope", F_OK) == -1 && errno == ENOTDIR,
          "access non-dir component", "access through a file must fail ENOTDIR");
    errno = 0;
    check(access(NULL, F_OK) == -1 && errno == EFAULT, "access NULL path",
          "access must reject a NULL path with EFAULT");

    /* O_CREAT|O_EXCL on an existing file is EEXIST. */
    (void) unlink(excl);
    fd = open(excl, O_RDWR | O_CREAT | O_EXCL, 0600);
    if (fd == -1) {
        test_fail("access-corner excl setup", strerror(errno));
        return;
    }
    (void) close(fd);
    errno = 0;
    fd = open(excl, O_RDWR | O_CREAT | O_EXCL, 0600);
    check(fd == -1 && errno == EEXIST, "open O_EXCL existing",
          "O_CREAT|O_EXCL on an existing file is EEXIST");
    if (fd != -1) /* regression: the exclusive open wrongly succeeded */
        (void) close(fd);

    /* A regular file (not just a device) used as a path prefix component is
     * ENOTDIR. This resolves through a different object type than the device
     * case above, so cover it too while the file exists.
     */
    {
        char sub[64];
        (void) snprintf(sub, sizeof sub, "%s/x", excl);
        errno = 0;
        check(open(sub, O_RDONLY) == -1 && errno == ENOTDIR,
              "open non-dir file component",
              "a regular file as a path component must fail ENOTDIR");
    }
    (void) unlink(excl);
}

/* creat() is open() with O_WRONLY|O_CREAT|O_TRUNC.  Confirm it creates and
 * opens a writable descriptor and the file becomes visible.
 */
static void test_posix_creat(void)
{
    const char path[] = "/dev/shmem/rtmux-creat";
    int fd;

    (void) unlink(path);
    fd = creat(path, 0600);
    check(fd >= 0, "creat returns a writable fd",
          "creat must create and open a file for writing");
    if (fd >= 0) {
        check(access(path, F_OK) == 0, "creat file is visible",
              "a created file must resolve through the namespace");
        (void) close(fd);
    }
    (void) unlink(path);
}

/* unlinkat() delegates the file-removal case to unlink for AT_FDCWD and
 * absolute paths. RTMux has no directory removal, so AT_REMOVEDIR is a
 * recognized-but-unsupported flag (ENOSYS), and an undefined flag is EINVAL.
 */
static void test_posix_unlinkat_contract(void)
{
    const char path[] = "/dev/shmem/rtmux-unlinkat";
    const char relname[] = "rtmux-unlinkat-rel";
    const char relpath[] = "/dev/shmem/rtmux-unlinkat-rel";
    int fd;
    int dfd;

    (void) unlink(path);
    (void) unlink(relpath);
    fd = creat(path, 0600);
    if (fd < 0) {
        test_fail("unlinkat fixture", strerror(errno));
        return;
    }
    (void) close(fd);
    check(access(path, F_OK) == 0, "unlinkat fixture visible",
          "the fixture file must exist before removal");
    check(unlinkat(AT_FDCWD, path, 0) == 0, "unlinkat AT_FDCWD",
          "unlinkat(AT_FDCWD, path, 0) must remove the file");
    check(access(path, F_OK) == -1 && errno == ENOENT, "unlinkat removed",
          "the file must be gone after unlinkat");

    fd = creat(relpath, 0600);
    dfd = open("/dev/shmem", O_RDONLY);
    if (fd < 0 || dfd < 0) {
        test_fail("unlinkat dirfd fixture", strerror(errno));
        if (fd >= 0)
            (void) close(fd);
        if (dfd >= 0)
            (void) close(dfd);
        (void) unlink(relpath);
        return;
    }
    (void) close(fd);
    check(unlinkat(dfd, relname, 0) == 0, "unlinkat dirfd relative",
          "unlinkat(dfd, relative, 0) must remove relative to the dirfd");
    check(access(relpath, F_OK) == -1 && errno == ENOENT,
          "unlinkat dirfd relative removed",
          "the dirfd-relative target must be gone after unlinkat");
    (void) close(dfd);

#define EXPECT_UNLINKAT_FAIL(tag, dirfd, target, flags, err, detail)          \
    do {                                                                      \
        int rc_##tag;                                                         \
                                                                              \
        errno = 0;                                                            \
        rc_##tag = unlinkat((dirfd), (target), (flags));                      \
        check(rc_##tag == -1 && errno == (err), "unlinkat " #tag, (detail));  \
    } while (0)

    EXPECT_UNLINKAT_FAIL(missing_path, AT_FDCWD, "/no/such/path", 0, ENOENT,
                         "unlinkat on a missing path must fail ENOENT");
    EXPECT_UNLINKAT_FAIL(bad_dirfd, 4095, relname, 0, EBADF,
                         "a relative unlinkat on an invalid descriptor must "
                         "fail EBADF");

    fd = creat(path, 0600);
    if (fd < 0) {
        test_fail("unlinkat AT_REMOVEDIR fixture", strerror(errno));
        return;
    }
    (void) close(fd);
    errno = 0;
    check(unlinkat(AT_FDCWD, path, AT_REMOVEDIR) == -1 && errno == ENOTDIR,
          "unlinkat AT_REMOVEDIR non-directory",
          "AT_REMOVEDIR on a non-directory must fail ENOTDIR");
    check(access(path, F_OK) == 0, "unlinkat AT_REMOVEDIR preserves file",
          "failing closed for AT_REMOVEDIR must not unlink a non-directory");
    (void) unlink(path);

    EXPECT_UNLINKAT_FAIL(at_removedir_missing_path, AT_FDCWD, "/no/such/path",
                         AT_REMOVEDIR, ENOENT,
                         "AT_REMOVEDIR on a missing path must fail ENOENT");
    EXPECT_UNLINKAT_FAIL(at_removedir_directory_unsupported, AT_FDCWD,
                         "/dev/shmem", AT_REMOVEDIR, ENOSYS,
                         "native directory removal is unsupported -> ENOSYS");
    EXPECT_UNLINKAT_FAIL(bad_flag, AT_FDCWD, path, 0x0400, EINVAL,
                         "unlinkat must reject an undefined AT_ flag with "
                         "EINVAL");

#undef EXPECT_UNLINKAT_FAIL
}

/* opendir() on a non-directory is ENOTDIR (Futura getdents/readdir wrong-type
 * corner). A device node resolves and opens read-only, but the directory
 * iterator must reject it once the stat shows a non-directory type.
 */
static void test_posix_opendir_wrong_type(void)
{
    DIR *dp;

    errno = 0;
    dp = opendir("/dev/null");
    check(dp == NULL && errno == ENOTDIR, "opendir non-directory",
          "opendir on a non-directory must fail ENOTDIR");
    if (dp)
        (void) closedir(dp);

    /* Control: a real directory still opens. */
    dp = opendir("/dev");
    check(dp != NULL, "opendir directory control",
          "opendir on a directory must succeed");
    if (dp)
        (void) closedir(dp);
}

/* A directory opens read-only (that is the descriptor opendir() wraps) but
 * cannot be opened for writing: O_WRONLY and O_RDWR must fail with EISDIR.
 */
static void test_posix_open_directory_write_is_eisdir(void)
{
    struct stat st;
    int fd;

    /* Anchor the target: /dev/shmem opens read-only and is truly a directory,
     * so the write failures below are about directory-ness, not a bad path.
     */
    fd = open("/dev/shmem", O_RDONLY);
    check(fd != -1 && fstat(fd, &st) == 0 && S_ISDIR(st.st_mode),
          "open directory O_RDONLY control",
          "/dev/shmem must open read-only as a directory");
    if (fd != -1)
        (void) close(fd);

    errno = 0;
    fd = open("/dev/shmem", O_WRONLY);
    check(fd == -1 && errno == EISDIR, "open directory O_WRONLY",
          "opening a directory for writing must fail EISDIR");
    if (fd != -1)
        (void) close(fd);

    errno = 0;
    fd = open("/dev/shmem", O_RDWR);
    check(fd == -1 && errno == EISDIR, "open directory O_RDWR",
          "opening a directory for read-write must fail EISDIR");
    if (fd != -1)
        (void) close(fd);
}

/* The same EISDIR contract must hold for namespace directories (/dev, /), which
 * the roottask path server resolves and binds through a different code path
 * (path/open.c) than the devmem /dev/shmem root above.
 */
static void test_posix_open_namespace_directory_write_is_eisdir(void)
{
    struct stat st;
    int fd;

    fd = open("/dev", O_RDONLY);
    check(fd != -1 && fstat(fd, &st) == 0 && S_ISDIR(st.st_mode),
          "open namespace directory O_RDONLY control",
          "/dev must open read-only as a directory");
    if (fd != -1)
        (void) close(fd);

    errno = 0;
    fd = open("/dev", O_WRONLY);
    check(fd == -1 && errno == EISDIR, "open namespace directory O_WRONLY",
          "opening a namespace directory for writing must fail EISDIR");
    if (fd != -1)
        (void) close(fd);

    errno = 0;
    fd = open("/dev", O_RDWR);
    check(fd == -1 && errno == EISDIR, "open namespace directory O_RDWR",
          "opening a namespace directory for read-write must fail EISDIR");
    if (fd != -1)
        (void) close(fd);

    /* O_TRUNC is write intent even under O_RDONLY: it too must fail EISDIR on a
     * directory, matching the cpiofs directory handling.
     */
    errno = 0;
    fd = open("/dev", O_RDONLY | O_TRUNC);
    check(fd == -1 && errno == EISDIR, "open namespace directory O_TRUNC",
          "truncating a namespace directory must fail EISDIR");
    if (fd != -1)
        (void) close(fd);

    /* The root "/" and cpio subdirectories are served by the cpiofs root mount,
     * a distinct code path from the namespace directories above.
     */
    errno = 0;
    fd = open("/", O_RDWR);
    check(fd == -1 && errno == EISDIR, "open root directory O_RDWR",
          "opening the root directory for read-write must fail EISDIR");
    if (fd != -1)
        (void) close(fd);

    errno = 0;
    fd = open("/bin", O_WRONLY);
    check(fd == -1 && errno == EISDIR, "open cpiofs subdirectory O_WRONLY",
          "opening a cpiofs subdirectory for writing must fail EISDIR");
    if (fd != -1)
        (void) close(fd);
}

/* cpiofs is a read-only filesystem, so opening one of its regular files for
 * writing must fail EROFS at open time rather than deferring to a later write.
 * O_CREAT on the read-only cpio filesystem: a create fails EROFS once the path
 * prefix resolves to an existing directory, but path-resolution errors take
 * precedence -- a missing prefix component is ENOENT, a non-directory one is
 * ENOTDIR. /bin is a cpio directory and /bin/true is a cpio regular file.
 */
static void test_posix_open_readonly_fs_create_precedence(void)
{
    int fd;

    /* Valid parent (/bin is a directory), missing leaf -> EROFS. */
    errno = 0;
    fd = open("/bin/rtmux-no-such-leaf", O_WRONLY | O_CREAT, 0644);
    check(fd == -1 && errno == EROFS, "cpiofs create valid parent",
          "O_CREAT under an existing read-only directory must fail EROFS");
    if (fd != -1)
        (void) close(fd);

    /* Missing prefix component -> ENOENT (not EROFS). */
    errno = 0;
    fd = open("/bin/rtmux-no-such-dir/leaf", O_WRONLY | O_CREAT, 0644);
    check(fd == -1 && errno == ENOENT, "cpiofs create missing prefix",
          "O_CREAT under a missing path component must fail ENOENT");
    if (fd != -1)
        (void) close(fd);

    /* Non-directory prefix component (/bin/true is a file) -> ENOTDIR. */
    errno = 0;
    fd = open("/bin/true/rtmux-leaf", O_WRONLY | O_CREAT, 0644);
    check(fd == -1 && errno == ENOTDIR, "cpiofs create non-dir prefix",
          "O_CREAT under a non-directory path component must fail ENOTDIR");
    if (fd != -1)
        (void) close(fd);
}

static void test_posix_open_readonly_fs_write_is_erofs(void)
{
    struct stat st;
    int fd;

    fd = open("/bin/true", O_RDONLY);
    check(fd != -1 && fstat(fd, &st) == 0 && S_ISREG(st.st_mode),
          "open read-only file control",
          "/bin/true must open read-only as a regular file");
    if (fd != -1)
        (void) close(fd);

    errno = 0;
    fd = open("/bin/true", O_WRONLY);
    check(fd == -1 && errno == EROFS, "open read-only fs file O_WRONLY",
          "writing a file on a read-only filesystem must fail EROFS");
    if (fd != -1)
        (void) close(fd);

    errno = 0;
    fd = open("/bin/true", O_RDWR);
    check(fd == -1 && errno == EROFS, "open read-only fs file O_RDWR",
          "read-write of a file on a read-only filesystem must fail EROFS");
    if (fd != -1)
        (void) close(fd);

    /* O_TRUNC is write intent even with O_RDONLY: truncating a file on a
     * read-only filesystem must also fail EROFS.
     */
    errno = 0;
    fd = open("/bin/true", O_RDONLY | O_TRUNC);
    check(fd == -1 && errno == EROFS, "open read-only fs file O_TRUNC",
          "truncating a file on a read-only filesystem must fail EROFS");
    if (fd != -1)
        (void) close(fd);
}

/* capfs (/cap, read-only capability introspection) exposes a directory (/cap)
 * and a regular file (/cap/self). Write intent must fail at open with the
 * type-specific errno -- the directory EISDIR, the file EROFS -- and O_TRUNC
 * counts as write intent. A non-existent path is ENOENT without O_CREAT and
 * EROFS with it (creation on a read-only filesystem). A trailing slash requires
 * the path to name a directory (POSIX pathname resolution): open() of a
 * non-directory with a trailing slash must fail ENOTDIR. The resolver sets
 * _IO_CONNECT_EFLAG_DIR for a trailing slash; the I/O servers must honor it. A
 * directory with a trailing slash still opens. romfs (the read-only /usr mount)
 * has the same read-only write-open contract: write intent must fail at open
 * with the type-specific errno (directory EISDIR, file EROFS), a non-existent
 * path opened for write without O_CREAT is ENOENT, and a trailing slash on a
 * non-directory is ENOTDIR. /usr/conf is a directory and /usr/conf/passwd a
 * regular file (shipped credentials).
 */
static void test_posix_open_romfs_write_semantics(void)
{
    struct stat st;
    int fd;

    fd = open("/usr/conf", O_RDONLY);
    check(fd != -1 && fstat(fd, &st) == 0 && S_ISDIR(st.st_mode),
          "romfs dir control", "/usr/conf must open read-only as a directory");
    if (fd != -1)
        (void) close(fd);

    fd = open("/usr/conf/passwd", O_RDONLY);
    check(fd != -1 && fstat(fd, &st) == 0 && S_ISREG(st.st_mode),
          "romfs file control",
          "/usr/conf/passwd must open read-only as a regular file");
    if (fd != -1)
        (void) close(fd);

    errno = 0;
    fd = open("/usr/conf", O_WRONLY);
    check(fd == -1 && errno == EISDIR, "romfs dir O_WRONLY",
          "writing a romfs directory must fail EISDIR");
    if (fd != -1)
        (void) close(fd);

    errno = 0;
    fd = open("/usr/conf/passwd", O_WRONLY);
    check(fd == -1 && errno == EROFS, "romfs file O_WRONLY",
          "writing a romfs file must fail EROFS");
    if (fd != -1)
        (void) close(fd);

    errno = 0;
    fd = open("/usr/conf/passwd/", O_RDONLY);
    check(fd == -1 && errno == ENOTDIR, "romfs trailing slash on file",
          "a trailing slash on a romfs file must fail ENOTDIR");
    if (fd != -1)
        (void) close(fd);

    errno = 0;
    fd = open("/usr/rtmux-no-such", O_WRONLY);
    check(fd == -1 && errno == ENOENT, "romfs missing path write",
          "a non-existent romfs path opened for write must fail ENOENT");
    if (fd != -1)
        (void) close(fd);

    /* O_CREAT|O_EXCL on an existing path is EEXIST, not a successful open. */
    errno = 0;
    fd = open("/usr/conf/passwd", O_WRONLY | O_CREAT | O_EXCL, 0644);
    check(fd == -1 && errno == EEXIST, "romfs O_CREAT|O_EXCL existing",
          "O_CREAT|O_EXCL on an existing path must fail EEXIST");
    if (fd != -1)
        (void) close(fd);

    /* O_CREAT prefix precedence: a valid parent (existing directory) gives
     * EROFS, a missing prefix component ENOENT, a non-directory one ENOTDIR.
     */
    errno = 0;
    fd = open("/usr/conf/rtmux-no-such-leaf", O_WRONLY | O_CREAT, 0644);
    check(fd == -1 && errno == EROFS, "romfs create valid parent",
          "O_CREAT under an existing romfs directory must fail EROFS");
    if (fd != -1)
        (void) close(fd);

    errno = 0;
    fd = open("/usr/rtmux-no-such-dir/leaf", O_WRONLY | O_CREAT, 0644);
    check(fd == -1 && errno == ENOENT, "romfs create missing prefix",
          "O_CREAT under a missing romfs path component must fail ENOENT");
    if (fd != -1)
        (void) close(fd);

    errno = 0;
    fd = open("/usr/conf/passwd/leaf", O_WRONLY | O_CREAT, 0644);
    check(fd == -1 && errno == ENOTDIR, "romfs create non-dir prefix",
          "O_CREAT under a non-directory romfs component must fail ENOTDIR");
    if (fd != -1)
        (void) close(fd);
}

static void test_posix_open_trailing_slash_non_directory(void)
{
    int fd;

    fd = open("/bin/", O_RDONLY);
    check(fd != -1, "trailing slash on directory",
          "a trailing slash on a directory must still open");
    if (fd != -1)
        (void) close(fd);

    errno = 0;
    fd = open("/bin/true/", O_RDONLY);
    check(fd == -1 && errno == ENOTDIR, "trailing slash on cpiofs file",
          "a trailing slash on a regular file must fail ENOTDIR");
    if (fd != -1)
        (void) close(fd);

    errno = 0;
    fd = open("/dev/null/", O_RDONLY);
    check(fd == -1 && errno == ENOTDIR, "trailing slash on device node",
          "a trailing slash on a device node must fail ENOTDIR");
    if (fd != -1)
        (void) close(fd);
}

/* O_CREAT|O_EXCL on an existing path must fail EEXIST regardless of the file's
 * access mode or the read-only medium -- the file already exists, so exclusive
 * creation cannot proceed. Covers the read-only filesystems whose open handlers
 * resolve the path but did not check O_EXCL (capfs, cpiofs). fstat reports the
 * file type in st_mode. Programs routinely branch on S_ISFIFO/S_ISCHR/etc., so
 * confirm the pipe and character-device types are reported correctly (directory
 * and regular-file types are already covered by the open-semantics controls
 * above).
 */
static void test_posix_fstat_file_types(void)
{
    struct stat st;
    int fds[2];
    int fd;

    if (pipe(fds) == 0) {
        check(fstat(fds[0], &st) == 0 && S_ISFIFO(st.st_mode),
              "fstat pipe is a FIFO",
              "fstat on a pipe end must report S_ISFIFO");
        (void) close(fds[0]);
        (void) close(fds[1]);
    } else {
        test_fail("fstat pipe fixture", strerror(errno));
    }

    fd = open("/dev/null", O_RDONLY);
    if (fd != -1) {
        check(fstat(fd, &st) == 0 && S_ISCHR(st.st_mode),
              "fstat /dev/null is a char device",
              "fstat on a character device must report S_ISCHR");
        (void) close(fd);
    } else {
        test_fail("fstat devnull fixture", strerror(errno));
    }
}

/* poll() reports descriptor readiness via the _IO_NOTIFY protocol. A fresh pipe
 * read end is not POLLIN-ready until data is written; its write end is
 * POLLOUT-ready (buffer space). A regular file has no readiness service and is
 * always ready. A negative descriptor is ignored. (This covers the non-blocking
 * step; blocking poll with a timeout is a follow-up.)
 */
static void test_posix_poll_readiness(void)
{
    struct pollfd pfd;
    int fds[2];
    int fd;

    if (pipe(fds) != 0) {
        test_fail("poll pipe fixture", strerror(errno));
        return;
    }

    pfd.fd = fds[0];
    pfd.events = POLLIN;
    pfd.revents = 0;
    check(poll(&pfd, 1, 0) == 0 && (pfd.revents & POLLIN) == 0,
          "poll empty pipe not readable",
          "an empty pipe read end must not be POLLIN-ready");

    pfd.fd = fds[1];
    pfd.events = POLLOUT;
    pfd.revents = 0;
    check(poll(&pfd, 1, 0) == 1 && (pfd.revents & POLLOUT),
          "poll pipe write end writable",
          "a pipe write end with buffer space must be POLLOUT-ready");

    if (write(fds[1], "x", 1) == 1) {
        pfd.fd = fds[0];
        pfd.events = POLLIN;
        pfd.revents = 0;
        check(poll(&pfd, 1, 0) == 1 && (pfd.revents & POLLIN),
              "poll pipe read end readable",
              "a pipe read end with data must be POLLIN-ready");
    }

    (void) close(fds[0]);
    (void) close(fds[1]);

    fd = open("/bin/true", O_RDONLY);
    if (fd != -1) {
        pfd.fd = fd;
        pfd.events = POLLIN;
        pfd.revents = 0;
        check(poll(&pfd, 1, 0) == 1 && (pfd.revents & POLLIN),
              "poll regular file readable",
              "a regular file is always POLLIN-ready");
        (void) close(fd);
    }

    pfd.fd = -1;
    pfd.events = POLLIN;
    pfd.revents = 0x7f;
    check(poll(&pfd, 1, 0) == 0 && pfd.revents == 0,
          "poll ignores negative fd",
          "a negative descriptor must be ignored with revents 0");
}

/* /dev/null and /dev/zero never block, so poll reports them readable and
 * writable at once and select counts them ready; a condition they never raise
 * (POLLPRI) waits out its timeout instead of reporting POLLERR.
 */
static void test_posix_poll_null_devices(void)
{
    static const char *const devs[] = {"/dev/null", "/dev/zero"};

    for (size_t i = 0; i < sizeof devs / sizeof devs[0]; i++) {
        struct pollfd pfd;
        fd_set rset, wset;
        struct timeval tv = {0};
        char name[64];
        int fd = open(devs[i], O_RDWR);

        if (fd < 0) {
            snprintf(name, sizeof name, "poll %s open", devs[i]);
            test_fail(name, strerror(errno));
            continue;
        }
        pfd = (struct pollfd){.fd = fd, .events = POLLIN | POLLOUT};
        snprintf(name, sizeof name, "poll %s readable and writable", devs[i]);
        check(poll(&pfd, 1, 0) == 1 &&
                  pfd.revents == (POLLRDNORM | POLLOUT),
              name,
              "a device that never blocks must poll POLLRDNORM|POLLOUT only");

        pfd = (struct pollfd){.fd = fd, .events = POLLPRI};
        snprintf(name, sizeof name, "poll %s POLLPRI times out", devs[i]);
        check(poll(&pfd, 1, 50) == 0 && pfd.revents == 0, name,
              "a condition the device never raises must not report POLLERR");

        FD_ZERO(&rset);
        FD_ZERO(&wset);
        FD_SET(fd, &rset);
        FD_SET(fd, &wset);
        snprintf(name, sizeof name, "select %s ready", devs[i]);
        check(select(fd + 1, &rset, &wset, NULL, &tv) == 2 &&
                  FD_ISSET(fd, &rset) && FD_ISSET(fd, &wset),
              name, "select must count a never-blocking device ready both ways");
        (void) close(fd);
    }
}

/* A service in this process whose _IO_NOTIFY handler misbehaves on demand, to
 * check how poll() takes a service that runs out of memory, and that a poll
 * cancelled while it waits takes its arms away.
 */
#define POLL_FAULT_PATH "/poll-fault-test"

enum {
    POLL_FAULT_NOMEM_ALL, /* every _IO_NOTIFY fails with ENOMEM */
    POLL_FAULT_NOMEM_ARM, /* not ready; recording an arm fails with ENOMEM */
    POLL_FAULT_WAIT,      /* not ready; arms are recorded and never fire */
};

static atomic_int poll_fault_mode;
static atomic_int poll_fault_arms;    /* arms recorded in POLL_FAULT_WAIT */
static atomic_int poll_fault_disarms; /* POLL passes seen in POLL_FAULT_WAIT */
static int poll_fault_ocb;

static int poll_fault_open(ioserv_context_t *ctp,
                           io_open_t *msg,
                           void *handle,
                           void *extra)
{
    (void) msg;
    (void) handle;
    (void) extra;
    return ioserv_open_bind(ctp, &poll_fault_ocb, NULL) == -1 ? errno : EOK;
}

static int poll_fault_notify(ioserv_context_t *ctp, io_notify_t *msg, void *ocb)
{
    int mode = atomic_load(&poll_fault_mode);
    int action = msg->i.action;

    (void) ocb;
    if (mode == POLL_FAULT_NOMEM_ALL)
        return ENOMEM;
    if (action == _NOTIFY_ACTION_CONDARM) {
        if (mode == POLL_FAULT_NOMEM_ARM)
            return ENOMEM;
        atomic_fetch_add(&poll_fault_arms, 1);
        return EAGAIN;
    }
    if (action == _NOTIFY_ACTION_POLL && mode == POLL_FAULT_WAIT)
        atomic_fetch_add(&poll_fault_disarms, 1);
    memset(&msg->o, 0, sizeof msg->o);
    return IOSERV_PTR(ctp, &msg->o, sizeof msg->o);
}

static const ioserv_connect_ops_t poll_fault_connect_ops = {
    .nfuncs = IOSERV_CONNECT_NFUNCS,
    .open = poll_fault_open,
};

static const ioserv_ops_t poll_fault_io_ops = {
    .nfuncs = IOSERV_IO_NFUNCS,
    .notify = poll_fault_notify,
};

static void *poll_fault_server(void *raw)
{
    dispatch_context_t *ctp = dispatch_context_alloc(raw);

    while (ctp && (ctp = dispatch_block(ctp)))
        (void) dispatch_handler(ctp);
    return NULL;
}

static void *poll_fault_waiter(void *raw)
{
    struct pollfd pfd = {.fd = *(int *) raw, .events = POLLIN};

    (void) poll(&pfd, 1, 20000);
    return NULL;
}

static void test_posix_poll_service_faults(void)
{
    struct pollfd pfd;
    fd_set rset;
    struct timeval tv = {0};
    dispatch_t *dpp;
    pthread_t server, waiter;
    int id, fd, rc, err, arms, disarms;

    if (!(dpp = dispatch_create())) {
        test_fail("poll fault service dispatch", strerror(errno));
        return;
    }
    /* SELF: roottask resolves a path to a server in the caller's own process
     * only for a mount that asks for it, and the client here is that process.
     */
    id = ioserv_attach(dpp, NULL, POLL_FAULT_PATH, _FTYPE_ANY,
                       IOSERV_FLAG_DIR | IOSERV_FLAG_SELF,
                       &poll_fault_connect_ops, &poll_fault_io_ops, NULL);
    if (id == -1) {
        test_skip("poll fault service", "cannot attach a test path");
        (void) dispatch_destroy(dpp);
        return;
    }
    if (pthread_create(&server, NULL, poll_fault_server, dpp) != 0) {
        test_fail("poll fault service thread", "pthread_create failed");
        (void) ioserv_detach(dpp, id, 0);
        (void) dispatch_destroy(dpp);
        return;
    }
    fd = open(POLL_FAULT_PATH "/node", O_RDONLY);
    if (fd < 0) {
        test_fail("poll fault service open", strerror(errno));
        goto out;
    }

    /* Out of memory answering the poll pass: an error, not POLLERR. */
    atomic_store(&poll_fault_mode, POLL_FAULT_NOMEM_ALL);
    pfd = (struct pollfd){.fd = fd, .events = POLLIN};
    errno = 0;
    rc = poll(&pfd, 1, 0);
    err = errno;
    check(rc == -1 && err == EAGAIN, "poll service ENOMEM fails with EAGAIN",
          "a service out of memory must not be reported as POLLERR");
    FD_ZERO(&rset);
    FD_SET(fd, &rset);
    errno = 0;
    rc = select(fd + 1, &rset, NULL, NULL, &tv);
    err = errno;
    check(rc == -1 && err == EAGAIN, "select service ENOMEM fails with EAGAIN",
          "select must not count a service out of memory as ready");

    /* Out of memory recording the arm of a blocking wait. */
    atomic_store(&poll_fault_mode, POLL_FAULT_NOMEM_ARM);
    pfd = (struct pollfd){.fd = fd, .events = POLLIN};
    errno = 0;
    rc = poll(&pfd, 1, 100);
    err = errno;
    check(rc == -1 && err == EAGAIN, "poll arm ENOMEM fails with EAGAIN",
          "a service that cannot record an arm must not be reported as POLLERR");

    /* A poll cancelled while it waits disarms what it armed. */
    atomic_store(&poll_fault_mode, POLL_FAULT_WAIT);
    atomic_store(&poll_fault_arms, 0);
    atomic_store(&poll_fault_disarms, 0);
    if (pthread_create(&waiter, NULL, poll_fault_waiter, &fd) != 0) {
        test_fail("poll cancel waiter", "pthread_create failed");
        goto out_fd;
    }
    for (int i = 0; i < 100 && atomic_load(&poll_fault_arms) == 0; i++)
        sleep_ms(10);
    sleep_ms(50);
    disarms = atomic_load(&poll_fault_disarms);
    (void) pthread_cancel(waiter);
    (void) pthread_join(waiter, NULL);
    arms = atomic_load(&poll_fault_arms);
    check(arms == 1 && atomic_load(&poll_fault_disarms) > disarms,
          "poll cancelled while waiting disarms",
          "a cancelled poll must send the disarm pass for what it armed");
out_fd:
    (void) close(fd);
out:
    (void) ioserv_detach(dpp, id, 0);
    (void) pthread_cancel(server);
    (void) pthread_join(server, NULL);
    (void) dispatch_destroy(dpp);
}

/* A service in this process that answers _IO_NOTIFY with the real
 * ioserv_notify, to check what it arms: a hangup already known is the reply, so
 * nothing is armed for it, whether the request is a condition arm or a
 * transition arm; with no hangup the same requests are armed.
 */
#define HUP_ARM_PATH "/hup-arm-test"

static ioserv_notify_t hup_arm_lists[IOSERV_NOTIFY_COUNT];
static atomic_uint hup_arm_hup;   /* the hangup codes the service reports */
static atomic_int hup_arm_armed;  /* lists armed after the last request */
static int hup_arm_ocb;

static int hup_arm_open(ioserv_context_t *ctp,
                        io_open_t *msg,
                        void *handle,
                        void *extra)
{
    (void) msg;
    (void) handle;
    (void) extra;
    return ioserv_open_bind(ctp, &hup_arm_ocb, NULL) == -1 ? errno : EOK;
}

static int hup_arm_notify(ioserv_context_t *ctp, io_notify_t *msg, void *ocb)
{
    int ret, armed = 0;

    (void) ocb;
    ret = ioserv_notify(ctp, msg, hup_arm_lists, 0, atomic_load(&hup_arm_hup),
                        NULL);
    for (int i = 0; i < IOSERV_NOTIFY_COUNT; i++)
        armed += ioserv_notify_armed(hup_arm_lists, i, INT_MAX) != 0;
    atomic_store(&hup_arm_armed, armed);
    return ret;
}

static const ioserv_connect_ops_t hup_arm_connect_ops = {
    .nfuncs = IOSERV_CONNECT_NFUNCS,
    .open = hup_arm_open,
};

static const ioserv_ops_t hup_arm_io_ops = {
    .nfuncs = IOSERV_IO_NFUNCS,
    .notify = hup_arm_notify,
};

/* Send fd an _IO_NOTIFY of the given action that asks for input and carries a
 * pulse event. Returns 0 or the errno of the send.
 */
static int hup_arm_send(int fd, int action, unsigned *hup)
{
    io_notify_t msg = {0};

    msg.i.type = _IO_NOTIFY;
    msg.i.combine_len = sizeof msg.i;
    msg.i.action = (uint16_t) action;
    msg.i.flags = (unsigned) _NOTIFY_COND_INPUT;
    SIGEV_NOTIFICATION_INIT(&msg.i.event, fd, -1, 0, 0);
    if (msg_sendnc(fd, &msg.i, sizeof msg.i, &msg.o, sizeof msg.o) == -1)
        return errno;
    *hup = msg.o.flags_exten;
    return 0;
}

static void test_notify_hangup_arm(void)
{
    static const struct {
        int action;
        const char *name;
    } kinds[] = {
        {_NOTIFY_ACTION_CONDARM, "condition arm"},
        {_NOTIFY_ACTION_TRANARM, "transition arm"},
    };
    dispatch_t *dpp;
    pthread_t server;
    char what[96];
    int id, fd;

    if (!(dpp = dispatch_create())) {
        test_fail("hangup arm service dispatch", strerror(errno));
        return;
    }
    id = ioserv_attach(dpp, NULL, HUP_ARM_PATH, _FTYPE_ANY,
                       IOSERV_FLAG_DIR | IOSERV_FLAG_SELF,
                       &hup_arm_connect_ops, &hup_arm_io_ops, NULL);
    if (id == -1) {
        test_skip("hangup arm service", "cannot attach a test path");
        (void) dispatch_destroy(dpp);
        return;
    }
    if (pthread_create(&server, NULL, poll_fault_server, dpp) != 0) {
        test_fail("hangup arm service thread", "pthread_create failed");
        (void) ioserv_detach(dpp, id, 0);
        (void) dispatch_destroy(dpp);
        return;
    }
    fd = open(HUP_ARM_PATH "/node", O_RDONLY);
    if (fd < 0) {
        test_fail("hangup arm service open", strerror(errno));
        goto out;
    }
    /* Both kinds, with a hangup and without. Only one transition arm may be
     * held on the input, and a condition arm leaves a node there, so the
     * transition arm comes first when something is armed.
     */
    for (int hung = 1; hung >= 0; hung--) {
        for (size_t n = 0; n < sizeof kinds / sizeof *kinds; n++) {
            size_t k = hung ? n : sizeof kinds / sizeof *kinds - 1 - n;
            unsigned hup = 0;
            int err, armed;

            atomic_store(&hup_arm_hup, hung ? (unsigned) POLLHUP : 0u);
            atomic_store(&hup_arm_armed, -1);
            err = hup_arm_send(fd, kinds[k].action, &hup);
            armed = atomic_load(&hup_arm_armed);
            snprintf(what, sizeof what, "hangup %s %s",
                     hung ? "known: no arm for a" : "unknown: an arm for a",
                     kinds[k].name);
            if (hung)
                check((err == 0 && (hup & POLLHUP) && armed == 0), what,
                      "a hangup already known must be the reply, with nothing "
                      "armed");
            else
                check((err == EAGAIN || err == 0) && armed == 1, what,
                      "without a hangup the request must be armed");
        }
    }
    (void) close(fd);
out:
    (void) ioserv_detach(dpp, id, 0);
    (void) pthread_cancel(server);
    (void) pthread_join(server, NULL);
    (void) dispatch_destroy(dpp);
}

/* Make a pipe read end readable ~50 ms from now, while a blocked poll waits. */
static void *poll_wake_writer(void *raw)
{
    int wfd = *(int *) raw;
    const struct timespec delay = {.tv_nsec = 50 * 1000 * 1000L};

    (void) nanosleep(&delay, NULL);
    (void) write(wfd, "x", 1);
    return NULL;
}

/* Blocking poll: a bounded wait on a quiet descriptor times out to 0, and a
 * wait wakes with POLLIN once a writer makes the descriptor readable.
 */
static void test_posix_poll_blocking(void)
{
    struct pollfd pfd;
    int fds[2];
    pthread_t tid;
    int rc;

    if (pipe(fds) != 0) {
        test_fail("poll blocking pipe fixture", strerror(errno));
        return;
    }

    pfd.fd = fds[0];
    pfd.events = POLLIN;
    pfd.revents = 0;
    rc = poll(&pfd, 1, 50);
    check(rc == 0 && pfd.revents == 0,
          "poll blocks then times out",
          "a bounded poll on a quiet pipe read end must return 0");

    if (pthread_create(&tid, NULL, poll_wake_writer, &fds[1]) != 0) {
        test_fail("poll wake writer", "pthread_create failed");
        (void) close(fds[0]);
        (void) close(fds[1]);
        return;
    }
    pfd.fd = fds[0];
    pfd.events = POLLIN;
    pfd.revents = 0;
    rc = poll(&pfd, 1, 2000);
    check(rc == 1 && (pfd.revents & POLLIN),
          "poll blocks then wakes on readiness",
          "a blocked poll must wake with POLLIN when data arrives");
    (void) pthread_join(tid, NULL);

    (void) close(fds[0]);
    (void) close(fds[1]);
}

/* Close a pipe write end ~50 ms from now, while a blocked poll waits. */
static void *poll_hup_closer(void *raw)
{
    int wfd = *(int *) raw;
    const struct timespec delay = {.tv_nsec = 50 * 1000 * 1000L};

    (void) nanosleep(&delay, NULL);
    (void) close(wfd);
    return NULL;
}

/* POLLHUP/POLLERR: a pipe read end whose writer closed reports POLLHUP; a write
 * end whose reader closed reports POLLERR; and a blocked poll wakes on hangup.
 */
static void test_posix_poll_hup(void)
{
    struct pollfd pfd;
    pthread_t tid;
    int fds[2];

    if (pipe(fds) != 0) {
        test_fail("poll hup pipe fixture", strerror(errno));
        return;
    }
    (void) close(fds[1]); /* drop the only writer */
    pfd.fd = fds[0];
    pfd.events = POLLIN;
    pfd.revents = 0;
    check(poll(&pfd, 1, 0) == 1 && (pfd.revents & POLLHUP),
          "poll reports POLLHUP on writer close",
          "a pipe read end whose writer closed must report POLLHUP");
    (void) close(fds[0]);

    if (pipe(fds) == 0) {
        (void) close(fds[0]); /* drop the only reader */
        pfd.fd = fds[1];
        pfd.events = POLLOUT;
        pfd.revents = 0;
        check(poll(&pfd, 1, 0) >= 1 && (pfd.revents & POLLERR),
              "poll reports POLLERR on reader close",
              "a pipe write end whose reader closed must report POLLERR");
        (void) close(fds[1]);
    }

    if (pipe(fds) == 0) {
        if (pthread_create(&tid, NULL, poll_hup_closer, &fds[1]) == 0) {
            pfd.fd = fds[0];
            pfd.events = POLLIN;
            pfd.revents = 0;
            check(poll(&pfd, 1, 2000) == 1 && (pfd.revents & POLLHUP),
                  "poll blocks then wakes on hup",
                  "a blocked poll must wake with POLLHUP when the writer closes");
            (void) pthread_join(tid, NULL);
        }
        (void) close(fds[0]);
    }
}

/* select() over poll(): non-blocking readiness, the FD_* macros, a bounded
 * timeout, and a blocking wait woken by a writer.
 */
static void test_posix_select(void)
{
    fd_set rset, wset;
    struct timeval tv;
    int fds[2];
    pthread_t tid;
    int rc;

    /* FD_* macro sanity. */
    FD_ZERO(&rset);
    FD_SET(7, &rset);
    FD_SET(31, &rset);
    check(FD_ISSET(7, &rset) && FD_ISSET(31, &rset) && !FD_ISSET(8, &rset),
          "select FD_SET/FD_ISSET", "FD_SET bits must read back via FD_ISSET");
    FD_CLR(7, &rset);
    check(!FD_ISSET(7, &rset) && FD_ISSET(31, &rset), "select FD_CLR",
          "FD_CLR must clear only the named bit");

    /* pselect with a NULL sigmask is select() with a timespec. With a temporary
     * mask, timeout, ready-fd, signal-interrupt, and error paths must restore
     * the caller's mask.
     */
    {
        struct timespec pts = {.tv_sec = 0, .tv_nsec = 0};
        sigset_t base, block, current, empty;
        struct sigaction act = {.sa_handler = pause_signal_handler};
        struct sigaction oldact;

        rc = pselect(0, NULL, NULL, NULL, &pts, NULL);
        check(rc == 0, "pselect null sigmask",
              "pselect(0, ..., NULL) must behave as a zero-timeout select");
        sigemptyset(&empty);
        errno = 0;
        rc = pselect(0, NULL, NULL, NULL, &pts, &empty);
        check(rc == 0, "pselect unchanged sigmask",
              "pselect may use select when the requested mask is unchanged");
        sigprocmask(SIG_SETMASK, NULL, &base);
        block = base;
        sigaddset(&block, SIGUSR1);
        sigprocmask(SIG_SETMASK, &block, NULL);
        errno = 0;
        rc = pselect(0, NULL, NULL, NULL, &pts, &empty);
        sigprocmask(SIG_SETMASK, NULL, &current);
        check(rc == 0 && current.bits[0] == block.bits[0] &&
                  current.bits[1] == block.bits[1],
              "pselect zero-timeout changed sigmask",
              "pselect must restore the caller mask after zero-timeout polling");
        pts.tv_nsec = 10 * 1000 * 1000L;
        errno = 0;
        rc = pselect(0, NULL, NULL, NULL, &pts, &empty);
        sigprocmask(SIG_SETMASK, NULL, &current);
        check(rc == 0 && current.bits[0] == block.bits[0] &&
                  current.bits[1] == block.bits[1],
              "pselect blocking timeout restores sigmask",
              "pselect must restore the caller mask after a blocking timeout");
        if (pipe(fds) == 0) {
            (void) write(fds[1], "r", 1);
            FD_ZERO(&rset);
            FD_SET(fds[0], &rset);
            pts.tv_sec = 1;
            pts.tv_nsec = 0;
            errno = 0;
            rc = pselect(fds[0] + 1, &rset, NULL, NULL, &pts, &empty);
            sigprocmask(SIG_SETMASK, NULL, &current);
            check(rc == 1 && FD_ISSET(fds[0], &rset) &&
                      current.bits[0] == block.bits[0] &&
                      current.bits[1] == block.bits[1],
                  "pselect ready fd restores sigmask",
                  "pselect must restore the caller mask after descriptor readiness");
            (void) close(fds[0]);
            (void) close(fds[1]);
        } else {
            test_fail("pselect ready pipe fixture", strerror(errno));
        }
        if (sigaction(SIGUSR1, &act, &oldact) == 0) {
            atomic_store(&pause_signal_seen, 0);
            pts.tv_sec = 2;
            pts.tv_nsec = 0;
            if (pthread_create(&tid, NULL, pause_signal_sender, NULL) == 0) {
                errno = 0;
                rc = pselect(0, NULL, NULL, NULL, &pts, &empty);
                sigprocmask(SIG_SETMASK, NULL, &current);
                check(rc == -1 && errno == EINTR &&
                          atomic_load(&pause_signal_seen) == 1 &&
                          current.bits[0] == block.bits[0] &&
                          current.bits[1] == block.bits[1],
                      "pselect signal interrupt restores sigmask",
                      "pselect must wake on an unblocked caught signal");
                (void) pthread_join(tid, NULL);
            } else {
                test_fail("pselect signal sender", "pthread_create failed");
            }
            (void) sigaction(SIGUSR1, &oldact, NULL);
        } else {
            test_fail("pselect signal setup", strerror(errno));
        }
        FD_ZERO(&rset);
        FD_SET(FD_SETSIZE - 1, &rset);
        pts.tv_sec = 0;
        pts.tv_nsec = 0;
        errno = 0;
        rc = pselect(FD_SETSIZE, &rset, NULL, NULL, &pts, &empty);
        sigprocmask(SIG_SETMASK, NULL, &current);
        check(rc == -1 && errno == EBADF &&
                  current.bits[0] == block.bits[0] &&
                  current.bits[1] == block.bits[1],
              "pselect EBADF restores sigmask",
              "pselect must restore the caller mask after descriptor errors");
        errno = 0;
        rc = pselect(-1, NULL, NULL, NULL, &pts, &empty);
        check(rc == -1 && errno == EINVAL, "pselect bad nfds before sigmask",
              "pselect must reject invalid nfds before the unsupported sigmask");
        pts.tv_nsec = 1000000000L;
        errno = 0;
        rc = pselect(0, NULL, NULL, NULL, &pts, &empty);
        check(rc == -1 && errno == EINVAL,
              "pselect bad timeout before sigmask",
              "pselect must reject invalid timeout before the unsupported sigmask");
        sigprocmask(SIG_SETMASK, &base, NULL);
    }

    if (pipe(fds) != 0) {
        test_fail("select pipe fixture", strerror(errno));
        return;
    }

    /* Empty pipe read end: not ready under a zero timeout. */
    FD_ZERO(&rset);
    FD_SET(fds[0], &rset);
    tv.tv_sec = 0;
    tv.tv_usec = 0;
    rc = select(fds[0] + 1, &rset, NULL, NULL, &tv);
    check(rc == 0 && !FD_ISSET(fds[0], &rset), "select empty pipe not readable",
          "an empty pipe read end must not select readable");

    /* Write end is writable now. */
    FD_ZERO(&wset);
    FD_SET(fds[1], &wset);
    tv.tv_sec = 0;
    tv.tv_usec = 0;
    rc = select(fds[1] + 1, NULL, &wset, NULL, &tv);
    check(rc == 1 && FD_ISSET(fds[1], &wset), "select pipe write end writable",
          "a pipe write end with space must select writable");

    /* After a write the read end becomes readable. */
    if (write(fds[1], "x", 1) == 1) {
        FD_ZERO(&rset);
        FD_SET(fds[0], &rset);
        tv.tv_sec = 0;
        tv.tv_usec = 0;
        rc = select(fds[0] + 1, &rset, NULL, NULL, &tv);
        check(rc == 1 && FD_ISSET(fds[0], &rset), "select pipe read end readable",
              "a pipe read end with data must select readable");
        (void) read(fds[0], &rc, 1);
    }

    /* Bounded wait on a quiet read end returns 0. */
    FD_ZERO(&rset);
    FD_SET(fds[0], &rset);
    tv.tv_sec = 0;
    tv.tv_usec = 50000;
    rc = select(fds[0] + 1, &rset, NULL, NULL, &tv);
    check(rc == 0 && !FD_ISSET(fds[0], &rset), "select blocks then times out",
          "a bounded select on a quiet pipe must return 0");

    /* A writer makes the read end readable while select blocks. */
    if (pthread_create(&tid, NULL, poll_wake_writer, &fds[1]) != 0) {
        test_fail("select wake writer", "pthread_create failed");
        (void) close(fds[0]);
        (void) close(fds[1]);
        return;
    }
    FD_ZERO(&rset);
    FD_SET(fds[0], &rset);
    tv.tv_sec = 2;
    tv.tv_usec = 0;
    rc = select(fds[0] + 1, &rset, NULL, NULL, &tv);
    check(rc == 1 && FD_ISSET(fds[0], &rset), "select blocks then wakes",
          "a blocked select must wake readable when data arrives");
    (void) pthread_join(tid, NULL);

    (void) close(fds[0]);
    (void) close(fds[1]);
}

/* poll() and select() readiness on a terminal (dev-serial-8250 at /dev/ser1).
 * Run by the serial-unblock mode, whose harness writes each byte asked for by
 * serial_inject() to the UART within about a second. Covers an idle line
 * timing out, a close while polled, two pollers waking on the same byte, a
 * queued byte being reported at once, and a partial canonical line staying
 * unreadable until its terminator arrives.
 */
#define SERIAL_POLL_DEV "/dev/ser1"

/* How a serial poller waits: poll() on one entry, select(), or poll() with a
 * second entry for the same descriptor asking only for POLLPRI, which a
 * terminal never raises and which must not cancel the first entry's arm.
 */
enum { SERIAL_WAIT_POLL, SERIAL_WAIT_SELECT, SERIAL_WAIT_POLL_REPEAT };

struct serial_poller {
    int fd;
    int how;
    int timeout_ms;
    atomic_int done;
    int rc;
    int ready; /* 1 readable, 0 not, -1 POLLNVAL */
};

/* One readability wait on fd, in the given SERIAL_WAIT_* way. */
static int serial_poll_once(int fd, int how, int timeout_ms, int *ready)
{
    int rc;

    if (how == SERIAL_WAIT_SELECT) {
        struct timeval tv = {.tv_sec = timeout_ms / 1000,
                             .tv_usec = (timeout_ms % 1000) * 1000};
        fd_set rset;

        FD_ZERO(&rset);
        FD_SET(fd, &rset);
        rc = select(fd + 1, &rset, NULL, NULL, &tv);
        *ready = rc > 0 && FD_ISSET(fd, &rset);
    } else {
        struct pollfd pfd[2] = {{.fd = fd, .events = POLLIN},
                                {.fd = fd, .events = POLLPRI}};

        rc = poll(pfd, how == SERIAL_WAIT_POLL_REPEAT ? 2 : 1, timeout_ms);
        *ready = rc > 0 && (pfd[0].revents & POLLIN);
        if (rc > 0 && (pfd[0].revents & POLLNVAL))
            *ready = -1;
    }
    return rc;
}

static void *serial_poller_thread(void *raw)
{
    struct serial_poller *p = raw;

    p->rc = serial_poll_once(p->fd, p->how, p->timeout_ms, &p->ready);
    atomic_store(&p->done, 1);
    return NULL;
}

static int serial_poller_start(pthread_t *tid, struct serial_poller *p)
{
    atomic_init(&p->done, 0);
    if (pthread_create(tid, NULL, serial_poller_thread, p) == 0)
        return 0;
    test_fail("serial_poll poller thread", "pthread_create failed");
    return -1;
}

/* Read one byte only once poll says it is there, so a lost injection fails the
 * check instead of hanging the test. Returns the byte or -1.
 */
static int serial_poll_read_ready(int fd)
{
    char ch;
    int ready;

    if (serial_poll_once(fd, SERIAL_WAIT_POLL, 0, &ready) != 1 || ready != 1 ||
        read(fd, &ch, 1) != 1)
        return -1;
    return (unsigned char) ch;
}

/* Hang the line up (output speed B0) a moment after the poller starts. */
static void *tty_hangup_setter(void *arg)
{
    int fd = (int) (intptr_t) arg;
    struct termios t;

    sleep_ms(300);
    if (tcgetattr(fd, &t) == 0 && cfsetospeed(&t, B0) == 0)
        (void) tcsetattr(fd, TCSANOW, &t);
    return NULL;
}

/* A poll that asks for no event waits for hangup alone: it times out on a live
 * line, ends at once when the line is hung up (B0) and keeps reporting it until
 * a speed is set again.
 */
static void tty_hangup_check(const char *name, const char *path, int fd)
{
    char what[96];
    struct termios saved;
    struct pollfd pfd = {.fd = fd, .events = 0};
    struct timespec t0, t1;
    pthread_t th;
    long ms;
    char ch;
    int rc;
    bool hup_ok, out_ok, write_ok, read_ok;
    int nfd;

    if (tcgetattr(fd, &saved) != 0) {
        test_fail(name, "hangup tcgetattr failed");
        return;
    }
    if (saved.c_ospeed == B0)
        cfsetospeed(&saved, B38400);
    snprintf(what, sizeof what, "%s hangup-only poll times out on a live line", name);
    rc = poll(&pfd, 1, 100);
    check(rc == 0, what, "poll with no events must wait for hangup alone");

    if (pthread_create(&th, NULL, tty_hangup_setter, (void *) (intptr_t) fd) != 0) {
        test_fail(name, "hangup thread failed");
        return;
    }
    nfd = open(path, O_RDWR | O_NONBLOCK);
    clock_gettime(CLOCK_MONOTONIC, &t0);
    rc = poll(&pfd, 1, 10000);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    (void) pthread_join(th, NULL);
    ms = (t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000;
    hup_ok = rc == 1 && (pfd.revents & POLLHUP) && ms < 5000;

    /* The terminal may be where the results are printed, and a hung-up line
     * refuses writes: everything is measured first and reported after the
     * speed is set again.
     */
    pfd = (struct pollfd){.fd = fd, .events = POLLOUT};
    out_ok = poll(&pfd, 1, 0) == 1 && (pfd.revents & POLLHUP) &&
             !(pfd.revents & POLLOUT);
    errno = 0;
    write_ok = write(fd, "x", 1) == -1 && errno == EIO;
    /* A descriptor opened non blocking, so that a line that wrongly waits
     * fails this check rather than hanging the test.
     */
    read_ok = nfd >= 0 && read(nfd, &ch, 1) == 0;

    (void) tcsetattr(fd, TCSANOW, &saved);
    if (nfd >= 0)
        (void) close(nfd);
    snprintf(what, sizeof what, "%s hangup-only poll returns at once on hangup", name);
    check(hup_ok, what,
          "a hangup-only poll must return POLLHUP when the line is hung up");
    snprintf(what, sizeof what, "%s hung-up line reports POLLHUP, not POLLOUT", name);
    check(out_ok, what,
          "a hung-up line must report POLLHUP at once and not be writable");
    snprintf(what, sizeof what, "%s write on a hung-up line fails with EIO", name);
    check(write_ok, what, "a write to a hung-up line must fail with EIO");
    snprintf(what, sizeof what, "%s read on a hung-up line is at end of file", name);
    check(read_ok, what, "a read from a hung-up line must return end of file");
    pfd = (struct pollfd){.fd = fd, .events = 0};
    snprintf(what, sizeof what, "%s hangup ends when a speed is set", name);
    check(poll(&pfd, 1, 50) == 0, what,
          "setting a speed again must end the hangup");
}

void test_serial_poll(void)
{
    struct serial_poller a = {0}, b = {0}, c = {0}, d = {0};
    struct pollfd pfd;
    struct termios t, saved;
    pthread_t ta, tb, tc, td;
    int fd, fd2, rc, ready, c1, c2;

    printf("[serial-poll]\n");
    fd = open(SERIAL_POLL_DEV, O_RDWR);
    if (fd < 0) {
        test_fail("serial_poll open", strerror(errno));
        return;
    }
    if (tcgetattr(fd, &saved) != 0) {
        test_fail("serial_poll tcgetattr", strerror(errno));
        (void) close(fd);
        return;
    }
    t = saved;
    t.c_lflag &= ~(tcflag_t) (ICANON | ECHO);
    if (tcsetattr(fd, TCSAFLUSH, &t) != 0) {
        test_fail("serial_poll tcsetattr", strerror(errno));
        goto out;
    }

    /* Idle line: a bounded wait times out with nothing ready. */
    rc = serial_poll_once(fd, SERIAL_WAIT_POLL, 50, &ready);
    check(rc == 0, "serial_poll idle poll times out",
          "poll({ser, POLLIN}, 50) on an idle line must return 0");
    rc = serial_poll_once(fd, SERIAL_WAIT_SELECT, 50, &ready);
    check(rc == 0, "serial_poll idle select times out",
          "select on an idle line must return 0");

    /* Output is synchronous, so the line is always writable. */
    pfd = (struct pollfd){.fd = fd, .events = POLLOUT};
    check(poll(&pfd, 1, 0) == 1 && (pfd.revents & POLLOUT),
          "serial_poll writable", "a terminal must poll POLLOUT");
    tty_hangup_check("serial_poll", SERIAL_POLL_DEV, fd);

    /* Close while polled: the armed notification goes with the open, and the
     * poller ends at its timeout instead of hanging or waking spuriously.
     */
    fd2 = open(SERIAL_POLL_DEV, O_RDWR);
    if (fd2 < 0) {
        test_fail("serial_poll second open", strerror(errno));
        goto out;
    }
    c.fd = fd2;
    c.timeout_ms = 600;
    if (serial_poller_start(&tc, &c) != 0) {
        (void) close(fd2);
        goto out;
    }
    sleep_ms(200);
    (void) close(fd2);
    (void) pthread_join(tc, NULL);
    check(c.rc == 0 || (c.rc == 1 && c.ready == -1), "serial_poll close while polled",
          "a poll whose descriptor was closed must time out or report POLLNVAL");

    /* Concurrent pollers, by poll, by select and by poll with a repeated entry
     * for the descriptor, all wake on the one injected byte, which none
     * consumes.
     */
    a.fd = b.fd = d.fd = fd;
    a.timeout_ms = b.timeout_ms = d.timeout_ms = 20000;
    b.how = SERIAL_WAIT_SELECT;
    d.how = SERIAL_WAIT_POLL_REPEAT;
    if (serial_poller_start(&ta, &a) != 0)
        goto out;
    if (serial_poller_start(&tb, &b) != 0) {
        (void) pthread_join(ta, NULL);
        goto out;
    }
    if (serial_poller_start(&td, &d) != 0) {
        (void) pthread_join(ta, NULL);
        (void) pthread_join(tb, NULL);
        goto out;
    }
    sleep_ms(300);
    check(!atomic_load(&a.done) && !atomic_load(&b.done) && !atomic_load(&d.done),
          "serial_poll pollers wait on an idle line",
          "a poller returned before any byte arrived");
    serial_inject('y');
    (void) pthread_join(ta, NULL);
    (void) pthread_join(tb, NULL);
    (void) pthread_join(td, NULL);
    check(a.rc == 1 && a.ready == 1, "serial_poll poll wakes on injected byte",
          "poll({ser, POLLIN}) must return 1 once a byte arrives");
    check(b.rc == 1 && b.ready == 1, "serial_poll select wakes on injected byte",
          "select must report the line readable once a byte arrives");
    check(d.rc == 1 && d.ready == 1, "serial_poll repeated entry keeps the arm",
          "a second pollfd for the same terminal must not cancel the first's wait");

    /* The byte is still queued: both calls report it at once. */
    rc = serial_poll_once(fd, SERIAL_WAIT_POLL, 50, &ready);
    check(rc == 1 && ready == 1, "serial_poll queued byte polls readable",
          "data present before poll must be reported");
    rc = serial_poll_once(fd, SERIAL_WAIT_SELECT, 50, &ready);
    check(rc == 1 && ready == 1, "serial_poll queued byte selects readable",
          "data present before select must be reported");
    check(serial_poll_read_ready(fd) == 'y', "serial_poll read injected byte",
          "wrong byte, or no byte to read");
    rc = serial_poll_once(fd, SERIAL_WAIT_POLL, 50, &ready);
    check(rc == 0, "serial_poll drained line idle again",
          "a drained line must not poll readable");

    /* Canonical mode: 'z' alone is a partial line and must not be readable;
     * the CR (ICRNL: newline) completes it and wakes a blocked poller.
     */
    t.c_lflag |= ICANON;
    if (tcsetattr(fd, TCSANOW, &t) != 0) {
        test_fail("serial_poll canonical tcsetattr", strerror(errno));
        goto out;
    }
    serial_inject('z');
    rc = serial_poll_once(fd, SERIAL_WAIT_POLL, 3000, &ready);
    check(rc == 0, "serial_poll partial canonical line not readable",
          "a line without its terminator must not poll readable");
    a = (struct serial_poller){.fd = fd, .timeout_ms = 20000};
    if (serial_poller_start(&ta, &a) != 0)
        goto out;
    sleep_ms(300);
    check(!atomic_load(&a.done), "serial_poll canonical poller waits",
          "a poller returned on a partial line");
    serial_inject('\r');
    (void) pthread_join(ta, NULL);
    check(a.rc == 1 && a.ready == 1, "serial_poll line terminator wakes poller",
          "completing a canonical line must make it readable");
    c1 = serial_poll_read_ready(fd);
    c2 = serial_poll_read_ready(fd);
    check(c1 == 'z' && c2 == '\n', "serial_poll read completed line",
          "the completed line must read back as \"z\\n\"");
out:
    (void) tcsetattr(fd, TCSANOW, &saved);
    (void) close(fd);
}

/* poll() and select() readiness on the console (/dev/text). Run by the
 * serial-unblock mode before the serial driver starts, so the bytes the harness
 * writes for console_inject() reach the console. Covers an idle console timing
 * out, output always ready, a poller and a select waking on a typed character,
 * and a typed character being reported at once until it is read.
 */
/* A reader that waits on the console when the line is hung up and the speed is
 * set again at once, before the reader looks again, still gets end of file:
 * the hangup is not lost because it was over by the time the reader woke.
 */
struct hangup_reader {
    int fd;
    atomic_int done;
    ssize_t rc;
};

static void *hangup_reader_main(void *raw)
{
    struct hangup_reader *r = raw;
    char ch;

    r->rc = read(r->fd, &ch, 1);
    atomic_store(&r->done, 1);
    return NULL;
}

static void console_hangup_wakes_reader(int fd)
{
    struct hangup_reader r = {.fd = fd};
    struct termios saved, hung;
    pthread_t th;
    bool woke;

    if (tcgetattr(fd, &saved) != 0 ||
        pthread_create(&th, NULL, hangup_reader_main, &r) != 0) {
        test_fail("console_poll hangup wakes a reader", "setup failed");
        return;
    }
    sleep_ms(150); /* the reader is waiting */
    hung = saved;
    cfsetospeed(&hung, B0);
    (void) tcsetattr(fd, TCSANOW, &hung);
    (void) tcsetattr(fd, TCSANOW, &saved);
    for (int i = 0; i < 200 && !atomic_load(&r.done); i++)
        sleep_ms(10);
    woke = atomic_load(&r.done) && r.rc == 0;
    if (!atomic_load(&r.done))
        console_inject('g'); /* let the reader go */
    (void) pthread_join(th, NULL);
    check(woke, "console_poll hangup wakes a reader",
          "a reader that waits must get end of file when the line is hung up, "
          "also when the speed is set again before it looks");
}

void test_console_poll(void)
{
    struct serial_poller a = {0}, b = {0};
    struct pollfd pfd;
    pthread_t ta, tb;
    int fd, rc, ready;

    printf("[console-poll]\n");
    fd = open("/dev/text", O_RDWR);
    if (fd < 0) {
        test_fail("console_poll open", strerror(errno));
        return;
    }

    rc = serial_poll_once(fd, SERIAL_WAIT_POLL, 50, &ready);
    check(rc == 0, "console_poll idle poll times out",
          "poll({/dev/text, POLLIN}, 50) with nothing typed must return 0");
    rc = serial_poll_once(fd, SERIAL_WAIT_SELECT, 50, &ready);
    check(rc == 0, "console_poll idle select times out",
          "select on an idle console must return 0");
    pfd = (struct pollfd){.fd = fd, .events = POLLOUT};
    check(poll(&pfd, 1, 0) == 1 && pfd.revents == POLLOUT,
          "console_poll writable", "the console must poll POLLOUT only");
    tty_hangup_check("console_poll", "/dev/text", fd);
    test_notify_hangup_arm();
    console_hangup_wakes_reader(fd);

    a.fd = b.fd = fd;
    a.timeout_ms = b.timeout_ms = 20000;
    b.how = SERIAL_WAIT_SELECT;
    if (serial_poller_start(&ta, &a) != 0)
        goto out;
    if (serial_poller_start(&tb, &b) != 0) {
        (void) pthread_join(ta, NULL);
        goto out;
    }
    sleep_ms(300);
    check(!atomic_load(&a.done) && !atomic_load(&b.done),
          "console_poll pollers wait on an idle console",
          "a poller returned before anything was typed");
    console_inject('k');
    (void) pthread_join(ta, NULL);
    (void) pthread_join(tb, NULL);
    check(a.rc == 1 && a.ready == 1, "console_poll poll wakes on typed byte",
          "poll({/dev/text, POLLIN}) must return 1 once a byte is typed");
    check(b.rc == 1 && b.ready == 1, "console_poll select wakes on typed byte",
          "select must report the console readable once a byte is typed");

    rc = serial_poll_once(fd, SERIAL_WAIT_POLL, 50, &ready);
    check(rc == 1 && ready == 1, "console_poll typed byte polls readable",
          "a byte typed before poll must be reported");
    check(serial_poll_read_ready(fd) == 'k', "console_poll read typed byte",
          "wrong byte, or no byte to read");
    rc = serial_poll_once(fd, SERIAL_WAIT_POLL, 50, &ready);
    check(rc == 0, "console_poll drained console idle again",
          "a drained console must not poll readable");
out:
    (void) close(fd);
}

/* A pipe end carries a fixed direction: lseek on a pipe is ESPIPE, writing the
 * read end (O_RDONLY) or reading the write end (O_WRONLY) is EBADF -- the
 * descriptor is not open in that direction.
 */
static void test_posix_pipe_access_mode(void)
{
    int fds[2];
    char buf[4];

    if (pipe(fds) != 0) {
        test_fail("pipe access-mode fixture", strerror(errno));
        return;
    }

    errno = 0;
    check(lseek(fds[0], 0, SEEK_SET) == (off_t) -1 && errno == ESPIPE,
          "lseek on a pipe", "lseek on a pipe must fail ESPIPE");

    errno = 0;
    check(write(fds[0], "x", 1) == -1 && errno == EBADF,
          "write pipe read end",
          "writing the read-only end of a pipe must fail EBADF");

    errno = 0;
    check(read(fds[1], buf, sizeof buf) == -1 && errno == EBADF,
          "read pipe write end",
          "reading the write-only end of a pipe must fail EBADF");

    (void) close(fds[0]);
    (void) close(fds[1]);
}

static void test_posix_open_creat_excl_existing(void)
{
    int fd;

    errno = 0;
    fd = open("/cap/self", O_RDONLY | O_CREAT | O_EXCL, 0644);
    check(fd == -1 && errno == EEXIST, "capfs O_CREAT|O_EXCL existing",
          "O_CREAT|O_EXCL on an existing capfs path must fail EEXIST");
    if (fd != -1)
        (void) close(fd);

    errno = 0;
    fd = open("/bin/true", O_RDONLY | O_CREAT | O_EXCL, 0644);
    check(fd == -1 && errno == EEXIST, "cpiofs O_CREAT|O_EXCL existing",
          "O_CREAT|O_EXCL on an existing cpiofs path must fail EEXIST");
    if (fd != -1)
        (void) close(fd);

    /* The cpiofs mount root is a distinct code path; it exists too. */
    errno = 0;
    fd = open("/", O_RDONLY | O_CREAT | O_EXCL, 0644);
    check(fd == -1 && errno == EEXIST, "cpiofs root O_CREAT|O_EXCL",
          "O_CREAT|O_EXCL on the existing root directory must fail EEXIST");
    if (fd != -1)
        (void) close(fd);
}

static void test_posix_open_capfs_write_semantics(void)
{
    struct stat st;
    int fd;

    fd = open("/cap", O_RDONLY);
    check(fd != -1 && fstat(fd, &st) == 0 && S_ISDIR(st.st_mode),
          "capfs dir control", "/cap must open read-only as a directory");
    if (fd != -1)
        (void) close(fd);

    fd = open("/cap/self", O_RDONLY);
    check(fd != -1 && fstat(fd, &st) == 0 && S_ISREG(st.st_mode),
          "capfs file control",
          "/cap/self must open read-only as a regular file");
    if (fd != -1)
        (void) close(fd);

    errno = 0;
    fd = open("/cap", O_WRONLY);
    check(fd == -1 && errno == EISDIR, "capfs dir O_WRONLY",
          "writing a capfs directory must fail EISDIR");
    if (fd != -1)
        (void) close(fd);

    errno = 0;
    fd = open("/cap", O_RDONLY | O_TRUNC);
    check(fd == -1 && errno == EISDIR, "capfs dir O_TRUNC",
          "truncating a capfs directory must fail EISDIR");
    if (fd != -1)
        (void) close(fd);

    errno = 0;
    fd = open("/cap/self", O_WRONLY);
    check(fd == -1 && errno == EROFS, "capfs file O_WRONLY",
          "writing a capfs file must fail EROFS");
    if (fd != -1)
        (void) close(fd);

    errno = 0;
    fd = open("/cap/self", O_RDONLY | O_TRUNC);
    check(fd == -1 && errno == EROFS, "capfs file O_TRUNC",
          "truncating a capfs file must fail EROFS");
    if (fd != -1)
        (void) close(fd);

    errno = 0;
    fd = open("/cap/rtmux-no-such", O_WRONLY);
    check(fd == -1 && errno == ENOENT, "capfs missing path",
          "a non-existent capfs path must fail ENOENT without O_CREAT");
    if (fd != -1)
        (void) close(fd);

    errno = 0;
    fd = open("/cap/rtmux-no-such", O_WRONLY | O_CREAT, 0644);
    check(fd == -1 && errno == EROFS, "capfs create",
          "creating on the read-only capfs must fail EROFS");
    if (fd != -1)
        (void) close(fd);
}

/* lstat() reports the final path component without following a symlink, while
 * stat() follows it. A symlink to /dev/null reads back as a link under lstat
 * (S_ISLNK) but resolves to its non-symlink target under stat.
 */
static void test_posix_lstat(void)
{
    const char link_path[] = "/rtmux-lstat-link";
    struct stat ls, s;

    (void) unlink(link_path);
    if (namespace_symlink("/dev/null", link_path) == -1) {
        test_fail("lstat symlink fixture", strerror(errno));
        return;
    }
    check(lstat(link_path, &ls) == 0 && S_ISLNK(ls.st_mode),
          "lstat sees the symlink",
          "lstat must report the link itself, not its target");
    check(stat(link_path, &s) == 0 && !S_ISLNK(s.st_mode),
          "stat follows the symlink",
          "stat must resolve the link to its target");
    (void) unlink(link_path);
}

/* rewinddir() resets a directory stream so the next readdir restarts from the
 * first entry.
 */
static void test_posix_rewinddir(void)
{
    DIR *dp = opendir("/dev");
    struct dirent *first, *again;
    char firstname[256];

    if (!dp) {
        test_fail("rewinddir fixture", strerror(errno));
        return;
    }
    first = readdir(dp);
    if (!first) {
        test_fail("rewinddir empty directory", "/dev returned no entries");
        (void) closedir(dp);
        return;
    }

    /* readdir returns a pointer into the DIR's own buffer, which the second
     * readdir reuses; copy the name before rewinding so the compare is real.
     */
    (void) snprintf(firstname, sizeof firstname, "%s", first->d_name);
    rewinddir(dp);
    again = readdir(dp);
    check(again && !strcmp(firstname, again->d_name),
          "rewinddir restarts the stream",
          "readdir after rewinddir must return the first entry again");
    (void) closedir(dp);
}

/* readdir_r copies each entry into a caller-owned buffer, so unlike readdir()
 * (which hands back a pointer into the DIR's shared buffer) two results can be
 * held at once. End-of-directory is signalled by return 0 with *result NULL.
 */
static void test_posix_readdir_r(void)
{
    DIR *dp = opendir("/dev");
    char b1[offsetof(struct dirent, d_name) + 256];
    char b2[offsetof(struct dirent, d_name) + 256];
    struct dirent *e1 = (struct dirent *) b1;
    struct dirent *e2 = (struct dirent *) b2;
    struct dirent *r1, *r2;
    int count = 0;

    if (!dp) {
        test_fail("readdir_r fixture", strerror(errno));
        return;
    }

    if (readdir_r(dp, e1, &r1) == 0 && r1 == e1 &&
        readdir_r(dp, e2, &r2) == 0 && r2 == e2) {
        /* Two distinct caller buffers stay valid at once. */
        check(e1->d_name[0] != '\0' && e2->d_name[0] != '\0',
              "readdir_r fills independent caller buffers", "empty name(s)");
        count = 2;
    } else {
        test_fail("readdir_r first two entries",
                  "expected two entries from /dev");
    }

    while (readdir_r(dp, e1, &r1) == 0 && r1)
        count++;
    check(r1 == NULL, "readdir_r ends with rc 0 and NULL result",
          "end-of-directory not signalled");
    check(count >= 2, "readdir_r walked /dev", "too few entries");
    (void) closedir(dp);
}

/* rand_r is reentrant (the caller owns the seed): equal seeds give equal
 * sequences, the seed advances, and results stay within [0, RAND_MAX].
 */
static void test_posix_rand_r(void)
{
    unsigned int s1 = 12345, s2 = 12345, s3 = 12345;
    int i, inrange = 1;

    check(rand_r(&s1) == rand_r(&s2), "rand_r deterministic for equal seeds",
          "sequences differ");
    check(s1 != s3, "rand_r advances the caller seed", "seed unchanged");

    for (i = 0; i < 1000; i++) {
        int v = rand_r(&s1);
        if (v < 0 || v > RAND_MAX) {
            inrange = 0;
            break;
        }
    }
    check(inrange, "rand_r stays within [0, RAND_MAX]", "out-of-range value");
}

/* _exit() terminates the caller immediately with the given status, bypassing
 * the stdio flush and atexit handlers that exit() runs. A successful _exit
 * cannot be observed in-process, so a spawned child calls _exit(42) and the
 * parent confirms the exact status propagates through waitpid.
 */
static void test_posix__exit(void)
{
    char *argv[] = {"syscall_testing", "--exit-code", "42", NULL};
    pid_t pid;
    int status = 0;
    int rc;

    rc = posix_spawn(&pid, "/bin/syscall_testing", NULL, NULL, argv, NULL);
    if (rc != EOK) {
        test_fail("_exit child fixture", strerror(rc));
        return;
    }
    check(waitpid(pid, &status, 0) == pid && WIFEXITED(status) &&
              WEXITSTATUS(status) == 42,
          "_exit propagates the exact status",
          "_exit(42) must make the child exit with status 42");
}

/* crypt() (XSI) implements the SHA-512 modular form "$6$salt$hash". Hashing is
 * deterministic for a given key and salt, a different key hashes differently,
 * and the result carries the $6$ prefix. crypt returns a pointer to a static
 * buffer, so the first result is copied out before re-calling.
 */
static void test_posix_crypt(void)
{
    const char salt[] = "$6$rtmuxsalt";
    char first[128];
    char *h;

    h = crypt("rtmux-secret", salt);
    if (!h) {
        test_fail("crypt returned NULL", strerror(errno));
        return;
    }
    (void) snprintf(first, sizeof first, "%s", h);
    check(strncmp(first, "$6$", 3) == 0, "crypt emits the SHA-512 form",
          "crypt must return a $6$ modular-crypt string");

    h = crypt("rtmux-secret", salt);
    check(h && !strcmp(h, first), "crypt is deterministic",
          "the same key and salt must hash to the same string");

    h = crypt("other-secret", salt);
    check(h && strcmp(h, first) != 0, "crypt separates keys",
          "a different key must hash differently");
}

/* Session / job-control interfaces, at their several maturity levels. setpgid
 * enforces the POSIX argument rules; getpgrp reports roottask's process-group
 * state; setsid creates a new session for the suite process; and the terminal
 * foreground-group calls fail closed on a non-terminal or a process group
 * outside the caller's session.
 */
static void test_posix_session_jobcontrol(void)
{
    int fd;
    pid_t pgrp;
    int was_leader = getpgrp() == getpid();

    errno = 0;
    check(setpgid(0, -1) == -1 && errno == EINVAL, "setpgid negative pgid",
          "setpgid must reject a negative process-group id");

    pgrp = getpgrp();
    check(pgrp > 0, "getpgrp reports process group",
          "getpgrp must return a positive roottask process-group id");

    /* Before setsid() the suite is still in its starter's session, whose
     * controlling terminal stdin normally is: the foreground group is settable.
     * (The libc call refuses a group outside the session first; the job control
     * suite in signals-test.c covers the terminal servers' own checks.)
     */
    if (isatty(STDIN_FILENO) && !was_leader && tcgetpgrp(STDIN_FILENO) == pgrp) {
        check(tcsetpgrp(STDIN_FILENO, pgrp) == 0 &&
                  tcgetpgrp(STDIN_FILENO) == pgrp,
              "tc pgrp terminal round trip",
              "terminal foreground process-group get/set must round trip");
        check(tcgetsid(STDIN_FILENO) == getsid(0),
              "tcgetsid names the controlling terminal's session",
              "tcgetsid must return the caller's session leader");
        errno = 0;
        check(tcsetpgrp(STDIN_FILENO, 1000000) == -1 && errno == EPERM,
              "tcsetpgrp rejects foreign pgrp",
              "a terminal foreground group must exist in the caller's session");
    }

    /* A second run in the same process (syscall_testing --laps) starts as the
     * session leader the first run made, so setsid() must refuse instead.
     */
    errno = 0;
    check(was_leader ? (setsid() == -1 && errno == EPERM) : setsid() == getpid(),
          "setsid creates a session",
          "setsid must return the caller's pid as the new session id");
    check(getpgrp() == getpid(), "getpgrp tracks setsid",
          "setsid must make the caller's process group equal its pid");
    check(getsid(0) == getpid() && getsid(getpid()) == getpid(),
          "getsid tracks setsid",
          "a session leader's session id must be its own pid");
    errno = 0;
    check(setsid() == -1 && errno == EPERM, "setsid rejects group leader",
          "setsid must fail once the caller is a process-group leader");

    /* A session leader with no terminal of its own has no controlling terminal,
     * so /dev/tty has nothing to resolve to and tcsetpgrp is refused.
     */
    errno = 0;
    check(open("/dev/tty", O_RDWR | O_NOCTTY) == -1 && errno == ENXIO,
          "tty_after_setsid",
          "a session without a controlling terminal must see ENXIO");
    if (isatty(STDIN_FILENO)) {
        errno = 0;
        check(tcsetpgrp(STDIN_FILENO, getpgrp()) == -1 && errno == ENOTTY,
              "tcsetpgrp on a terminal that is not ours",
              "tcsetpgrp must fail ENOTTY on another session's terminal");
        errno = 0;
        check(tcgetsid(STDIN_FILENO) == -1 && errno == ENOTTY,
              "tcgetsid on a terminal that is not ours",
              "tcgetsid must fail ENOTTY on another session's terminal");
    }

    fd = open("/dev/null", O_RDONLY);
    if (fd == -1) {
        test_fail("tc-pgrp fixture", strerror(errno));
        return;
    }
    errno = 0;
    check(tcgetpgrp(fd) == -1 && errno == ENOTTY, "tcgetpgrp on a non-terminal",
          "tcgetpgrp must fail ENOTTY on a non-terminal descriptor");
    errno = 0;
    check(tcgetsid(fd) == -1 && errno == ENOTTY, "tcgetsid on a non-terminal",
          "tcgetsid must fail ENOTTY on a non-terminal descriptor");
    errno = 0;
    check(tcgetsid(-1) == -1 && errno == EBADF, "tcgetsid bad fd",
          "tcgetsid must fail EBADF on a bad descriptor");
    errno = 0;
    check(tcsetpgrp(fd, getpid()) == -1 && errno == ENOTTY,
          "tcsetpgrp on a non-terminal",
          "tcsetpgrp must fail ENOTTY on a non-terminal descriptor");
    (void) close(fd);
}

/* fsync()/fdatasync() marshal an _IO_SYNC request and reject a bad descriptor
 * with EBADF. RTMux's native filesystems are RAM-backed (read-only
 * romfs/cpiofs/capfs, mmap-backed shmem), so there is no slower durable tier to
 * flush to: the sync is trivially complete and must return 0, not ENOSYS. Each
 * native file service registers a no-op _IO_SYNC handler for this.
 */
static void test_posix_fsync(void)
{
    const char path[] = "/dev/shmem/rtmux-fsync";
    int fd;

    errno = 0;
    check(fsync(-1) == -1 && errno == EBADF, "fsync bad fd",
          "fsync on a bad descriptor must fail EBADF");
    errno = 0;
    check(fdatasync(-1) == -1 && errno == EBADF, "fdatasync bad fd",
          "fdatasync on a bad descriptor must fail EBADF");

    (void) unlink(path);
    fd = open(path, O_RDWR | O_CREAT, 0600);
    if (fd == -1) {
        test_fail("fsync fixture", strerror(errno));
        return;
    }
    check(fsync(fd) == 0, "fsync shmem succeeds",
          "a native shmem object has nothing to flush, so fsync returns 0");
    check(fdatasync(fd) == 0, "fdatasync shmem succeeds",
          "a native shmem object has nothing to flush, so fdatasync returns 0");
    (void) close(fd);
    (void) unlink(path);

    /* The read-only native filesystems likewise have nothing to flush. */
    fd = open("/bin/true", O_RDONLY); /* cpiofs */
    if (fd != -1) {
        check(fsync(fd) == 0, "fsync cpiofs file",
              "fsync on a read-only cpiofs file must return 0");
        (void) close(fd);
    }
    fd = open("/usr/conf/passwd", O_RDONLY); /* romfs */
    if (fd != -1) {
        check(fsync(fd) == 0, "fsync romfs file",
              "fsync on a read-only romfs file must return 0");
        (void) close(fd);
    }
    fd = open("/cap/self", O_RDONLY); /* capfs */
    if (fd != -1) {
        check(fsync(fd) == 0, "fsync capfs file",
              "fsync on a read-only capfs file must return 0");
        (void) close(fd);
    }
}

/* sync() asks roottask to sweep every mounted server. With only native
 * filesystems mounted each answers ENOSYS or succeeds at once, so it must
 * return well inside the sweep's 2 s bound and leave the system usable.
 */
static void test_posix_sync(void)
{
    struct timespec t0, t1;
    long ms;
    int fd;

    (void) clock_gettime(CLOCK_MONOTONIC, &t0);
    sync();
    (void) clock_gettime(CLOCK_MONOTONIC, &t1);
    ms = (long) (t1.tv_sec - t0.tv_sec) * 1000 +
         (t1.tv_nsec - t0.tv_nsec) / 1000000;
    check(ms < 2000, "sync returns inside the sweep bound",
          "sync with only native filesystems must not hit the 2 s timeout");
    fd = open("/usr/conf/passwd", O_RDONLY);
    check(fd != -1, "namespace usable after sync",
          "an open after sync must still succeed");
    if (fd != -1)
        (void) close(fd);
}

/* Many concurrent sync() callers against a mount that never answers. Roottask
 * lets one caller sweep and at most two wait for the next sweep; the rest are
 * turned away at once, so a sync() loop cannot park every roottask worker.
 */
#define SYNC_CAP_THREADS 8
static long sync_cap_ms[SYNC_CAP_THREADS];

static void sync_cap_sleep_ms(long ms)
{
    struct timespec ts = {ms / 1000, (ms % 1000) * 1000000L};

    (void) nanosleep(&ts, NULL);
}

static void *sync_cap_caller(void *arg)
{
    long *out = arg;
    struct timespec t0, t1;

    (void) clock_gettime(CLOCK_MONOTONIC, &t0);
    sync();
    (void) clock_gettime(CLOCK_MONOTONIC, &t1);
    *out = (long) (t1.tv_sec - t0.tv_sec) * 1000 +
           (t1.tv_nsec - t0.tv_nsec) / 1000000;
    return NULL;
}

static void test_sync_waiter_cap(void)
{
    char *argv[] = {(char *) "sync-ignore", NULL};
    pthread_t th[SYNC_CAP_THREADS];
    pid_t pid;
    int i, started = 0, fast = 0, fd = -1;

    if (posix_spawn(&pid, "/bin/sync-ignore", NULL, NULL, argv, NULL) != 0) {
        test_fail("sync waiter cap spawn", strerror(errno));
        return;
    }
    /* The fixture has no stat handler: open its root to see it attached. */
    for (i = 0; i < 40; i++) {
        fd = open("/sync-ignore", O_RDONLY);
        if (fd != -1) {
            (void) close(fd);
            break;
        }
        sync_cap_sleep_ms(50);
    }
    if (fd == -1) {
        test_fail("sync waiter cap fixture", "/sync-ignore never attached");
        goto out;
    }
    for (i = 0; i < SYNC_CAP_THREADS; i++) {
        sync_cap_ms[i] = -1;
        if (pthread_create(&th[i], NULL, sync_cap_caller, &sync_cap_ms[i]) != 0)
            break;
        started++;
    }
    /* Roottask must keep answering while the sweep is stuck on the mount. */
    sync_cap_sleep_ms(300);
    fd = open("/usr/conf/passwd", O_RDONLY);
    check(fd != -1, "roottask answers while sync callers are blocked",
          "an open during concurrent wedged sync() calls must succeed");
    if (fd != -1)
        (void) close(fd);
    for (i = 0; i < started; i++) {
        (void) pthread_join(th[i], NULL);
        if (sync_cap_ms[i] >= 0 && sync_cap_ms[i] < 500)
            fast++;
    }
    check(started == SYNC_CAP_THREADS && fast >= SYNC_CAP_THREADS - 4,
          "sync callers beyond the waiter cap are turned away at once",
          "at most one sweeper and two waiters may block on a wedged mount");
out:
    (void) kill(pid, SIGKILL);
    (void) waitpid(pid, NULL, 0);
}

/* sleep(0) has nothing to wait for and returns 0 remaining seconds. */
static void test_posix_sleep(void)
{
    check(sleep(0) == 0, "sleep(0) returns immediately",
          "sleep with zero seconds must return 0 seconds remaining");
}

/* getopt() parses a flag and an option that takes an argument. */
static void test_posix_getopt(void)
{
    char *const av[] = {(char *) "prog", (char *) "-a", (char *) "-b",
                        (char *) "val", NULL};
    int saw_a = 0, saw_b = 0;
    int c;

    optind = 1;
    while ((c = getopt(4, av, "ab:")) != -1) {
        if (c == 'a')
            saw_a = 1;
        else if (c == 'b')
            saw_b = optarg && !strcmp(optarg, "val");
    }
    check(saw_a && saw_b, "getopt parses a flag and an option-argument",
          "getopt must return -a, then -b with optarg \"val\"");
}

/* The exec family replaces the current image on success, so only the failure
 * path is testable in-process: every variant reports -1 with ENOENT for a
 * binary that cannot be found, leaving the caller running.
 */
static void test_posix_exec_family(void)
{
    char *const argv[] = {(char *) "x", NULL};
    char *const envp[] = {NULL};
    const char missing[] = "/rtmux-no-such-binary";
    char saved_path[256];
    char *path;

    errno = 0;
    check(execv(missing, argv) == -1 && errno == ENOENT, "execv missing",
          "execv of a missing binary must fail ENOENT");
    errno = 0;
    check(execve(missing, argv, envp) == -1 && errno == ENOENT, "execve missing",
          "execve of a missing binary must fail ENOENT");
    errno = 0;
    check(execl(missing, "x", (char *) NULL) == -1 && errno == ENOENT,
          "execl missing", "execl of a missing binary must fail ENOENT");
    errno = 0;
    check(execle(missing, "x", (char *) NULL, envp) == -1 && errno == ENOENT,
          "execle missing", "execle of a missing binary must fail ENOENT");

    /* execvp/execlp search PATH; pin it to a directory with no executables so
     * the lookup cannot find a real binary and replace the test process.
     */
    saved_path[0] = '\0';
    if ((path = getenv("PATH")))
        (void) snprintf(saved_path, sizeof saved_path, "%s", path);
    (void) setenv("PATH", "/rtmux-no-such-dir", 1);

    errno = 0;
    check(execvp("rtmux-no-such-cmd", argv) == -1 && errno == ENOENT,
          "execvp missing", "execvp of a missing command must fail ENOENT");
    errno = 0;
    check(execlp("rtmux-no-such-cmd", "x", (char *) NULL) == -1 &&
              errno == ENOENT,
          "execlp missing", "execlp of a missing command must fail ENOENT");

    if (path)
        (void) setenv("PATH", saved_path, 1);
    else
        (void) unsetenv("PATH");
}

/* isatty() distinguishes a terminal from any other descriptor. It returns 0
 * (never -1) for a non-terminal, but POSIX requires errno set on that 0 return:
 * ENOTTY for a valid non-terminal descriptor (/dev/null), EBADF for a bad one.
 */
static void test_posix_isatty(void)
{
    int fd = open("/dev/null", O_RDONLY);

    if (fd == -1) {
        test_fail("isatty fixture", strerror(errno));
        return;
    }
    errno = 0;
    check(isatty(fd) == 0 && errno == ENOTTY, "isatty on a non-terminal",
          "a valid non-terminal fd must report 0 and set errno ENOTTY");
    (void) close(fd);

    errno = 0;
    check(isatty(-1) == 0 && errno == EBADF, "isatty on a bad fd",
          "a bad descriptor must report 0 and set errno EBADF");
}

/* Process credential get/set. The getters return a stable identity; the
 * single-id setters reject the -1 sentinel with EINVAL (a libc-level guard);
 * and an all-(-1) setre*id is a no-op that must succeed. All non-mutating so
 * the test cannot disturb the identity later cases run under.
 */
static void test_posix_credentials(void)
{
    uid_t u = getuid();
    uid_t eu = geteuid();
    gid_t g = getgid();
    gid_t eg = getegid();

    check(getuid() == u && geteuid() == eu && getgid() == g && getegid() == eg,
          "credential getters are stable",
          "get{u,eu,g,eg}id must report a stable identity");

    check(getgroups(0, NULL) >= 0, "getgroups count query",
          "getgroups(0, NULL) must report the supplementary group count");

    errno = 0;
    check(setuid((uid_t) -1) == -1 && errno == EINVAL, "setuid(-1) EINVAL",
          "setuid must reject the -1 sentinel");
    errno = 0;
    check(seteuid((uid_t) -1) == -1 && errno == EINVAL, "seteuid(-1) EINVAL",
          "seteuid must reject the -1 sentinel");
    errno = 0;
    check(setgid((gid_t) -1) == -1 && errno == EINVAL, "setgid(-1) EINVAL",
          "setgid must reject the -1 sentinel");
    errno = 0;
    check(setegid((gid_t) -1) == -1 && errno == EINVAL, "setegid(-1) EINVAL",
          "setegid must reject the -1 sentinel");

    check(setreuid((uid_t) -1, (uid_t) -1) == 0, "setreuid(-1,-1) no-op",
          "setreuid with no change must succeed");
    check(setregid((gid_t) -1, (gid_t) -1) == 0, "setregid(-1,-1) no-op",
          "setregid with no change must succeed");
}

/* getlogin_r resolves the login name from $LOGNAME (set by the login program),
 * falling back to the current uid's passwd name. It is reentrant (caller
 * buffer); getlogin() wraps it with a static buffer.
 */
static void test_posix_getlogin(void)
{
    char buf[64];

    setenv("LOGNAME", "tsftester", 1);
    check(getlogin_r(buf, sizeof buf) == 0 && !strcmp(buf, "tsftester"),
          "getlogin_r reads $LOGNAME", "wrong name/rc");
    check(getlogin_r(buf, 3) == ERANGE, "getlogin_r ERANGE on short buffer",
          "expected ERANGE for an undersized buffer");

    char *p = getlogin();
    check(p != NULL && !strcmp(p, "tsftester"),
          "getlogin matches getlogin_r", "mismatch or NULL");

    unsetenv("LOGNAME");
}

/* ttyname_r distinguishes a bad fd (EBADF) from a non-terminal (ENOTTY), and on
 * RTMux any real terminal descriptor names the single console.
 */
static void test_posix_ttyname(void)
{
    char buf[64];
    int fd;

    check(ttyname_r(-1, buf, sizeof buf) == EBADF, "ttyname_r bad fd EBADF",
          "expected EBADF for a closed descriptor");

    fd = open("/dev/null", O_RDONLY);
    if (fd >= 0) {
        check(ttyname_r(fd, buf, sizeof buf) == ENOTTY,
              "ttyname_r non-terminal ENOTTY", "expected ENOTTY on /dev/null");
        (void) close(fd);
    }

    if (isatty(STDIN_FILENO)) {
        check(ttyname_r(STDIN_FILENO, buf, sizeof buf) == 0 &&
                  !strcmp(buf, "/dev/console"),
              "ttyname_r on a terminal names /dev/console", "wrong name/rc");
        check(ttyname_r(STDIN_FILENO, buf, 3) == ERANGE,
              "ttyname_r ERANGE on short buffer", "expected ERANGE");
        check(ttyname(STDIN_FILENO) != NULL, "ttyname non-NULL on a terminal",
              "expected a path");
    }
}

/* strtok_r and localtime_r are [TSF] reentrant helpers: the save pointer and
 * the result struct are caller-owned, so there is no shared static state.
 */
static void test_posix_reentrant_helpers(void)
{
    char in[] = "a,bb,,ccc";
    const char *want[] = {"a", "bb", "ccc"};
    char *save = NULL, *tok;
    int n = 0;
    time_t t = 0; /* the epoch */
    struct tm lt;

    for (tok = strtok_r(in, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
        if (n < 3)
            check(!strcmp(tok, want[n]), "strtok_r token", "wrong token");
        n++;
    }
    check(n == 3, "strtok_r skips empty fields and terminates",
          "wrong token count");

    /* tm_year near the epoch is 70 (UTC) or 69 with a negative TZ offset. */
    check(localtime_r(&t, &lt) == &lt && (lt.tm_year == 70 || lt.tm_year == 69),
          "localtime_r fills the caller struct", "unexpected return/tm");
}

/* Record locking (fcntl F_SETLK/F_GETLK/F_SETLKW). The roottask devmem server
 * implements whole-file, byte-range, and blocking advisory locks on shmem. This
 * exercises the single-process mechanics (acquire/query/downgrade/release,
 * byte-range split, descriptor access checks, and that F_GETLK never reports
 * the caller's own lock); cross-process conflict and release-on-exit are in
 * test_posix_fcntl_flock_crossproc.
 */
static void test_posix_file_locking(void)
{
    const char path[] = "/dev/shmem/rtmux-lock-corner";
    struct flock fl;
    int fd;

    (void) unlink(path);
    fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0600);
    if (fd == -1) {
        test_fail("file-lock fixture", strerror(errno));
        return;
    }

    /* Whole-file locks: acquire, query, downgrade, release. F_GETLK never
     * reports the caller's OWN lock as a conflict.
     */
    memset(&fl, 0, sizeof fl);
    fl.l_type = F_WRLCK;
    fl.l_whence = SEEK_SET;
    fl.l_start = 0;
    fl.l_len = 0;
    check(fcntl(fd, F_SETLK, &fl) == 0, "F_SETLK whole-file acquire",
          "a non-conflicting whole-file write lock must be granted");

    fl.l_type = F_WRLCK;
    fl.l_whence = SEEK_SET;
    fl.l_start = 0;
    fl.l_len = 0;
    check(fcntl(fd, F_GETLK, &fl) == 0 && fl.l_type == F_UNLCK,
          "F_GETLK ignores own lock",
          "F_GETLK reports other processes' conflicts, never the caller's own");

    fl.l_type = F_RDLCK;
    check(fcntl(fd, F_SETLK, &fl) == 0, "F_SETLK downgrade to read",
          "re-locking must replace the caller's held lock type");

    fl.l_type = F_UNLCK;
    check(fcntl(fd, F_SETLK, &fl) == 0, "F_SETLK unlock",
          "F_UNLCK must release the lock");

    /* F_SETLKW acquires when there is no conflict (only a real conflict would
     * need the unimplemented blocking wait).
     */
    fl.l_type = F_WRLCK;
    check(fcntl(fd, F_SETLKW, &fl) == 0, "F_SETLKW no-conflict acquire",
          "F_SETLKW must grant an uncontended lock without blocking");
    fl.l_type = F_UNLCK;
    (void) fcntl(fd, F_SETLK, &fl);

    /* Byte-range locks: acquire a range, unlock a sub-range (which splits the
     * held range in the server), then clear the remaining fragments. Conflict
     * across ranges is exercised cross-process below.
     */
    fl.l_type = F_WRLCK;
    fl.l_whence = SEEK_SET;
    fl.l_start = 0;
    fl.l_len = 100;
    check(fcntl(fd, F_SETLK, &fl) == 0, "F_SETLK byte-range acquire",
          "a byte-range write lock must be granted");
    fl.l_type = F_UNLCK;
    fl.l_start = 40;
    fl.l_len = 20;
    check(fcntl(fd, F_SETLK, &fl) == 0, "F_SETLK byte-range partial unlock",
          "unlocking a sub-range must split the held lock");
    fl.l_type = F_UNLCK;
    fl.l_start = 0;
    fl.l_len = 0;
    check(fcntl(fd, F_SETLK, &fl) == 0, "F_SETLK byte-range clear",
          "F_UNLCK over the whole file must drop the remaining fragments");

    (void) close(fd);

#define EXPECT_LOCK_FAIL(tag, file, flags, cmd, type, err, name, detail)       \
    do {                                                                      \
        int fd_##tag = open((file), (flags));                                  \
                                                                              \
        if (fd_##tag == -1) {                                                  \
            test_fail("file-lock " #tag " fixture", strerror(errno));         \
            (void) unlink(path);                                               \
            return;                                                            \
        }                                                                      \
        memset(&fl, 0, sizeof fl);                                             \
        fl.l_type = (type);                                                    \
        fl.l_whence = SEEK_SET;                                                \
        errno = 0;                                                             \
        check(fcntl(fd_##tag, (cmd), &fl) == -1 && errno == (err), (name),     \
              (detail));                                                       \
        (void) close(fd_##tag);                                                \
    } while (0)

    EXPECT_LOCK_FAIL(readonly, path, O_RDONLY, F_SETLK, F_WRLCK, EBADF,
                     "F_SETLK write lock needs write access",
                     "a write lock on a read-only descriptor must fail EBADF");
    EXPECT_LOCK_FAIL(writeonly, path, O_WRONLY, F_SETLK, F_RDLCK, EBADF,
                     "F_SETLK read lock needs read access",
                     "a read lock on a write-only descriptor must fail EBADF");
    EXPECT_LOCK_FAIL(image_setlk, "/bin/true", O_RDONLY, F_SETLK, F_RDLCK,
                     ENOSYS, "F_SETLK read-only image fails closed",
                     "read-only image files do not provide advisory lock state");
    EXPECT_LOCK_FAIL(image_getlk, "/bin/true", O_RDONLY, F_GETLK, F_RDLCK,
                     ENOSYS, "F_GETLK read-only image fails closed",
                     "read-only image files do not provide advisory lock state");

#undef EXPECT_LOCK_FAIL

    (void) unlink(path);
}

/* Cross-process advisory locking: a write lock held by another process must
 * conflict, F_GETLK must report the holder, and the lock releases when that
 * process exits. Coordinated with two named semaphores (reliable, unlike a
 * timed hold in a spawned child): the child posts "/flock-locked" once it holds
 * the lock and blocks on "/flock-release" until we let it exit.
 */
static void test_posix_fcntl_flock_crossproc(void)
{
    const char path[] = "/dev/shmem/rtmux-flock-xp";
    char *argv[] = {(char *) "syscall_testing", (char *) "--flock-hold",
                    (char *) path, NULL};
    struct flock fl;
    struct timespec deadline;
    sem_t *locked, *release;
    int fd;
    pid_t pid;

    (void) sem_unlink("/flock-locked");
    (void) sem_unlink("/flock-release");
    locked = sem_open("/flock-locked", O_CREAT, 0600, 0);
    release = sem_open("/flock-release", O_CREAT, 0600, 0);
    (void) unlink(path);
    fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0600);
    if (fd == -1 || locked == SEM_FAILED || release == SEM_FAILED) {
        test_fail("flock xp fixture", strerror(errno));
        goto out;
    }

    if (posix_spawn(&pid, "/bin/syscall_testing", NULL, NULL, argv, NULL) != 0) {
        test_fail("flock xp spawn", strerror(errno));
        goto out;
    }

    /* Wait (bounded) for the child to acquire the lock and post. */
    (void) clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += 5;
    if (sem_timedwait(locked, &deadline) == -1) {
        test_fail("flock xp child acquired", "child never signalled the lock");
        (void) sem_post(release);
        (void) waitpid(pid, NULL, 0);
        goto out;
    }
    check(1, "flock xp child acquired",
          "the child must acquire the [0,100) write lock");

    /* A byte range overlapping the child's [0,100) must conflict. */
    memset(&fl, 0, sizeof fl);
    fl.l_type = F_WRLCK;
    fl.l_whence = SEEK_SET;
    fl.l_start = 50;
    fl.l_len = 100; /* [50,150) overlaps [0,100) */
    errno = 0;
    check(fcntl(fd, F_SETLK, &fl) == -1 && errno == EAGAIN,
          "flock xp F_SETLK overlap conflict",
          "a range overlapping another process's write lock must EAGAIN");

    /* A disjoint range must be granted (then released). */
    memset(&fl, 0, sizeof fl);
    fl.l_type = F_WRLCK;
    fl.l_whence = SEEK_SET;
    fl.l_start = 100;
    fl.l_len = 100; /* [100,200) is disjoint from [0,100) */
    check(fcntl(fd, F_SETLK, &fl) == 0, "flock xp F_SETLK disjoint",
          "a non-overlapping byte range must be granted");
    fl.l_type = F_UNLCK;
    (void) fcntl(fd, F_SETLK, &fl);

    /* F_GETLK must report the child's exact range, type, and pid. */
    memset(&fl, 0, sizeof fl);
    fl.l_type = F_WRLCK;
    fl.l_whence = SEEK_SET;
    fl.l_start = 0;
    fl.l_len = 10; /* probe inside [0,100) */
    check(fcntl(fd, F_GETLK, &fl) == 0 && fl.l_type == F_WRLCK &&
              fl.l_pid == pid && fl.l_start == 0 && fl.l_len == 100,
          "flock xp F_GETLK reports holder",
          "F_GETLK must report the conflicting range, type, and holder pid");

    (void) sem_post(release);     /* let the child exit */
    (void) waitpid(pid, NULL, 0); /* reap it (drops the lock) */

    /* The holder is gone, so the range frees again. RTMux releases a dead
     * process's advisory locks asynchronously: waitpid is satisfied by the
     * _PROC_EXIT handler (ProcessExitMark), while the range is dropped when
     * roottask's dispatch loop later processes the exiting client's
     * connection-disconnect (ioserv _IO_CLOSE -> mem_flock_purge). So the range
     * frees within a bounded window after reap, not necessarily on the same
     * tick. Retry rather than assume same-instant release; exhausting the bound
     * still fails the test, so a lock that is never dropped is caught.
     */
    memset(&fl, 0, sizeof fl);
    fl.l_type = F_WRLCK;
    fl.l_whence = SEEK_SET;
    fl.l_start = 0;
    fl.l_len = 100;
    {
        int reacquired = 0;

        for (int attempt = 0; attempt < 200; attempt++) {
            if (fcntl(fd, F_SETLK, &fl) == 0) {
                reacquired = 1;
                break;
            }
            struct timespec d = {0, 1000 * 1000}; /* 1 ms */
            (void) nanosleep(&d, NULL);
        }
        check(reacquired, "flock xp reacquire after exit",
              "the range must be free within a bounded window once the holder "
              "exits");
    }
    fl.l_type = F_UNLCK;
    (void) fcntl(fd, F_SETLK, &fl);

out:
    if (fd != -1)
        (void) close(fd);
    (void) unlink(path);
    if (locked != SEM_FAILED)
        (void) sem_close(locked);
    if (release != SEM_FAILED)
        (void) sem_close(release);
    (void) sem_unlink("/flock-locked");
    (void) sem_unlink("/flock-release");
}

/* F_SETLKW blocking: a second process requesting a lock that conflicts with a
 * held one must block in fcntl() until the holder releases, then be granted.
 * Child A (--flock-hold) takes [0,100) and waits; child B (--flock-setlkw-wait)
 * issues F_SETLKW for the same range and blocks. While A holds it, B must not
 * have acquired (nothing posted "/flock-acquired"); once A exits, the server
 * wakes B, grants the lock, and B posts. Coordinated entirely by semaphores.
 */
static void test_posix_fcntl_flock_setlkw_block(void)
{
    const char path[] = "/dev/shmem/rtmux-flock-w";
    char *hold_argv[] = {(char *) "syscall_testing", (char *) "--flock-hold",
                         (char *) path, NULL};
    char *wait_argv[] = {(char *) "syscall_testing",
                         (char *) "--flock-setlkw-wait", (char *) path, NULL};
    struct timespec deadline, nap;
    sem_t *locked, *release, *acquired;
    pid_t holder = -1, waiter = -1;
    int fd;

    (void) sem_unlink("/flock-locked");
    (void) sem_unlink("/flock-release");
    (void) sem_unlink("/flock-acquired");
    locked = sem_open("/flock-locked", O_CREAT, 0600, 0);
    release = sem_open("/flock-release", O_CREAT, 0600, 0);
    acquired = sem_open("/flock-acquired", O_CREAT, 0600, 0);
    (void) unlink(path);
    fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0600);
    if (fd == -1 || locked == SEM_FAILED || release == SEM_FAILED ||
        acquired == SEM_FAILED) {
        test_fail("flock setlkw fixture", strerror(errno));
        goto out;
    }
    (void) close(fd);
    fd = -1;

    if (posix_spawn(&holder, "/bin/syscall_testing", NULL, NULL, hold_argv,
                    NULL) != 0) {
        test_fail("flock setlkw holder spawn", strerror(errno));
        holder = -1;
        goto out;
    }
    (void) clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += 5;
    if (sem_timedwait(locked, &deadline) == -1) {
        test_fail("flock setlkw holder acquired", "holder never signalled");
        goto out;
    }

    if (posix_spawn(&waiter, "/bin/syscall_testing", NULL, NULL, wait_argv,
                    NULL) != 0) {
        test_fail("flock setlkw waiter spawn", strerror(errno));
        waiter = -1;
        goto out;
    }

    /* Give the waiter time to reach fcntl() and block, then confirm it has NOT
     * acquired the lock while the holder still owns the range.
     */
    nap.tv_sec = 0;
    nap.tv_nsec = 300 * 1000 * 1000;
    (void) nanosleep(&nap, NULL);
    errno = 0;
    check(sem_trywait(acquired) == -1 && errno == EAGAIN,
          "flock setlkw waiter blocks",
          "F_SETLKW must block on the conflict, not acquire or fail early");

    /* Release the holder; the server must wake the waiter and grant the lock.
     */
    (void) sem_post(release);
    (void) waitpid(holder, NULL, 0);
    holder = -1;

    (void) clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += 5;
    if (sem_timedwait(acquired, &deadline) == -1) {
        test_fail("flock setlkw waiter granted",
                  "the blocked F_SETLKW must be granted once the holder releases");
        goto out;
    }
    test_ok("flock setlkw waiter granted");
    (void) waitpid(waiter, NULL, 0);
    waiter = -1;

out:
    if (holder != -1) {
        (void) sem_post(release);
        (void) waitpid(holder, NULL, 0);
    }
    if (waiter != -1) {
        (void) kill(waiter, SIGKILL);
        (void) waitpid(waiter, NULL, 0);
    }
    if (fd != -1)
        (void) close(fd);
    (void) unlink(path);
    if (locked != SEM_FAILED)
        (void) sem_close(locked);
    if (release != SEM_FAILED)
        (void) sem_close(release);
    if (acquired != SEM_FAILED)
        (void) sem_close(acquired);
    (void) sem_unlink("/flock-locked");
    (void) sem_unlink("/flock-release");
    (void) sem_unlink("/flock-acquired");
}

/* fpathconf() contract. The filesystem-invariant _PC_* values are answered
 * directly from the system limit/capability macros (no I/O service registers an
 * _IO_PATHCONF handler), so fpathconf reports NAME_MAX/PATH_MAX/PIPE_BUF/etc.
 * for any valid descriptor. A bad descriptor is still EBADF, _PC_2_SYMLINKS
 * resolves via the libc shim, and the remaining genuinely-per-filesystem names
 * fall through to the (currently unanswered) I/O service.
 */
static void test_posix_fpathconf_contract(void)
{
    int fd;

    errno = 0;
    check(fpathconf(-1, _PC_NAME_MAX) == -1 && errno == EBADF,
          "fpathconf bad fd", "fpathconf on a bad descriptor must fail EBADF");

    fd = open("/dev/zero", O_RDONLY);
    if (fd == -1) {
        test_fail("fpathconf fixture", strerror(errno));
        return;
    }

    errno = 0;
    check(fpathconf(fd, _PC_2_SYMLINKS) >= 0, "fpathconf _PC_2_SYMLINKS",
          "a _PC_2_SYMLINKS query must resolve to a value, not an error");
    errno = 0;
    check(fpathconf(fd, 0x7fff) == -1 && errno == EINVAL,
          "fpathconf bad name", "an unknown _PC_* name must fail EINVAL");

    /* Filesystem-invariant limits and capabilities are answered from the system
     * constants for any valid descriptor.
     */
    check(fpathconf(fd, _PC_NAME_MAX) == NAME_MAX, "fpathconf _PC_NAME_MAX",
          "must report the system NAME_MAX");
    check(fpathconf(fd, _PC_PATH_MAX) == PATH_MAX, "fpathconf _PC_PATH_MAX",
          "must report the system PATH_MAX");
    check(fpathconf(fd, _PC_LINK_MAX) == LINK_MAX, "fpathconf _PC_LINK_MAX",
          "must report the system LINK_MAX");
    check(fpathconf(fd, _PC_NO_TRUNC) == _POSIX_NO_TRUNC, "fpathconf _PC_NO_TRUNC",
          "must report the _POSIX_NO_TRUNC capability");
    check(fpathconf(fd, _PC_VDISABLE) == _POSIX_VDISABLE, "fpathconf _PC_VDISABLE",
          "must report the _POSIX_VDISABLE value");
    check(fpathconf(fd, _PC_CHOWN_RESTRICTED) == 1,
          "fpathconf _PC_CHOWN_RESTRICTED",
          "must report restricted ownership changes");
    check(fpathconf(fd, _PC_ASYNC_IO) == _POSIX_ASYNC_IO,
          "fpathconf _PC_ASYNC_IO",
          "must report the async-I/O capability");
    check(fpathconf(fd, _PC_PRIO_IO) == _POSIX_PRIO_IO,
          "fpathconf _PC_PRIO_IO",
          "must report the prioritized-I/O capability");
    check(fpathconf(fd, _PC_SYNC_IO) == _POSIX_SYNC_IO,
          "fpathconf _PC_SYNC_IO",
          "must report the synchronized-I/O capability");
    check(fpathconf(fd, _PC_FILESIZEBITS) == (long) (sizeof(off_t) * CHAR_BIT),
          "fpathconf _PC_FILESIZEBITS",
          "must report the file offset width");
    check(fpathconf(fd, _PC_SYMLINK_MAX) == _POSIX_SYMLINK_MAX,
          "fpathconf _PC_SYMLINK_MAX",
          "must report the POSIX symlink target maximum");
    errno = 0;
    check(fpathconf(fd, _PC_ALLOC_SIZE_MIN) == -1 && errno == 0,
          "fpathconf _PC_ALLOC_SIZE_MIN indeterminate",
          "valid indeterminate _PC_* names must return -1 without errno");

    /* _PC_PIPE_BUF is NOT invariant: the pipe service reports its own atomic
     * write size, while a non-pipe descriptor falls back to the system
     * PIPE_BUF.
     */
    check(fpathconf(fd, _PC_PIPE_BUF) == PIPE_BUF, "fpathconf non-pipe _PC_PIPE_BUF",
          "a non-pipe descriptor reports the system PIPE_BUF default");
    (void) close(fd);

    {
        int pfd[2];
        if (pipe(pfd) == 0) {
            check(fpathconf(pfd[0], _PC_PIPE_BUF) > 0,
                  "fpathconf pipe _PC_PIPE_BUF",
                  "a pipe reports its atomic write size from the pipe service");
            (void) close(pfd[0]);
            (void) close(pfd[1]);
        }
    }
}

/* pathconf() contract. The path-based sibling of fpathconf() resolves the path
 * with access(F_OK) and reports the same invariant limits from libc; an
 * unresolvable path must fail with the access() errno.
 */
static void test_posix_pathconf_contract(void)
{
    check(pathconf("/dev/zero", _PC_NAME_MAX) == NAME_MAX, "pathconf _PC_NAME_MAX",
          "pathconf must report the system NAME_MAX for a valid path");
    check(pathconf("/dev/zero", _PC_PATH_MAX) == PATH_MAX, "pathconf _PC_PATH_MAX",
          "pathconf must report the system PATH_MAX for a valid path");
    check(pathconf("/dev/zero", _PC_CHOWN_RESTRICTED) == 1,
          "pathconf _PC_CHOWN_RESTRICTED",
          "pathconf must report restricted ownership changes for a valid path");
    check(pathconf("/dev/zero", _PC_SYNC_IO) == _POSIX_SYNC_IO,
          "pathconf _PC_SYNC_IO",
          "pathconf must report synchronized I/O support for a valid path");
    check(pathconf("/dev/zero", _PC_FILESIZEBITS) ==
              (long) (sizeof(off_t) * CHAR_BIT),
          "pathconf _PC_FILESIZEBITS",
          "pathconf must report the file offset width for a valid path");
    check(pathconf("/dev/zero", _PC_SYMLINK_MAX) == _POSIX_SYMLINK_MAX,
          "pathconf _PC_SYMLINK_MAX",
          "pathconf must report the POSIX symlink target maximum");
    errno = 0;
    check(pathconf("/dev/zero", _PC_ALLOC_SIZE_MIN) == -1 && errno == 0,
          "pathconf _PC_ALLOC_SIZE_MIN indeterminate",
          "valid indeterminate _PC_* names must return -1 without errno");
    check(pathconf("/dev/zero", _PC_PIPE_BUF) == PIPE_BUF, "pathconf _PC_PIPE_BUF",
          "a non-FIFO pathname reports the system PIPE_BUF default");
    errno = 0;
    check(pathconf("/dev/zero", 0x7fff) == -1 && errno == EINVAL,
          "pathconf bad name", "an unknown _PC_* name must fail EINVAL");
    errno = 0;
    check(pathconf("/dev/zero", 11) == -1 && errno == EINVAL,
          "pathconf private name rejected",
          "roottask-private fpathconf names must not become POSIX pathconf names");

    errno = 0;
    check(pathconf("/no/such/path", _PC_NAME_MAX) == -1 && errno == ENOENT,
          "pathconf missing path",
          "pathconf on an unresolvable path must fail with the access() errno");
    errno = 0;
    check(pathconf(NULL, _PC_NAME_MAX) == -1 && errno == EFAULT,
          "pathconf NULL path",
          "pathconf must reject a NULL path with EFAULT");
}

/* confstr() contract. RTMux advertises confstr() (include/unistd.h) and seeds a
 * handful of _CS_ strings into the syspage typed-strings table
 * (_CS_ARCHITECTURE/_CS_RELEASE/_CS_VERSION, servers/roottask/sys/init.c). A
 * seeded name returns its value plus the byte count (including the NUL); a
 * zero-length call returns that same required size without writing. An INVALID
 * name (below _CS_PATH, in the gap below _CS_VENDOR) returns 0 and sets
 * errno=EINVAL, while a valid but unseeded name returns 0 without EINVAL -- the
 * name-range check lets confstr tell the two apart.
 */
static void test_posix_confstr_contract(void)
{
    char buf[64];
    size_t n, q;

    memset(buf, 0, sizeof buf);
    n = confstr(_CS_ARCHITECTURE, buf, sizeof buf);
    check(n > 0 && n <= sizeof buf && buf[0] != '\0',
          "confstr _CS_ARCHITECTURE",
          "a seeded _CS_ name must return its value and byte count");

    q = confstr(_CS_ARCHITECTURE, buf, 0);
    check(q == n, "confstr size query",
          "a zero-length confstr must report the required size");

    /* Invalid names (negative, and the gap between the standard and vendor
     * ranges) return 0 and set EINVAL.
     */
    errno = 0;
    check(confstr(-1, buf, sizeof buf) == 0 && errno == EINVAL,
          "confstr negative name", "an invalid _CS_ name must set EINVAL");
    errno = 0;
    check(confstr(100, buf, sizeof buf) == 0 && errno == EINVAL,
          "confstr out-of-range name", "a gap _CS_ name must set EINVAL");

    /* A valid but unseeded vendor name returns 0 -- but is not flagged EINVAL.
     */
    errno = 0;
    check(confstr(_CS_VENDOR, buf, sizeof buf) == 0 && errno != EINVAL,
          "confstr valid unseeded name",
          "a valid unseeded _CS_ name returns 0 without EINVAL");
}

/* uname() contract (Q14). The kernel publishes its release and build id in the
 * syspage build_info record; uname() must report them as release and version,
 * every field must be NUL-terminated, a NULL pointer must fail with EFAULT, and
 * roottask's /sys/kernelversion must carry the same pair.
 */
static void test_uname_matches_syspage(void)
{
    struct utsname u;
    const struct build_info_entry *bi = SYSPAGE_ENTRY(build_info);
    char want[80], got[80];
    ssize_t n;
    int fd;

    check(SYSPAGE_ENTRY_SIZE(build_info) >= sizeof(*bi),
          "uname syspage build_info present",
          "the syspage must carry a full build_info record");
    check(bi->build_id[0] != '\0' && bi->release[0] != '\0',
          "uname syspage build_info filled",
          "the kernel must publish a release and a build id");

    memset(&u, 0xA5, sizeof u);
    check(uname(&u) == 0, "uname succeeds", "uname(&u) must return 0");
    check(memchr(u.sysname, '\0', sizeof u.sysname) &&
              memchr(u.nodename, '\0', sizeof u.nodename) &&
              memchr(u.release, '\0', sizeof u.release) &&
              memchr(u.version, '\0', sizeof u.version) &&
              memchr(u.machine, '\0', sizeof u.machine),
          "uname fields NUL-terminated",
          "every utsname field must be a terminated string");
    check(strcmp(u.sysname, "RTMux") == 0, "uname sysname",
          "sysname must be RTMux");
    check(strcmp(u.machine, "riscv64") == 0, "uname machine",
          "machine must be riscv64");
    check(u.nodename[0] != '\0', "uname nodename",
          "nodename must be non-empty (/etc/hostname or rtmux)");
    check(strncmp(u.release, bi->release, sizeof bi->release) == 0,
          "uname release matches syspage",
          "release must be the syspage release");
    check(strncmp(u.version, bi->build_id, sizeof bi->build_id) == 0,
          "uname version matches syspage",
          "version must be the syspage build id");

    errno = 0;
    check(uname(NULL) == -1 && errno == EFAULT, "uname NULL pointer rejected",
          "uname(NULL) must fail with EFAULT");

    snprintf(want, sizeof want, "%.63s %.15s\n", bi->release, bi->build_id);
    memset(got, 0, sizeof got);
    fd = open("/sys/kernelversion", O_RDONLY);
    n = fd >= 0 ? read(fd, got, sizeof got - 1) : -1;
    if (fd >= 0)
        close(fd);
    check(n > 0 && strcmp(got, want) == 0, "/sys/kernelversion matches syspage",
          "/sys/kernelversion must be \"<release> <build id>\\n\"");
}

static int contains_field(const char *buf, ssize_t n, const char *field)
{
    size_t len = strlen(field);

    for (ssize_t i = 0; i + (ssize_t) len < n; i++) {
        if ((i == 0 || buf[i - 1] == '\n') &&
            memcmp(buf + i, field, len) == 0 && buf[i + (ssize_t) len] == ' ')
            return 1;
    }
    return 0;
}

static int read_counter_from_buf(const char *buf,
                                 ssize_t n,
                                 const char *field,
                                 unsigned long long *total)
{
    size_t field_len = strlen(field);

    for (ssize_t i = 0; i + (ssize_t) field_len < n; i++) {
        if ((i == 0 || buf[i - 1] == '\n') &&
            memcmp(buf + i, field, field_len) == 0 &&
            buf[i + (ssize_t) field_len] == ' ') {
            *total = strtoull(buf + i + field_len + 1, NULL, 10);
            return 0;
        }
    }
    errno = EINVAL;
    return -1;
}

static int read_namespace_counter(const char *field_name,
                                  unsigned long long *total)
{
    char buf[512];
    int fd;
    ssize_t n;

    fd = open("/sys/namespace-stats", O_RDONLY);
    if (fd == -1)
        return -1;
    n = read(fd, buf, sizeof(buf) - 1);
    (void) close(fd);
    if (n < 0)
        return -1;
    buf[n] = '\0';

    return read_counter_from_buf(buf, n, field_name, total);
}

static int read_cap_self_counter(const char *field_name,
                                 unsigned long long *total)
{
    char buf[1024];
    int fd;
    ssize_t n;

    fd = open("/cap/self", O_RDONLY);
    if (fd == -1)
        return -1;
    n = read(fd, buf, sizeof(buf) - 1);
    (void) close(fd);
    if (n < 0)
        return -1;
    buf[n] = '\0';
    return read_counter_from_buf(buf, n, field_name, total);
}

static int read_namespace_total(unsigned long long *total)
{
    return read_namespace_counter("total", total);
}

static int read_ioserv_accounting_total(const char *field,
                                        unsigned long long *total)
{
    char buf[1024];
    int fd;
    ssize_t n;
    size_t field_len = strlen(field);

    fd = open("/sys/ioserv-accounting", O_RDONLY);
    if (fd == -1)
        return -1;
    n = read(fd, buf, sizeof(buf) - 1);
    (void) close(fd);
    if (n < 0)
        return -1;
    buf[n] = '\0';

    for (ssize_t i = 0; i + (ssize_t) field_len < n; i++) {
        if ((i == 0 || buf[i - 1] == '\n') &&
            memcmp(buf + i, field, field_len) == 0 &&
            buf[i + (ssize_t) field_len] == ' ') {
            *total = strtoull(buf + i + field_len + 1, NULL, 10);
            return 0;
        }
    }
    errno = EINVAL;
    return -1;
}

static void test_namespace_stats_sysfs(void)
{
    char buf[512];
    struct stat st;
    int fd;
    ssize_t n;

    fd = open("/sys/namespace-stats", O_RDONLY);
    if (fd == -1) {
        test_fail("namespace stats open", strerror(errno));
        return;
    }

    n = read(fd, buf, sizeof(buf) - 1);
    if (n < 0) {
        test_fail("namespace stats read", strerror(errno));
        (void) close(fd);
        return;
    }
    buf[n] = '\0';

    check(contains_field(buf, n, "total") && contains_field(buf, n, "open") &&
              contains_field(buf, n, "readlink") &&
              contains_field(buf, n, "cache_misses") &&
              contains_field(buf, n, "cache_uncacheable_bypasses") &&
              contains_field(buf, n, "namespace_version"),
          "namespace stats fields",
          "/sys/namespace-stats must expose lookup, cache, and namespace "
          "counters");

    check(fstat(fd, &st) == EOK && S_ISREG(st.st_mode), "namespace stats stat",
          "stats endpoint must be a regular sysfs file");

    (void) close(fd);
}

static void test_cap_self_introspection(void)
{
    char buf[512];
    struct stat st;
    unsigned long long handle_count;
    unsigned long long fd_count;
    unsigned long long side_count;
    unsigned long long cap_alloc_count;
    unsigned long long endpoint_cap_count;
    unsigned long long first_endpoint_cap;
    int fd;
    ssize_t n;

    fd = open("/cap/self", O_RDONLY);
    if (fd == -1) {
        test_fail("cap self open", strerror(errno));
        return;
    }

    n = read(fd, buf, sizeof(buf) - 1);
    if (n < 0) {
        test_fail("cap self read", strerror(errno));
        (void) close(fd);
        return;
    }
    buf[n] = '\0';

    check(contains_field(buf, n, "version") && contains_field(buf, n, "pid") &&
              contains_field(buf, n, "scope") &&
              contains_field(buf, n, "endpoint_rights") &&
              contains_field(buf, n, "native_cap_slots") &&
              contains_field(buf, n, "transfer_max") &&
              contains_field(buf, n, "handle_count") &&
              contains_field(buf, n, "handle_fd_count") &&
              contains_field(buf, n, "handle_side_channel_count") &&
              contains_field(buf, n, "endpoint_cap_count") &&
              contains_field(buf, n, "first_endpoint_cap") &&
              contains_field(buf, n, "first_endpoint_rights") &&
              contains_field(buf, n, "cap_alloc_count") &&
              contains_field(buf, n, "cap_revoke_count") &&
              contains_field(buf, n, "cap_lookup_denied_count") &&
              contains_field(buf, n, "retired_slots") &&
              contains_field(buf, n, "last_denied") &&
              !strstr(buf, "unavailable") && strstr(buf, "redaction self-only"),
          "cap self fields",
          "/cap/self must expose read-only capability introspection metadata");

    check(
        read_counter_from_buf(buf, n, "handle_count", &handle_count) == 0 &&
            read_counter_from_buf(buf, n, "handle_fd_count", &fd_count) == 0 &&
            read_counter_from_buf(buf, n, "handle_side_channel_count",
                                  &side_count) == 0 &&
            read_counter_from_buf(buf, n, "endpoint_cap_count",
                                  &endpoint_cap_count) == 0 &&
            read_counter_from_buf(buf, n, "first_endpoint_cap",
                                  &first_endpoint_cap) == 0 &&
            read_counter_from_buf(buf, n, "cap_alloc_count",
                                  &cap_alloc_count) == 0 &&
            handle_count >= fd_count && handle_count >= side_count &&
            handle_count == fd_count + side_count && fd_count > 0 &&
            endpoint_cap_count > 0 && first_endpoint_cap != 0 &&
            cap_alloc_count > 0,
        "cap self handle counts",
        "/cap/self must expose live per-process handle and cap counters");

    check(fstat(fd, &st) == EOK && S_ISREG(st.st_mode), "cap self stat",
          "/cap/self must stat as a regular read-only file");

    (void) close(fd);

    fd = open("/cap/self", O_WRONLY);
    check(fd == -1 && errno == EROFS, "cap self read-only",
          "/cap/self must reject write opens without granting authority");
    if (fd != -1)
        (void) close(fd);
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

static void test_cap_self_native_messages(void)
{
    struct _io_native_stat stmsg;
    struct _io_native_read rdmsg;
    struct stat st;
    struct _server_info info;
    unsigned long long first_cap;
    char buf[64];
    int fd;
    int ret;

    if (read_cap_self_counter("first_endpoint_cap", &first_cap) == -1 ||
        first_cap == 0) {
        test_fail("cap self native transfer fixture", strerror(errno));
        return;
    }

    fd = open("/cap/self", O_RDONLY);
    if (fd == -1) {
        test_fail("cap self native fixture", strerror(errno));
        return;
    }

    memset(&stmsg, 0, sizeof(stmsg));
    init_native_hdr(&stmsg.hdr, _IO_NATIVE_STAT);
    stmsg.hdr.msg.combine_len = sizeof(stmsg);
    ret = msg_send(fd, &stmsg, sizeof(stmsg), &st, sizeof(st));
    check(ret == EOK && S_ISREG(st.st_mode) && st.st_size > 0,
          "cap self native stat",
          "native STAT must describe the authorized /cap/self object");

    memset(&info, 0, sizeof(info));
    if (connect_server_info(0, (uint64_t) fd, &info) != fd ||
        info.endpoint_cap == 0) {
        test_fail("cap self endpoint cap query", strerror(errno));
        (void) close(fd);
        return;
    }
    memset(&st, 0, sizeof(st));
    errno = 0;
    ret =
        msg_send_cap(info.endpoint_cap, &stmsg, sizeof(stmsg), &st, sizeof(st));
    check(ret == EOK && S_ISREG(st.st_mode), "cap self endpoint-cap send",
          "64-bit endpoint capability handles must authorize MsgSend");

    memset(&rdmsg, 0, sizeof(rdmsg));
    init_native_hdr(&rdmsg.hdr, _IO_NATIVE_READ);
    rdmsg.hdr.msg.combine_len = sizeof(rdmsg);
    rdmsg.nbytes = sizeof(buf) - 1;
    ret = msg_send(fd, &rdmsg, sizeof(rdmsg), buf, sizeof(buf) - 1);
    if (ret != EOK) {
        test_fail("cap self native read", strerror(errno));
    } else {
        buf[sizeof(buf) - 1] = '\0';
        check(!!strstr(buf, "version 1"), "cap self native read",
              "native READ must return /cap/self bytes without path lookup");
    }

    rdmsg.hdr.version = 0;
    ret = msg_send(fd, &rdmsg, sizeof(rdmsg), buf, sizeof(buf));
    check(ret == -1 && errno == EINVAL, "cap self native bad version",
          "native messages must reject unsupported protocol versions");

    init_native_hdr(&rdmsg.hdr, _IO_NATIVE_READ);
    rdmsg.hdr.msg.combine_len = sizeof(rdmsg);
    rdmsg.hdr.nxfers = _IO_NATIVE_CAP_TRANSFER_MAX + 1;
    ret = msg_send(fd, &rdmsg, sizeof(rdmsg), buf, sizeof(buf));
    check(ret == -1 && errno == E2BIG, "cap self native transfer bound",
          "native messages must reject oversize transfer metadata");

    init_native_hdr(&rdmsg.hdr, _IO_NATIVE_READ);
    rdmsg.hdr.msg.combine_len = sizeof(rdmsg);
    rdmsg.hdr.nxfers = 1;
    rdmsg.hdr.xfers[0].id = first_cap;
    rdmsg.hdr.xfers[0].rights = RTMUX_CAP_ENDPOINT_SEND;
    ret = msg_send(fd, &rdmsg, sizeof(rdmsg), buf, sizeof(buf));
    check(ret == EOK, "cap self native transfer install",
          "native messages must install bounded transferred endpoint caps");

    init_native_hdr(&rdmsg.hdr, _IO_NATIVE_WRITE);
    rdmsg.hdr.msg.combine_len = sizeof(rdmsg);
    rdmsg.hdr.nxfers = 0;
    ret = msg_send(fd, &rdmsg, sizeof(rdmsg), buf, sizeof(buf));
    check(ret == -1 && errno == EROFS, "cap self native read-only",
          "native WRITE must not grant authority on /cap/self");

    (void) close(fd);

    errno = 0;
    ret =
        msg_send_cap(info.endpoint_cap, &stmsg, sizeof(stmsg), &st, sizeof(st));
    check(ret == -1 && errno == EBADF, "cap self endpoint-cap stale send",
          "revoked endpoint capability handles must fail after close");
}

static void test_logger_native_messages(void)
{
    struct _io_native_stat stmsg;
    struct _io_native_read rdmsg;
    struct stat st;
    unsigned long long first_cap;
    char buf[96];
    int fd;
    int fd2;
    int ret;

    if (read_cap_self_counter("first_endpoint_cap", &first_cap) == -1 ||
        first_cap == 0) {
        test_fail("logger native transfer fixture", strerror(errno));
        return;
    }

    fd = open("/dev/slog", O_RDONLY | O_NONBLOCK);
    if (fd == -1) {
        test_fail("logger native fixture", strerror(errno));
        return;
    }

    memset(&stmsg, 0, sizeof(stmsg));
    init_native_hdr(&stmsg.hdr, _IO_NATIVE_STAT);
    stmsg.hdr.msg.combine_len = sizeof(stmsg);
    ret = msg_send(fd, &stmsg, sizeof(stmsg), &st, sizeof(st));
    check(ret == EOK && S_ISCHR(st.st_mode), "logger native stat",
          "standalone server native STAT must describe /dev/slog");

    memset(&rdmsg, 0, sizeof(rdmsg));
    init_native_hdr(&rdmsg.hdr, _IO_NATIVE_READ);
    rdmsg.hdr.msg.combine_len = sizeof(rdmsg);
    rdmsg.nbytes = sizeof(buf) - 1;
    ret = msg_send(fd, &rdmsg, sizeof(rdmsg), buf, sizeof(buf) - 1);
    if (ret == EOK) {
        buf[sizeof(buf) - 1] = '\0';
        check(strstr(buf, "buffer_ints") && strstr(buf, "used_ints"),
              "logger native read",
              "standalone server native READ must expose status bytes");
    } else {
        test_fail("logger native read", strerror(errno));
    }

    init_native_hdr(&rdmsg.hdr, _IO_NATIVE_WRITE);
    rdmsg.hdr.msg.combine_len = sizeof(rdmsg);
    ret = msg_send(fd, &rdmsg, sizeof(rdmsg), buf, sizeof(buf));
    check(ret == -1 && errno == EROFS, "logger native read-only",
          "standalone server native WRITE must fail closed");

    errno = 0;
    check(unlink("/dev/slog") == -1, "logger unlink fails closed",
          "device unlink must not clear /dev/slog");
    fd2 = open("/dev/slog", O_RDONLY | O_NONBLOCK);
    check(fd2 != -1, "logger unlink preserves node",
          "device unlink must not remove /dev/slog");
    if (fd2 != -1)
        (void) close(fd2);

    init_native_hdr(&rdmsg.hdr, _IO_NATIVE_READ);
    rdmsg.hdr.msg.combine_len = sizeof(rdmsg);
    rdmsg.hdr.nxfers = 1;
    rdmsg.hdr.xfers[0].id = first_cap;
    rdmsg.hdr.xfers[0].rights = RTMUX_CAP_ENDPOINT_SEND;
    ret = msg_send(fd, &rdmsg, sizeof(rdmsg), buf, sizeof(buf));
    check(ret == EOK, "logger native transfer install",
          "standalone native messages must receive bounded transferred "
          "endpoint caps");

    (void) close(fd);
}

static void test_namespace_version(void)
{
    const char path[] = "/dev/name/local/rtmux-version-probe";
    unsigned long long before;
    unsigned long long after;
    unsigned long long linked;
    int chid;

    if (read_namespace_counter("namespace_version", &before) == -1) {
        test_fail("namespace version baseline", strerror(errno));
        return;
    }

    chid = channel_create(0);
    if (chid == -1) {
        test_fail("namespace version fixture channel", strerror(errno));
        return;
    }
    (void) unlink(path);
    if (namespace_link(path, 0, getpid(), chid, 0, _FTYPE_NAME,
                       NAMESPACE_FLAG_STICKY) == -1) {
        test_fail("namespace version link", strerror(errno));
        (void) channel_destroy(chid);
        return;
    }
    if (read_namespace_counter("namespace_version", &linked) == -1) {
        test_fail("namespace version link readback", strerror(errno));
        (void) unlink(path);
        (void) channel_destroy(chid);
        return;
    }
    check(linked == before + 1, "namespace version link",
          "namespace_version must move on namespace attach");

    if (unlink(path) == -1) {
        test_fail("namespace version unlink", strerror(errno));
        (void) channel_destroy(chid);
        return;
    }
    if (read_namespace_counter("namespace_version", &after) == -1) {
        test_fail("namespace version unlink readback", strerror(errno));
        (void) channel_destroy(chid);
        return;
    }
    check(after == linked + 1, "namespace version unlink",
          "namespace_version must move on namespace detach");
    (void) channel_destroy(chid);
}

struct open_race_args {
    const char *name;
    atomic_int stop;
    atomic_int done;
    atomic_int failures;
};

static struct open_race_args namespace_open_race_args;

static void *namespace_open_race_churner(void *arg)
{
    struct open_race_args *args = arg;

    while (!atomic_load_explicit(&args->stop, memory_order_acquire)) {
        name_attach_t *attach = name_attach(NULL, args->name, 0);

        if (!attach) {
            if (errno != EEXIST)
                atomic_fetch_add_explicit(&args->failures, 1,
                                          memory_order_relaxed);
            continue;
        }
        if (name_detach(attach, 0) != EOK)
            atomic_fetch_add_explicit(&args->failures, 1, memory_order_relaxed);
    }
    atomic_store_explicit(&args->done, 1, memory_order_release);
    return NULL;
}

static void test_namespace_open_race(void)
{
    enum { ITERS = 128 };
    const char name[] = "rtmux-open-version-race";
    const char path[] = "/dev/name/local/rtmux-open-version-race";
    struct open_race_args *args = &namespace_open_race_args;
    pthread_t tid;
    int rc;
    int clean;
    int stale;
    int bad;

    args->name = name;
    (void) unlink(path);
    atomic_store_explicit(&args->stop, 0, memory_order_relaxed);
    atomic_store_explicit(&args->done, 0, memory_order_relaxed);
    atomic_store_explicit(&args->failures, 0, memory_order_relaxed);

    rc = pthread_create(&tid, NULL, namespace_open_race_churner, args);
    if (rc != EOK) {
        test_fail("namespace open race setup", strerror(rc));
        return;
    }
    (void) pthread_detach(tid);

    /* Progress lines: a guest that stalls here leaves the last one in the log,
     * which says whether the open loop or the wait for the churner stopped.
     */
    printf("  namespace open race: churner started, %d opens\n", (int) ITERS);
    fflush(stdout);
    clean = stale = bad = 0;
    for (int i = 0; i < ITERS; i++) {
        int fd;

        if (i && i % 32 == 0) {
            printf("  namespace open race: %d opens done\n", i);
            fflush(stdout);
        }

        errno = 0;
        fd = open("/sys/namespace-stats", O_RDONLY);
        if (fd != -1) {
            clean++;
            (void) close(fd);
        } else if (errno == ESTALE) {
            stale++;
        } else {
            bad++;
        }
    }

    printf("  namespace open race: opens done, stopping churner\n");
    fflush(stdout);
    atomic_store_explicit(&args->stop, 1, memory_order_release);
    while (!atomic_load_explicit(&args->done, memory_order_acquire))
        (void) __sched_yield();

    check(atomic_load_explicit(&args->failures, memory_order_relaxed) == 0 &&
              bad == 0 && clean + stale == ITERS,
          "namespace open race",
          "open under namespace churn must return a fd or ESTALE only");
    (void) unlink(path);
}

struct entry_generation_race_args {
    const char *name;
    atomic_int stop;
    atomic_int done;
    atomic_int failures;
};

static struct entry_generation_race_args entry_generation_race_args;

static void *entry_generation_race_toggler(void *arg)
{
    struct entry_generation_race_args *args = arg;

    while (!atomic_load_explicit(&args->stop, memory_order_acquire)) {
        name_attach_t *attach = name_attach(NULL, args->name, 0);

        if (!attach) {
            if (errno != EEXIST)
                atomic_fetch_add_explicit(&args->failures, 1,
                                          memory_order_relaxed);
            continue;
        }
        if (name_detach(attach, 0) != EOK)
            atomic_fetch_add_explicit(&args->failures, 1, memory_order_relaxed);
    }
    atomic_store_explicit(&args->done, 1, memory_order_release);
    return NULL;
}

static void test_namespace_open_entry_generation_race(void)
{
    enum { ITERS = 128 };
    const char name[] = "rtmux-entry-generation-race";
    const char path[] = "/dev/name/local/rtmux-entry-generation-race";
    struct entry_generation_race_args *args = &entry_generation_race_args;
    pthread_t tid;
    int rc;
    int clean;
    int stale;
    int absent;
    int bad;
    int last_bad_errno;
    int access_open_mismatch;
    int mismatch_without_generation_bump;

    (void) unlink(path);
    args->name = name;
    atomic_store_explicit(&args->stop, 0, memory_order_relaxed);
    atomic_store_explicit(&args->done, 0, memory_order_relaxed);
    atomic_store_explicit(&args->failures, 0, memory_order_relaxed);

    rc = pthread_create(&tid, NULL, entry_generation_race_toggler, args);
    if (rc != EOK) {
        test_fail("namespace entry-generation race setup", strerror(rc));
        return;
    }
    (void) pthread_detach(tid);

    clean = stale = absent = bad = last_bad_errno = access_open_mismatch = 0;
    mismatch_without_generation_bump = 0;
    for (int i = 0; i < ITERS; i++) {
        unsigned long long gen_before;
        unsigned long long gen_mid;
        unsigned long long gen_after;
        int fd;
        int saw_access;
        int have_gen;

        /* Bracket the access with two samples of the generation. A removal
         * that falls between the access and a single sample after it would
         * bump before the sample and read as a miss with no bump, while one
         * sample before it lets an attach in the same gap hide a removal that
         * never bumped. Only when the two agree did the access see the entry
         * at that generation, so any later removal must move it on.
         */
        gen_before = gen_mid = gen_after = 0;
        have_gen =
            read_namespace_counter("namespace_version", &gen_before) != -1;
        saw_access = access(path, F_OK) == 0;
        if (saw_access)
            have_gen = have_gen &&
                       read_namespace_counter("namespace_version", &gen_mid) !=
                           -1;
        errno = 0;
        fd = name_open(name, 0);
        if (fd != -1) {
            clean++;
            (void) name_close(fd);
        } else if (errno == ESTALE) {
            stale++;
        } else if (errno == ENOENT) {
            absent++;
            if (saw_access) {
                access_open_mismatch++;
                if (!have_gen ||
                    read_namespace_counter("namespace_version", &gen_after) ==
                        -1 ||
                    (gen_mid == gen_before && gen_after == gen_mid))
                    mismatch_without_generation_bump++;
            }
        } else {
            bad++;
            last_bad_errno = errno;
        }
    }

    atomic_store_explicit(&args->stop, 1, memory_order_release);
    while (!atomic_load_explicit(&args->done, memory_order_acquire))
        (void) __sched_yield();
    (void) unlink(path);

    {
        char detail[128];

        snprintf(detail, sizeof(detail),
                 "failures=%d clean=%d stale=%d absent=%d bad=%d errno=%d",
                 atomic_load_explicit(&args->failures, memory_order_relaxed),
                 clean, stale, absent, bad, last_bad_errno);
        check(atomic_load_explicit(&args->failures, memory_order_relaxed) ==
                  0 &&
                  bad == 0 && clean + stale + absent == ITERS,
              "namespace open entry-generation race", detail);
    }
    {
        char detail[96];

        snprintf(detail, sizeof(detail), "mismatch=%d no_bump=%d",
                 access_open_mismatch, mismatch_without_generation_bump);
        check(mismatch_without_generation_bump == 0,
              "namespace access/open generation", detail);
    }
}

/* ns_open_deferral: a stat or access that was in flight when an unlink of the
 * same node arrived still returns its result and leaves no connection state
 * behind.
 *
 * The namespace server parks the unlink behind the first request and restarts
 * it when that finishes. It used to answer the request first, with no result,
 * and skip the combined stat and close, so the call "succeeded" with a zeroed
 * buffer and its binding stayed on the connection id for good: the next request
 * that was given that id failed with EBUSY, long after the race. That is the
 * second round, an access that follows a stat. A node named
 * rtmux-open-stall-MS holds each open of it for MS ms in a CONFIG_DEBUG_INJECT
 * image, which keeps the first request in flight while the unlink arrives;
 * elsewhere it is not held and the case is skipped.
 */
#define NSD_STALL_MS 600
#define NSD_NAME "rtmux-open-stall-600"
#define NSD_PATH "/dev/name/local/" NSD_NAME
#define NSD_ATTEMPTS 3

static struct {
    bool use_access;
    atomic_int started;
    int rc;
    int err;
    struct stat st;
    long ms;
} nsd;

static long nsd_ms_since(const struct timespec *t0)
{
    struct timespec t1;

    clock_gettime(CLOCK_MONOTONIC, &t1);
    return (t1.tv_sec - t0->tv_sec) * 1000L + (t1.tv_nsec - t0->tv_nsec) / 1000000L;
}

static void *nsd_thread(void *arg)
{
    struct timespec t0;

    (void) arg;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    atomic_store(&nsd.started, 1);
    errno = 0;
    nsd.rc = nsd.use_access ? access(NSD_PATH, F_OK) : stat(NSD_PATH, &nsd.st);
    nsd.err = errno;
    nsd.ms = nsd_ms_since(&t0);
    return NULL;
}

/* One round. Returns 1 when the request was held and the unlink parked behind
 * it, 0 when the unlink was not held (a late thread; the caller retries), and
 * -1 after a failure it has reported.
 */
static int nsd_round(bool use_access)
{
    const char *what = use_access ? "access" : "stat";
    char name[48], why[112];
    name_attach_t *attach;
    pthread_t th;
    struct timespec t0;
    long unlink_ms;
    int unlink_rc, unlink_err;

    (void) unlink(NSD_PATH);
    attach = name_attach(NULL, NSD_NAME, 0);
    if (!attach) {
        test_fail("ns_open_deferral fixture", strerror(errno));
        return -1;
    }
    memset(&nsd, 0, sizeof nsd);
    nsd.use_access = use_access;
    if (pthread_create(&th, NULL, nsd_thread, NULL) != 0) {
        test_fail("ns_open_deferral fixture", "pthread_create");
        (void) name_detach(attach, 0);
        return -1;
    }
    while (!atomic_load(&nsd.started))
        (void) __sched_yield();
    /* The request is held in the server by now; this unlink parks behind it. */
    (void) nanosleep(&(struct timespec){.tv_nsec = (NSD_STALL_MS / 4) * 1000000L}, NULL);
    clock_gettime(CLOCK_MONOTONIC, &t0);
    errno = 0;
    unlink_rc = unlink(NSD_PATH);
    unlink_err = errno;
    unlink_ms = nsd_ms_since(&t0);
    (void) pthread_join(th, NULL);
    (void) name_detach(attach, 0);

    if (unlink_ms < NSD_STALL_MS / 4)
        return 0; /* it ran before the request reached the server, or after it */

    snprintf(name, sizeof name, "ns_open_deferral %s result", what);
    snprintf(why, sizeof why, "rc %d errno %d mode %#o after %ld ms; unlink rc %d errno %d", nsd.rc,
             nsd.err, (unsigned) nsd.st.st_mode, nsd.ms, unlink_rc, unlink_err);
    check(nsd.rc == 0 && (use_access || S_ISNAM(nsd.st.st_mode)), name, why);
    snprintf(name, sizeof name, "ns_open_deferral %s parked unlink", what);
    check(unlink_rc == 0 || unlink_err == ENOENT, name,
          "the unlink parked behind the request must complete");
    return 1;
}

int ns_open_deferral(void)
{
    struct stat st;
    struct timespec t0;
    name_attach_t *attach;
    long ms;
    int rc;

    /* Is the stall in this image? A held stat of the fixture node takes the
     * stall; judged on its time alone when it succeeds, so that a stat that
     * fails fast is a failure and not a skip.
     */
    (void) unlink(NSD_PATH);
    attach = name_attach(NULL, NSD_NAME, 0);
    if (!attach) {
        test_fail("ns_open_deferral fixture", strerror(errno));
        return 0;
    }
    clock_gettime(CLOCK_MONOTONIC, &t0);
    rc = stat(NSD_PATH, &st);
    ms = nsd_ms_since(&t0);
    (void) name_detach(attach, 0);
    if (rc != 0) {
        test_fail("ns_open_deferral fixture", strerror(errno));
        return 0;
    }
    if (ms < NSD_STALL_MS / 2)
        return TC_EXIT_SKIP;

    for (int round = 0; round < 2; round++) {
        int held = 0;

        for (int i = 0; i < NSD_ATTEMPTS && !held; i++)
            held = nsd_round(round == 1);
        if (held < 0)
            return 0;
        if (!held) {
            test_fail("ns_open_deferral", "the unlink was never held behind the request");
            return 0;
        }
    }
    return 0;
}

static void test_namespace_uncacheable_bypass(void)
{
    const char name[] = "rtmux-uncacheable-dir";
    const char path[] = "/dev/name/local/rtmux-uncacheable-dir";
    unsigned long long before;
    unsigned long long after;
    int chid;
    int fd;

    (void) unlink(path);

    chid = channel_create(0);
    if (chid == -1) {
        test_fail("namespace uncacheable fixture channel", strerror(errno));
        return;
    }

    if (read_namespace_counter("cache_uncacheable_bypasses", &before) == -1) {
        test_fail("namespace uncacheable baseline", strerror(errno));
        (void) channel_destroy(chid);
        return;
    }

    if (namespace_link(path, 0, getpid(), chid, 0, _FTYPE_NAME,
                       NAMESPACE_FLAG_STICKY | NAMESPACE_FLAG_DIR |
                           NAMESPACE_FLAG_UNCACHEABLE) == -1) {
        test_fail("namespace uncacheable register", strerror(errno));
        (void) channel_destroy(chid);
        return;
    }

    fd = name_open(name, 0);
    if (fd == -1) {
        test_fail("namespace uncacheable lookup", strerror(errno));
        (void) unlink(path);
        (void) channel_destroy(chid);
        return;
    }
    (void) name_close(fd);

    if (read_namespace_counter("cache_uncacheable_bypasses", &after) == -1) {
        test_fail("namespace uncacheable readback", strerror(errno));
        (void) unlink(path);
        (void) channel_destroy(chid);
        return;
    }

    check(after > before, "namespace uncacheable bypass counter",
          "lookups under an uncacheable registration must bypass cache policy");

    (void) unlink(path);
    (void) channel_destroy(chid);
}

static void test_memobj_metadata_devmem(void)
{
    devctl_mem_object_info info;
    int fd;

    fd = open("/dev/mem", O_RDONLY);
    if (fd == -1) {
        test_fail("memobj metadata /dev/mem open", strerror(errno));
        return;
    }

    memset(&info, 0, sizeof info);
    if (devctl(fd, DEVCTL_MEM_OBJECT_INFO, &info, sizeof info, NULL) != EOK) {
        test_fail("memobj metadata /dev/mem query", strerror(errno));
        (void) close(fd);
        return;
    }

    check(info.version == 1 && info.type == MEMOBJ_INFO_TYPE_DEV_MEM &&
              (info.flags & MEMOBJ_INFO_FLAG_FILE_BACKED) &&
              (info.flags & MEMOBJ_INFO_FLAG_PHYSICAL) &&
              (info.flags & MEMOBJ_INFO_FLAG_PRIVILEGED) &&
              info.cache_policy == MEMOBJ_INFO_CACHE_DEVICE_NOCACHE &&
              info.size != 0,
          "memobj metadata /dev/mem",
          "/dev/mem must expose explicit physical and cacheability metadata");

    (void) close(fd);
}

static int open_typed_mem_sysram_flags(int tflag, int oflag)
{
    return posix_typed_mem_open("/sysram", oflag, tflag);
}

static int open_typed_mem_sysram(int tflag)
{
    return open_typed_mem_sysram_flags(tflag, O_RDWR);
}

static void test_memobj_metadata_typed(void)
{
    devctl_mem_object_info info;
    struct posix_typed_mem_info posix_info;
    int fd;

    fd = open_typed_mem_sysram(POSIX_TYPED_MEM_ALLOCATE);
    if (fd == -1) {
        test_fail("memobj metadata typed open", strerror(errno));
        return;
    }

    memset(&info, 0, sizeof info);
    if (devctl(fd, DEVCTL_MEM_OBJECT_INFO, &info, sizeof info, NULL) != EOK) {
        test_fail("memobj metadata typed query", strerror(errno));
        (void) close(fd);
        return;
    }

    check(info.version == 1 && info.type == MEMOBJ_INFO_TYPE_TYPED &&
              (info.flags & MEMOBJ_INFO_FLAG_FILE_BACKED) &&
              (info.flags & MEMOBJ_INFO_FLAG_TYPED) &&
              (info.flags & MEMOBJ_INFO_FLAG_ALLOCATE) &&
              info.tflags == POSIX_TYPED_MEM_ALLOCATE,
          "memobj metadata typed",
          "typed-memory fds must expose allocation and type metadata");

    memset(&posix_info, 0xff, sizeof posix_info);
    check(posix_typed_mem_get_info(fd, &posix_info) == 0 &&
              posix_info.__reserved[0] == 0,
          "posix_typed_mem_get_info typed fd",
          "POSIX typed-memory info must return typed metadata");

    (void) close(fd);
}

static void test_memobj_handle_backed_shared_mapping(void)
{
    enum { MAP_LEN = 4096 };
    const char path[] = "/dev/shmem/rtmux-memcap";
    flock_t space;
    uint64_t ro_handle;
    uint64_t handle;
    char *addr;
    char *fd_addr;
    off_t map_off;
    size_t contig_len;
    int map_fd;
    int fd;

    (void) unlink(path);
    fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0600);
    if (fd == -1) {
        test_fail("memobj handle shm fixture", strerror(errno));
        return;
    }

    memset(&space, 0, sizeof(space));
    space.l_whence = SEEK_SET;
    space.l_len = MAP_LEN;
    if (fcntl(fd, F_ALLOCSP, &space) == -1) {
        test_fail("memobj handle shm resize", strerror(errno));
        (void) close(fd);
        (void) unlink(path);
        return;
    }

    handle = memobj_handle_from_fd(
        fd, RTMUX_CAP_MEM_MAP_READ | RTMUX_CAP_MEM_MAP_WRITE);
    if (handle == 0) {
        test_fail("memobj handle create", strerror(errno));
        (void) close(fd);
        (void) unlink(path);
        return;
    }

    addr = mmap_handle(NULL, MAP_LEN, PROT_READ | PROT_WRITE, MAP_SHARED,
                       handle, 0);
    if (addr == MAP_FAILED) {
        test_fail("memobj handle mmap", strerror(errno));
        (void) memobj_handle_close(handle);
        (void) close(fd);
        (void) unlink(path);
        return;
    }

    addr[0] = 'R';
    addr[1] = 'T';
    check(addr[0] == 'R' && addr[1] == 'T', "memobj handle shared mapping",
          "memory-object caps must authorize shared mmap without reusing fd");

    (void) munmap(addr, MAP_LEN);

    fd_addr = mmap(NULL, MAP_LEN, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (fd_addr == MAP_FAILED) {
        test_fail("posix_mem_offset mmap fixture", strerror(errno));
    } else {
        map_off = -1;
        contig_len = 0;
        map_fd = NOFD;
        check(posix_mem_offset(fd_addr, MAP_LEN, &map_off, &contig_len,
                               &map_fd) == 0 &&
                  map_off == 0 && contig_len == MAP_LEN && map_fd == fd,
              "posix_mem_offset shared fd mapping",
              "fd-backed shared mappings must report fd-relative offset");
        (void) munmap(fd_addr, MAP_LEN);
    }

    ro_handle = memobj_handle_from_fd(fd, RTMUX_CAP_MEM_MAP_READ);
    if (ro_handle == 0) {
        test_fail("memobj read-only handle create", strerror(errno));
    } else {
        errno = 0;
        addr = mmap_handle(NULL, MAP_LEN, PROT_WRITE, MAP_SHARED, ro_handle, 0);
        check(addr == MAP_FAILED && errno == EBADF,
              "memobj read-only handle denies write mmap",
              "memory-object caps must enforce map rights independent of fd");
        if (addr != MAP_FAILED)
            (void) munmap(addr, MAP_LEN);
        (void) memobj_handle_close(ro_handle);
    }

    check(memobj_handle_close(handle) == EOK, "memobj handle close",
          "memory-object cap close must revoke the handle");

    errno = 0;
    addr = mmap_handle(NULL, MAP_LEN, PROT_READ, MAP_SHARED, handle, 0);
    check(addr == MAP_FAILED && errno == EBADF, "memobj stale handle mmap",
          "revoked memory-object caps must not authorize mmap");
    if (addr != MAP_FAILED)
        (void) munmap(addr, MAP_LEN);

    (void) close(fd);
    (void) unlink(path);
}

static void test_memobj_handle_backed_typed_lifecycle(void)
{
    uint64_t handle;
    int fd;

    fd = open_typed_mem_sysram(POSIX_TYPED_MEM_ALLOCATE);
    if (fd == -1) {
        test_fail("memobj typed handle fixture", strerror(errno));
        return;
    }

    handle = memobj_handle_from_fd(fd, RTMUX_CAP_MEM_MAP_READ);
    if (handle == 0) {
        test_fail("memobj typed handle create", strerror(errno));
        (void) close(fd);
        return;
    }

    check(memobj_handle_close(handle) == EOK, "memobj typed handle close",
          "typed memory-object caps must revoke cleanly");
    (void) close(fd);
}

static void test_memobj_readonly_shared_write_denied(void)
{
    enum { MAP_LEN = 4096 };
    void *addr;
    int fd;
    int rc;

    fd = open("/dev/zero", O_RDONLY);
    if (fd == -1) {
        test_fail("memobj readonly mmap fixture", strerror(errno));
        return;
    }

    addr = mmap(NULL, MAP_LEN, PROT_READ, MAP_SHARED, fd, 0);
    if (addr == MAP_FAILED) {
        test_fail("memobj readonly mmap", strerror(errno));
        (void) close(fd);
        return;
    }

    errno = 0;
    rc = mprotect(addr, MAP_LEN, PROT_READ | PROT_WRITE);
    check(rc == -1 && (errno == EACCES || errno == EPERM),
          "memobj readonly mprotect denied",
          "read-only shared memory objects must not gain write authority");

    (void) munmap(addr, MAP_LEN);
    (void) close(fd);
}

static void test_posix_msync_contract(void)
{
    enum { MAP_LEN = 4096 };
    const char path[] = "/dev/shmem/rtmux-msync";
    flock_t space;
    char *file_addr;
    char *addr;
    int fd;

    (void) unlink(path);
    fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0600);
    if (fd == -1) {
        test_fail("msync shmem fixture", strerror(errno));
        return;
    }
    memset(&space, 0, sizeof(space));
    space.l_whence = SEEK_SET;
    space.l_len = MAP_LEN;
    if (fcntl(fd, F_ALLOCSP, &space) == -1) {
        test_fail("msync shmem resize", strerror(errno));
        (void) close(fd);
        (void) unlink(path);
        return;
    }
    file_addr = mmap(NULL, MAP_LEN, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (file_addr == MAP_FAILED) {
        test_fail("msync shmem mmap", strerror(errno));
        (void) close(fd);
        (void) unlink(path);
        return;
    }

    file_addr[0] = 's';
    errno = 0;
    check(msync(file_addr, MAP_LEN, MS_SYNC) == 0, "msync shared shmem mapping",
          "msync must accept a shared file-backed RAM mapping");

    (void) munmap(file_addr, MAP_LEN);
    (void) close(fd);
    (void) unlink(path);

    addr = mmap(NULL, MAP_LEN, PROT_READ | PROT_WRITE,
                MAP_PRIVATE | MAP_ANON, NOFD, 0);
    if (addr == MAP_FAILED) {
        test_fail("msync mmap fixture", strerror(errno));
        return;
    }

    addr[0] = 'm';
    errno = 0;
    check(msync(addr, MAP_LEN, MS_SYNC) == 0, "msync mapped page",
          "msync must accept a valid mapped range");

    errno = 0;
    check(msync(addr, MAP_LEN, MS_SYNC | MS_ASYNC) == -1 && errno == EINVAL,
          "msync rejects sync and async",
          "MS_SYNC and MS_ASYNC are mutually exclusive");

    errno = 0;
    check(msync(addr, MAP_LEN, 0x40000000) == -1 && errno == EINVAL,
          "msync rejects unknown flags", "unknown msync flags must be EINVAL");

    (void) munmap(addr, MAP_LEN);

    errno = 0;
    check(msync(addr, MAP_LEN, MS_SYNC) == -1 && errno == ENOMEM,
          "msync rejects unmapped range", "unmapped msync ranges must be ENOMEM");
}

static void test_memobj_stale_mapping_faults_after_munmap(void)
{
    char *argv[] = {
        [0] = "syscall_testing",
        [1] = "--munmap-stale-read",
        [2] = NULL,
    };
    pid_t pid;
    int status = 0;
    int rc;

    rc = posix_spawn(&pid, "/bin/syscall_testing", NULL, NULL, argv, NULL);
    if (rc != EOK) {
        test_fail("memobj stale-map child fixture", strerror(rc));
        return;
    }

    check(waitpid(pid, &status, 0) == pid && WIFSIGNALED(status) &&
              WTERMSIG(status) == SIGSEGV,
          "memobj stale mapping faults after munmap",
          "unmapped memory must not remain readable through stale TLB state");
}

static void test_ioserv_accounting_sysfs(void)
{
    char buf[1024];
    struct stat st;
    int fd;
    ssize_t n;

    fd = open("/sys/ioserv-accounting", O_RDONLY);
    if (fd == -1) {
        test_fail("I/O service accounting open", strerror(errno));
        return;
    }

    n = read(fd, buf, sizeof(buf) - 1);
    if (n < 0) {
        test_fail("I/O service accounting read", strerror(errno));
        (void) close(fd);
        return;
    }
    buf[n] = '\0';

    check(contains_field(buf, n, "total_current_objects") &&
              contains_field(buf, n, "total_highwater_objects"),
          "I/O service accounting totals",
          "/sys/ioserv-accounting must expose current and high-water totals");
    check(strstr(buf, "\ntype server current ") ||
              strncmp(buf, "type server current ", 20) == 0,
          "I/O service accounting type fields",
          "/sys/ioserv-accounting must expose object counts by type");
    check(strstr(buf, "\npid ") || strncmp(buf, "pid ", 4) == 0,
          "I/O service accounting pid fields",
          "/sys/ioserv-accounting must expose object counts by creator pid");

    check(fstat(fd, &st) == EOK && S_ISREG(st.st_mode),
          "I/O service accounting stat",
          "accounting endpoint must be a regular sysfs file");

    (void) close(fd);
}

static void test_ioserv_reply_status_taxonomy(void)
{
    ioserv_reply_counters_t before;
    ioserv_reply_counters_t after;
    char buf[256];

    check(ioserv_reply_status_from_errno(EOK) == IOSERV_REPLY_OK &&
              strcmp(ioserv_reply_status_name(IOSERV_REPLY_OK), "OK") == 0,
          "I/O service status OK", "EOK must map to OK");
    check(ioserv_reply_status_from_errno(EACCES) == IOSERV_REPLY_DENIED &&
              ioserv_reply_status_from_errno(EPERM) == IOSERV_REPLY_DENIED,
          "I/O service status denied",
          "authority failures must map to DENIED");
    check(ioserv_reply_status_from_errno(ETIMEDOUT) == IOSERV_REPLY_TIMEOUT,
          "I/O service status timeout", "timeouts must map to TIMEOUT");
    check(ioserv_reply_status_from_errno(EINVAL) == IOSERV_REPLY_BAD_REQUEST,
          "I/O service status bad request",
          "malformed requests must map to BAD_REQUEST");
    check(ioserv_reply_status_from_errno(ENOENT) ==
              IOSERV_REPLY_UNAVAILABLE,
          "I/O service status unavailable",
          "missing resources must map to UNAVAILABLE");
    check(ioserv_reply_status_from_errno(EBUSY) == IOSERV_REPLY_BUSY,
          "I/O service status busy", "busy queues must map to BUSY");
    check(ioserv_reply_status_from_errno(EFAULT) == IOSERV_REPLY_FAULTED,
          "I/O service status faulted", "copy faults must map to FAULTED");

    ioserv_reply_counters_snapshot(&before);
    ioserv_reply_counters_record_receive(2);
    ioserv_reply_counters_record(EACCES, 2);
    ioserv_reply_counters_record(ETIMEDOUT, 1);
    ioserv_reply_counters_record(EINVAL, 3);
    ioserv_reply_counters_snapshot(&after);

    check(after.sends == before.sends + 3 &&
              after.receives == before.receives + 1 &&
              after.denied_sends == before.denied_sends + 1 &&
              after.timeouts == before.timeouts + 1 &&
              after.max_depth >= 3,
          "I/O service status counters",
          "canonical counters must move monotonically by status class");
    check(ioserv_reply_counters_format(buf, sizeof(buf)) == EOK &&
              strstr(buf, "ioserv_sends ") &&
              strstr(buf, "ioserv_denied_sends ") &&
              strstr(buf, "ioserv_timeouts ") &&
              strstr(buf, "ioserv_queue_full ") &&
              strstr(buf, "ioserv_max_depth "),
          "I/O service counter text",
          "stats text must expose the canonical counter names");
    check(ioserv_reply_counters_format(buf, 8) == ENOSPC,
          "I/O service counter text bounds",
          "short stats buffers must not publish truncated counters");
}

static void test_ioserv_accounting_create_destroy(void)
{
    const char link_path[] = "/rtmux-accounting-test-link";
    unsigned long long before;
    unsigned long long after_create;
    unsigned long long after_unlink;

    (void) unlink(link_path);
    if (read_ioserv_accounting_total("total_current_objects", &before) == -1) {
        test_fail("I/O service accounting baseline", strerror(errno));
        return;
    }

    if (namespace_symlink("/dev/null", link_path) == -1) {
        test_fail("I/O service accounting create fixture", strerror(errno));
        return;
    }
    if (read_ioserv_accounting_total("total_current_objects", &after_create) ==
        -1) {
        test_fail("I/O service accounting after create", strerror(errno));
        (void) unlink(link_path);
        return;
    }
    check(after_create == before + 1, "I/O service accounting create count",
          "creating a persistent namespace object must increase accounting");

    if (unlink(link_path) == -1) {
        test_fail("I/O service accounting unlink fixture", strerror(errno));
        return;
    }
    if (read_ioserv_accounting_total("total_current_objects", &after_unlink) ==
        -1) {
        test_fail("I/O service accounting after unlink", strerror(errno));
        return;
    }
    check(after_unlink == before, "I/O service accounting destroy count",
          "destroying the namespace object must restore the current total");
}

static void test_ioserv_accounting_admission_limit(void)
{
    enum { ATTEMPTS = 80 };
    char paths[ATTEMPTS][40];
    unsigned long long before;
    unsigned long long after_cleanup;
    int created = 0;
    int limited = 0;

    if (read_ioserv_accounting_total("total_current_objects", &before) == -1) {
        test_fail("I/O service admission baseline", strerror(errno));
        return;
    }

    for (int i = 0; i < ATTEMPTS; i++) {
        snprintf(paths[i], sizeof(paths[i]), "/rtmux-account-limit-%02d", i);
        (void) unlink(paths[i]);
        if (namespace_symlink("/dev/null", paths[i]) == -1) {
            if (errno == EDQUOT) {
                limited = 1;
                break;
            }

            /* Anything else at the ceiling — notably ENOMEM — means the quota
             * path regressed to a generic allocation failure instead of
             * enforcing EDQUOT.
             */
            test_fail("I/O service admission fixture", strerror(errno));
            break;
        }
        created++;
    }

    check(limited, "I/O service accounting admission limit",
          "namespace accounting must reject over-limit persistent state with "
          "EDQUOT");

    for (int i = 0; i < created; i++)
        (void) unlink(paths[i]);

    if (read_ioserv_accounting_total("total_current_objects", &after_cleanup) ==
        -1) {
        test_fail("I/O service admission cleanup readback", strerror(errno));
        return;
    }
    check(after_cleanup == before, "I/O service accounting admission cleanup",
          "over-limit test cleanup must restore object counters");
}

static void test_namespace_symlink_loop_is_bounded(void)
{
    const char loop_path[] = "/rtmux-eloop-self";
    int fd;

    (void) unlink(loop_path);
    if (namespace_symlink(loop_path, loop_path) == -1) {
        test_fail("namespace symlink loop fixture", strerror(errno));
        return;
    }

    errno = 0;
    fd = open(loop_path, O_RDONLY);
    check(fd == -1 && errno == ELOOP, "namespace symlink loop rejects open",
          "namespace resolution must fail bounded symlink loops with ELOOP");
    if (fd != -1)
        (void) close(fd);

    (void) unlink(loop_path);
}

/* A mutual symlink cycle (a -> b -> a) is a distinct topology from a direct
 * self-loop: a resolver that spots self-reference by comparing a link's target
 * to its own path would catch a -> a yet walk a -> b -> a forever, so only the
 * hop counter (SYMLOOP_MAX) bounds it. Both the open() and stat() resolvers
 * must terminate the walk with ELOOP rather than spinning.
 */
static void test_namespace_symlink_mutual_loop_is_bounded(void)
{
    const char a[] = "/rtmux-eloop-a";
    const char b[] = "/rtmux-eloop-b";
    struct stat st;
    int fd;

    (void) unlink(a);
    (void) unlink(b);
    if (namespace_symlink(b, a) == -1 || namespace_symlink(a, b) == -1) {
        test_fail("mutual symlink loop fixture", strerror(errno));
        (void) unlink(a);
        (void) unlink(b);
        return;
    }

    errno = 0;
    fd = open(a, O_RDONLY);
    check(fd == -1 && errno == ELOOP, "namespace mutual symlink loop open",
          "a mutual symlink cycle must fail open() with ELOOP");
    if (fd != -1)
        (void) close(fd);

    errno = 0;
    check(stat(a, &st) == -1 && errno == ELOOP,
          "namespace mutual symlink loop stat",
          "a mutual symlink cycle must fail stat() with ELOOP");

    (void) unlink(a);
    (void) unlink(b);
}

static void test_transport_does_not_roundtrip_namespace(void)
{
    unsigned long long before, after_baseline, after_transport;
    unsigned long long stats_read_delta;
    int fd[2] = {
        [0] = -1,
        [1] = -1,
    };
    char byte = 'x';

    if (pipe(fd) == -1) {
        test_fail("pipe transport namespace setup", strerror(errno));
        return;
    }
    if (read_namespace_total(&before) == -1 ||
        read_namespace_total(&after_baseline) == -1) {
        test_fail("namespace stats baseline", strerror(errno));
        (void) close(fd[0]);
        (void) close(fd[1]);
        return;
    }
    stats_read_delta = after_baseline - before;

    if (write(fd[1], &byte, 1) != 1 || read(fd[0], &byte, 1) != 1) {
        test_fail("pipe transport namespace io", strerror(errno));
        (void) close(fd[0]);
        (void) close(fd[1]);
        return;
    }

    if (read_namespace_total(&after_transport) == -1) {
        test_fail("namespace stats transport readback", strerror(errno));
        (void) close(fd[0]);
        (void) close(fd[1]);
        return;
    }

    check(after_transport - after_baseline == stats_read_delta,
          "transport avoids namespace roundtrip",
          "read/write on an existing pipe must not add namespace lookups");

    (void) close(fd[0]);
    (void) close(fd[1]);
}

static void test_at_syscalls_at_fdcwd(void)
{
    char buf[64];
    struct stat st;
    int fd;
    ssize_t n;

    fd = openat(AT_FDCWD, "/sys/namespace-stats", O_RDONLY);
    if (fd == -1) {
        test_fail("openat AT_FDCWD", strerror(errno));
        return;
    }
    check(fstat(fd, &st) == EOK && S_ISREG(st.st_mode), "openat fstat",
          "openat(AT_FDCWD) must return a usable file descriptor");
    (void) close(fd);

    check(fstatat(AT_FDCWD, "/sys/namespace-stats", &st, 0) == EOK &&
              S_ISREG(st.st_mode),
          "fstatat AT_FDCWD", "fstatat(AT_FDCWD) must resolve path names");

    n = readlinkat(AT_FDCWD, "/etc", buf, sizeof(buf));
    check(n > 0, "readlinkat AT_FDCWD",
          "readlinkat(AT_FDCWD) must resolve namespace links");

    check(faccessat(AT_FDCWD, "/dev/zero", F_OK, 0) == 0,
          "faccessat AT_FDCWD F_OK",
          "faccessat(AT_FDCWD) must resolve an existing path");
    check(faccessat(AT_FDCWD, "/dev/zero", R_OK, AT_EACCESS) == 0,
          "faccessat AT_EACCESS",
          "faccessat with AT_EACCESS checks access via the effective ids");
    check(faccessat(AT_FDCWD, "/dev/zero", F_OK, AT_SYMLINK_NOFOLLOW) == 0,
          "faccessat AT_SYMLINK_NOFOLLOW",
          "faccessat must accept AT_SYMLINK_NOFOLLOW on a non-symlink path");
    errno = 0;
    check(faccessat(AT_FDCWD, "/no/such/path", F_OK, 0) == -1 &&
              errno == ENOENT,
          "faccessat missing path",
          "faccessat on a missing path must fail ENOENT");
    errno = 0;
    check(faccessat(AT_FDCWD, "/dev/zero", F_OK, 0x0400) == -1 &&
              errno == EINVAL,
          "faccessat bad flag",
          "faccessat must reject an undefined AT_ flag with EINVAL");
}

static void test_namespace_sandbox_conformance(void)
{
    const char hidden_name[] = "rtmux-sandbox-hidden";
    const char hidden_path[] = "/dev/name/local/rtmux-sandbox-hidden";
    const char hidden_link[] = "/rtmux-sandbox-hidden-link";
    name_attach_t *attach;
    char cwd[128];
    char proc_fd_path[64];
    struct stat st;
    int cwd_fd;
    int fd;
    int probe;

    fd = open("/dev/zero", O_RDONLY);
    check(fd != -1, "namespace sandbox minimal allowed open",
          "allowed device paths must behave like normal POSIX opens");
    if (fd != -1)
        (void) close(fd);

    fd = open("/cap/self", O_WRONLY);
    check(fd == -1 && errno == EROFS,
          "namespace sandbox visible unauthorized denied",
          "visible resources without write authority must fail capability "
          "checks");
    if (fd != -1)
        (void) close(fd);

    fd = open("/net/tcp/clone", O_RDWR);
    check(fd == -1 && errno == ENOENT, "namespace sandbox network denied",
          "network-denied namespace shape must not expose /net/tcp/clone");
    if (fd != -1)
        (void) close(fd);

    fd = open("/fs/root", O_RDONLY);
    check(fd == -1 && errno == ENOENT, "namespace sandbox lkl fs denied",
          "absent lkl-fsd must not expose /fs/root");
    if (fd != -1)
        (void) close(fd);

    fd = open("/bin/true", O_RDONLY);
    check(fd != -1, "namespace sandbox buildbox binary allowed",
          "buildbox-shaped namespaces must expose executable inputs");
    if (fd != -1)
        (void) close(fd);

    fd = open("/sys/namespace-stats", O_RDONLY);
    check(fd != -1, "namespace sandbox buildbox sysfs allowed",
          "buildbox-shaped namespaces must expose diagnostic read handles");
    if (fd != -1)
        (void) close(fd);

    cwd_fd = -1;
    check(chdir("/dev") == 0 && getcwd(cwd, sizeof(cwd)) &&
              !strcmp(cwd, "/dev") &&
              (cwd_fd = open("zero", O_RDONLY)) != -1,
          "namespace sandbox cwd allowed path",
          "relative opens inside an allowed cwd must keep POSIX behavior");
    if (cwd_fd != -1)
        (void) close(cwd_fd);
    (void) chdir("/");

    (void) unlink(hidden_link);
    (void) unlink(hidden_path);
    attach = name_attach(NULL, hidden_name, 0);
    if (!attach) {
        test_fail("namespace sandbox hidden fixture", strerror(errno));
        return;
    }
    fd = name_open(hidden_name, 0);
    if (fd == -1) {
        test_fail("namespace sandbox hidden fd", strerror(errno));
        (void) name_detach(attach, 0);
        return;
    }
    if (namespace_symlink(hidden_path, hidden_link) == -1) {
        test_fail("namespace sandbox hidden symlink fixture", strerror(errno));
        (void) name_close(fd);
        (void) name_detach(attach, 0);
        return;
    }

    if (unlink(hidden_path) == -1) {
        test_fail("namespace sandbox hide fixture", strerror(errno));
        (void) name_close(fd);
        (void) unlink(hidden_link);
        (void) name_detach(attach, 0);
        return;
    }
    test_ok("namespace sandbox hide fixture");

    errno = 0;
    probe = name_open(hidden_name, 0);
    check(probe == -1 && errno == ENOENT,
          "namespace sandbox hidden canonical path",
          "hidden names must not reopen through their canonical path");
    if (probe != -1)
        (void) name_close(probe);

    errno = 0;
    probe = open(hidden_link, O_RDONLY);
    check(probe == -1 && errno == ENOENT,
          "namespace sandbox hidden symlink traversal",
          "symlink traversal must not rediscover hidden names");
    if (probe != -1)
        (void) close(probe);

    errno = 0;
    if (chdir("/") == 0) {
        probe = open("rtmux-sandbox-hidden-link", O_RDONLY);
        check(probe == -1 && errno == ENOENT,
              "namespace sandbox hidden cwd traversal",
              "relative cwd traversal must not rediscover hidden names");
        if (probe != -1)
            (void) close(probe);
    } else {
        test_fail("namespace sandbox hidden cwd fixture", strerror(errno));
    }

    (void) snprintf(proc_fd_path, sizeof(proc_fd_path), "/proc/self/fd/%d", fd);
    (void) name_close(fd);
    errno = 0;
    probe = open(proc_fd_path, O_RDONLY);
    check(probe == -1 && errno == EBADF,
          "namespace sandbox stale procfd hidden fd",
          "stale /proc/self/fd paths must not recover hidden authority");
    if (probe != -1)
        (void) close(probe);

    errno = 0;
    check(stat(hidden_path, &st) == -1 && errno == ENOENT,
          "namespace sandbox hidden stat",
          "hidden names must not be discoverable by stat");

    (void) unlink(hidden_link);
    (void) name_detach(attach, 0);
}

#define POSIX_CAP_CHECK(tag, expr, detail) \
    check((expr), "posix-cap " #tag, (detail))
#define POSIX_CAP_ERR(tag, expr, want, detail) do {                         \
    errno = 0;                                                              \
    int rc_##tag = (expr);                                                  \
    check(rc_##tag == -1 && errno == (want), "posix-cap " #tag, (detail));  \
} while (0)

int posix_cap_boundary_child(void)
{
    int fd;

    if (chroot("/dev") != 0 || chdir("/") != 0)
        return EXIT_FAILURE;
    fd = open("/zero", O_RDONLY);
    if (fd == -1)
        return EXIT_FAILURE;
    (void) close(fd);
    errno = 0;
    if (open("/etc/passwd", O_RDONLY) != -1 || errno != ENOENT)
        return EXIT_FAILURE;
    errno = 0;
    if (open("../etc/passwd", O_RDONLY) != -1 || errno != ENOENT)
        return EXIT_FAILURE;
    return EXIT_SUCCESS;
}

void test_posix_cap_boundary(void)
{
    char *argv[] = {
        "syscall_testing",
        "--posix-cap-boundary-child",
        NULL,
    };
    struct stat st;
    char byte;
    pid_t pid;
    int status;
    int dfd = -1;
    int fd = -1;
    int dupfd = -1;
    int pfd[2] = {
        [0] = -1,
        [1] = -1,
    };
    int rc;

    POSIX_CAP_CHECK(stdio,
                    fstat(STDIN_FILENO, &st) == 0 &&
                        fstat(STDOUT_FILENO, &st) == 0 &&
                        fstat(STDERR_FILENO, &st) == 0,
                    "fd 0/1/2 must be ordinary inherited descriptors");

    dfd = open("/dev", O_RDONLY);
    POSIX_CAP_CHECK(dirfd, dfd != -1,
                    "a directory fd is the POSIX handle for relative lookup");
    if (dfd == -1)
        goto out;

    fd = openat(dfd, "zero", O_RDONLY);
    POSIX_CAP_CHECK(openat, fd != -1,
                    "openat must resolve relative to the directory fd");
    if (fd == -1)
        goto out;
    POSIX_CAP_CHECK(readfd, read(fd, &byte, 1) == 1,
                    "a readable fd must carry read authority");

    dupfd = dup2(fd, 7);
    POSIX_CAP_CHECK(dup2, dupfd == 7 && read(dupfd, &byte, 1) == 1,
                    "dup2 must derive another fd for the same open object");

    if (pipe(pfd) == -1) {
        test_fail("posix-cap pipe", strerror(errno));
        goto out;
    }
    POSIX_CAP_CHECK(pipe,
                    write(pfd[1], "x", 1) == 1 && read(pfd[0], &byte, 1) == 1 &&
                        byte == 'x',
                    "pipe endpoints must behave as fd capabilities");

    int cwd_fd = -1;
    POSIX_CAP_CHECK(chdir,
                    chdir("/dev") == 0 &&
                        (cwd_fd = open("null", O_RDONLY)) != -1,
                    "cwd is process state used by relative POSIX lookup");
    if (cwd_fd != -1)
        (void) close(cwd_fd);
    (void) chdir("/");

    POSIX_CAP_ERR(missing, openat(dfd, "no-such-entry", O_RDONLY), ENOENT,
                  "forbidden or absent paths must fail closed");

    rc = posix_spawn(&pid, "/bin/syscall_testing", NULL, NULL, argv, NULL);
    POSIX_CAP_CHECK(spawn, rc == EOK && pid > 0,
                    "posix_spawn must start a child with explicit inherited fds");
    if (rc == EOK && pid > 0) {
        POSIX_CAP_CHECK(wait,
                        waitpid(pid, &status, 0) == pid &&
                            WIFEXITED(status) && WEXITSTATUS(status) == 0,
                        "child namespace narrowing must preserve permitted paths "
                        "and reject paths outside its root");
    }

out:
    if (pfd[0] != -1)
        (void) close(pfd[0]);
    if (pfd[1] != -1)
        (void) close(pfd[1]);
    if (dupfd != -1)
        (void) close(dupfd);
    if (fd != -1)
        (void) close(fd);
    if (dfd != -1)
        (void) close(dfd);
    (void) chdir("/");
}

static int dir_has_entry(const char *path, const char *name)
{
    DIR *dir;
    struct dirent *de;
    int found;

    dir = opendir(path);
    if (!dir)
        return -1;

    found = 0;
    while ((de = readdir(dir))) {
        if (!strcmp(de->d_name, name)) {
            found = 1;
            break;
        }
    }

    (void) closedir(dir);
    return found;
}

static int proc_fd_dir_has(int fd)
{
    char path[64];
    char name[16];

    (void) snprintf(path, sizeof(path), "/proc/%d/fd", (int) getpid());
    (void) snprintf(name, sizeof(name), "%d", fd);
    return dir_has_entry(path, name);
}

static void test_proc_fd_directory_shape(void)
{
    char path[64];
    struct stat st;
    DIR *dir;
    int fd;
    int found;

    found = dir_has_entry("/proc/self", "fd");
    check(found == 1, "proc self fd entry",
          "/proc/self must advertise the fd directory");

    (void) snprintf(path, sizeof(path), "/proc/%d/fd", (int) getpid());
    check(stat(path, &st) == EOK && S_ISDIR(st.st_mode), "proc pid fd stat",
          "/proc/<pid>/fd must stat as a directory");

    dir = opendir(path);
    check(!!dir, "proc pid fd opendir",
          "/proc/<pid>/fd must open as a directory");
    if (dir)
        (void) closedir(dir);

    fd = open("/sys/namespace-stats", O_RDONLY);
    if (fd == -1) {
        test_fail("proc fd fixture open", strerror(errno));
        return;
    }
    found = proc_fd_dir_has(fd);
    check(found == 1, "proc pid fd lists descriptor",
          "/proc/<pid>/fd must list live descriptor numbers");
    (void) close(fd);
}

static void test_proc_fd_reopen(void)
{
    char path[64];
    char buf[64];
    struct stat st_orig;
    struct stat st_dup;
    int fd;
    int dupfd;
    int denied;
    ssize_t n;

    fd = open("/dev/zero", O_RDONLY);
    if (fd == -1) {
        test_fail("proc fd reopen fixture", strerror(errno));
        return;
    }

    (void) snprintf(path, sizeof(path), "/proc/self/fd/%d", fd);
    dupfd = open(path, O_RDONLY | O_CLOEXEC);
    if (dupfd == -1) {
        test_fail("proc fd reopen", strerror(errno));
        (void) close(fd);
        return;
    }

    check(fstat(fd, &st_orig) == EOK && fstat(dupfd, &st_dup) == EOK &&
              st_orig.st_mode == st_dup.st_mode,
          "proc fd reopen stat",
          "/proc/self/fd/<n> must reopen through the source descriptor");

    n = read(dupfd, buf, sizeof(buf));
    check(n == (ssize_t) sizeof(buf) && buf[0] == '\0', "proc fd reopen read",
          "reopened descriptor must use the source connection transport");

    denied = open(path, O_RDWR);
    check(denied == -1 && errno == EACCES, "proc fd reopen permissions",
          "reopening a read-only source descriptor must not widen access");
    if (denied != -1)
        (void) close(denied);

    (void) snprintf(path, sizeof(path), "/proc/0/fd/%d", fd);
    denied = open(path, O_RDONLY);
    check(denied == -1 && errno == ENOENT, "proc fd rejects pid zero",
          "/proc/0/fd/<n> must not alias the caller's fd table");
    if (denied != -1)
        (void) close(denied);

    (void) close(dupfd);
    n = read(fd, buf, sizeof(buf));
    check(n == (ssize_t) sizeof(buf) && buf[0] == '\0',
          "proc fd reopen close semantics",
          "closing the reopened descriptor must not close the source fd");

    (void) close(fd);
}

static void test_proc_fd_closed_descriptor_hidden(void)
{
    char path[64];
    struct _server_info info;
    int fd;
    int reopened;

    fd = open("/dev/zero", O_RDONLY);
    if (fd == -1) {
        test_fail("proc fd closed fixture", strerror(errno));
        return;
    }

    (void) snprintf(path, sizeof(path), "/proc/self/fd/%d", fd);
    (void) close(fd);

    check(connect_server_info(0, (uint64_t) fd, &info) != fd,
          "proc fd closed descriptor hidden",
          "closed descriptors must disappear from the fd connection table");

    reopened = open(path, O_RDONLY);
    check(reopened == -1 && errno == EBADF, "proc fd closed descriptor reopen",
          "stale /proc/self/fd paths must fail before fd-number reuse");
    if (reopened != -1)
        (void) close(reopened);
}

static void test_spawn_cloexec_fd_not_inherited(void)
{
    char fdarg[16];
    char *argv[] = {
        "syscall_testing",
        "--expect-fd-closed",
        fdarg,
        NULL,
    };
    pid_t pid;
    int child_fd;
    int status = 0;
    int rc;
    bool exited;

    child_fd = open("/dev/zero", O_RDONLY | O_CLOEXEC);
    if (child_fd == -1) {
        test_fail("spawn cloexec fixture", strerror(errno));
        return;
    }

    (void) snprintf(fdarg, sizeof(fdarg), "%d", child_fd);
    rc = posix_spawn(&pid, "/bin/syscall_testing", NULL, NULL, argv, NULL);
    if (rc != EOK) {
        test_fail("spawn cloexec child", strerror(rc));
        (void) close(child_fd);
        return;
    }

    exited = wait_child_exit(pid, &status, 100);

    check(exited && WIFEXITED(status) && WEXITSTATUS(status) == 0,
          "spawn cloexec fd not inherited",
          "close-on-exec descriptors must not leak into spawned children");
    (void) close(child_fd);
}

static void test_proc_wait_handle_shape(void)
{
    char path[64];
    char buf[16];
    struct stat st;
    int fd;
    ssize_t n;
    int found;

    found = dir_has_entry("/proc/self", "wait");
    check(found == 1, "proc self wait entry",
          "/proc/self must advertise the wait process handle");

    (void) snprintf(path, sizeof(path), "/proc/%d/wait", (int) getpid());
    check(stat(path, &st) == EOK && S_ISREG(st.st_mode), "proc pid wait stat",
          "/proc/<pid>/wait must stat as a file");

    fd = open(path, O_RDONLY);
    if (fd == -1) {
        test_fail("proc pid wait open", strerror(errno));
        return;
    }
    n = read(fd, buf, sizeof(buf) - 1);
    if (n < 0) {
        test_fail("proc pid wait read", strerror(errno));
        (void) close(fd);
        return;
    }
    buf[n] = '\0';
    check(!strcmp(buf, "live\n"), "proc pid wait live state",
          "/proc/<pid>/wait must report live for the current process");
    (void) close(fd);
}

static void test_proc_wait_handle_stale_after_reap(void)
{
    char *argv[] = {"true", NULL};
    char path[64];
    char buf[16];
    pid_t pid;
    int fd;
    int status;
    int rc;
    ssize_t n;

    rc = posix_spawn(&pid, "/bin/true", NULL, NULL, argv, NULL);
    if (rc != EOK) {
        test_fail("proc wait spawn fixture", strerror(rc));
        return;
    }

    (void) snprintf(path, sizeof(path), "/proc/%d/wait", (int) pid);
    fd = open(path, O_RDONLY);
    if (fd == -1) {
        test_fail("proc child wait open", strerror(errno));
        (void) waitpid(pid, &status, 0);
        return;
    }

    check(waitpid(pid, &status, 0) == pid && WIFEXITED(status),
          "proc wait child reap", "spawned child must be reapable");

    (void) lseek(fd, 0, SEEK_SET);
    n = read(fd, buf, sizeof(buf) - 1);
    if (n < 0) {
        test_fail("proc child wait stale read", strerror(errno));
        (void) close(fd);
        return;
    }
    buf[n] = '\0';
    check(!strcmp(buf, "stale\n"), "proc wait stale after reap",
          "open wait handle must not target a reaped process generation");
    (void) close(fd);
}

static void test_kill_zero_after_reap(void)
{
    char *argv[] = {"true", NULL};
    pid_t pid = (pid_t) -1;
    int status;
    int rc;

    rc = posix_spawn(&pid, "/bin/true", NULL, NULL, argv, NULL);
    if (rc != EOK) {
        test_fail("kill zero reap fixture", strerror(rc));
        return;
    }

    check(waitpid(pid, &status, 0) == pid && WIFEXITED(status),
          "kill zero reap wait", "spawned child must be reapable");

    errno = 0;
    check(kill(pid, 0) == -1 && errno == ESRCH, "kill zero stale pid",
          "kill(pid, 0) must not report a reaped process as live");
}

static void test_spawn_returns_wait_handle(void)
{
    char *argv[] = {"true", NULL};
    char buf[32];
    pid_t pid;
    int fd;
    int status;
    int rc;
    ssize_t n = -1;

    rc = rtmux_spawn_waitfd(&pid, &fd, "/bin/true", NULL, NULL, argv, NULL);
    if (rc != EOK) {
        test_fail("spawn waitfd fixture", strerror(rc));
        return;
    }

    for (int i = 0; i < 10000; i++) {
        (void) lseek(fd, 0, SEEK_SET);
        n = read(fd, buf, sizeof(buf) - 1);
        if (n < 0) {
            test_fail("spawn waitfd read", strerror(errno));
            (void) close(fd);
            (void) waitpid(pid, &status, 0);
            return;
        }
        buf[n] = '\0';
        if (!strcmp(buf, "exited 0\n"))
            break;
        (void) __sched_yield();
    }

    check(n > 0 && !strcmp(buf, "exited 0\n"), "spawn returns wait handle",
          "handle-returning spawn must expose the child exit status");

    (void) close(fd);
    check(waitpid(pid, &status, 0) == pid && WIFEXITED(status),
          "spawn waitfd child reap",
          "handle-returning spawn must leave the child waitpid-reapable");
}

static void test_spawn_missing_binary_is_unobservable(void)
{
    char *argv[] = {"missing-rtmux-test-binary", NULL};
    pid_t pid = (pid_t) -1;
    int fd = -1;
    int rc;

    rc = rtmux_spawn_waitfd(&pid, &fd, "/bin/missing-rtmux-test-binary", NULL,
                            NULL, argv, NULL);
    check(rc == ENOENT && pid == (pid_t) -1 && fd == -1,
          "spawn missing binary unobservable",
          "failed spawn must not publish a pid or wait fd before commit");
    if (fd != -1)
        (void) close(fd);
}

static void test_spawn_fault_reports_sigsegv(void)
{
    char *argv[] = {"syscall_testing", "--fault-sigsegv", NULL};
    pid_t pid;
    int status = 0;
    int rc;
    bool exited;

    rc = posix_spawn(&pid, "/bin/syscall_testing", NULL, NULL, argv, NULL);
    if (rc != EOK) {
        test_fail("spawn fault child fixture", strerror(rc));
        return;
    }

    exited = wait_child_exit(pid, &status, 500);
    check(exited && WIFSIGNALED(status) && WTERMSIG(status) == SIGSEGV,
          "spawn fault reports SIGSEGV",
          "faulting child must report WIFSIGNALED/SIGSEGV to waitpid");
}

static void test_mprotect_readonly_rejects_write(void)
{
    char *pos_argv[] = {"syscall_testing", "--mprotect-ro-readable", NULL};
    char *neg_argv[] = {"syscall_testing", "--fault-mprotect-ro", NULL};
    pid_t pid;
    int status = 0;
    int rc;
    bool exited;

    /* Proves the page-isolation primitive the servers depend on. The positive
     * control (map writable, fill, mprotect read-only, read back) must exit
     * cleanly, which pins every step before the final write as working. The
     * negative case then writes the read-only page, which must fault with
     * SIGSEGV. See --mprotect-ro-readable / --fault-mprotect-ro in main.c.
     */
    rc = posix_spawn(&pid, "/bin/syscall_testing", NULL, NULL, pos_argv, NULL);
    if (rc != EOK) {
        test_fail("mprotect read-only control fixture", strerror(rc));
        return;
    }
    exited = wait_child_exit(pid, &status, 500);
    check(exited && WIFEXITED(status) && WEXITSTATUS(status) == 0,
          "mprotect read-only stays readable",
          "fill, mprotect PROT_READ, and read-back must all succeed");

    rc = posix_spawn(&pid, "/bin/syscall_testing", NULL, NULL, neg_argv, NULL);
    if (rc != EOK) {
        test_fail("mprotect read-only fault fixture", strerror(rc));
        return;
    }
    exited = wait_child_exit(pid, &status, 500);
    check(exited && WIFSIGNALED(status) && WTERMSIG(status) == SIGSEGV,
          "mprotect read-only rejects write",
          "a write to an mprotect PROT_READ page must fault with SIGSEGV");
}

#define SPAWN_ARGV_READABLE(name, fd) \
    char *name##_argv[] = {"syscall_testing", "--expect-fd-readable", #fd, NULL}
#define SPAWN_ARGV_CLOSED(name, fd) \
    char *name##_argv[] = {"syscall_testing", "--expect-fd-closed", #fd, NULL}
#define SPAWN_ARGV_READABLE_CLOSED(name, rfd, cfd) \
    char *name##_argv[] = { \
        "syscall_testing", "--expect-fd-readable-closed", #rfd, #cfd, NULL \
    }

static void test_spawn_file_actions_addopen(void)
{
    posix_spawn_file_actions_t actions;
    SPAWN_ARGV_READABLE(read_stdin, 0);
    char *missing_argv[] = {"true", NULL};
    SPAWN_ARGV_READABLE_CLOSED(ordered, 4, 3);
    SPAWN_ARGV_READABLE(fd3_readable, 3);
    SPAWN_ARGV_CLOSED(fd3_closed, 3);
    SPAWN_ARGV_READABLE(fd4_readable, 4);
    SPAWN_ARGV_READABLE_CLOSED(fd4_readable_fd3_closed, 4, 3);
    SPAWN_ARGV_READABLE_CLOSED(fd5_readable_fd3_closed, 5, 3);
    SPAWN_ARGV_READABLE(fd40_readable, 40);
    const char side_effect_path[] = "/dev/shmem/spawn-noeffect";
    pid_t pid = (pid_t) -1;
    int status;
    int rc;

    rc = posix_spawn_file_actions_init(&actions);
    if (rc != EOK) {
        test_fail("spawn addopen init", strerror(rc));
        return;
    }
    if (posix_spawn_file_actions_addclose(&actions, STDIN_FILENO) != EOK ||
        posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/zero",
                                         O_RDONLY, 0) != EOK) {
        test_fail("spawn addopen replacement setup", "add action failed");
        (void) posix_spawn_file_actions_destroy(&actions);
        return;
    }
    rc = posix_spawn(&pid, "/bin/syscall_testing", &actions, NULL,
                     read_stdin_argv, NULL);
    (void) posix_spawn_file_actions_destroy(&actions);
    check(rc == EOK && pid > 0, "spawn addopen replacement",
          "addopen must replace an existing child descriptor");
    if (rc == EOK && pid > 0) {
        check(waitpid(pid, &status, 0) == pid && WIFEXITED(status) &&
                  WEXITSTATUS(status) == 0,
              "spawn addopen replacement child",
              "child must read the descriptor opened by addopen");
    }

    rc = posix_spawn_file_actions_init(&actions);
    if (rc != EOK) {
        test_fail("spawn addopen missing init", strerror(rc));
        return;
    }
    rc = posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null",
                                          O_RDONLY, 0);
    if (rc != EOK) {
        test_fail("spawn addopen missing setup", strerror(rc));
        (void) posix_spawn_file_actions_destroy(&actions);
        return;
    }
    rc = posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO,
                                          "/missing-spawn-addopen", O_RDONLY,
                                          0);
    if (rc != EOK) {
        test_fail("spawn addopen missing setup 2", strerror(rc));
        (void) posix_spawn_file_actions_destroy(&actions);
        return;
    }
    pid = (pid_t) -1;
    rc = posix_spawn(&pid, "/bin/true", &actions, NULL, missing_argv, NULL);
    (void) posix_spawn_file_actions_destroy(&actions);
    check(rc == ENOENT && pid == (pid_t) -1, "spawn addopen failure before spawn",
          "a failing addopen must not publish a child");

    (void) unlink(side_effect_path);
    rc = posix_spawn_file_actions_init(&actions);
    if (rc != EOK) {
        test_fail("spawn addopen no-side-effect init", strerror(rc));
        return;
    }
    rc = posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO,
                                          side_effect_path,
                                          O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (rc != EOK) {
        test_fail("spawn addopen no-side-effect setup", strerror(rc));
        (void) posix_spawn_file_actions_destroy(&actions);
        return;
    }
    pid = (pid_t) -1;
    rc = posix_spawn(&pid, "/bin/missing-spawn-image", &actions, NULL,
                     missing_argv, NULL);
    (void) posix_spawn_file_actions_destroy(&actions);
    errno = 0;
    check(rc == ENOENT && pid == (pid_t) -1 &&
              access(side_effect_path, F_OK) == -1 && errno == ENOENT,
          "spawn missing image skips addopen side effects",
          "failed spawn must not create addopen targets");
    (void) unlink(side_effect_path);

    {
        char *full_fd_argv[] = {
            "syscall_testing", "--spawn-full-fd-child", NULL,
        };

        pid = (pid_t) -1;
        rc = posix_spawn(&pid, "/bin/syscall_testing", NULL, NULL, full_fd_argv,
                         NULL);
        check(rc == EOK && pid > 0, "spawn full-fd fixture child",
              "fd-exhaustion regression fixture must start");
        if (rc == EOK && pid > 0) {
            char detail[96];

            rc = waitpid(pid, &status, 0);
            snprintf(detail, sizeof(detail), "wait=%d status=0x%x", rc,
                     status);
            check(rc == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0,
                  "spawn with no spare parent fd", detail);
        }
    }

    rc = posix_spawn_file_actions_init(&actions);
    if (rc != EOK) {
        test_fail("spawn addopen ordered init", strerror(rc));
        return;
    }
    if (posix_spawn_file_actions_addopen(&actions, 3, "/dev/zero", O_RDONLY,
                                         0) != EOK ||
        posix_spawn_file_actions_adddup2(&actions, 3, 4) != EOK ||
        posix_spawn_file_actions_addclose(&actions, 3) != EOK) {
        test_fail("spawn addopen ordered setup", "add action failed");
        (void) posix_spawn_file_actions_destroy(&actions);
        return;
    }
    pid = (pid_t) -1;
    rc = posix_spawn(&pid, "/bin/syscall_testing", &actions, NULL,
                     ordered_argv, NULL);
    (void) posix_spawn_file_actions_destroy(&actions);
    check(rc == EOK && pid > 0, "spawn addopen action ordering",
          "open, dup2, and close actions must be applied in order");
    if (rc == EOK && pid > 0) {
        check(waitpid(pid, &status, 0) == pid && WIFEXITED(status) &&
                  WEXITSTATUS(status) == 0,
              "spawn addopen ordered child",
              "child must see the ordered file-action result");
    }

    (void) close(3);
    rc = posix_spawn_file_actions_init(&actions);
    if (rc != EOK) {
        test_fail("spawn addopen identity init", strerror(rc));
        return;
    }
    if (posix_spawn_file_actions_addopen(&actions, 3, "/dev/zero", O_RDONLY,
                                         0) != EOK) {
        test_fail("spawn addopen identity setup", "add action failed");
        (void) posix_spawn_file_actions_destroy(&actions);
        return;
    }
    pid = (pid_t) -1;
    rc = posix_spawn(&pid, "/bin/syscall_testing", &actions, NULL,
                     fd3_readable_argv, NULL);
    (void) posix_spawn_file_actions_destroy(&actions);
    check(rc == EOK && pid > 0, "spawn addopen identity temp fd",
          "an open that returns the target fd must still be inherited");
    if (rc == EOK && pid > 0) {
        check(waitpid(pid, &status, 0) == pid && WIFEXITED(status) &&
                  WEXITSTATUS(status) == 0,
              "spawn addopen identity child",
              "child must inherit an addopen descriptor even when temp fd matches");
    }

    (void) close(3);
    rc = posix_spawn_file_actions_init(&actions);
    if (rc != EOK) {
        test_fail("spawn addopen cloexec init", strerror(rc));
        return;
    }
    if (posix_spawn_file_actions_addopen(&actions, 3, "/dev/zero",
                                         O_RDONLY | O_CLOEXEC, 0) != EOK) {
        test_fail("spawn addopen cloexec setup", "add action failed");
        (void) posix_spawn_file_actions_destroy(&actions);
        return;
    }
    pid = (pid_t) -1;
    rc = posix_spawn(&pid, "/bin/syscall_testing", &actions, NULL,
                     fd3_closed_argv, NULL);
    (void) posix_spawn_file_actions_destroy(&actions);
    check(rc == EOK && pid > 0, "spawn addopen O_CLOEXEC",
          "O_CLOEXEC addopen descriptors must not survive exec");
    if (rc == EOK && pid > 0) {
        check(waitpid(pid, &status, 0) == pid && WIFEXITED(status) &&
                  WEXITSTATUS(status) == 0,
              "spawn addopen O_CLOEXEC child",
              "child must see the addopen O_CLOEXEC descriptor closed");
    }

    (void) close(3);
    (void) close(4);
    rc = posix_spawn_file_actions_init(&actions);
    if (rc != EOK) {
        test_fail("spawn addopen cloexec dup init", strerror(rc));
        return;
    }
    if (posix_spawn_file_actions_addopen(&actions, 3, "/dev/zero",
                                         O_RDONLY | O_CLOEXEC, 0) != EOK ||
        posix_spawn_file_actions_adddup2(&actions, 3, 4) != EOK) {
        test_fail("spawn addopen cloexec dup setup", "add action failed");
        (void) posix_spawn_file_actions_destroy(&actions);
        return;
    }
    pid = (pid_t) -1;
    rc = posix_spawn(&pid, "/bin/syscall_testing", &actions, NULL,
                     ordered_argv, NULL);
    (void) posix_spawn_file_actions_destroy(&actions);
    check(rc == EOK && pid > 0, "spawn addopen O_CLOEXEC ordered dup",
          "later dup2 must see an O_CLOEXEC addopen before exec closes it");
    if (rc == EOK && pid > 0) {
        check(waitpid(pid, &status, 0) == pid && WIFEXITED(status) &&
                  WEXITSTATUS(status) == 0,
              "spawn addopen O_CLOEXEC ordered child",
              "fd 4 must survive while fd 3 closes at exec");
    }

    (void) close(3);
    (void) close(4);
    rc = posix_spawn_file_actions_init(&actions);
    if (rc != EOK) {
        test_fail("spawn addopen preserves parent cloexec init", strerror(rc));
        return;
    }
    {
        int parent_fd = open("/dev/zero", O_RDONLY | O_CLOEXEC);

        if (parent_fd != 3) {
            test_fail("spawn addopen preserves parent cloexec fixture",
                      parent_fd == -1 ? strerror(errno) : "fd 3 unavailable");
            if (parent_fd != -1)
                (void) close(parent_fd);
            (void) posix_spawn_file_actions_destroy(&actions);
            return;
        }
        if (posix_spawn_file_actions_addopen(&actions, 4, "/dev/zero",
                                             O_RDONLY, 0) != EOK) {
            test_fail("spawn addopen preserves parent cloexec setup",
                      "add action failed");
            (void) close(parent_fd);
            (void) posix_spawn_file_actions_destroy(&actions);
            return;
        }
        pid = (pid_t) -1;
        rc = posix_spawn(&pid, "/bin/syscall_testing", &actions, NULL,
                         fd4_readable_fd3_closed_argv, NULL);
        (void) close(parent_fd);
    }
    (void) posix_spawn_file_actions_destroy(&actions);
    check(rc == EOK && pid > 0, "spawn addopen preserves parent FD_CLOEXEC",
          "explicit addopen fd maps must not leak unrelated cloexec parent fds");
    if (rc == EOK && pid > 0) {
        check(waitpid(pid, &status, 0) == pid && WIFEXITED(status) &&
                  WEXITSTATUS(status) == 0,
              "spawn addopen preserves parent FD_CLOEXEC child",
              "fd 4 must be readable while unrelated parent fd 3 is closed");
    }

    (void) close(3);
    (void) close(4);
    (void) close(5);
    rc = posix_spawn_file_actions_init(&actions);
    if (rc != EOK) {
        test_fail("spawn addopen parent cloexec usable init", strerror(rc));
        return;
    }
    {
        int parent_fd = open("/dev/zero", O_RDONLY | O_CLOEXEC);

        if (parent_fd != 3) {
            test_fail("spawn addopen parent cloexec usable fixture",
                      parent_fd == -1 ? strerror(errno) : "fd 3 unavailable");
            if (parent_fd != -1)
                (void) close(parent_fd);
            (void) posix_spawn_file_actions_destroy(&actions);
            return;
        }
        if (posix_spawn_file_actions_addopen(&actions, 4, "/dev/null",
                                             O_RDONLY, 0) != EOK ||
            posix_spawn_file_actions_adddup2(&actions, 3, 5) != EOK) {
            test_fail("spawn addopen parent cloexec usable setup",
                      "add action failed");
            (void) close(parent_fd);
            (void) posix_spawn_file_actions_destroy(&actions);
            return;
        }
        pid = (pid_t) -1;
        rc = posix_spawn(&pid, "/bin/syscall_testing", &actions, NULL,
                         fd5_readable_fd3_closed_argv, NULL);
        (void) close(parent_fd);
    }
    (void) posix_spawn_file_actions_destroy(&actions);
    check(rc == EOK && pid > 0, "spawn addopen parent FD_CLOEXEC usable",
          "parent cloexec fds must remain usable during file actions");
    if (rc == EOK && pid > 0) {
        check(waitpid(pid, &status, 0) == pid && WIFEXITED(status) &&
                  WEXITSTATUS(status) == 0,
              "spawn addopen parent FD_CLOEXEC usable child",
              "dup2 from parent cloexec fd must survive at destination");
    }

    (void) close(3);
    (void) close(4);
    (void) close(5);
    rc = posix_spawn_file_actions_init(&actions);
    if (rc != EOK) {
        test_fail("spawn addopen temp fd isolation init", strerror(rc));
        return;
    }
    {
        int pad3_fd = open("/dev/zero", O_RDONLY);
        int pad4_fd = open("/dev/zero", O_RDONLY);
        int parent_fd = open("/dev/null", O_RDONLY);

        if (pad3_fd != 3 || pad4_fd != 4 || parent_fd != 5) {
            test_fail("spawn addopen temp fd isolation fixture",
                      parent_fd == -1 ? strerror(errno) : "fd layout unavailable");
            if (pad3_fd != -1)
                (void) close(pad3_fd);
            if (pad4_fd != -1)
                (void) close(pad4_fd);
            if (parent_fd != -1)
                (void) close(parent_fd);
            (void) posix_spawn_file_actions_destroy(&actions);
            return;
        }
        (void) close(pad3_fd);
        if (posix_spawn_file_actions_addopen(&actions, 4, "/dev/zero",
                                             O_RDONLY, 0) != EOK ||
            posix_spawn_file_actions_adddup2(&actions, 5, 3) != EOK) {
            test_fail("spawn addopen temp fd isolation setup",
                      "add action failed");
            (void) close(pad4_fd);
            (void) close(parent_fd);
            (void) posix_spawn_file_actions_destroy(&actions);
            return;
        }
        pid = (pid_t) -1;
        rc = posix_spawn(&pid, "/bin/syscall_testing", &actions, NULL,
                         fd4_readable_argv, NULL);
        (void) close(pad4_fd);
        (void) close(parent_fd);
    }
    (void) posix_spawn_file_actions_destroy(&actions);
    check(rc == EOK && pid > 0, "spawn addopen temp fd isolation",
          "materialized addopen sources must not be remapped as child fds");
    if (rc == EOK && pid > 0) {
        check(waitpid(pid, &status, 0) == pid && WIFEXITED(status) &&
                  WEXITSTATUS(status) == 0,
              "spawn addopen temp fd isolation child",
              "fd 4 must still read from the addopen file");
    }

    (void) close(3);
    (void) close(4);
    rc = posix_spawn_file_actions_init(&actions);
    if (rc != EOK) {
        test_fail("spawn addopen closed gap init", strerror(rc));
        return;
    }
    if (posix_spawn_file_actions_addopen(&actions, 4, "/dev/zero", O_RDONLY,
                                         0) != EOK) {
        test_fail("spawn addopen closed gap setup", "add action failed");
        (void) posix_spawn_file_actions_destroy(&actions);
        return;
    }
    pid = (pid_t) -1;
    rc = posix_spawn(&pid, "/bin/syscall_testing", &actions, NULL,
                     fd4_readable_fd3_closed_argv, NULL);
    (void) posix_spawn_file_actions_destroy(&actions);
    check(rc == EOK && pid > 0, "spawn addopen preserves closed fd gap",
          "closed parent fds below addopen target must stay closed");
    if (rc == EOK && pid > 0) {
        check(waitpid(pid, &status, 0) == pid && WIFEXITED(status) &&
                  WEXITSTATUS(status) == 0,
              "spawn addopen closed fd gap child",
              "fd 4 must be readable while closed parent fd 3 stays closed");
    }

    {
        int fds[41];
        int high_fd;
        int nfds = 0;

        while (nfds < (int) (sizeof(fds) / sizeof(fds[0]))) {
            int fd = open("/dev/zero", O_RDONLY);

            if (fd == -1) {
                test_fail("spawn addopen high fd inherit fixture",
                          strerror(errno));
                while (nfds > 0)
                    (void) close(fds[--nfds]);
                return;
            }
            fds[nfds++] = fd;
            if (fd == 40)
                break;
        }
        if (fds[nfds - 1] != 40) {
            test_fail("spawn addopen high fd inherit fixture",
                      "fd 40 unavailable");
            while (nfds > 0)
                (void) close(fds[--nfds]);
            return;
        }
        for (int i = 0; i < nfds - 1; i++)
            (void) close(fds[i]);

        rc = posix_spawn_file_actions_init(&actions);
        if (rc != EOK) {
            test_fail("spawn addopen high fd inherit init", strerror(rc));
            (void) close(fds[nfds - 1]);
            return;
        }
        if (posix_spawn_file_actions_addopen(&actions, 3, "/dev/null",
                                             O_RDONLY, 0) != EOK) {
            test_fail("spawn addopen high fd inherit setup",
                      "add action failed");
            (void) close(fds[nfds - 1]);
            (void) posix_spawn_file_actions_destroy(&actions);
            return;
        }
        pid = (pid_t) -1;
        high_fd = fds[nfds - 1];
        rc = posix_spawn(&pid, "/bin/syscall_testing", &actions, NULL,
                         fd40_readable_argv, NULL);
        (void) posix_spawn_file_actions_destroy(&actions);
        if (rc != EOK || pid <= 0)
            (void) close(high_fd);
    }
    check(rc == EOK && pid > 0, "spawn addopen preserves high inherited fd",
          "materialized fd maps must include unrelated high parent fds");
    if (rc == EOK && pid > 0) {
        check(waitpid(pid, &status, 0) == pid && WIFEXITED(status) &&
                  WEXITSTATUS(status) == 0,
              "spawn addopen high inherited fd child",
              "unrelated high parent fds must remain inherited");
        (void) close(40);
    }
}

static void test_spawn_file_actions_close_dup2(void)
{
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_t bad_actions = 1;
    char *argv[] = {"true", NULL};
    pid_t pid = (pid_t) -1;
    int rc;

    rc = posix_spawn_file_actions_init(&actions);
    if (rc != EOK) {
        test_fail("spawn file actions init", strerror(rc));
        return;
    }

    check(posix_spawn_file_actions_addclose(&actions, STDIN_FILENO) == EOK,
          "spawn file actions addclose", "expected EOK");
    check(posix_spawn_file_actions_adddup2(&actions, STDOUT_FILENO,
                                           STDERR_FILENO) == EOK,
          "spawn file actions adddup2", "expected EOK");
    check(posix_spawn_file_actions_addclose(&actions, -1) == EINVAL,
          "spawn file actions addclose bad fd", "expected EINVAL");
    check(
        posix_spawn_file_actions_adddup2(&actions, -1, STDERR_FILENO) == EINVAL,
        "spawn file actions adddup2 bad fd", "expected EINVAL");
    check(posix_spawn_file_actions_addclose(&actions, UINT16_MAX) == EINVAL,
          "spawn file actions addclose huge fd", "expected EINVAL");
    check(posix_spawn_file_actions_adddup2(&actions, UINT16_MAX,
                                           STDERR_FILENO) == EINVAL,
          "spawn file actions adddup2 huge source", "expected EINVAL");
    check(posix_spawn_file_actions_adddup2(&actions, STDOUT_FILENO,
                                           UINT16_MAX) == EINVAL,
          "spawn file actions adddup2 huge target", "expected EINVAL");
    check(posix_spawn_file_actions_addopen(&actions, UINT16_MAX, "/dev/null",
                                           O_RDONLY, 0) == EINVAL,
          "spawn file actions addopen huge target", "expected EINVAL");
    check(posix_spawn(&pid, "/bin/true", &bad_actions, NULL, argv, NULL) ==
              EINVAL &&
              pid == (pid_t) -1,
          "spawn file actions bad object",
          "an invalid opaque file-actions token must fail before spawn");
    check(posix_spawn_file_actions_destroy(&actions) == EOK,
          "spawn file actions destroy", "expected EOK");
}

#define ROOT_LOG_MARK_TEST_MAX 128 /* _SYS_CMD_LOG_MARK text limit */

/* msg_sendnc_r returns the reply status or -errno directly and never sets
 * errno, so a -1 is EPERM, not the libc failure marker; reading errno there
 * returned a stale value.
 */
static int roottask_errno_status(int status)
{
    return status < 0 ? -status : status;
}

/* _SYS_CMD_LOG_MARK: ask roottask to log "text" (len bytes, no NUL sent).
 * Returns 0 or the errno roottask replied with.
 */
static int send_log_mark(const char *text, size_t len)
{
    struct {
        struct _sys_cmd_log_mark hdr;
        char text[ROOT_LOG_MARK_TEST_MAX + 16];
    } msg;

    memset(&msg, 0, sizeof msg);
    msg.hdr.type = _SYS_CMD;
    msg.hdr.cmd = _SYS_CMD_LOG_MARK;
    msg.hdr.len = (uint16_t) len;
    memcpy(msg.text, text, len);
    return roottask_errno_status(
        msg_sendnc_r(ROOTTASK_COID, &msg, sizeof msg.hdr + len, NULL, 0));
}

/* Read /sys/log from fd to EOF into buf (cap bytes); every read must be at most
 * 4096 bytes.
 *
 * Returns the byte count, or -errno from the failing read.
 */
static ssize_t read_sys_log(int fd, char *buf, size_t cap)
{
    size_t total = 0;

    for (;;) {
        ssize_t n = read(fd, buf + total, cap - total);

        if (n < 0)
            return -errno;
        if (n == 0)
            return (ssize_t) total;
        if (n > 4096)
            return -EMSGSIZE;
        total += (size_t) n;
        if (total == cap)
            return (ssize_t) total;
    }
}

/* Child: as uid 1000, /sys/log (0400) refuses open and stat with EACCES (the
 * gate is the open path, which stat shares) while a world-readable /sys entry
 * still opens, and a log mark is refused with roottask's own EPERM.
 */
int sys_log_child(void)
{
    struct stat st;
    int fd;

    if (setgid(1000) != 0 || setuid(1000) != 0)
        return 2;
    errno = 0;
    fd = open("/sys/log", O_RDONLY);
    if (fd >= 0) {
        (void) close(fd);
        return 3;
    }
    if (errno != EACCES)
        return 4;
    fd = open("/sys/kstats", O_RDONLY);
    if (fd < 0)
        return 5;
    (void) close(fd);
    errno = 0;
    if (stat("/sys/log", &st) == 0 || errno != EACCES)
        return 7;
    fd = send_log_mark("uid1000", 7);
    if (fd != EPERM)
        return 6;
    return 0;
}

/* /sys/log: a root mark appears last, a rewind resamples, reads stay within
 * 4096 bytes, and uid 1000 is refused.
 */
static void test_sys_log_read(void)
{
    static char buf[1024 * 1024 + 4096];
    char mark[64], line[96], long_mark[ROOT_LOG_MARK_TEST_MAX + 1];
    char *argv[] = {"syscall_testing", "--sys-log-child", NULL};
    const char *last, *lit;
    struct stat st;
    ssize_t n;
    pid_t pid;
    int fd, status = -1;

    if (geteuid() != 0)
        return;
    snprintf(mark, sizeof mark, "ctl %d\nRT   forged", (int) getpid());
    check(send_log_mark(mark, strlen(mark)) == 0, "sys_log_read mark with newline",
          "root _SYS_CMD_LOG_MARK with embedded newline must succeed");
    memset(long_mark, 'x', sizeof long_mark);
    check(send_log_mark(long_mark, sizeof long_mark) == EINVAL, "sys_log_read mark over 128 bytes",
          "a 129-byte mark must fail with EINVAL");
    check(send_log_mark("%s%s%n", 6) == 0, "sys_log_read literal format",
          "mark text must be logged verbatim, never as a format");
    snprintf(mark, sizeof mark, "sys_log_read marker %d", (int) getpid());
    check(send_log_mark(mark, strlen(mark)) == 0, "sys_log_read mark accepted",
          "root _SYS_CMD_LOG_MARK must succeed");

    fd = open("/sys/log", O_RDONLY);
    if (fd < 0) {
        test_fail("sys_log_read open", strerror(errno));
        return;
    }
    check(fstat(fd, &st) == 0 && (st.st_mode & 07777) == 0400 && S_ISREG(st.st_mode),
          "sys_log_read mode 0400", "/sys/log must be a regular file with mode 0400");
    n = read_sys_log(fd, buf, sizeof buf - 1);
    check(n > 0, "sys_log_read reads data", "empty or failed read");
    if (n > 0) {
        buf[n] = '\0';

        /* Not wrapped (the ring is 1 MiB), so the oldest byte is the first
         * roottask line.
         */
        if (n < 1024 * 1024)
            check(!strncmp(buf, "RT", 2), "sys_log_read oldest first",
                  "an unwrapped ring must start at its first line");

        /* Roottask may log between the mark and this read, so the mark need not
         * be the last line; it must be one whole line that follows the
         * literal-format one.
         */
        snprintf(line, sizeof line, "\nRT   %s\n", mark);
        last = strstr(buf, line);
        lit = strstr(buf, "\nRT   %s%s%n\n");
        check(last && lit && lit < last, "sys_log_read marker ordered",
              "the mark must be a whole line after the literal-format one");
        snprintf(line, sizeof line, "\nRT   ctl %d?RT   forged\n", (int) getpid());
        check(strstr(buf, line) != NULL, "sys_log_read mark control bytes",
              "a newline in a mark must be logged as '?', not start a new line");
    }

    /* Rewind to a fresh window: a later mark shows up on the same descriptor.
     */
    snprintf(mark, sizeof mark, "sys_log_read second %d", (int) getpid());
    (void) send_log_mark(mark, strlen(mark));
    check(lseek(fd, 0, SEEK_SET) == 0, "sys_log_read rewind", "lseek(0) failed");
    n = read_sys_log(fd, buf, sizeof buf - 1);
    if (n > 0)
        buf[n] = '\0';
    check(n > 0 && strstr(buf, mark) != NULL, "sys_log_read rewind resamples",
          "the mark sent after open must appear after lseek(0)");
    (void) close(fd);

    if (posix_spawn(&pid, "/bin/syscall_testing", NULL, NULL, argv, NULL) != 0) {
        test_fail("sys_log_read spawn", strerror(errno));
        return;
    }
    check(wait_child_exit(pid, &status, 500) && WIFEXITED(status) && WEXITSTATUS(status) == 0,
          "sys_log_read uid 1000 is EACCES",
          "uid 1000 must get EACCES on /sys/log and a refused log mark");
}

/* Run only under qemu-regress.sh --mode syslog-wrap
 * (ROOTTASK_SYSLOG_SIZE=4096): 200 numbered markers overrun the ring, the
 * survivors read back contiguously and newest last, and a window that was
 * overwritten after open reads EOVERFLOW.
 */
#define WRAP_RING 4096
#define WRAP_MARKS 200

static void wrap_emit(int first, int count)
{
    char line[64], why[64];
    int rc;

    for (int i = first; i < first + count; i++) {
        /* Padded so 200 lines (46 bytes each) overrun a 4096-byte ring. */
        snprintf(line, sizeof line, "wrapmark %05d ....................", i);
        rc = send_log_mark(line, strlen(line));
        if (rc != 0) {
            snprintf(why, sizeof why, "mark %d: errno %d len %d", i, rc, (int) strlen(line));
            test_fail("sys_log_wrap emit", why);
            return;
        }
    }
}

/* Parse the numbered lines in buf; sets *first and *last, returns the count of
 * lines whose number did not follow the previous one by exactly 1.
 */
static int wrap_scan(char *buf, size_t n, int *first, int *last, int *count)
{
    int gaps = 0, prev = -1;
    char *p = memchr(buf, '\n', n); /* the first line may be cut by the wrap */

    *first = *last = -1;
    *count = 0;
    while (p && p + 1 < buf + n) {
        int v;

        p++;
        if (!strncmp(p, "RT   wrapmark ", 14)) {
            v = (int) strtol(p + 14, NULL, 10);
            if (prev >= 0 && v != prev + 1)
                gaps++;
            if (*first < 0)
                *first = v;
            prev = *last = v;
            (*count)++;
        }
        p = memchr(p, '\n', (size_t) (buf + n - p));
    }
    return gaps;
}

void test_sys_log_wrap(void)
{
    static char buf[WRAP_RING + 64];
    int first, last, count, gaps, fd, fd2;
    struct stat st;
    ssize_t n;

    wrap_emit(0, WRAP_MARKS);
    fd = open("/sys/log", O_RDONLY);
    if (fd < 0) {
        test_fail("sys_log_wrap open", strerror(errno));
        return;
    }
    check(fstat(fd, &st) == 0 && st.st_size == WRAP_RING, "sys_log_wrap ring is full",
          "needs CONFIG_ROOTTASK_SYSLOG_SIZE=4096 (qemu-regress.sh --mode syslog-wrap)");
    n = read_sys_log(fd, buf, sizeof buf - 1);
    check(n == WRAP_RING, "sys_log_wrap reads the whole ring", "short read of a full ring");
    if (n > 0) {
        buf[n] = '\0';
        gaps = wrap_scan(buf, (size_t) n, &first, &last, &count);
        char why[128];

        snprintf(why, sizeof why, "gaps=%d count=%d first=%d last=%d (want marker %d last)", gaps,
                 count, first, last, WRAP_MARKS - 1);
        check(gaps == 0 && count >= 50 && first > 0 && last == WRAP_MARKS - 1,
              "sys_log_wrap contiguous numbering after wrap", why);
    }

    /* A second window opened now is overwritten by 200 more markers. */
    fd2 = open("/sys/log", O_RDONLY);
    if (fd2 < 0) {
        test_fail("sys_log_wrap open 2", strerror(errno));
        (void) close(fd);
        return;
    }
    wrap_emit(WRAP_MARKS, WRAP_MARKS);
    errno = 0;
    check(read(fd2, buf, 512) == -1 && errno == EOVERFLOW, "sys_log_wrap overwritten window",
          "reading a window the ring has overwritten must fail with EOVERFLOW");
    check(lseek(fd2, 0, SEEK_SET) == 0, "sys_log_wrap rewind after overflow", "lseek failed");
    n = read_sys_log(fd2, buf, sizeof buf - 1);
    if (n > 0) {
        buf[n] = '\0';
        gaps = wrap_scan(buf, (size_t) n, &first, &last, &count);
    }
    check(n == WRAP_RING && gaps == 0 && last == 2 * WRAP_MARKS - 1,
          "sys_log_wrap rewind recovers", "a fresh window must read cleanly");
    (void) close(fd2);
    (void) close(fd);
}

static void test_spawn_malformed_messages_fail_closed(void)
{
    proc_spawn_t spawn_msg;
    proc_posixspawn_t pspawn_msg;
    int rc;

    memset(&spawn_msg, 0, sizeof(spawn_msg));
    spawn_msg.i.type = _PROC_SPAWN;
    spawn_msg.i.subtype = _PROC_SPAWN_START;
    spawn_msg.i.pathlen = 1;
    rc = roottask_errno_status(msg_sendnc_r(ROOTTASK_COID, &spawn_msg.i,
                                            sizeof(spawn_msg.i), NULL, 0));
    check(rc == EMSGSIZE, "spawn rejects short native message",
          "_PROC_SPAWN must reject declared bytes missing from msglen");

    memset(&spawn_msg, 0, sizeof(spawn_msg));
    spawn_msg.i.type = _PROC_SPAWN;
    spawn_msg.i.subtype = _PROC_SPAWN_START;
    spawn_msg.i.nbytes = (size_t) INT_MAX + 1U;
    rc = roottask_errno_status(msg_sendnc_r(ROOTTASK_COID, &spawn_msg.i,
                                            sizeof(spawn_msg.i), NULL, 0));
    check(rc == EMSGSIZE || rc == E2BIG,
          "spawn rejects oversized native message",
          "_PROC_SPAWN must reject size computations before allocation");

    memset(&pspawn_msg, 0, sizeof(pspawn_msg));
    pspawn_msg.i.type = _PROC_POSIX_SPAWN;
    pspawn_msg.i.subtype = _PROC_SPAWN_START;
    pspawn_msg.i.attr_bytes = UINT32_MAX;
    rc = roottask_errno_status(msg_sendnc_r(ROOTTASK_COID, &pspawn_msg.i,
                                            sizeof(pspawn_msg.i), NULL, 0));
    check(rc == EMSGSIZE, "spawn rejects short POSIX message",
          "_PROC_POSIX_SPAWN must reject declared regions outside msglen");

    /* The sender picks attr_bytes, so the file actions can start at an odd
     * address; the receiver must refuse that rather than build a key mask.
     */
    {
        static union {
            proc_posixspawn_t m;
            char raw[sizeof(proc_posixspawn_t) + 4096 + 1 + 8];
        } buf;
        proc_posixspawn_t *m = &buf.m;

        memset(&buf, 0, sizeof buf);
        m->i.type = _PROC_POSIX_SPAWN;
        m->i.subtype = _PROC_SPAWN_START;
        m->i.attr_bytes = 4096 + 1;
        m->i.filact_bytes = 8;
        rc = roottask_errno_status(
            msg_sendnc_r(ROOTTASK_COID, &buf, sizeof buf, NULL, 0));
        check(rc == EINVAL, "spawn rejects misaligned file actions",
              "an odd attr_bytes must fail EINVAL, not reach key_mask_create");
    }
}

/* End-to-end: a valid close/dup2 file-actions object must actually spawn. */
static void test_spawn_file_actions_close_dup2_spawn(void)
{
    posix_spawn_file_actions_t actions;
    char *argv[] = {"true", NULL};
    SPAWN_ARGV_CLOSED(fd3_closed, 3);
    SPAWN_ARGV_READABLE_CLOSED(fd5_readable_fd3_closed, 5, 3);
    SPAWN_ARGV_READABLE(fd40_readable, 40);
    pid_t pid = (pid_t) -1;
    int fd;
    int rc, status;

    rc = posix_spawn_file_actions_init(&actions);
    if (rc != EOK) {
        test_fail("spawn close/dup2 run init", strerror(rc));
        return;
    }

    /* Harmless actions for /bin/true: redirect stderr to stdout, close stdin.
     */
    if (posix_spawn_file_actions_adddup2(&actions, STDOUT_FILENO,
                                         STDERR_FILENO) != EOK ||
        posix_spawn_file_actions_addclose(&actions, STDIN_FILENO) != EOK) {
        test_fail("spawn close/dup2 run setup", "add action failed");
        (void) posix_spawn_file_actions_destroy(&actions);
        return;
    }
    rc = posix_spawn(&pid, "/bin/true", &actions, NULL, argv, NULL);
    (void) posix_spawn_file_actions_destroy(&actions);
    check(rc == EOK && pid > 0, "spawn with close/dup2 file actions",
          "a valid file-actions spawn must succeed");
    if (rc == EOK && pid > 0)
        (void) waitpid(pid, &status, 0);

    (void) close(3);
    rc = posix_spawn_file_actions_init(&actions);
    if (rc != EOK) {
        test_fail("spawn identity dup2 init", strerror(rc));
        return;
    }
    fd = open("/dev/zero", O_RDONLY | O_CLOEXEC);
    if (fd != 3) {
        test_fail("spawn identity dup2 fixture",
                  fd == -1 ? strerror(errno) : "fd 3 unavailable");
        if (fd != -1)
            (void) close(fd);
        (void) posix_spawn_file_actions_destroy(&actions);
        return;
    }
    if (posix_spawn_file_actions_adddup2(&actions, fd, fd) != EOK) {
        test_fail("spawn identity dup2 setup", "add action failed");
        (void) close(fd);
        (void) posix_spawn_file_actions_destroy(&actions);
        return;
    }
    pid = (pid_t) -1;
    rc = posix_spawn(&pid, "/bin/syscall_testing", &actions, NULL,
                     fd3_closed_argv, NULL);
    (void) close(fd);
    (void) posix_spawn_file_actions_destroy(&actions);
    check(rc == EOK && pid > 0, "spawn identity dup2 cloexec",
          "dup2(fd, fd) file action must not clear FD_CLOEXEC");
    if (rc == EOK && pid > 0) {
        check(waitpid(pid, &status, 0) == pid && WIFEXITED(status) &&
                  WEXITSTATUS(status) == 0,
              "spawn identity dup2 cloexec child",
              "child must still see the descriptor closed on exec");
    }

    (void) close(3);
    (void) close(4);
    (void) close(5);
    rc = posix_spawn_file_actions_init(&actions);
    if (rc != EOK) {
        test_fail("spawn chained dup2 init", strerror(rc));
        return;
    }
    fd = open("/dev/zero", O_RDONLY);
    if (fd != 3) {
        test_fail("spawn chained dup2 fixture",
                  fd == -1 ? strerror(errno) : "fd 3 unavailable");
        if (fd != -1)
            (void) close(fd);
        (void) posix_spawn_file_actions_destroy(&actions);
        return;
    }
    if (posix_spawn_file_actions_adddup2(&actions, 3, 4) != EOK ||
        posix_spawn_file_actions_addclose(&actions, 3) != EOK ||
        posix_spawn_file_actions_adddup2(&actions, 4, 5) != EOK) {
        test_fail("spawn chained dup2 setup", "add action failed");
        (void) close(fd);
        (void) posix_spawn_file_actions_destroy(&actions);
        return;
    }
    pid = (pid_t) -1;
    rc = posix_spawn(&pid, "/bin/syscall_testing", &actions, NULL,
                     fd5_readable_fd3_closed_argv, NULL);
    (void) close(fd);
    (void) posix_spawn_file_actions_destroy(&actions);
    check(rc == EOK && pid > 0, "spawn chained dup2 ordering",
          "later dup2 actions must see earlier dup2 results");
    if (rc == EOK && pid > 0) {
        check(waitpid(pid, &status, 0) == pid && WIFEXITED(status) &&
                  WEXITSTATUS(status) == 0,
              "spawn chained dup2 child",
              "fd 5 must inherit the source duplicated through fd 4");
    }

    {
        int fds[41];
        int high_fd;
        int nfds = 0;

        while (nfds < (int) (sizeof(fds) / sizeof(fds[0]))) {
            int next_fd = open("/dev/zero", O_RDONLY);

            if (next_fd == -1) {
                test_fail("spawn close-only high fd inherit fixture",
                          strerror(errno));
                while (nfds > 0)
                    (void) close(fds[--nfds]);
                return;
            }
            fds[nfds++] = next_fd;
            if (next_fd == 40)
                break;
        }
        if (fds[nfds - 1] != 40) {
            test_fail("spawn close-only high fd inherit fixture",
                      "fd 40 unavailable");
            while (nfds > 0)
                (void) close(fds[--nfds]);
            return;
        }
        for (int i = 0; i < nfds - 1; i++)
            (void) close(fds[i]);

        rc = posix_spawn_file_actions_init(&actions);
        if (rc != EOK) {
            test_fail("spawn close-only high fd inherit init", strerror(rc));
            (void) close(fds[nfds - 1]);
            return;
        }
        if (posix_spawn_file_actions_addclose(&actions, STDIN_FILENO) != EOK) {
            test_fail("spawn close-only high fd inherit setup",
                      "add action failed");
            (void) close(fds[nfds - 1]);
            (void) posix_spawn_file_actions_destroy(&actions);
            return;
        }
        pid = (pid_t) -1;
        high_fd = fds[nfds - 1];
        rc = posix_spawn(&pid, "/bin/syscall_testing", &actions, NULL,
                         fd40_readable_argv, NULL);
        (void) posix_spawn_file_actions_destroy(&actions);
        if (rc != EOK || pid <= 0)
            (void) close(high_fd);
    }
    check(rc == EOK && pid > 0, "spawn close-only preserves high inherited fd",
          "close-only fd maps must include unrelated high parent fds");
    if (rc == EOK && pid > 0) {
        check(waitpid(pid, &status, 0) == pid && WIFEXITED(status) &&
                  WEXITSTATUS(status) == 0,
              "spawn close-only high inherited fd child",
              "unrelated high parent fds must remain inherited");
        (void) close(40);
    }
}

static void test_spawnp_absolute_path(void)
{
    char *argv[] = {"true", NULL};
    pid_t pid = (pid_t) -1;
    int status;
    int rc;

    rc = posix_spawnp(&pid, "/bin/true", NULL, NULL, argv, NULL);
    if (rc != EOK) {
        test_fail("posix_spawnp absolute path", strerror(rc));
        return;
    }
    check(wait_child_exit(pid, &status, 500) && WIFEXITED(status) &&
              WEXITSTATUS(status) == 0,
          "posix_spawnp absolute path", "expected clean child exit");

    pid = (pid_t) -1;
    check(posix_spawnp(&pid, "", NULL, NULL, argv, NULL) == EINVAL &&
              pid == (pid_t) -1,
          "posix_spawnp empty file", "expected EINVAL without child");
}

static void test_spawnattr_flags(void)
{
    posix_spawnattr_t attr;
    sigset_t sigset;
    struct sched_param param = {.sched_priority = 7};
    struct sched_param got_param;
    short flags = 0;
    pid_t pgroup = (pid_t) -1;
    int policy = -1;
    int rc;

    rc = posix_spawnattr_init(&attr);
    if (rc != EOK) {
        test_fail("spawnattr flags init", strerror(rc));
        return;
    }

    rc = posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETPGROUP);
    check(rc == EOK && posix_spawnattr_getflags(&attr, &flags) == EOK &&
              flags == POSIX_SPAWN_SETPGROUP,
          "spawnattr flags round trip",
          "supported posix_spawn flags must be stored");

    rc = posix_spawnattr_setpgroup(&attr, 0);
    check(rc == EOK && posix_spawnattr_getpgroup(&attr, &pgroup) == EOK &&
              pgroup == 0,
          "spawnattr pgroup round trip",
          "posix_spawn process-group attribute must be stored");

    (void) sigemptyset(&sigset);
    rc = posix_spawnattr_setsigmask(&attr, &sigset);
    check(rc == EOK && posix_spawnattr_getsigmask(&attr, &sigset) == EOK,
          "spawnattr sigmask round trip",
          "posix_spawn signal-mask attribute must link and store");

    (void) sigaddset(&sigset, SIGTERM);
    rc = posix_spawnattr_setsigdefault(&attr, &sigset);
    check(rc == EOK && posix_spawnattr_getsigdefault(&attr, &sigset) == EOK &&
              sigismember(&sigset, SIGTERM),
          "spawnattr sigdefault round trip",
          "posix_spawn signal-default attribute must link and store");

    rc = posix_spawnattr_setschedpolicy(&attr, SCHED_FIFO);
    check(rc == EOK && posix_spawnattr_getschedpolicy(&attr, &policy) == EOK &&
              policy == SCHED_FIFO,
          "spawnattr schedpolicy round trip",
          "posix_spawn scheduler policy attribute must be stored");

    rc = posix_spawnattr_setschedparam(&attr, &param);
    check(rc == EOK &&
              posix_spawnattr_getschedparam(&attr, &got_param) == EOK &&
              got_param.sched_priority == param.sched_priority,
          "spawnattr schedparam round trip",
          "posix_spawn scheduler parameter attribute must be stored");

    rc = posix_spawnattr_setflags(&attr, POSIX_SPAWN_RESETIDS);
    check(rc == EOK && posix_spawnattr_getflags(&attr, &flags) == EOK &&
              flags == POSIX_SPAWN_RESETIDS,
          "spawnattr resetids round trip",
          "POSIX_SPAWN_RESETIDS must be accepted and stored");

    (void) posix_spawnattr_destroy(&attr);
}

static void test_spawnattr_resetids_runtime(void)
{
    posix_spawnattr_t attr;
    char *argv[] = {"syscall_testing", "--expect-resetids", NULL};
    uid_t uid = getuid();
    gid_t gid = getgid();
    uid_t euid = geteuid();
    gid_t egid = getegid();
    pid_t pid = (pid_t) -1;
    int status;
    int rc;

    if (uid != 0 || gid != 0) {
        test_ok("spawnattr resetids runtime skipped");
        return;
    }

    rc = posix_spawnattr_init(&attr);
    if (rc != EOK) {
        test_fail("spawnattr resetids runtime init", strerror(rc));
        return;
    }
    rc = posix_spawnattr_setflags(&attr, POSIX_SPAWN_RESETIDS);
    if (rc != EOK) {
        test_fail("spawnattr resetids runtime flags", strerror(rc));
        (void) posix_spawnattr_destroy(&attr);
        return;
    }

    if (setegid((gid_t) 1) == -1 || seteuid((uid_t) 1) == -1) {
        test_fail("spawnattr resetids runtime setup", strerror(errno));
        (void) seteuid(euid);
        (void) setegid(egid);
        (void) posix_spawnattr_destroy(&attr);
        return;
    }

    rc = posix_spawn(&pid, "/bin/syscall_testing", NULL, &attr, argv, NULL);
    (void) seteuid(euid);
    (void) setegid(egid);
    (void) posix_spawnattr_destroy(&attr);

    check(rc == EOK && waitpid(pid, &status, 0) == pid && WIFEXITED(status) &&
              WEXITSTATUS(status) == 0,
          "spawnattr resetids runtime",
          "POSIX_SPAWN_RESETIDS must reset child effective ids to real ids");
}

/* Spawn path with the given actions, attributes and environment (NULL for the
 * caller's). Returns the posix_spawn() result; on success *code is the child's
 * exit status, or -1 if it did not exit normally.
 */
static int spawn_wait_code(const char *path,
                           const posix_spawn_file_actions_t *actions,
                           const posix_spawnattr_t *attr,
                           char *argv[],
                           char *envp[],
                           pid_t *pid_out,
                           int *code)
{
    pid_t pid = (pid_t) -1;
    int status;
    int rc;

    *code = -1;
    rc = posix_spawn(&pid, path, actions, attr, argv, envp);
    if (pid_out)
        *pid_out = pid;
    if (rc == EOK && wait_child_exit(pid, &status, 500) && WIFEXITED(status))
        *code = WEXITSTATUS(status);
    return rc;
}

static unsigned spawn_count_open_fds(void)
{
    long max = sysconf(_SC_OPEN_MAX);
    unsigned n = 0;

    for (long fd = 0; fd < max; fd++)
        if (fcntl((int) fd, F_GETFD) != -1)
            n++;
    return n;
}

static void test_spawnattr_setsid(void)
{
    posix_spawnattr_t attr;
    char *argv[] = {"syscall_testing", "--expect-setsid", NULL};
    short flags = 0;
    int code;
    int rc;

    rc = posix_spawnattr_init(&attr);
    if (rc != EOK) {
        test_fail("spawnattr setsid init", strerror(rc));
        return;
    }
    rc = posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSID);
    check(rc == EOK && posix_spawnattr_getflags(&attr, &flags) == EOK &&
              flags == POSIX_SPAWN_SETSID,
          "spawnattr setsid round trip",
          "POSIX_SPAWN_SETSID must be accepted and stored");
    if (rc == EOK) {
        rc = spawn_wait_code("/bin/syscall_testing", NULL, &attr, argv, NULL,
                             NULL, &code);
        check(rc == EOK && code == 0, "spawnattr setsid runtime",
              "a SETSID child must lead a new session with no controlling "
              "terminal");
    }
    (void) posix_spawnattr_destroy(&attr);
}

/* One chdir/fchdir file-action scenario: the setup adds the actions, then the
 * spawn must return want_rc and, on success, the child must see cwd `want`.
 */
struct spawn_chdir_case {
    const char *name;
    const char *path;   /* image, NULL for syscall_testing; a relative one
                         * resolves after the actions */
    const char *want;   /* expected child cwd */
    const char *fds[3]; /* descriptors the child must find open */
    int want_rc;
};

static void run_spawn_chdir_case(const struct spawn_chdir_case *c,
                                 posix_spawn_file_actions_t *actions)
{
    char *argv[7] = {"syscall_testing", "--expect-cwd", (char *) c->want};
    char detail[96];
    pid_t pid;
    int code;
    int rc;
    int nargs = 3;

    for (int i = 0; i < 3 && c->fds[i]; i++)
        argv[nargs++] = (char *) c->fds[i];
    argv[nargs] = NULL;
    rc = spawn_wait_code(c->path ? c->path : "/bin/syscall_testing", actions,
                         NULL, argv, NULL, &pid, &code);
    (void) posix_spawn_file_actions_destroy(actions);
    snprintf(detail, sizeof detail, "rc=%d want=%d code=%d", rc, c->want_rc,
             code);
    if (c->want_rc != EOK)
        check(rc == c->want_rc && pid == (pid_t) -1, c->name, detail);
    else
        check(rc == EOK && code == 0, c->name, detail);
}

static void test_spawn_file_actions_chdir(void)
{
    const char shm_file[] = "/dev/shmem/spawn-chdir-f2";
    const char created[] = "/dev/shmem/spawn-chdir-created";
    posix_spawn_file_actions_t fa;
    char saved[PATH_MAX + 1];
    char shm[PATH_MAX + 1];
    char dirfd_arg[16];
    unsigned before;
    int fd, dfd;

    if (!getcwd(saved, sizeof saved) || chdir("/dev/shmem") != 0 ||
        !getcwd(shm, sizeof shm) || chdir(saved) != 0) {
        test_fail("spawn chdir fixture", strerror(errno));
        return;
    }

    check(posix_spawn_file_actions_init(&fa) == EOK &&
              posix_spawn_file_actions_addfchdir(&fa, -1) == EBADF &&
              posix_spawn_file_actions_addchdir(&fa, NULL) == EINVAL,
          "spawn addfchdir/addchdir argument checks",
          "a negative fd is EBADF and a NULL path EINVAL");
    (void) posix_spawn_file_actions_destroy(&fa);

    /* addchdir to an absolute directory. */
    (void) posix_spawn_file_actions_init(&fa);
    (void) posix_spawn_file_actions_addchdir(&fa, "/dev/shmem");
    run_spawn_chdir_case(
        &(struct spawn_chdir_case){
            "spawn addchdir sets child cwd", NULL, shm, {NULL}, EOK},
        &fa);

    /* A relative chdir resolves against the previous one. */
    (void) posix_spawn_file_actions_init(&fa);
    (void) posix_spawn_file_actions_addchdir(&fa, "/dev");
    (void) posix_spawn_file_actions_addchdir(&fa, "shmem");
    run_spawn_chdir_case(
        &(struct spawn_chdir_case){
            "spawn relative addchdir chains", NULL, shm, {NULL}, EOK},
        &fa);

    /* A relative image path resolves in the directory the actions leave. */
    (void) posix_spawn_file_actions_init(&fa);
    (void) posix_spawn_file_actions_addchdir(&fa, "/bin");
    run_spawn_chdir_case(
        &(struct spawn_chdir_case){"spawn relative image after addchdir",
                                   "syscall_testing",
                                   "/bin",
                                   {NULL},
                                   EOK},
        &fa);

    /* After a chdir, an empty chdir or open path is still ENOENT. */
    (void) posix_spawn_file_actions_init(&fa);
    (void) posix_spawn_file_actions_addchdir(&fa, "/dev");
    (void) posix_spawn_file_actions_addchdir(&fa, "");
    run_spawn_chdir_case(
        &(struct spawn_chdir_case){
            "spawn empty addchdir after addchdir is ENOENT", NULL, shm, {NULL},
            ENOENT},
        &fa);
    (void) posix_spawn_file_actions_init(&fa);
    (void) posix_spawn_file_actions_addchdir(&fa, "/dev");
    (void) posix_spawn_file_actions_addopen(&fa, 3, "", O_RDONLY, 0);
    run_spawn_chdir_case(
        &(struct spawn_chdir_case){
            "spawn empty addopen after addchdir is ENOENT", NULL, shm, {NULL},
            ENOENT},
        &fa);

    /* Without chdir actions a relative image resolves in the caller's cwd. */
    if (chdir("/bin") == 0) {
        char *argv[] = {"syscall_testing", "--expect-cwd", "/bin", NULL};
        int code;
        int rc = spawn_wait_code("syscall_testing", NULL, NULL, argv, NULL,
                                 NULL, &code);

        (void) chdir(saved);
        check(rc == EOK && code == 0, "spawn relative image in caller cwd",
              "a relative path must resolve against the caller's cwd");
    } else {
        test_fail("spawn relative image fixture", strerror(errno));
    }

    /* A missing directory fails the spawn with ENOENT and no child. */
    (void) posix_spawn_file_actions_init(&fa);
    (void) posix_spawn_file_actions_addchdir(&fa, "/missing-spawn-chdir");
    run_spawn_chdir_case(
        &(struct spawn_chdir_case){
            "spawn addchdir missing dir is ENOENT", NULL, shm, {NULL}, ENOENT},
        &fa);

    /* open, chdir, open: f1 resolves in the caller's cwd, f2 in the new one.
     * Each file exists only where the right resolution looks for it.
     */
    fd = open(shm_file, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0 || chdir("/dev") != 0) {
        test_fail("spawn chdir ordered fixture", strerror(errno));
        if (fd >= 0)
            (void) close(fd);
        (void) chdir(saved);
        return;
    }
    (void) close(fd);
    (void) posix_spawn_file_actions_init(&fa);
    (void) posix_spawn_file_actions_addopen(&fa, 3, "zero", O_RDONLY, 0);
    (void) posix_spawn_file_actions_addchdir(&fa, shm);
    (void) posix_spawn_file_actions_addopen(&fa, 4, "spawn-chdir-f2", O_RDONLY,
                                            0);
    run_spawn_chdir_case(
        &(struct spawn_chdir_case){
            "spawn addopen/addchdir/addopen order", NULL, shm, {"3", "4"}, EOK},
        &fa);
    (void) chdir(saved);
    (void) unlink(shm_file);

    /* fchdir to a descriptor an earlier addopen produced. */
    (void) posix_spawn_file_actions_init(&fa);
    (void) posix_spawn_file_actions_addopen(&fa, 5, "/dev/shmem", O_RDONLY, 0);
    (void) posix_spawn_file_actions_addfchdir(&fa, 5);
    run_spawn_chdir_case(
        &(struct spawn_chdir_case){
            "spawn addopen then addfchdir", NULL, shm, {"5"}, EOK},
        &fa);

    /* fchdir to an addopen of a file is ENOTDIR. */
    (void) posix_spawn_file_actions_init(&fa);
    (void) posix_spawn_file_actions_addopen(&fa, 5, "/dev/zero", O_RDONLY, 0);
    (void) posix_spawn_file_actions_addfchdir(&fa, 5);
    run_spawn_chdir_case(
        &(struct spawn_chdir_case){"spawn addfchdir non-directory is ENOTDIR",
                                   NULL,
                                   shm,
                                   {NULL},
                                   ENOTDIR},
        &fa);

    dfd = open("/dev/shmem", O_RDONLY);
    if (dfd < 0) {
        test_fail("spawn fchdir dirfd fixture", strerror(errno));
        return;
    }
    snprintf(dirfd_arg, sizeof dirfd_arg, "%d", dfd);

    /* fchdir follows the descriptor an earlier dup2 placed. */
    (void) posix_spawn_file_actions_init(&fa);
    (void) posix_spawn_file_actions_adddup2(&fa, dfd, 6);
    (void) posix_spawn_file_actions_addfchdir(&fa, 6);
    run_spawn_chdir_case(
        &(struct spawn_chdir_case){
            "spawn adddup2 then addfchdir", NULL, shm, {"6"}, EOK},
        &fa);

    /* fchdir of a descriptor an earlier addclose closed is EBADF, even though
     * the caller still has it open.
     */
    (void) posix_spawn_file_actions_init(&fa);
    (void) posix_spawn_file_actions_addclose(&fa, dfd);
    (void) posix_spawn_file_actions_addfchdir(&fa, dfd);
    run_spawn_chdir_case(
        &(struct spawn_chdir_case){
            "spawn addclose then addfchdir is EBADF", NULL, shm, {NULL}, EBADF},
        &fa);
    (void) close(dfd);

    /* A failing chdir after an open must not leak the parent's temporary
     * descriptor.
     */
    (void) unlink(created);
    before = spawn_count_open_fds();
    (void) posix_spawn_file_actions_init(&fa);
    (void) posix_spawn_file_actions_addopen(&fa, 7, created, O_WRONLY | O_CREAT,
                                            0600);
    (void) posix_spawn_file_actions_addchdir(&fa, "/missing-spawn-chdir");
    run_spawn_chdir_case(
        &(struct spawn_chdir_case){
            "spawn addchdir failure after addopen", NULL, shm, {NULL}, ENOENT},
        &fa);
    check(spawn_count_open_fds() == before,
          "spawn addchdir failure closes temps",
          "a failed spawn must close the descriptors its actions opened");
    (void) unlink(created);
}

/* Large-argument spawns. roottask receives at most one proc message union
 * inline (a few KiB); the rest of a longer message stays with the sender. A
 * native _PROC_SPAWN that long is streamed: the loader fetches its arguments
 * with _PROC_SPAWN_ARGS at their offset in the sender's message. posix_spawn()
 * sends another layout, which roottask must read whole and rebuild. Every
 * pattern string is SPAWN_BIG_LEN bytes counting its NUL and derives from its
 * index, so the child rebuilds each one and compares it byte for byte.
 */
enum { SPAWN_BIG_LEN = 1024, SPAWN_BIG_HIGH_FD = 1000 };

static void spawn_big_fill(char *buf, unsigned idx, bool env)
{
    int n = env ? snprintf(buf, SPAWN_BIG_LEN, "BIGENV%u=", idx) : 0;

    for (int k = n; k < SPAWN_BIG_LEN - 1; k++)
        buf[k] = (char) (0x21 + (idx * 31u + (unsigned) k * 7u) % 94u);
    buf[SPAWN_BIG_LEN - 1] = '\0';
}

static bool spawn_big_matches(const char *s, unsigned idx, bool env)
{
    char want[SPAWN_BIG_LEN];

    if (!s)
        return false;
    spawn_big_fill(want, idx, env);
    return memcmp(s, want, SPAWN_BIG_LEN) == 0;
}

/* Child: argv is name, mode, nargs, nenv, fd, then nargs pattern strings; the
 * environment must be exactly nenv pattern strings, and fd (unless -1) open.
 */
int spawn_bigargs_child(int argc, char *argv[])
{
    unsigned nargs, nenv;
    int fd;
    struct stat st;

    if (argc < 5)
        return 2;
    nargs = (unsigned) atoi(argv[2]);
    nenv = (unsigned) atoi(argv[3]);
    fd = atoi(argv[4]);
    if ((unsigned) argc != 5 + nargs)
        return 3;
    for (unsigned i = 0; i < nargs; i++)
        if (!spawn_big_matches(argv[5 + i], i, false))
            return 4;
    if (!environ)
        return 8;
    for (unsigned i = 0; i < nenv; i++)
        if (!spawn_big_matches(environ[i], i, true))
            return 5;
    if (environ[nenv])
        return 6;
    if (fd != -1 && fstat(fd, &st) != 0)
        return 7;
    return EXIT_SUCCESS;
}

/* Spawn the child with nargs + nenv pattern strings and, when fd != -1, fd
 * as a copy of stdout. `native` sends a _PROC_SPAWN through rtmux_spawn()
 * instead of posix_spawn(). Returns the spawn result; *code as for
 * spawn_wait_code().
 */
static int spawn_big(bool native,
                     unsigned nargs,
                     unsigned nenv,
                     int fd,
                     int *code)
{
    posix_spawn_file_actions_t fa, *fap = NULL;
    char nums[3][16];
    char **argv = calloc(6 + nargs, sizeof(*argv));
    char **envp = calloc(1 + nenv, sizeof(*envp));
    char *pool = malloc((size_t) (nargs + nenv) * SPAWN_BIG_LEN + 1);
    int *fd_map = NULL;
    int rc = ENOMEM;

    *code = -1;
    if (!argv || !envp || !pool)
        goto out;
    snprintf(nums[0], sizeof nums[0], "%u", nargs);
    snprintf(nums[1], sizeof nums[1], "%u", nenv);
    snprintf(nums[2], sizeof nums[2], "%d", fd);
    argv[0] = "syscall_testing";
    argv[1] = "--spawn-bigargs-child";
    argv[2] = nums[0];
    argv[3] = nums[1];
    argv[4] = nums[2];
    for (unsigned i = 0; i < nargs; i++) {
        argv[5 + i] = pool + (size_t) i * SPAWN_BIG_LEN;
        spawn_big_fill(argv[5 + i], i, false);
    }
    for (unsigned i = 0; i < nenv; i++) {
        envp[i] = pool + (size_t) (nargs + i) * SPAWN_BIG_LEN;
        spawn_big_fill(envp[i], i, true);
    }
    if (native) {
        int nfds = fd != -1 ? fd + 1 : 3;
        pid_t pid;
        int status;

        /* The fd map rides ahead of the arguments, so a long one moves the
         * offset the streamed arguments are fetched from.
         */
        if (!(fd_map = malloc((size_t) nfds * sizeof(*fd_map))))
            goto out;
        for (int i = 0; i < nfds; i++)
            fd_map[i] = i < 3 ? i : SPAWN_FDCLOSED;
        if (fd != -1)
            fd_map[fd] = 1;
        pid =
            rtmux_spawn("/bin/syscall_testing", nfds, fd_map, NULL, argv, envp);
        rc = pid == -1 ? errno : EOK;
        if (rc == EOK && wait_child_exit(pid, &status, 500) &&
            WIFEXITED(status))
            *code = WEXITSTATUS(status);
        goto out;
    }
    if (fd != -1) {
        if ((rc = posix_spawn_file_actions_init(&fa)) != EOK)
            goto out;
        fap = &fa;
        if ((rc = posix_spawn_file_actions_adddup2(fap, 1, fd)) != EOK)
            goto out;
    }
    rc = spawn_wait_code("/bin/syscall_testing", fap, NULL, argv, envp, NULL,
                         code);
out:
    if (fap)
        (void) posix_spawn_file_actions_destroy(fap);
    free(fd_map);
    free(pool);
    free(envp);
    free(argv);
    return rc;
}

static void test_spawn_large_arguments(void)
{
    static const struct {
        const char *name;
        bool native;
        unsigned nargs, nenv;
        bool high_fd;
    } cases[] = {
        /* sizes either side of the inline limit, then 32 KiB of argv plus
         * 32 KiB of envp
         */
        {"spawn 4 KiB envp", false, 2, 4, false},
        {"spawn 32 KiB argv", false, 32, 0, false},
        {"spawn 32 KiB envp", false, 2, 32, false},
        {"spawn 64 KiB argv+envp byte for byte", false, 32, 32, false},
        /* a request that fits inline, but the dup2 target needs a 4 KiB
         * fdmap, which pushes the _PROC_SPAWN roottask builds past it
         */
        {"spawn high-fd dup2 with tiny arguments", false, 0, 0, true},
        {"spawn high-fd dup2 with 4 KiB arguments", false, 2, 2, true},
        {"spawn 64 KiB arguments with a high-fd dup2", false, 32, 32, true},
        /* the streamed native path, with and without a long fd map */
        {"native spawn streams 32 KiB envp", true, 2, 32, false},
        {"native spawn streams 64 KiB arguments", true, 32, 32, false},
        {"native spawn streams arguments after a long fd map", true, 32, 32,
         true},
    };
    /* the highest descriptor a child may have */
    long open_max = sysconf(_SC_OPEN_MAX);
    int high_fd = (int) (open_max > 0 && open_max <= SPAWN_BIG_HIGH_FD + 1
                             ? open_max - 1
                             : SPAWN_BIG_HIGH_FD);
    char detail[64];
    int code;

    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        int rc = spawn_big(cases[i].native, cases[i].nargs, cases[i].nenv,
                           cases[i].high_fd ? high_fd : -1, &code);

        snprintf(detail, sizeof detail, "rc=%d (%s) child=%d", rc, strerror(rc),
                 code);
        check(rc == EOK && code == 0, cases[i].name, detail);
    }
}

/* Spawn a --getsid-hold child, with POSIX_SPAWN_SETSID when `setsid`, and wait
 * until it runs. It exits once *hold is closed. Returns its pid, or -1.
 */
static pid_t getsid_hold_spawn(bool setsid, int *hold)
{
    char *argv[] = {"syscall_testing", "--getsid-hold", NULL};
    posix_spawn_file_actions_t fa;
    posix_spawnattr_t attr;
    int in[2], out[2];
    pid_t pid = (pid_t) -1;
    char c;
    int rc;

    *hold = -1;
    if (pipe(in) != 0)
        return -1;
    if (pipe(out) != 0) {
        (void) close(in[0]);
        (void) close(in[1]);
        return -1;
    }
    /* Only this child may hold the write end, or its EOF never comes. */
    (void) fcntl(in[1], F_SETFD, FD_CLOEXEC);
    (void) fcntl(out[0], F_SETFD, FD_CLOEXEC);
    (void) posix_spawn_file_actions_init(&fa);
    (void) posix_spawn_file_actions_adddup2(&fa, in[0], 0);
    (void) posix_spawn_file_actions_adddup2(&fa, out[1], 1);
    (void) posix_spawn_file_actions_addclose(&fa, in[0]);
    (void) posix_spawn_file_actions_addclose(&fa, out[1]);
    (void) posix_spawnattr_init(&attr);
    if (setsid)
        (void) posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSID);
    rc = posix_spawn(&pid, "/bin/syscall_testing", &fa, &attr, argv, NULL);
    (void) posix_spawnattr_destroy(&attr);
    (void) posix_spawn_file_actions_destroy(&fa);
    (void) close(in[0]);
    (void) close(out[1]);
    if (rc == EOK && read(out[0], &c, 1) == 1) {
        *hold = in[1];
    } else {
        (void) close(in[1]);
        if (rc == EOK)
            (void) wait_child_exit(pid, &(int) {0}, 500);
        pid = (pid_t) -1;
    }
    (void) close(out[0]);
    return pid;
}

/* Release a getsid_hold_spawn() child and reap it. True when it saw EOF and
 * exited cleanly rather than being killed.
 */
static bool getsid_hold_release(pid_t pid, int hold)
{
    int status;

    (void) close(hold);
    return wait_child_exit(pid, &status, 500) && WIFEXITED(status) &&
           WEXITSTATUS(status) == 0;
}

/* getsid table: the caller, a child in the caller's session, a child leading
 * its own session, and pids that name no process. Each row gives the expected
 * result, or -1 with the expected errno. POSIX lets getsid refuse a process in
 * another session with EPERM, and RTMux does.
 */
static void test_getsid_cases(void)
{
    pid_t self = getsid(0);
    pid_t child, leader;
    int child_hold, leader_hold;
    char detail[64];

    check(self > 0, "getsid self",
          "getsid(0) must report the caller's session");
    child = getsid_hold_spawn(false, &child_hold);
    leader = getsid_hold_spawn(true, &leader_hold);
    check(child != (pid_t) -1 && leader != (pid_t) -1, "getsid spawn holders",
          "the getsid table needs two live children");
    if (self > 0 && child != (pid_t) -1 && leader != (pid_t) -1) {
        const struct {
            const char *name;
            pid_t pid;
            pid_t want;
            int want_errno;
        } cases[] = {
            {"getsid self by pid", getpid(), self, 0},
            {"getsid child shares session", child, self, 0},
            {"getsid other session is EPERM", leader, -1, EPERM},
            {"getsid negative pid is ESRCH", -2, -1, ESRCH},
            {"getsid INT_MAX pid is ESRCH", INT_MAX, -1, ESRCH},
        };

        for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
            pid_t got;
            int err;

            errno = 0;
            got = getsid(cases[i].pid);
            err = errno;
            snprintf(detail, sizeof detail, "got %d errno %d, want %d errno %d",
                     (int) got, err, (int) cases[i].want, cases[i].want_errno);
            check(got == cases[i].want &&
                      (got != -1 || err == cases[i].want_errno),
                  cases[i].name, detail);
        }
    }
    if (leader != (pid_t) -1)
        check(getsid_hold_release(leader, leader_hold),
              "getsid session leader released",
              "the held session leader must exit on EOF");
    if (child != (pid_t) -1) {
        bool clean = getsid_hold_release(child, child_hold);
        pid_t got;
        int err;

        /* A reaped child's pid names no process. */
        errno = 0;
        got = getsid(child);
        err = errno;
        snprintf(detail, sizeof detail, "clean exit %d, got %d errno %d",
                 (int) clean, (int) got, err);
        check(clean && got == -1 && err == ESRCH, "getsid reaped pid is ESRCH",
              detail);
    }
}

/* Release a getsid_hold_spawn() child ahead of its siblings, ignoring a child
 * that never started.
 */
static bool hold_release(pid_t pid, int hold)
{
    return pid == (pid_t) -1 || getsid_hold_release(pid, hold);
}

/* getpgid table, setpgid by the parent, and a group that outlives its leader.
 * A process group is its members: a spawned child starts in its parent's
 * group, the parent may move it (setpgid from parent to spawned child), and a
 * group stays joinable, and signalled by kill(-pgid), while any member lives
 * even after the process it is named for has exited.
 */
static void test_pgrp_model(void)
{
    pid_t self_pg = getpgrp();
    pid_t a, b, d, e, l, kid;
    int ah, bh, dh, eh, lh, kh;
    char detail[96];
    int err;

    kid = getsid_hold_spawn(false, &kh);
    l = getsid_hold_spawn(true, &lh);
    check(kid != (pid_t) -1 && l != (pid_t) -1, "pgrp model spawn children",
          "the group table needs two live children");
    if (kid == (pid_t) -1 || l == (pid_t) -1)
        goto out_kid;

    /* getpgid: the caller, a child (which starts in the caller's group), a
     * child in another session (EPERM, as for getsid), and no process.
     */
    {
        const struct {
            const char *name;
            pid_t pid;
            pid_t want;
            int want_errno;
        } cases[] = {
            {"getpgid(0) is getpgrp", 0, self_pg, 0},
            {"getpgid self by pid", getpid(), self_pg, 0},
            {"getpgid child starts in the caller's group", kid, self_pg, 0},
            {"getpgid other session is EPERM", l, -1, EPERM},
            {"getpgid negative pid is ESRCH", -2, -1, ESRCH},
            {"getpgid INT_MAX pid is ESRCH", INT_MAX, -1, ESRCH},
        };

        for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
            pid_t got;

            errno = 0;
            got = getpgid(cases[i].pid);
            err = errno;
            snprintf(detail, sizeof detail, "got %d errno %d, want %d errno %d",
                     (int) got, err, (int) cases[i].want, cases[i].want_errno);
            check(got == cases[i].want &&
                      (got != -1 || err == cases[i].want_errno),
                  cases[i].name, detail);
        }
    }

    /* setpgid from the parent to a spawned child: into a group of its own, and
     * back into the parent's.
     */
    check(setpgid(kid, kid) == 0 && getpgid(kid) == kid,
          "setpgid parent to spawned child leads a group",
          "the parent must be able to move a spawned child into its own group");
    check(kill(-kid, 0) == 0, "the child's new group is signalable",
          "kill(-pgid, 0) must reach the new group");
    errno = 0;
    err = setpgid(kid, l) == -1 ? errno : 0;
    check(err == EPERM, "setpgid to a group of another session is EPERM",
          "a group in another session cannot be joined");
    check(setpgid(kid, self_pg) == 0 && getpgid(kid) == self_pg,
          "setpgid parent moves a child back to the parent's group",
          "joining the caller's own group must work");
    errno = 0;
    err = setpgid(l, l) == -1 ? errno : 0;
    check(err == EPERM, "setpgid of another session's leader is EPERM",
          "a session leader may not change group");

    /* A group that outlives its leader. */
    a = getsid_hold_spawn(false, &ah);
    b = getsid_hold_spawn(false, &bh);
    if (a != (pid_t) -1 && b != (pid_t) -1) {
        bool a_ok;
        pid_t grp, a_pid;

        check(setpgid(a, a) == 0 && setpgid(b, a) == 0 && getpgid(b) == a,
              "two children share a group led by the first",
              "the second child must join the first's group");
        a_pid = a;
        a_ok = hold_release(a, ah);
        a = (pid_t) -1;
        check(a_ok, "the group leader exits", "the held child must exit");
        grp = getpgid(b);
        check(grp == a_pid && kill(-grp, 0) == 0,
              "a group is still signalable after its leader is gone",
              "kill(-pgid, 0) must find the surviving member");
        d = getsid_hold_spawn(false, &dh);
        if (d != (pid_t) -1) {
            snprintf(detail, sizeof detail, "group %d, d in %d", (int) grp,
                     (int) getpgid(d));
            check(setpgid(d, grp) == 0 && getpgid(d) == grp,
                  "a group whose leader exited can still be joined", detail);
            (void) hold_release(d, dh);
        }
        (void) hold_release(b, bh);
        b = (pid_t) -1;
        /* No member is left: the group no longer exists. */
        errno = 0;
        e = getsid_hold_spawn(false, &eh);
        err = (e != (pid_t) -1 && setpgid(e, grp) == -1) ? errno : 0;
        check(err == EPERM && kill(-grp, 0) == -1,
              "a group with no member left cannot be joined or signalled",
              "an empty group must not exist");
        (void) hold_release(e, eh);
    } else {
        test_fail("pgrp model spawn group", "two children are needed");
    }
    (void) hold_release(a, ah);
    (void) hold_release(b, bh);

out_kid:
    (void) hold_release(l, lh);
    (void) hold_release(kid, kh);
}

static bool spawn_read_report(int fd, char *buf, size_t sz);

/* A --session-probe child: its report, and the pipe that holds it alive. */
struct session_probe {
    pid_t pid;
    int hold;
    int sid, pg0, rc, err, pg1;
};

/* The integer after key in line, or `none` when key is absent. */
static int probe_field(const char *line, const char *key, int none)
{
    const char *p = strstr(line, key);

    return p ? atoi(p + strlen(key)) : none;
}

/* Spawn a --session-probe child (in a session of its own when `setsid`, joining
 * group `join` when it is positive), and read its one-line report. False if it
 * did not start or reported nothing readable.
 */
static bool session_probe_spawn(bool setsid,
                                pid_t join,
                                struct session_probe *sp)
{
    char joinarg[16];
    char *argv[] = {"syscall_testing", "--session-probe", NULL, NULL};
    posix_spawn_file_actions_t fa;
    posix_spawnattr_t attr;
    int in[2], out[2];
    char line[128] = "";
    int rc;

    memset(sp, 0, sizeof *sp);
    sp->pid = (pid_t) -1;
    sp->hold = -1;
    if (join > 0) {
        snprintf(joinarg, sizeof joinarg, "%d", (int) join);
        argv[2] = joinarg;
    }
    if (pipe(in) != 0)
        return false;
    if (pipe(out) != 0) {
        (void) close(in[0]);
        (void) close(in[1]);
        return false;
    }
    (void) fcntl(in[1], F_SETFD, FD_CLOEXEC);
    (void) fcntl(out[0], F_SETFD, FD_CLOEXEC);
    (void) posix_spawn_file_actions_init(&fa);
    (void) posix_spawn_file_actions_adddup2(&fa, in[0], 0);
    (void) posix_spawn_file_actions_adddup2(&fa, out[1], 1);
    (void) posix_spawn_file_actions_addclose(&fa, in[0]);
    (void) posix_spawn_file_actions_addclose(&fa, out[1]);
    (void) posix_spawnattr_init(&attr);
    if (setsid)
        (void) posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSID);
    rc = posix_spawn(&sp->pid, "/bin/syscall_testing", &fa, &attr, argv, NULL);
    (void) posix_spawnattr_destroy(&attr);
    (void) posix_spawn_file_actions_destroy(&fa);
    (void) close(in[0]);
    (void) close(out[1]);
    if (rc == EOK && !spawn_read_report(out[0], line, sizeof line))
        line[0] = '\0';
    (void) close(out[0]);
    if (rc == EOK && strstr(line, "pg1=")) {
        sp->sid = probe_field(line, "sid=", -1);
        sp->pg0 = probe_field(line, "pg0=", -1);
        sp->rc = probe_field(line, "rc=", -2);
        sp->err = probe_field(line, "err=", -1);
        sp->pg1 = probe_field(line, "pg1=", -1);
        sp->hold = in[1];
        return true;
    }
    (void) close(in[1]);
    if (rc == EOK)
        (void) wait_child_exit(sp->pid, &(int){0}, 500);
    sp->pid = (pid_t) -1;
    return false;
}

static void session_probe_release(struct session_probe *sp)
{
    if (sp->pid != (pid_t) -1)
        (void) getsid_hold_release(sp->pid, sp->hold);
}

/* One rule for "no session": the caller, two children that never called setsid
 * (one joins the other's group), one that did, and a child that tries to join
 * that one's group, must agree between getsid, setpgid and kill. A process that
 * never called setsid belongs to its starter's session, so it reports the
 * caller's session, may lead a group or join one in that session, and is
 * signalled by group like any other; a session leader reports itself, is
 * refused setpgid (EPERM), and is another session for getsid (EPERM) and for
 * joining its group (EPERM). (tcgetsid needs a terminal, so test_posix_session_
 * jobcontrol covers it.)
 */
static void test_session_rule(void)
{
    struct session_probe a, b, c, l;
    pid_t self = getsid(0);
    bool ok_a = session_probe_spawn(false, 0, &a);
    bool ok_l = session_probe_spawn(true, 0, &l);
    bool ok_b = ok_a && session_probe_spawn(false, a.pid, &b);
    bool ok_c = ok_l && session_probe_spawn(false, l.pid, &c);
    char detail[160];

    check(ok_a && ok_l && ok_b && ok_c, "session rule spawn probes",
          "the session table needs four live children");
    if (!(ok_a && ok_l && ok_b && ok_c))
        goto out;

    snprintf(detail, sizeof detail, "a sid %d b sid %d self %d", a.sid, b.sid,
             (int) self);
    check(a.sid == self && b.sid == self && a.sid == b.sid,
          "never-setsid children report the caller's session", detail);
    snprintf(detail, sizeof detail, "rc %d err %d pg %d pid %d", a.rc, a.err,
             a.pg1, (int) a.pid);
    check(a.rc == 0 && a.pg1 == a.pid,
          "setpgid(0, 0) in a never-setsid child leads its own group", detail);
    snprintf(detail, sizeof detail, "rc %d err %d pg %d want %d", b.rc, b.err,
             b.pg1, (int) a.pid);
    check(b.rc == 0 && b.pg1 == a.pid,
          "setpgid into another group unlocks the caller once (join succeeds)",
          detail);
    snprintf(detail, sizeof detail, "rc %d err %d pg %d", c.rc, c.err, c.pg1);
    check(c.rc == -1 && c.err == EPERM && c.pg1 == c.pg0,
          "setpgid into a group of another session is EPERM", detail);
    snprintf(detail, sizeof detail, "sid %d pid %d rc %d err %d", l.sid,
             (int) l.pid, l.rc, l.err);
    check(l.sid == l.pid && l.rc == -1 && l.err == EPERM && l.pg1 == l.pid,
          "a session leader reports itself and setpgid refuses it", detail);

    {
        pid_t got_a, got_b, got_l;
        int e_l, k_a, k_l;

        got_a = getsid(a.pid);
        got_b = getsid(b.pid);
        errno = 0;
        got_l = getsid(l.pid);
        e_l = errno;
        snprintf(detail, sizeof detail, "a %d b %d l %d errno %d self %d",
                 (int) got_a, (int) got_b, (int) got_l, e_l, (int) self);
        check(got_a == self && got_b == self && got_l == -1 && e_l == EPERM,
              "getsid from the caller: same session, other session EPERM",
              detail);
        k_a = kill(-a.pid, 0);
        k_l = kill(-l.pid, 0);
        snprintf(detail, sizeof detail, "kill(-a) %d kill(-l) %d", k_a, k_l);
        check(k_a == 0 && k_l == 0,
              "kill by group reaches both sessions' groups", detail);
    }
out:
    if (ok_c)
        session_probe_release(&c);
    if (ok_b)
        session_probe_release(&b);
    if (ok_a)
        session_probe_release(&a);
    if (ok_l)
        session_probe_release(&l);
}

static int name_fd_matches(int fd, const name_attach_t *attach)
{
    struct _server_info info;

    if (connect_server_info(0, (uint64_t) fd, &info) != fd)
        return 0;
    return info.pid == getpid() && info.chid == attach->chid;
}

static void test_named_channel_registry(void)
{
    const char name_a[] = "rtmux-name-registry-a";
    const char name_b[] = "rtmux-name-registry-b";
    const char path_a[] = "/dev/name/local/rtmux-name-registry-a";
    const char path_b[] = "/dev/name/local/rtmux-name-registry-b";
    name_attach_t *attach_a;
    name_attach_t *attach_b;
    int fd_a;
    int fd_b;
    int reopened;

    (void) unlink(path_a);
    (void) unlink(path_b);

    attach_a = name_attach(NULL, name_a, NAME_FLAG_ATTACH_MOUNTABLE);
    if (!attach_a) {
        test_fail("named channel create", strerror(errno));
        return;
    }

    fd_a = name_open(name_a, 0);
    if (fd_a == -1) {
        test_fail("named channel connect", strerror(errno));
        (void) name_detach(attach_a, 0);
        return;
    }

    check(name_fd_matches(fd_a, attach_a), "named channel connect target",
          "name_open must connect to the registered pid/chid");

    check(
        unlink(path_a) == EOK, "named channel unlink before close",
        "unlink must remove the registry name while existing connections live");
    reopened = name_open(name_a, 0);
    check(reopened == -1 && errno == ENOENT,
          "named channel removed name rejects reopen",
          "unlink-before-close must remove the namespace entry");
    if (reopened != -1)
        (void) name_close(reopened);

    check(name_fd_matches(fd_a, attach_a),
          "named channel connection survives unlink",
          "existing connections must survive unlink-before-close");
    (void) name_close(fd_a);
    (void) name_detach(attach_a, 0);

    attach_b = name_attach(NULL, name_b, 0);
    if (!attach_b) {
        test_fail("named channel close-before-unlink setup", strerror(errno));
        return;
    }
    fd_b = name_open(name_b, 0);
    if (fd_b == -1) {
        test_fail("named channel close-before-unlink open", strerror(errno));
        (void) name_detach(attach_b, 0);
        return;
    }
    (void) name_close(fd_b);

    reopened = name_open(name_b, 0);
    check(reopened != -1, "named channel close before unlink",
          "closing one connection must not unregister the endpoint");
    if (reopened != -1) {
        check(name_fd_matches(reopened, attach_b),
              "named channel reopen after close",
              "endpoint must remain discoverable until owner detach");
        (void) name_close(reopened);
    }

    check(name_detach(attach_b, 0) == EOK, "named channel owner detach",
          "owner detach must unregister the endpoint");
    reopened = name_open(name_b, 0);
    check(reopened == -1 && errno == ENOENT,
          "named channel detach removes endpoint",
          "future opens must fail after owner detach");
    if (reopened != -1)
        (void) name_close(reopened);

    errno = 0;
    attach_a = name_attach(NULL, "/dev/mem", 0);
    check(!attach_a && errno == EINVAL, "named channel rejects resource shadow",
          "rooted names outside /dev/name must not shadow existing paths");
    if (attach_a)
        (void) name_detach(attach_a, 0);
}

struct endpoint_revoke_args {
    const char *name;
    int chid;
    int max_iters;
    atomic_int stop;
    atomic_int ready;
    atomic_int sends;
    atomic_int failures;
    atomic_int clients_done;
};

static struct endpoint_revoke_args endpoint_revoke_args;

struct endpoint_probe_args {
    int chid;
    int coid;
    int max_iters;
    atomic_int stop;
    atomic_int ready;
    atomic_int sends;
    atomic_int failures;
    atomic_int priority_ok;
    atomic_int client_done;
    atomic_int server_done;
    atomic_ulong max_roundtrip_ns;
};

static struct endpoint_probe_args endpoint_probe_args;

struct endpoint_cap_lifecycle_args {
    int chid;
    atomic_int ready;
    atomic_int done;
};

struct endpoint_single_consumer_args {
    int chid;
    atomic_int ready;
    atomic_int consumed;
    atomic_int done;
};

static void endpoint_probe_record_max(atomic_ulong *target, unsigned long value)
{
    unsigned long old = atomic_load_explicit(target, memory_order_relaxed);

    while (old < value && !atomic_compare_exchange_weak_explicit(
                              target, &old, value, memory_order_relaxed,
                              memory_order_relaxed)) {
    }
}

static void *endpoint_revoke_server(void *arg)
{
    struct endpoint_revoke_args *args = arg;

    /* The channel delivers a disconnect notice per closed client, and a receive
     * buffer smaller than struct _notification would fail on it.
     */
    union {
        int msg;
        struct _notification note;
    } buf;

    atomic_store_explicit(&args->ready, 1, memory_order_release);
    while (!atomic_load_explicit(&args->stop, memory_order_acquire)) {
        int rcvid = msg_receive(args->chid, &buf, sizeof(buf), NULL);

        if (rcvid < 0)
            break;
        /* rcvid 0 is that notification. */
        if (rcvid == 0)
            continue;
        (void) msg_reply(rcvid, EOK, &buf.msg, sizeof(buf.msg));
    }
    return NULL;
}

static void *endpoint_revoke_client(void *arg)
{
    struct endpoint_revoke_args *args = arg;

    for (int i = 0; i < args->max_iters &&
                    !atomic_load_explicit(&args->stop, memory_order_acquire);
         i++) {
        int fd = name_open(args->name, 0);
        int msg = 0x7157;
        int reply = 0;

        if (fd == -1) {
            atomic_fetch_add_explicit(&args->failures, 1, memory_order_relaxed);
            break;
        }
        if (msg_send(fd, &msg, sizeof(msg), &reply, sizeof(reply)) == 0 &&
            reply == msg) {
            atomic_fetch_add_explicit(&args->sends, 1, memory_order_relaxed);
        } else {
            atomic_fetch_add_explicit(&args->failures, 1, memory_order_relaxed);
            (void) name_close(fd);
            break;
        }
        (void) name_close(fd);
        (void) __sched_yield();
    }
    atomic_fetch_add_explicit(&args->clients_done, 1, memory_order_release);
    return NULL;
}

static void *endpoint_probe_server(void *arg)
{
    struct endpoint_probe_args *args = arg;
    int msg;

    atomic_store_explicit(&args->ready, 1, memory_order_release);
    while (!atomic_load_explicit(&args->stop, memory_order_acquire)) {
        int rcvid = msg_receive(args->chid, &msg, sizeof(msg), NULL);

        if (rcvid <= 0)
            break;
        (void) msg_reply(rcvid, EOK, &msg, sizeof(msg));
    }
    atomic_store_explicit(&args->server_done, 1, memory_order_release);
    return NULL;
}

static void *endpoint_cap_lifecycle_server(void *arg)
{
    struct endpoint_cap_lifecycle_args *args = arg;
    int msg;

    atomic_store_explicit(&args->ready, 1, memory_order_release);
    for (;;) {
        int rcvid = msg_receive(args->chid, &msg, sizeof(msg), NULL);

        if (rcvid <= 0)
            break;
        (void) msg_reply(rcvid, EOK, &msg, sizeof(msg));
    }
    atomic_store_explicit(&args->done, 1, memory_order_release);
    return NULL;
}

static void *endpoint_single_consumer_receiver(void *arg)
{
    struct endpoint_single_consumer_args *args = arg;
    int msg;
    int rcvid;

    atomic_fetch_add_explicit(&args->ready, 1, memory_order_release);
    rcvid = msg_receive(args->chid, &msg, sizeof(msg), NULL);
    if (rcvid > 0) {
        atomic_fetch_add_explicit(&args->consumed, 1, memory_order_relaxed);
        (void) msg_reply(rcvid, EOK, &msg, sizeof(msg));
    }
    atomic_fetch_add_explicit(&args->done, 1, memory_order_release);
    return NULL;
}

static void *endpoint_probe_client(void *arg)
{
    struct endpoint_probe_args *args = arg;
    struct sched_param sp;

    sp.sched_priority = 40;
    if (__sched_set(0, 0, SCHED_FIFO, &sp) == EOK)
        atomic_store_explicit(&args->priority_ok, 1, memory_order_release);
    else
        atomic_fetch_add_explicit(&args->failures, 1, memory_order_relaxed);
    for (int i = 0; i < args->max_iters &&
                    !atomic_load_explicit(&args->stop, memory_order_acquire);
         i++) {
        int msg = 0x7135;
        int reply = 0;
        uint64_t start;
        uint64_t end;

        if (__clock_time(CLOCK_MONOTONIC, NULL, &start) == -1)
            start = 0;
        if (msg_send(args->coid, &msg, sizeof(msg), &reply, sizeof(reply)) ==
                0 &&
            reply == msg) {
            atomic_fetch_add_explicit(&args->sends, 1, memory_order_relaxed);
            if (start != 0 && __clock_time(CLOCK_MONOTONIC, NULL, &end) != -1 &&
                end >= start) {
                endpoint_probe_record_max(&args->max_roundtrip_ns,
                                          (unsigned long) (end - start));
            }
        } else {
            atomic_fetch_add_explicit(&args->failures, 1, memory_order_relaxed);
            break;
        }
        (void) __sched_yield();
    }
    atomic_store_explicit(&args->client_done, 1, memory_order_release);
    return NULL;
}

static void endpoint_probe_stop(struct endpoint_probe_args *args,
                                int client_started,
                                int server_started,
                                int wait_loops)
{
    atomic_store_explicit(&args->stop, 1, memory_order_release);
    if (client_started) {
        for (int i = 0;
             i < wait_loops &&
             !atomic_load_explicit(&args->client_done, memory_order_acquire);
             i++)
            (void) __sched_yield();
    }
    (void) __connect_detach((uint64_t) args->coid);
    (void) channel_destroy(args->chid);
    if (server_started) {
        for (int i = 0;
             i < wait_loops &&
             !atomic_load_explicit(&args->server_done, memory_order_acquire);
             i++)
            (void) __sched_yield();
    }
}

static void test_endpoint_cap_lifecycle_handles(void)
{
    enum {
        WAIT_LOOPS = 2000000,
    };
    struct endpoint_cap_lifecycle_args args;
    struct _server_info info;
    pthread_t server;
    uint64_t cap;
    int chid;
    int msg = 0x7163;
    int reply = 0;
    int ret;
    int rc;

    chid = channel_create(0);
    if (chid == -1) {
        test_fail("endpoint cap lifecycle channel", strerror(errno));
        return;
    }

    args.chid = chid;
    atomic_store_explicit(&args.ready, 0, memory_order_relaxed);
    atomic_store_explicit(&args.done, 0, memory_order_relaxed);

    rc = pthread_create(&server, NULL, endpoint_cap_lifecycle_server, &args);
    if (rc != EOK) {
        test_fail("endpoint cap lifecycle server", strerror(rc));
        (void) channel_destroy(chid);
        return;
    }
    (void) pthread_detach(server);

    while (!atomic_load_explicit(&args.ready, memory_order_acquire))
        (void) __sched_yield();

    cap = connect_attach_cap(0, getpid(), chid, RTMUX_SIDE_CHANNEL, 0);
    if (cap == 0) {
        test_fail("endpoint cap attach", strerror(errno));
        (void) channel_destroy(chid);
        return;
    }

    memset(&info, 0, sizeof(info));
    ret = connect_server_info(0, (uint64_t) cap, &info);
    check(ret != -1 && info.endpoint_cap != 0 &&
              info.endpoint_rights == RTMUX_CAP_ENDPOINT_ALL,
          "endpoint cap server info",
          "connect_server_info must accept 64-bit endpoint handles");

    errno = 0;
    ret = connect_flags(0, (uint64_t) cap, 0, 0);
    check(ret != -1, "endpoint cap flags",
          "connect_flags must accept 64-bit endpoint handles");

    reply = 0;
    errno = 0;
    ret = msg_send_cap(cap, &msg, sizeof(msg), &reply, sizeof(reply));
    check(ret == EOK && reply == msg, "endpoint cap send",
          "MsgSend-cap must send through a 64-bit endpoint handle");

    errno = 0;
    ret = __connect_detach((uint64_t) cap);
    check(ret == EOK, "endpoint cap detach",
          "connect detach must accept and revoke a 64-bit endpoint handle");

    reply = 0;
    errno = 0;
    ret = msg_send_cap(cap, &msg, sizeof(msg), &reply, sizeof(reply));
    check(ret == -1 && errno == EBADF, "endpoint cap stale after detach",
          "detaching a cap handle must revoke the endpoint capability");

    errno = 0;
    ret = msg_send_cap(cap ^ 0x100000000ULL, &msg, sizeof(msg), &reply,
                       sizeof(reply));
    check(ret == -1 && errno == EBADF, "endpoint cap rejects forged handle",
          "unallocated 64-bit endpoint capabilities must not authorize sends");

    (void) channel_destroy(chid);
    for (int i = 0; i < WAIT_LOOPS &&
                    !atomic_load_explicit(&args.done, memory_order_acquire);
         i++)
        (void) __sched_yield();
    check(atomic_load_explicit(&args.done, memory_order_acquire) == 1,
          "endpoint cap lifecycle server exit",
          "channel destroy must unblock the lifecycle server");
}

static void test_endpoint_single_consumer(void)
{
    enum {
        NRECEIVERS = 2,
        WAIT_LOOPS = 200000,
    };
    struct endpoint_single_consumer_args args;
    pthread_t receiver[NRECEIVERS];
    int chid;
    int coid;
    int msg = 0x5163;
    int reply = 0;
    int rc;

    chid = channel_create(0);
    if (chid == -1) {
        test_fail("endpoint single-consumer channel", strerror(errno));
        return;
    }
    coid = connect_attach(0, getpid(), chid, RTMUX_SIDE_CHANNEL, 0);
    if (coid == -1) {
        test_fail("endpoint single-consumer connect", strerror(errno));
        (void) channel_destroy(chid);
        return;
    }

    args.chid = chid;
    atomic_store_explicit(&args.ready, 0, memory_order_relaxed);
    atomic_store_explicit(&args.consumed, 0, memory_order_relaxed);
    atomic_store_explicit(&args.done, 0, memory_order_relaxed);
    for (int i = 0; i < NRECEIVERS; i++) {
        rc = pthread_create(&receiver[i], NULL,
                            endpoint_single_consumer_receiver, &args);
        if (rc != EOK) {
            test_fail("endpoint single-consumer receiver", strerror(rc));
            (void) __connect_detach((uint64_t) coid);
            (void) channel_destroy(chid);
            return;
        }
        (void) pthread_detach(receiver[i]);
    }

    for (int i = 0;
         i < WAIT_LOOPS &&
         atomic_load_explicit(&args.ready, memory_order_acquire) < NRECEIVERS;
         i++)
        (void) __sched_yield();

    rc = msg_send(coid, &msg, sizeof(msg), &reply, sizeof(reply));
    check(rc == EOK && reply == msg, "endpoint queued send deliverable",
          "a sent message must be delivered to a blocked receiver and replied");
    check(atomic_load_explicit(&args.consumed, memory_order_acquire) == 1,
          "endpoint one receiver consumes message",
          "one queued message must wake at most one receiver");

    (void) __connect_detach((uint64_t) coid);
    (void) channel_destroy(chid);
    for (int i = 0;
         i < WAIT_LOOPS &&
         atomic_load_explicit(&args.done, memory_order_acquire) < NRECEIVERS;
         i++)
        (void) __sched_yield();
    check(atomic_load_explicit(&args.done, memory_order_acquire) == NRECEIVERS,
          "endpoint receiver teardown",
          "channel destroy must unblock every detached receiver");
}

static void test_proc_lifecycle_burst_wait(void)
{
    enum {
        NCHILDREN = 8,
        RESTARTS = 12,
        WAIT_LOOPS = 200000,
    };
    char *argv[] = {"true", NULL};
    pid_t pids[NCHILDREN];
    int reaped[NCHILDREN];
    int status;
    int got_any;
    int rc;

    memset(reaped, 0, sizeof(reaped));
    for (int i = 0; i < NCHILDREN; i++) {
        rc = posix_spawn(&pids[i], "/bin/true", NULL, NULL, argv, NULL);
        if (rc != EOK) {
            test_fail("proc lifecycle burst spawn", strerror(rc));
            return;
        }
    }

    got_any = 0;
    for (int loop = 0; loop < WAIT_LOOPS && !got_any; loop++) {
        pid_t got;

        errno = 0;
        got = waitpid(-1, &status, WNOHANG);
        if (got == 0) {
            (void) __sched_yield();
            continue;
        }
        if (got == -1) {
            if (errno == EINTR)
                continue;
            break;
        }
        got_any = 1;
        for (int i = 0; i < NCHILDREN; i++) {
            if (pids[i] == got) {
                reaped[i] = 1;
                break;
            }
        }
        check(WIFEXITED(status) && WEXITSTATUS(status) == 0,
              "proc lifecycle burst child status",
              "short-lived children must report clean exit status");
    }

    for (int i = 0; i < NCHILDREN; i++) {
        if (reaped[i] == 0) {
            pid_t got;

            do {
                got = waitpid(pids[i], &status, 0);
            } while (got == -1 && errno == EINTR);
            reaped[i] =
                got == pids[i] && WIFEXITED(status) && WEXITSTATUS(status) == 0;
        }
    }
    check(got_any, "proc lifecycle WNOHANG prompt",
          "waitpid(-1, WNOHANG) must promptly report an exited child");
    for (int i = 0; i < NCHILDREN; i++)
        check(reaped[i] == 1, "proc lifecycle pid generation unique",
              "each child pid generation must be observed once");

    for (int i = 0; i < RESTARTS; i++) {
        pid_t pid;

        rc = posix_spawn(&pid, "/bin/true", NULL, NULL, argv, NULL);
        if (rc != EOK) {
            test_fail("proc lifecycle restart spawn", strerror(rc));
            return;
        }
        check(waitpid(pid, &status, 0) == pid && WIFEXITED(status) &&
                  WEXITSTATUS(status) == 0,
              "proc lifecycle bounded restart reap",
              "restart-style spawn loops must reap each generation");
    }
}

struct name_unlink_race_args {
    const char *name;
    atomic_int stop;
    atomic_int opens;
    atomic_int done;
};

static struct name_unlink_race_args name_unlink_race_args;

static void *name_unlink_race_opener(void *arg)
{
    struct name_unlink_race_args *args = arg;

    while (!atomic_load_explicit(&args->stop, memory_order_acquire)) {
        int fd = name_open(args->name, 0);

        if (fd != -1) {
            atomic_fetch_add_explicit(&args->opens, 1, memory_order_relaxed);
            (void) name_close(fd);
        }
    }
    atomic_fetch_add_explicit(&args->done, 1, memory_order_release);
    return NULL;
}

/* Several threads open one namespace node while its owner detaches (an unlink
 * that must be deferred behind the opens in flight) and re-attaches it. The
 * path manager parks the unlink and the opens that arrive behind it, and the
 * first open to finish must restart only those parked operations. Before the
 * fix it also stole the entries of the opens still running on other roottask
 * threads, which corrupted the pending list and left the parked unlink (and its
 * caller) in MsgSend forever. A regression shows up as a hang here, so the
 * suite timeout is the failure signal.
 */
static void test_name_unlink_open_race(void)
{
    enum {
        NOPENERS = 4,
        CYCLES = 200,
        WAIT_LOOPS = 200000,
    };
    const char name[] = "rtmux-unlink-race";
    pthread_t openers[NOPENERS];
    int started = 0;
    int detach_failures = 0;

    atomic_store_explicit(&name_unlink_race_args.stop, 0, memory_order_relaxed);
    atomic_store_explicit(&name_unlink_race_args.opens, 0, memory_order_relaxed);
    atomic_store_explicit(&name_unlink_race_args.done, 0, memory_order_relaxed);
    name_unlink_race_args.name = name;

    for (int i = 0; i < NOPENERS; i++) {
        if (pthread_create(&openers[i], NULL, name_unlink_race_opener,
                           &name_unlink_race_args) != EOK)
            break;
        (void) pthread_detach(openers[i]);
        started++;
    }
    if (started != NOPENERS) {
        test_fail("name unlink race openers", "pthread_create failed");
        atomic_store_explicit(&name_unlink_race_args.stop, 1,
                              memory_order_release);
        return;
    }

    for (int i = 0; i < CYCLES; i++) {
        name_attach_t *attach = name_attach(NULL, name, 0);

        if (!attach) {
            test_fail("name unlink race attach", strerror(errno));
            break;
        }
        (void) __sched_yield();
        if (name_detach(attach, 0) != EOK)
            detach_failures++;
    }

    atomic_store_explicit(&name_unlink_race_args.stop, 1, memory_order_release);
    for (int i = 0;
         i < WAIT_LOOPS &&
         atomic_load_explicit(&name_unlink_race_args.done,
                              memory_order_acquire) < NOPENERS;
         i++)
        (void) __sched_yield();

    check(detach_failures == 0, "name unlink race detach",
          "detach must succeed while other threads open the name");
    check(atomic_load_explicit(&name_unlink_race_args.done,
                               memory_order_acquire) == NOPENERS,
          "name unlink race openers finish",
          "every opener must return from name_open after the churn");
}

static void test_named_endpoint_destroy_stress(void)
{
    enum {
        NCLIENTS = 4,
        CLIENT_ITERS = 64,
        PROBE_ITERS = 128,
        WAIT_LOOPS = 200000,
        START_WAIT_LOOPS = 500,
        DETACH_BUDGET_NS = 100000000,
        PROBE_BUDGET_NS = 100000000,
    };
    const struct timespec start_delay = {.tv_nsec = 10000000L};
    const char name[] = "rtmux-endpoint-revoke";
    name_attach_t *attach;
    pthread_t server;
    pthread_t probe_server;
    pthread_t probe_client;
    pthread_t clients[NCLIENTS];
    uint64_t detach_start;
    uint64_t detach_end;
    int rc;
    int launched;
    int probe_chid;
    int probe_coid;
    int probe_server_started;
    int probe_client_started;

    (void) unlink("/dev/name/local/rtmux-endpoint-revoke");
    attach = name_attach(NULL, name, 0);
    if (!attach) {
        test_fail("endpoint revoke attach", strerror(errno));
        return;
    }

    probe_chid = channel_create(0);
    if (probe_chid == -1) {
        test_fail("endpoint revoke probe channel", strerror(errno));
        (void) name_detach(attach, 0);
        return;
    }
    probe_coid = connect_attach(0, getpid(), probe_chid, RTMUX_SIDE_CHANNEL, 0);
    if (probe_coid == -1) {
        test_fail("endpoint revoke probe connect", strerror(errno));
        (void) channel_destroy(probe_chid);
        (void) name_detach(attach, 0);
        return;
    }
    probe_server_started = 0;
    probe_client_started = 0;

    endpoint_revoke_args.name = name;
    endpoint_revoke_args.chid = attach->chid;
    endpoint_revoke_args.max_iters = CLIENT_ITERS;
    atomic_store_explicit(&endpoint_revoke_args.stop, 0, memory_order_relaxed);
    atomic_store_explicit(&endpoint_revoke_args.ready, 0, memory_order_relaxed);
    atomic_store_explicit(&endpoint_revoke_args.sends, 0, memory_order_relaxed);
    atomic_store_explicit(&endpoint_revoke_args.failures, 0,
                          memory_order_relaxed);
    atomic_store_explicit(&endpoint_revoke_args.clients_done, 0,
                          memory_order_relaxed);

    endpoint_probe_args.chid = probe_chid;
    endpoint_probe_args.coid = probe_coid;
    endpoint_probe_args.max_iters = PROBE_ITERS;
    atomic_store_explicit(&endpoint_probe_args.stop, 0, memory_order_relaxed);
    atomic_store_explicit(&endpoint_probe_args.ready, 0, memory_order_relaxed);
    atomic_store_explicit(&endpoint_probe_args.sends, 0, memory_order_relaxed);
    atomic_store_explicit(&endpoint_probe_args.failures, 0,
                          memory_order_relaxed);
    atomic_store_explicit(&endpoint_probe_args.priority_ok, 0,
                          memory_order_relaxed);
    atomic_store_explicit(&endpoint_probe_args.client_done, 0,
                          memory_order_relaxed);
    atomic_store_explicit(&endpoint_probe_args.server_done, 0,
                          memory_order_relaxed);
    atomic_store_explicit(&endpoint_probe_args.max_roundtrip_ns, 0,
                          memory_order_relaxed);

    rc = pthread_create(&probe_server, NULL, endpoint_probe_server,
                        &endpoint_probe_args);
    if (rc != EOK) {
        test_fail("endpoint revoke probe server", strerror(rc));
        (void) __connect_detach((uint64_t) probe_coid);
        (void) channel_destroy(probe_chid);
        (void) name_detach(attach, 0);
        return;
    }
    probe_server_started = 1;
    (void) pthread_detach(probe_server);

    for (int i = 0;
         i < START_WAIT_LOOPS &&
         !atomic_load_explicit(&endpoint_probe_args.ready, memory_order_acquire);
         i++)
        (void) nanosleep(&start_delay, NULL);
    if (!atomic_load_explicit(&endpoint_probe_args.ready,
                              memory_order_acquire)) {
        test_fail("endpoint revoke probe server ready",
                  "probe server did not reach receive");
        endpoint_probe_stop(&endpoint_probe_args, 0, probe_server_started,
                            WAIT_LOOPS);
        (void) name_detach(attach, 0);
        return;
    }

    rc = pthread_create(&probe_client, NULL, endpoint_probe_client,
                        &endpoint_probe_args);
    if (rc != EOK) {
        test_fail("endpoint revoke probe client", strerror(rc));
        endpoint_probe_stop(&endpoint_probe_args, 0, probe_server_started,
                            WAIT_LOOPS);
        (void) name_detach(attach, 0);
        return;
    }
    probe_client_started = 1;
    (void) pthread_detach(probe_client);

    rc = pthread_create(&server, NULL, endpoint_revoke_server,
                        &endpoint_revoke_args);
    if (rc != EOK) {
        test_fail("endpoint revoke server", strerror(rc));
        endpoint_probe_stop(&endpoint_probe_args, probe_client_started,
                            probe_server_started, WAIT_LOOPS);
        (void) name_detach(attach, 0);
        return;
    }
    (void) pthread_detach(server);

    for (int i = 0;
         i < START_WAIT_LOOPS &&
         !atomic_load_explicit(&endpoint_revoke_args.ready,
                               memory_order_acquire);
         i++)
        (void) nanosleep(&start_delay, NULL);
    if (!atomic_load_explicit(&endpoint_revoke_args.ready,
                              memory_order_acquire)) {
        test_fail("endpoint revoke server ready",
                  "endpoint server did not reach receive");
        atomic_store_explicit(&endpoint_revoke_args.stop, 1,
                              memory_order_release);
        endpoint_probe_stop(&endpoint_probe_args, probe_client_started,
                            probe_server_started, WAIT_LOOPS);
        (void) name_detach(attach, 0);
        return;
    }

    launched = 0;
    for (int i = 0; i < NCLIENTS; i++) {
        rc = pthread_create(&clients[i], NULL, endpoint_revoke_client,
                            &endpoint_revoke_args);
        if (rc != EOK)
            break;
        (void) pthread_detach(clients[i]);
        launched++;
    }
    if (launched != NCLIENTS) {
        test_fail("endpoint revoke clients", "pthread_create failed");
        atomic_store_explicit(&endpoint_revoke_args.stop, 1,
                              memory_order_release);
        endpoint_probe_stop(&endpoint_probe_args, probe_client_started,
                            probe_server_started, WAIT_LOOPS);
        (void) name_detach(attach, 0);
        return;
    }

    for (int i = 0; i < START_WAIT_LOOPS &&
                    atomic_load_explicit(&endpoint_revoke_args.sends,
                                         memory_order_relaxed) < NCLIENTS;
         i++)
        (void) nanosleep(&start_delay, NULL);
    if (atomic_load_explicit(&endpoint_revoke_args.sends,
                             memory_order_relaxed) < NCLIENTS) {
        test_fail("endpoint revoke warmup", "clients did not reach endpoint");
        atomic_store_explicit(&endpoint_revoke_args.stop, 1,
                              memory_order_release);
        endpoint_probe_stop(&endpoint_probe_args, probe_client_started,
                            probe_server_started, WAIT_LOOPS);
        (void) name_detach(attach, 0);
        return;
    }
    for (int i = 0;
         i < START_WAIT_LOOPS &&
         atomic_load_explicit(&endpoint_probe_args.sends,
                              memory_order_relaxed) == 0;
         i++)
        (void) nanosleep(&start_delay, NULL);

    if (__clock_time(CLOCK_MONOTONIC, NULL, &detach_start) == -1)
        detach_start = 0;
    rc = name_detach(attach, 0);
    if (__clock_time(CLOCK_MONOTONIC, NULL, &detach_end) == -1)
        detach_end = detach_start;
    check(rc == EOK, "endpoint revoke detach",
          "owner detach must destroy the named endpoint under client churn");
    check(detach_start == 0 || detach_end - detach_start < DETACH_BUDGET_NS,
          "endpoint revoke detach latency",
          "endpoint detach under sender churn must remain bounded");
    check(
        atomic_load_explicit(&endpoint_probe_args.sends, memory_order_relaxed) >
                0 &&
            atomic_load_explicit(&endpoint_probe_args.priority_ok,
                                 memory_order_acquire) == 1 &&
            atomic_load_explicit(&endpoint_probe_args.failures,
                                 memory_order_relaxed) == 0 &&
            atomic_load_explicit(&endpoint_probe_args.max_roundtrip_ns,
                                 memory_order_relaxed) < PROBE_BUDGET_NS,
        "endpoint revoke unrelated ipc latency",
        "unrelated high-priority IPC must complete while endpoint revoke runs");

    for (int i = 0; i < WAIT_LOOPS; i++) {
        if (atomic_load_explicit(&endpoint_revoke_args.clients_done,
                                 memory_order_acquire) == NCLIENTS)
            break;
        (void) __sched_yield();
    }
    atomic_store_explicit(&endpoint_revoke_args.stop, 1, memory_order_release);
    endpoint_probe_stop(&endpoint_probe_args, probe_client_started,
                        probe_server_started, WAIT_LOOPS);

    {
        int sends = atomic_load_explicit(&endpoint_revoke_args.sends,
                                         memory_order_relaxed);
        int done = atomic_load_explicit(&endpoint_revoke_args.clients_done,
                                        memory_order_acquire);
        int failures = atomic_load_explicit(&endpoint_revoke_args.failures,
                                            memory_order_relaxed);
        char detail[128];

        snprintf(detail, sizeof(detail), "sends=%d done=%d failures=%d", sends,
                 done, failures);
        check(sends >= NCLIENTS && done == NCLIENTS &&
                  (failures > 0 || sends >= NCLIENTS * CLIENT_ITERS),
              "endpoint revoke concurrent senders", detail);
    }
}

static void test_endpoint_sender_identity_from_kernel(void)
{
    const char name[] = "rtmux-forged-pid";
    char *argv[] = {
        "syscall_testing", "--send-forged-pid", (char *) name, "1", NULL,
    };
    name_attach_t *attach;
    struct _msg_info info;
    pid_t claimed = 0;
    pid_t child;
    int rcvid;
    int reply = 0;
    int status = 0;
    int reply_rc;
    int rc;
    bool exited;

    (void) unlink("/dev/name/local/rtmux-forged-pid");
    attach = name_attach(NULL, name, 0);
    if (!attach) {
        test_fail("endpoint sender identity attach", strerror(errno));
        return;
    }

    rc = posix_spawn(&child, "/bin/syscall_testing", NULL, NULL, argv, NULL);
    if (rc != EOK) {
        test_fail("endpoint sender identity child", strerror(rc));
        (void) name_detach(attach, 0);
        return;
    }

    memset(&info, 0, sizeof(info));
    rcvid = msg_receive(attach->chid, &claimed, sizeof(claimed), &info);
    if (rcvid <= 0) {
        test_fail("endpoint sender identity receive", strerror(errno));
        (void) waitpid(child, &status, 0);
        (void) name_detach(attach, 0);
        return;
    }
    reply_rc = msg_reply(rcvid, EOK, &reply, sizeof(reply));

    check(claimed == 1 && info.pid == child && info.pid != claimed,
          "endpoint sender identity is kernel supplied",
          "servers must use receive-time pid, not sender-controlled payload");
    exited = wait_child_exit(child, &status, 500);
    {
        char detail[96];

        snprintf(detail, sizeof(detail), "reply=%d exited=%d status=%d",
                 reply_rc, exited, status);
        check(reply_rc == 0 && exited && WIFEXITED(status) &&
                  WEXITSTATUS(status) == 0,
              "endpoint sender identity child exit", detail);
    }
    (void) name_detach(attach, 0);
}

static void test_name_attach_rejects_unknown_flags(void)
{
    name_attach_t *attach;

    errno = 0;

    /* 0x80 is outside NAME_FLAG_ATTACH_MOUNTABLE; a silent accept here would
     * mean a future caller's typo gets a different attach semantic than they
     * asked for.
     */
    attach = name_attach(NULL, "rtmux-name-flag-probe", 0x80);
    check(!attach && errno == EINVAL, "name_attach rejects unknown flag bit",
          "unknown attach flag bits must fail closed with EINVAL");
    if (attach)
        (void) name_detach(attach, 0);
}

static void test_name_detach_savedpp_preserves_channel(void)
{
    name_attach_t *attach;
    dispatch_t *saved_dpp;
    int saved_chid;
    int probe_fd;
    int channel_alive;

    /* Defensive cleanup against a prior run that crashed between attach and
     * detach; without this a stale registry node makes the next name_attach()
     * fail with EEXIST for reasons unrelated to SAVEDPP.
     */
    (void) unlink("/dev/name/local/rtmux-name-savedpp");

    attach = name_attach(NULL, "rtmux-name-savedpp", 0);
    if (!attach) {
        test_fail("name savedpp attach", strerror(errno));
        return;
    }
    saved_dpp = attach->dpp;
    saved_chid = attach->chid;

    check(name_detach(attach, NAME_FLAG_DETACH_SAVEDPP) == EOK,
          "name_detach SAVEDPP returns ok", "detach with SAVEDPP must succeed");
    /* name_detach always frees `attach`; do not touch it from here on. */

    /* SAVEDPP only preserves the dispatch and channel; the registry name itself
     * must still be unlinked. Without this probe a partial regression that kept
     * the channel but failed the unlink would pass.
     */
    errno = 0;
    probe_fd = name_open("rtmux-name-savedpp", 0);
    check(probe_fd == -1 && errno == ENOENT,
          "name_detach SAVEDPP unlinks the registry name",
          "SAVEDPP must not leave the name discoverable");
    if (probe_fd != -1)
        (void) name_close(probe_fd);

    /* Probe channel survival. channel_destroy on a stale chid returns -1 safely
     * (chids are integer handles, no pointer dereference).
     */
    channel_alive = channel_destroy(saved_chid) == 0;
    check(channel_alive, "name_detach SAVEDPP preserves channel",
          "channel must outlive a SAVEDPP detach");

    /* Only release the orphaned dispatch_t if the channel probe confirmed
     * SAVEDPP was honored. If SAVEDPP regressed, name_detach already destroyed
     * the dispatch and saved_dpp is freed memory -- calling dispatch_destroy on
     * it would be UAF.
     */
    if (channel_alive)
        (void) dispatch_destroy(saved_dpp);
}

static void *pthread_null_destructor_worker(void *arg)
{
    pthread_key_t key;

    (void) arg;
    if (pthread_key_create(&key, NULL) != EOK)
        return (void *) 1;
    if (pthread_setspecific(key, (void *) 0x1234) != EOK)
        return (void *) 2;
    pthread_exit(NULL);
}

static void test_pthread_null_key_destructor(void)
{
    pthread_t tid;
    void *ret = (void *) 1;
    int rc;

    rc = pthread_create(&tid, NULL, pthread_null_destructor_worker, NULL);
    if (rc != EOK) {
        test_fail("pthread null key destructor setup", strerror(rc));
        return;
    }
    rc = pthread_join(tid, &ret);
    check(rc == EOK && !ret, "pthread null key destructor exit",
          "thread exit must ignore NULL key destructors");
}

static void check_pthread_cancel_child(const char *mode, const char *name)
{
    char *argv[] = {"syscall_testing", "--pthread-cancel-child", (char *) mode,
                    NULL};
    pid_t pid;
    int status = 0;
    int rc;
    bool exited;

    rc = posix_spawn(&pid, "/bin/syscall_testing", NULL, NULL, argv, NULL);
    if (rc != EOK) {
        test_fail(name, strerror(rc));
        return;
    }

    exited = wait_child_exit(pid, &status, 2000);
    check(exited && WIFEXITED(status) && WEXITSTATUS(status) == 0,
          name, "pthread_cancel delivery child must exit cleanly");
}

static void test_pthread_cancel_contract(void)
{
    check_pthread_cancel_child("testcancel", "pthread cancel testcancel");
    check_pthread_cancel_child("disabled-enable",
                               "pthread cancel disabled then enabled");
    check_pthread_cancel_child("async", "pthread cancel async");
    check_pthread_cancel_child("sem-wait", "pthread cancel sem_wait");
    check_pthread_cancel_child("cond-wait", "pthread cancel cond_wait");
    check_pthread_cancel_child("cond-timedwait",
                               "pthread cancel cond_timedwait");
    check_pthread_cancel_child("nanosleep", "pthread cancel nanosleep");
    check_pthread_cancel_child("join", "pthread cancel pthread_join");
}

static void check_sched_foreign_pid_fails_closed(pid_t pid, bool live)
{
    struct sched_param posix_param;
    struct timespec rr_interval;
    int posix_policy = sched_getscheduler(0);

    if (posix_policy == -1 || sched_getparam(0, &posix_param) != EOK) {
        test_fail(live ? "sched live foreign fixture" :
                  "sched stale foreign fixture",
                  strerror(errno));
        return;
    }

    errno = 0;
    check(sched_getparam(pid, &posix_param) == -1 && errno == ENOSYS,
          live ? "sched live foreign getparam fails closed" :
                 "sched stale foreign getparam fails closed",
          "sched_getparam must fail closed for foreign pids");
    errno = 0;
    check(sched_getscheduler(pid) == -1 && errno == ENOSYS,
          live ? "sched live foreign getscheduler fails closed" :
                 "sched stale foreign getscheduler fails closed",
          "sched_getscheduler must fail closed for foreign pids");
    errno = 0;
    check(sched_setparam(pid, &posix_param) == -1 && errno == ENOSYS,
          live ? "sched live foreign setparam fails closed" :
                 "sched stale foreign setparam fails closed",
          "sched_setparam must fail closed for foreign pids");
    errno = 0;
    check(sched_setscheduler(pid, posix_policy, &posix_param) == -1 &&
              errno == ENOSYS,
          live ? "sched live foreign setscheduler fails closed" :
                 "sched stale foreign setscheduler fails closed",
          "sched_setscheduler must fail closed for foreign pids");
    errno = 0;
    check(sched_rr_get_interval(pid, &rr_interval) == -1 && errno == ENOSYS,
          live ? "sched live foreign rr interval fails closed" :
                 "sched stale foreign rr interval fails closed",
          "sched_rr_get_interval must fail closed for foreign pids");
}

static void test_posix_sched_contract(void)
{
    char *argv[] = {"syscall_testing", "--proc-cap-hold", NULL};
    struct sched_param posix_param;
    struct timespec rr_interval;
    sem_t *ready;
    sem_t *release;
    pid_t current_pid = getpid();
    pid_t child_pid = (pid_t) -1;
    int status;
    int rc;
    int posix_policy = sched_getscheduler(0);

    check(posix_policy != -1 && sched_getparam(0, &posix_param) == EOK,
          "sched get self", "POSIX sched getters must report current thread");
    check(sched_setparam(0, &posix_param) == EOK &&
              sched_setscheduler(0, posix_policy, &posix_param) == EOK,
          "sched set self no-op",
          "POSIX sched setters must accept current scheduler params");
    check(sched_yield() == EOK, "sched yield",
          "POSIX sched_yield must yield without error");
    check(sched_get_priority_min(SCHED_FIFO) >= 0 &&
              sched_get_priority_max(SCHED_FIFO) >=
                  sched_get_priority_min(SCHED_FIFO),
          "sched priority range", "scheduler priority range must be ordered");
    check(sched_rr_get_interval(0, &rr_interval) == EOK, "sched rr interval",
          "round-robin interval must be queryable");
    check(sched_getscheduler(current_pid) != -1 &&
              sched_getparam(current_pid, &posix_param) == EOK &&
              sched_setparam(current_pid, &posix_param) == EOK &&
              sched_setscheduler(current_pid, posix_policy, &posix_param) ==
                  EOK &&
              sched_rr_get_interval(current_pid, &rr_interval) == EOK,
          "sched current pid",
          "explicit current-pid scheduling must map to the current thread");

    (void) sem_unlink("/proccap-ready");
    (void) sem_unlink("/proccap-release");
    ready = sem_open("/proccap-ready", O_CREAT, 0600, 0);
    release = sem_open("/proccap-release", O_CREAT, 0600, 0);
    if (ready == SEM_FAILED || release == SEM_FAILED) {
        test_fail("sched foreign pid semaphore", strerror(errno));
        if (ready != SEM_FAILED)
            sem_close(ready);
        if (release != SEM_FAILED)
            sem_close(release);
        sem_unlink("/proccap-ready");
        sem_unlink("/proccap-release");
        return;
    }

    rc = posix_spawn(&child_pid, "/bin/syscall_testing", NULL, NULL, argv, NULL);
    if (rc != EOK || child_pid <= 0) {
        test_fail("sched foreign pid child", rc != EOK ? strerror(rc) :
                  "invalid child pid");
        sem_close(ready);
        sem_close(release);
        sem_unlink("/proccap-ready");
        sem_unlink("/proccap-release");
        return;
    }
    (void) sem_wait(ready);

    check_sched_foreign_pid_fails_closed(child_pid, true);

    sem_post(release);
    rc = waitpid(child_pid, &status, 0);
    check(rc == child_pid && WIFEXITED(status) && WEXITSTATUS(status) == 0,
          "sched foreign pid child exits", "held child must exit cleanly");

    check_sched_foreign_pid_fails_closed(child_pid, false);

    sem_close(ready);
    sem_close(release);
    sem_unlink("/proccap-ready");
    sem_unlink("/proccap-release");
}

static void test_pthread_identity_and_tls(void)
{
    pthread_key_t key;
    pthread_t self = pthread_self();

    check(getppid() > 0, "getppid", "parent pid must be positive");
    check(pthread_equal(self, pthread_self()) != 0, "pthread self equality",
          "pthread_self and pthread_equal must identify the calling thread");
    pthread_testcancel();
    test_ok("pthread testcancel without pending cancellation");
    int oldstate = -1;
    check(pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &oldstate) == EOK &&
              oldstate == PTHREAD_CANCEL_ENABLE,
          "pthread cancel disable",
          "pthread_setcancelstate must return the previous state");
    check(pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, &oldstate) == EOK &&
              oldstate == PTHREAD_CANCEL_DISABLE,
          "pthread cancel enable",
          "pthread_setcancelstate must restore cancellation state");
    check(pthread_setcancelstate(-1, NULL) == EINVAL,
          "pthread cancel bad state rejected",
          "invalid cancellation states must fail closed");
    int oldtype = -1;
    check(pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS, &oldtype) == EOK &&
              oldtype == PTHREAD_CANCEL_DEFERRED,
          "pthread cancel async type",
          "pthread_setcanceltype must return the previous type");
    check(pthread_setcanceltype(PTHREAD_CANCEL_DEFERRED, &oldtype) == EOK &&
              oldtype == PTHREAD_CANCEL_ASYNCHRONOUS,
          "pthread cancel deferred type",
          "pthread_setcanceltype must restore cancellation type");
    check(pthread_setcanceltype(-1, NULL) == EINVAL,
          "pthread cancel bad type rejected",
          "invalid cancellation types must fail closed");
    struct sched_param schedparam;
    int policy = -1;
    check(pthread_getschedparam(self, &policy, &schedparam) == EOK &&
              policy >= SCHED_FIFO,
          "pthread getschedparam self",
          "pthread_getschedparam must report the calling thread scheduler");
    check(pthread_setschedparam(self, policy, &schedparam) == EOK,
          "pthread setschedparam self no-op",
          "pthread_setschedparam must accept the current scheduler params");
    check(pthread_setschedprio(self, schedparam.sched_priority) == EOK,
          "pthread setschedprio self no-op",
          "pthread_setschedprio must accept the current priority");
    check(pthread_getschedparam(self, NULL, &schedparam) == EINVAL,
          "pthread getschedparam null policy",
          "pthread_getschedparam must reject null policy output");
    check(pthread_setschedparam(self, policy, NULL) == EINVAL,
          "pthread setschedparam null param",
          "pthread_setschedparam must reject null scheduler params");
    test_posix_sched_contract();
    if (pthread_key_create(&key, NULL) != EOK) {
        test_fail("pthread tls key create", strerror(errno));
        return;
    }
    check(!pthread_getspecific(key), "pthread tls default null",
          "new pthread keys must read as NULL before first set");
    check(pthread_setspecific(key, (void *) 0x5678) == EOK &&
              pthread_getspecific(key) == (void *) 0x5678,
          "pthread tls round trip",
          "pthread_setspecific value must be readable by pthread_getspecific");
    check(pthread_key_delete(key) == EOK, "pthread key delete",
          "pthread_key_delete must release a live key");
    check(pthread_setspecific(key, NULL) == EINVAL,
          "pthread deleted key rejected",
          "deleted pthread keys must fail closed");
}

static void test_pthread_attr_setters(void)
{
    pthread_attr_t attr;
    void *stackaddr = NULL;
    void *got_stackaddr = NULL;
    size_t stacksize = 0;
    size_t guardsize = 0;
    struct sched_param param = {.sched_priority = 3};
    struct sched_param got_param;
    int detachstate = -1;
    int inheritsched = -1;
    int policy = -1;
    int scope = -1;

    if (pthread_attr_init(&attr) != EOK) {
        test_fail("pthread attr setter setup", strerror(errno));
        return;
    }
    check(pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED) == EOK &&
              pthread_attr_getdetachstate(&attr, &detachstate) == EOK &&
              detachstate == PTHREAD_CREATE_DETACHED,
          "pthread attr detach set",
          "PTHREAD_CREATE_DETACHED must be stored in attr flags");
    check(pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE) == EOK &&
              pthread_attr_getdetachstate(&attr, &detachstate) == EOK &&
              detachstate == PTHREAD_CREATE_JOINABLE,
          "pthread attr joinable set",
          "PTHREAD_CREATE_JOINABLE must clear detached attr flag");
    check(pthread_attr_setdetachstate(&attr, -1) == EINVAL,
          "pthread attr bad detach rejected",
          "invalid detach state must fail closed");
    check(pthread_attr_setdetachstate(NULL, PTHREAD_CREATE_DETACHED) == EINVAL,
          "pthread attr null detach rejected",
          "pthread_attr_setdetachstate must reject null attrs");
    check(pthread_attr_setstacksize(&attr, PTHREAD_STACK_MIN) == EOK &&
              pthread_attr_getstacksize(&attr, &stacksize) == EOK &&
              stacksize == PTHREAD_STACK_MIN,
          "pthread attr stack size set", "valid stack size must be stored");
    check(pthread_attr_setstacksize(&attr, PTHREAD_STACK_MIN - 1) == EINVAL,
          "pthread attr small stack rejected",
          "too-small stack size must fail closed");
    check(pthread_attr_setstacksize(NULL, PTHREAD_STACK_MIN) == EINVAL,
          "pthread attr null stack size rejected",
          "pthread_attr_setstacksize must reject null attrs");
    check(pthread_attr_setstackaddr(&attr, stackaddr) == EOK &&
              pthread_attr_getstackaddr(&attr, &got_stackaddr) == EOK &&
              got_stackaddr == stackaddr,
          "pthread attr stack addr round trip",
          "stack address attribute must be stored");
    check(pthread_attr_setstack(&attr, stackaddr, PTHREAD_STACK_MIN) == EOK &&
              pthread_attr_getstack(&attr, &got_stackaddr, &stacksize) == EOK &&
              got_stackaddr == stackaddr && stacksize == PTHREAD_STACK_MIN,
          "pthread attr stack round trip",
          "stack address and size attributes must be stored");
    check(pthread_attr_getstacksize(&attr, NULL) == EINVAL,
          "pthread attr null stack size rejected",
          "attribute getters must reject null output pointers");
    check(pthread_attr_setguardsize(&attr, 0) == EOK &&
              pthread_attr_getguardsize(&attr, &guardsize) == EOK &&
              guardsize == 0,
          "pthread attr guard size round trip",
          "guard size attribute must be stored");
    check(pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED) == EOK &&
              pthread_attr_getinheritsched(&attr, &inheritsched) == EOK &&
              inheritsched == PTHREAD_EXPLICIT_SCHED,
          "pthread attr inheritsched round trip",
          "inherit-scheduler attribute must be stored");
    check(pthread_attr_setinheritsched(&attr, -1) == EINVAL,
          "pthread attr bad inheritsched rejected",
          "invalid inherit scheduler mode must fail closed");
    check(pthread_attr_setschedparam(&attr, &param) == EOK &&
              pthread_attr_getschedparam(&attr, &got_param) == EOK &&
              got_param.sched_priority == param.sched_priority,
          "pthread attr schedparam round trip",
          "scheduler parameter attribute must be stored");
    check(pthread_attr_setschedpolicy(&attr, SCHED_FIFO) == EOK &&
              pthread_attr_getschedpolicy(&attr, &policy) == EOK &&
              policy == SCHED_FIFO,
          "pthread attr schedpolicy round trip",
          "scheduler policy attribute must be stored");
    check(pthread_attr_setschedpolicy(&attr, -1) == EINVAL,
          "pthread attr bad schedpolicy rejected",
          "invalid scheduler policy must fail closed");
    check(pthread_attr_setscope(&attr, PTHREAD_SCOPE_SYSTEM) == EOK &&
              pthread_attr_getscope(&attr, &scope) == EOK &&
              scope == PTHREAD_SCOPE_SYSTEM,
          "pthread attr scope system",
          "system contention scope must be stored");
    check(pthread_attr_setscope(&attr, PTHREAD_SCOPE_PROCESS) == ENOTSUP,
          "pthread attr process scope rejected",
          "process contention scope is not implemented");
    check(pthread_attr_setscope(&attr, -1) == EINVAL,
          "pthread attr bad scope rejected",
          "an unknown contention scope must fail EINVAL");
    check(pthread_attr_getscope(&attr, NULL) == EINVAL,
          "pthread attr null scope output",
          "attribute getters must reject null output pointers");
    check(pthread_setconcurrency(4) == EOK && pthread_getconcurrency() == 4,
          "pthread concurrency hint round trip",
          "pthread concurrency hint must be stored process-locally");
    check(pthread_setconcurrency(-1) == EINVAL,
          "pthread concurrency bad level rejected",
          "negative pthread concurrency hints must fail closed");
    check(pthread_attr_init(NULL) == EINVAL, "pthread attr null init rejected",
          "pthread_attr_init must reject null attrs");
    check(pthread_attr_destroy(&attr) == EOK, "pthread attr destroy",
          "pthread_attr_destroy must be a successful no-op for plain attrs");
}

/* Reentrant system-database lookups. The error and not-found paths run
 * regardless of whether /etc/passwd exists in this boot; the found/ERANGE paths
 * run only where the database is present, and cross-check the _r result against
 * the plain lookup.
 */
static void test_system_db_r(void)
{
    char buf[512];
    struct passwd pw, *pwres;
    struct group gr, *grres;
    int rc;

    check(getpwnam_r(NULL, &pw, buf, sizeof buf, &pwres) == EINVAL,
          "getpwnam_r(NULL name)", "expected EINVAL");
    check(getpwnam_r("x", NULL, buf, sizeof buf, &pwres) == EINVAL,
          "getpwnam_r(NULL pwd)", "expected EINVAL");
    check(getpwnam_r("x", &pw, buf, sizeof buf, NULL) == EINVAL,
          "getpwnam_r(NULL result)", "expected EINVAL");
    check(getgrgid_r(0, NULL, buf, sizeof buf, &grres) == EINVAL,
          "getgrgid_r(NULL grp)", "expected EINVAL");

    rc = getpwnam_r("__rtmux_no_such_user__", &pw, buf, sizeof buf, &pwres);
    check(rc == 0 && pwres == NULL, "getpwnam_r(missing) not found",
          "an absent user must return 0 with a NULL result");
    rc = getgrnam_r("__rtmux_no_such_group__", &gr, buf, sizeof buf, &grres);
    check(rc == 0 && grres == NULL, "getgrnam_r(missing) not found",
          "an absent group must return 0 with a NULL result");

    struct passwd *plain = getpwnam("root");
    if (plain != NULL) {
        uid_t ruid = plain->pw_uid;
        char tiny[1];

        rc = getpwnam_r("root", &pw, buf, sizeof buf, &pwres);
        check(rc == 0 && pwres == &pw && !strcmp(pw.pw_name, "root") &&
                  pw.pw_uid == ruid,
              "getpwnam_r(root) matches getpwnam",
              "the reentrant result must match the plain lookup");
        rc = getpwuid_r(ruid, &pw, buf, sizeof buf, &pwres);
        check(rc == 0 && pwres == &pw && pw.pw_uid == ruid,
              "getpwuid_r(root uid) found", "expected the same entry by uid");
        check(getpwnam_r("root", &pw, tiny, sizeof tiny, &pwres) == ERANGE &&
                  pwres == NULL,
              "getpwnam_r(root, tiny buf)", "a too-small buffer must ERANGE");
    }
}

/* F_DUPFD_CLOEXEC duplicates like F_DUPFD but leaves the new descriptor
 * close-on-exec, where a plain F_DUPFD clears it.
 */
static void test_fcntl_dupfd_cloexec(void)
{
    int fd = open("/dev/zero", O_RDONLY);
    int plain, cloexec;

    if (fd == -1) {
        test_fail("dupfd_cloexec fixture", strerror(errno));
        return;
    }
    plain = fcntl(fd, F_DUPFD, 0);
    cloexec = fcntl(fd, F_DUPFD_CLOEXEC, 0);
    {
        /* F_SETFD must only touch the close-on-exec bit: it once passed an
         * all-ones mask that also revoked the connection's send right, so the
         * descriptor failed every later request with EBADF.
         */
        int p[2];
        char c = 'x';

        if (pipe(p) == 0) {
            struct pollfd pf = {.fd = p[0], .events = POLLIN};

            check(fcntl(p[0], F_SETFD, FD_CLOEXEC) != -1 &&
                      (fcntl(p[0], F_GETFD) & FD_CLOEXEC) &&
                      fcntl(p[1], F_SETFD, FD_CLOEXEC) != -1 &&
                      write(p[1], &c, 1) == 1 && poll(&pf, 1, 100) == 1 &&
                      (pf.revents & POLLIN) && read(p[0], &c, 1) == 1,
                  "F_SETFD keeps the descriptor usable",
                  "set close-on-exec must not revoke the connection");
            (void) close(p[0]);
            (void) close(p[1]);
        }
        check(sysconf(_SC_OPEN_MAX) > 0 && sysconf(_SC_OPEN_MAX) < 65536,
              "sysconf(_SC_OPEN_MAX) is a sane limit",
              "a 32-bit gauge was once read as a long");
    }
    check(plain >= 0 && (fcntl(plain, F_GETFD, 0) & FD_CLOEXEC) == 0,
          "F_DUPFD clears close-on-exec", "a plain dup must not be close-on-exec");
    check(cloexec >= 0 && (fcntl(cloexec, F_GETFD, 0) & FD_CLOEXEC) == FD_CLOEXEC,
          "F_DUPFD_CLOEXEC sets close-on-exec",
          "the duplicate must be close-on-exec");
    if (plain >= 0)
        (void) close(plain);
    if (cloexec >= 0)
        (void) close(cloexec);
    (void) close(fd);
}

/* dup / dup2 duplication semantics, including the POSIX edge cases: dup2 forces
 * its target descriptor, dup2(fd, fd) returns fd for a valid fd, and a bad
 * oldfd fails without disturbing newfd.
 */
static void test_dup_dup2(void)
{
    int fd = open("/dev/zero", O_RDONLY);
    int a, t;
    char c;

    if (fd == -1) {
        test_fail("dup fixture", strerror(errno));
        return;
    }
    a = dup(fd);
    check(a >= 0 && a != fd && read(a, &c, 1) == 1,
          "dup returns a usable new descriptor", "dup must duplicate fd");

    t = dup2(fd, a); /* a is open: dup2 must close and reuse exactly a */
    check(t == a && read(a, &c, 1) == 1, "dup2 forces its target descriptor",
          "dup2(old, newfd) must return newfd");

    check(dup2(fd, fd) == fd, "dup2(fd, fd) returns fd",
          "a valid fd duplicated onto itself is a no-op");

    check(dup(-1) == -1 && errno == EBADF, "dup(bad fd)", "expected EBADF");

    check(dup2(-1, a) == -1 && errno == EBADF, "dup2(bad old) fails",
          "expected EBADF");
    check(read(a, &c, 1) == 1, "dup2(bad old) leaves newfd open",
          "a failed dup2 must not close newfd");

    /* A stale (closed) oldfd is caught before newfd is touched, too. */
    t = dup(fd);
    if (t >= 0) {
        (void) close(t);
        check(dup2(t, a) == -1 && errno == EBADF, "dup2(stale old) fails",
              "a closed oldfd must fail EBADF");
        check(read(a, &c, 1) == 1, "dup2(stale old) leaves newfd open",
              "a failed dup2 must not close newfd");
    }

    if (a >= 0)
        (void) close(a);
    (void) close(fd);
}

/* _IO_DUP fd lifetime across service types.
 *
 * A dup'd descriptor shares one open file description — a single file offset —
 * with its source, and the server-side connection outlives a partial close: the
 * survivor keeps working until the LAST descriptor closes. test_dup_dup2 above
 * checks the dup/dup2 API surface on a native device; this drives the two
 * lifetime invariants against each I/O-service type the syscall-testing boot
 * can reach — a roottask native path service (the cpiofs initramfs root) and
 * the standalone romfs server mounted at /usr over the NVMe block device — plus
 * the /dev/zero native device for the survive-close half alone. All send
 * _IO_DUP through lib/libc/ioserv/dup-handler.c, which refcounts the shared
 * binding. (An LKL-backed file exercises the same path but needs a
 * CONFIG_LKL_SERVERS boot, so it is out of this default suite.)
 */

/* Read up to n bytes, looping over legal short reads until n or EOF/error. */
static ssize_t read_fully(int fd, char *buf, size_t n)
{
    size_t off = 0;
    ssize_t r;

    while (off < n && (r = read(fd, buf + off, n - off)) > 0)
        off += (size_t) r;
    return (ssize_t) off;
}

/* Prove _IO_DUP shares one open file description, not a copied offset: split
 * the fixture in thirds, read [0,q) then dup, read [q,2q) through the SOURCE
 * (moving the offset AFTER the dup exists), close the source, then read the
 * final third through the dup. A shared OFD leaves the dup at 2q so it returns
 * [2q,total); an implementation that copied the offset at dup time — or reset
 * it — would re-read from q or 0 and mismatch the reference. The final read
 * also runs after the source is closed, so the same assertion proves the
 * connection survives a partial close. `optional` skips (vs fails) when the
 * service is not mounted.
 */
static void dup_shared_ofd_check(const char *service, const char *path,
                                 int optional)
{
    char ref[256], got[256], name[96];
    int rf, fd, dupfd;
    ssize_t total, a, b, c;
    size_t q, tail;

    rf = open(path, O_RDONLY);
    if (rf == -1) {
        (void) snprintf(name, sizeof name, "iodup %s %s", service,
                        optional ? "unavailable, skipped" : "fixture");
        if (optional)
            test_ok(name);
        else
            test_fail(name, strerror(errno));
        return;
    }
    total = read_fully(rf, ref, sizeof ref);
    (void) close(rf);

    /* Reject a fixture whose correct final third [2q,total) is
     * indistinguishable from what a broken dup would return: a copied-at-dup
     * offset re-reads from q ([q,...)), a reset offset re-reads from 0
     * ([0,...)). If either matches, the oracle can't tell a shared OFD from a
     * broken one — need a better fixture.
     */
    q = (size_t) total / 3;
    tail = (size_t) total - 2 * q;
    if (q < 1 || memcmp(ref + 2 * q, ref + q, tail) == 0 ||
        memcmp(ref + 2 * q, ref, tail) == 0) {
        (void) snprintf(name, sizeof name, "iodup %s fixture", service);
        test_fail(name, "need a >=3-byte non-repeating regular file");
        return;
    }

    fd = open(path, O_RDONLY);
    if (fd == -1) {
        (void) snprintf(name, sizeof name, "iodup %s reopen", service);
        test_fail(name, strerror(errno));
        return;
    }
    a = read_fully(fd, got, q);            /* source: [0, q)              */
    dupfd = dup(fd);
    b = (dupfd >= 0) ? read_fully(fd, got + a, q) : -1; /* source AFTER dup: [q, 2q) */
    (void) close(fd);                      /* source gone; the dup survives */
    c = (dupfd >= 0 && a == (ssize_t) q && b == (ssize_t) q)
            ? read_fully(dupfd, got + a + b, (size_t) (total - a - b))
            : -1;
    if (dupfd >= 0)
        (void) close(dupfd);

    (void) snprintf(name, sizeof name, "iodup %s shares offset across close",
                    service);
    check(a == (ssize_t) q && b == (ssize_t) q && c > 0 && a + b + c == total &&
              memcmp(got, ref, (size_t) total) == 0,
          name,
          "a source read after dup must advance the dup's shared offset, and the "
          "dup must outlive the source close");
}

static void test_iodup_fd_lifetime(void)
{
    int fd, dupfd;
    char c;

    /* Native path service (roottask cpiofs): the initramfs root serves regular
     * files with a real read offset. Core fixture — fail, don't skip.
     */
    dup_shared_ofd_check("native", "/bin/toolbox", 0);

    /* romfs over the NVMe block device, mounted read-only at /usr. Optional:
     * absent when the suite runs without the block device.
     */
    dup_shared_ofd_check("romfs", "/usr/conf/passwd", 1);

    /* Native device (roottask /dev/zero): the offset is not meaningful, but the
     * connection must still survive closing the source descriptor.
     */
    fd = open("/dev/zero", O_RDONLY);
    if (fd == -1) {
        test_fail("iodup native device fixture", strerror(errno));
        return;
    }
    dupfd = dup(fd);
    (void) close(fd);
    check(dupfd >= 0 && read(dupfd, &c, 1) == 1,
          "iodup native device survives source close",
          "a dup of a device must outlive the source descriptor");
    if (dupfd >= 0)
        (void) close(dupfd);
}

/* Reentrant strerror: a known message copies out and returns 0, a too-small
 * buffer truncates and returns ERANGE, a NULL buffer is EINVAL.
 */
static void test_strerror_r(void)
{
    char buf[64];
    char tiny[4];

    check(strerror_r(EINVAL, buf, sizeof buf) == 0 &&
              !strcmp(buf, "Invalid argument"),
          "strerror_r(EINVAL) message", "expected the known message");
    check(strerror_r(EINVAL, tiny, sizeof tiny) == ERANGE && tiny[3] == '\0',
          "strerror_r(tiny buf) truncates", "expected ERANGE and a NUL terminator");
    check(strerror_r(EINVAL, NULL, 10) == EINVAL, "strerror_r(NULL buf)",
          "expected EINVAL");
    check(strerror_r(EINVAL, buf, 0) == EINVAL, "strerror_r(buflen 0)",
          "a zero-length buffer must fail EINVAL");
    check(strerror_r(0x7fff, buf, sizeof buf) == 0 && buf[0] != '\0',
          "strerror_r(unknown) formats", "an untabled error still yields a message");

    /* Exact-fit success: buffer sized to the message plus its NUL. */
    check(strerror_r(EINVAL, buf, strlen("Invalid argument") + 1) == 0 &&
              !strcmp(buf, "Invalid argument"),
          "strerror_r(exact fit)", "a buffer of exactly len+1 must succeed");
}

/* Reentrant asctime/ctime write the fixed POSIX date string into the caller's
 * buffer; the epoch is a stable oracle (UTC, no timezone).
 */
static void test_asctime_ctime_r(void)
{
    const char *epoch_str = "Thu Jan  1 00:00:00 1970\n";
    time_t epoch = 0;
    struct tm tm;
    char buf[32];

    gmtime_r(&epoch, &tm);
    check(asctime_r(&tm, buf) == buf && !strcmp(buf, epoch_str),
          "asctime_r(epoch)", "expected the epoch date string");
    check(ctime_r(&epoch, buf) == buf && !strcmp(buf, epoch_str),
          "ctime_r(epoch)", "expected the epoch date string");
    check(asctime_r(NULL, buf) == NULL && errno == EINVAL,
          "asctime_r(NULL tm)", "expected EINVAL");
    check(ctime_r(NULL, buf) == NULL && errno == EINVAL, "ctime_r(NULL time)",
          "expected EINVAL");
    check(!strcmp(asctime(&tm), epoch_str), "asctime(epoch)",
          "the static-buffer form yields the same string");
    check(!strcmp(ctime(&epoch), epoch_str), "ctime(epoch)",
          "the static-buffer form yields the same string");

    /* An out-of-range weekday is EINVAL and must not write the caller buffer.
     */
    struct tm bad = tm;
    char canary[32];
    bad.tm_wday = 9;
    memset(canary, '#', sizeof canary);
    check(asctime_r(&bad, canary) == NULL && errno == EINVAL && canary[0] == '#',
          "asctime_r(bad wday)", "invalid weekday must fail without writing buf");

    /* A year whose string would exceed 26 bytes overflows rather than
     * truncates.
     */
    struct tm big = tm;
    big.tm_year = 8000000;
    check(asctime_r(&big, buf) == NULL && errno == EOVERFLOW,
          "asctime_r(huge year)", "output over 26 bytes must return EOVERFLOW");
}

/* POSIX message queues (shm-backed): the queue-lifecycle slice — mq_open
 * create/exclusive/open, mq_close, mq_unlink, and the name/oflag edges.
 */
static void test_mqueue_lifecycle(void)
{
    const char *name = "/rtmux-mqtest";
    struct mq_attr attr = {.mq_maxmsg = 8, .mq_msgsize = 64};
    mqd_t mq, mq2;

    (void) mq_unlink(name); /* clear any stale queue */

    check(mq_open("bad-name", O_CREAT | O_RDWR, 0600, &attr) == (mqd_t) -1 &&
              errno == EINVAL,
          "mq_open(no leading slash)", "expected EINVAL");

    mq = mq_open(name, O_CREAT | O_RDWR, 0600, &attr);
    check(mq != (mqd_t) -1, "mq_open(O_CREAT)", "creating a queue must succeed");

    check(mq_open(name, O_CREAT | O_EXCL | O_RDWR, 0600, &attr) == (mqd_t) -1 &&
              errno == EEXIST,
          "mq_open(O_CREAT|O_EXCL existing)", "expected EEXIST");

    mq2 = mq_open(name, O_RDWR);
    check(mq2 != (mqd_t) -1, "mq_open(existing)", "opening an existing queue must succeed");

    check(mq_open("/rtmux-mq-absent", O_RDWR) == (mqd_t) -1 && errno == ENOENT,
          "mq_open(absent, no O_CREAT)", "expected ENOENT");

    check(mq_close(mq) == 0 && mq_close(mq2) == 0, "mq_close", "expected 0");
    check(mq_unlink(name) == 0, "mq_unlink", "expected 0");
    check(mq_open(name, O_RDWR) == (mqd_t) -1 && errno == ENOENT,
          "mq_open after unlink", "an unlinked queue is gone");
}

/* mq_send / mq_receive priority ordering, mq_getattr/mq_setattr, and the
 * EMSGSIZE / EAGAIN edges (single process; blocking is a later slice).
 */
static void test_mqueue_transfer(void)
{
    const char *name = "/rtmux-mqxfer";
    struct mq_attr attr = {.mq_maxmsg = 4, .mq_msgsize = 32};
    struct mq_attr got;
    char buf[32];
    unsigned prio;
    mqd_t mq;

    (void) mq_unlink(name);
    mq = mq_open(name, O_CREAT | O_RDWR, 0600, &attr);
    if (mq == (mqd_t) -1) {
        test_fail("mqxfer open", strerror(errno));
        return;
    }

    check(mq_send(mq, "lo", 2, 1) == 0 && mq_send(mq, "hi", 2, 3) == 0 &&
              mq_send(mq, "mid", 3, 2) == 0,
          "mq_send three priorities", "sends must succeed");

    check(mq_getattr(mq, &got) == 0 && got.mq_curmsgs == 3 &&
              got.mq_maxmsg == 4 && got.mq_msgsize == 32,
          "mq_getattr after sends", "curmsgs and limits must be reported");

    /* Highest priority first, FIFO within a priority. */
    prio = 99;
    check(mq_receive(mq, buf, sizeof buf, &prio) == 2 && prio == 3 &&
              memcmp(buf, "hi", 2) == 0,
          "mq_receive highest priority", "prio 3 message comes out first");
    check(mq_receive(mq, buf, sizeof buf, &prio) == 3 && prio == 2 &&
              memcmp(buf, "mid", 3) == 0,
          "mq_receive next priority", "prio 2 message is second");
    check(mq_receive(mq, buf, sizeof buf, &prio) == 2 && prio == 1 &&
              memcmp(buf, "lo", 2) == 0,
          "mq_receive lowest priority", "prio 1 message is last");

    /* Switch to non-blocking before the empty-queue edges: on a blocking queue
     * mq_receive on empty would block forever (POSIX), so the EAGAIN checks
     * below require O_NONBLOCK. mq_setattr only changes mq_flags.
     */
    attr.mq_flags = O_NONBLOCK;
    check(mq_setattr(mq, &attr, &got) == 0 && got.mq_flags == 0,
          "mq_setattr returns old flags", "old mq_flags were 0");
    check(mq_getattr(mq, &got) == 0 && (got.mq_flags & O_NONBLOCK),
          "mq_setattr sets O_NONBLOCK", "the flag must persist");

    check(mq_receive(mq, buf, sizeof buf, &prio) == -1 && errno == EAGAIN,
          "mq_receive on empty", "an empty non-blocking queue returns EAGAIN");

    check(mq_send(mq, buf, 33, 0) == -1 && errno == EMSGSIZE,
          "mq_send oversize", "len > mq_msgsize must return EMSGSIZE");
    check(mq_receive(mq, buf, 8, &prio) == -1 && errno == EMSGSIZE,
          "mq_receive undersize buffer", "buf < mq_msgsize must return EMSGSIZE");
    check(mq_send(mq, "x", 1, 100u) == -1 && errno == EINVAL,
          "mq_send bad priority", "prio >= MQ_PRIO_MAX (32) must return EINVAL");
    check(mq_send(mq, NULL, 4, 0) == -1 && errno == EFAULT,
          "mq_send NULL buffer", "a NULL message with non-zero length is EFAULT");
    check(mq_getattr((mqd_t) -1, &got) == -1,
          "mq_getattr bad descriptor", "a stray descriptor must fail");

    check(mq_close(mq) == 0 && mq_unlink(name) == 0, "mqxfer cleanup",
          "expected 0");
}

/* Slice 3: per-descriptor access mode (EBADF), O_NONBLOCK (EAGAIN on a full
 * queue), and the timed variants (ETIMEDOUT past the deadline).
 */
static void test_mqueue_blocking(void)
{
    const char *name = "/rtmux-mqblock";
    struct mq_attr attr = {.mq_maxmsg = 1, .mq_msgsize = 16};
    struct mq_attr nb = {.mq_flags = O_NONBLOCK};
    struct timespec ts;
    char buf[16];
    unsigned prio = 0;
    mqd_t rw, ro, wo;

    (void) mq_unlink(name);
    rw = mq_open(name, O_CREAT | O_RDWR, 0600, &attr);
    if (rw == (mqd_t) -1) {
        test_fail("mqblock open", strerror(errno));
        return;
    }
    ro = mq_open(name, O_RDONLY);
    wo = mq_open(name, O_WRONLY);
    if (ro == (mqd_t) -1 || wo == (mqd_t) -1) {
        test_fail("mqblock reopen", strerror(errno));
        (void) mq_close(rw);
        (void) mq_unlink(name);
        return;
    }

    /* Access mode belongs to the descriptor, not the shared queue. */
    check(mq_send(ro, "x", 1, 0) == -1 && errno == EBADF,
          "mq_send on O_RDONLY", "sending on a read-only handle is EBADF");
    check(mq_receive(wo, buf, sizeof buf, NULL) == -1 && errno == EBADF,
          "mq_receive on O_WRONLY", "receiving on a write-only handle is EBADF");

    /* O_NONBLOCK on a full queue returns EAGAIN instead of blocking. */
    check(mq_setattr(wo, &nb, NULL) == 0, "mqblock set O_NONBLOCK",
          "setattr must succeed");
    check(mq_send(wo, "a", 1, 0) == 0, "mqblock fill", "one slot fills");
    check(mq_send(wo, "b", 1, 0) == -1 && errno == EAGAIN,
          "mq_send full O_NONBLOCK", "a full non-blocking queue returns EAGAIN");

    /* Drain, then a timed receive on the empty queue hits the deadline. */
    check(mq_receive(rw, buf, sizeof buf, &prio) == 1 && buf[0] == 'a',
          "mqblock drain", "the queued message comes back");
    check(clock_gettime(CLOCK_REALTIME, &ts) == 0, "mqblock clock",
          "clock_gettime must work");
    ts.tv_nsec += 20 * 1000 * 1000; /* 20 ms deadline */
    if (ts.tv_nsec >= 1000000000L) {
        ts.tv_sec++;
        ts.tv_nsec -= 1000000000L;
    }
    check(mq_timedreceive(ro, buf, sizeof buf, NULL, &ts) == -1 &&
              errno == ETIMEDOUT,
          "mq_timedreceive empty", "an empty queue past the deadline is ETIMEDOUT");

    /* A timed send with room succeeds regardless of the (possibly elapsed)
     * deadline, and the message is received back.
     */
    check(mq_timedsend(rw, "z", 1, 0, &ts) == 0, "mq_timedsend room",
          "space available means the deadline is not consulted");
    check(mq_receive(rw, buf, sizeof buf, NULL) == 1 && buf[0] == 'z',
          "mq_timedsend delivered", "the timed-sent message is received");

    check(mq_close(rw) == 0 && mq_close(ro) == 0 && mq_close(wo) == 0 &&
              mq_unlink(name) == 0,
          "mqblock cleanup", "expected 0");
}

/* fd-relative *at: a descriptor opened with an absolute path resolves relative
 * paths for openat/fstatat/faccessat/readlinkat; error precedence holds.
 */
static void check_spawn_inherited_atfd(const char *dirpath,
                                       const char *relpath,
                                       const char *name)
{
    char fdarg[16];
    char *argv[] = {
        "syscall_testing",
        "--expect-inherited-atfd",
        fdarg,
        (char *) relpath,
        NULL,
    };
    pid_t pid = (pid_t) -1;
    int status;
    int rc;
    int dfd;

    dfd = open(dirpath, O_RDONLY);
    if (dfd < 0) {
        test_fail(name, strerror(errno));
        return;
    }

    (void) snprintf(fdarg, sizeof fdarg, "%d", dfd);
    rc = posix_spawn(&pid, "/bin/syscall_testing", NULL, NULL, argv, NULL);
    check(rc == EOK && pid > 0, name,
          "an exec'd child must resolve relative paths from an inherited dirfd");
    if (rc == EOK && pid > 0) {
        check(waitpid(pid, &status, 0) == pid && WIFEXITED(status) &&
                  WEXITSTATUS(status) == 0,
              "spawn inherited dirfd child",
              "child openat/fstatat/faccessat must resolve through the dirfd");
    }
    (void) close(dfd);
}

static void test_posix_at_fd_relative(void)
{
    struct stat st;
    int dfd, fd;

    dfd = open("/sys", O_RDONLY);
    if (dfd < 0) {
        test_fail("at dirfd fixture", strerror(errno));
        return;
    }

    fd = openat(dfd, "namespace-stats", O_RDONLY);
    check(fd >= 0, "openat resolves relative to dirfd",
          "openat(dfd, \"namespace-stats\") should open /sys/namespace-stats");
    if (fd >= 0)
        (void) close(fd);

    check(fstatat(dfd, "namespace-stats", &st, 0) == 0,
          "fstatat resolves relative to dirfd",
          "fstatat(dfd, \"namespace-stats\") should stat the file");

    check(faccessat(dfd, "namespace-stats", F_OK, 0) == 0,
          "faccessat resolves relative to dirfd",
          "faccessat(dfd, \"namespace-stats\", F_OK) should succeed");

    check_spawn_inherited_atfd("/sys", "namespace-stats",
                               "spawn inherits sysfs dirfd for *at");
    check_spawn_inherited_atfd("/bin", "true",
                               "spawn inherits cpiofs dirfd for *at");
    check_spawn_inherited_atfd("/cap", "self",
                               "spawn inherits capfs dirfd for *at");
    check_spawn_inherited_atfd("/proc", "self/name",
                               "spawn inherits procfs dirfd for *at");
    {
        const char *path = "/dev/shmem/atfd-inherit";

        fd = open(path, O_CREAT | O_RDWR, 0600);
        if (fd >= 0) {
            (void) close(fd);
            check_spawn_inherited_atfd("/dev/shmem", "atfd-inherit",
                                       "spawn inherits shmem dirfd for *at");
            (void) unlink(path);
        } else {
            test_fail("at shmem dirfd fixture", strerror(errno));
        }
    }
    check_spawn_inherited_atfd("/dev/sem", ".",
                               "spawn inherits namedsem dirfd for *at");

    check(openat(dfd, "no-such-entry", O_RDONLY) == -1 && errno == ENOENT,
          "openat relative missing -> ENOENT",
          "a missing relative name must fail with ENOENT after resolution");

    /* A dup'd dirfd inherits the tracked path. */
    fd = dup(dfd);
    if (fd >= 0) {
        check(fstatat(fd, "namespace-stats", &st, 0) == 0,
              "fstatat via dup'd dirfd",
              "a dup of a tracked dirfd resolves relative paths too");
        (void) close(fd);
    }

    (void) close(dfd);

    /* A relative *at on an invalid descriptor -> EBADF. */
    check(openat(4095, "x", O_RDONLY) == -1 && errno == EBADF,
          "openat bad dirfd -> EBADF",
          "a relative openat on an invalid descriptor must fail EBADF");

    /* A relative *at on a non-directory descriptor -> ENOTDIR. */
    fd = open("/sys/namespace-stats", O_RDONLY);
    if (fd >= 0) {
        check(fstatat(fd, "x", &st, 0) == -1 && errno == ENOTDIR,
              "fstatat non-dir dirfd -> ENOTDIR",
              "a relative *at on a file descriptor must fail ENOTDIR");
        (void) close(fd);
    }

    /* A dirfd opened with a RELATIVE path is resolved against the cwd, so it
     * too backs fd-relative *at.
     */
    {
        char saved[PATH_MAX + 1];

        if (getcwd(saved, sizeof saved) && chdir("/") == 0) {
            dfd = open("sys", O_RDONLY);
            if (dfd >= 0) {
                check(fstatat(dfd, "namespace-stats", &st, 0) == 0,
                      "fstatat via relative-opened dirfd",
                      "a dirfd opened with a relative path resolves *at too");
                (void) close(dfd);
            }
            (void) chdir(saved);
        }
    }
}

/* Q32: concurrent posix_spawn with exact argv/envp delivery. Each worker owns
 * one close-on-exec report pipe created before any thread starts and kept open
 * for the whole run: libc lists every open non-CLOEXEC descriptor and roottask
 * validates the list afterwards, so a sibling's close() in between would
 * legitimately fail the spawn with EBADF.
 */
enum { SPAWN_WORKERS = 8, SPAWN_ROUNDS = 100, SPAWN_REPORT_FD = 3 };
enum { SPAWN_RESIDENTS_MAX = 256 };

struct spawn_worker {
    pthread_t th;
    int pipefd[2];
    pthread_barrier_t *bar;
    int idx;
    int bad_spawn, bad_report, bad_exit;
    int spawn_err;   /* first posix_spawn error, for the failure detail */
    int spawn_round; /* and the round it failed in */
    /* The first bad report: its round, whether the read itself failed, and what
     * was read or why the read failed, printed when the check fails.
     */
    int bad_round;
    bool bad_read;
    char bad_line[64];
    /* Progress, read by the joining thread to name a stalled worker. */
    _Atomic int round, phase, done;
    pid_t pids[SPAWN_ROUNDS];
};

int spawn_echo_child(const char *tag)
{
    const char *env = getenv("SPAWN_TAG");
    char buf[64];
    int n;

    if (!env || strcmp(env, tag))
        return EXIT_FAILURE;
    n = snprintf(buf, sizeof buf, "%s %d\n", tag, (int) getpid());
    return write(SPAWN_REPORT_FD, buf, (size_t) n) == n ? EXIT_SUCCESS
                                                       : EXIT_FAILURE;
}

/* Read one report line. On failure buf holds why instead: which call failed,
 * with what, and how many bytes had arrived.
 */
static bool spawn_read_report(int fd, char *buf, size_t sz)
{
    size_t len = 0;

    while (len + 1 < sz) {
        struct pollfd pfd = {.fd = fd, .events = POLLIN};
        ssize_t n;
        int pr = poll(&pfd, 1, 2000);

        if (pr != 1) {
            snprintf(buf, sz, "poll %d (%s) after %zu bytes", pr,
                     pr ? strerror(errno) : "timeout", len);
            return false;
        }
        n = read(fd, buf + len, sz - 1 - len);
        if (n <= 0) {
            snprintf(buf, sz, "read %zd (%s) revents %#x after %zu bytes", n,
                     n ? strerror(errno) : "EOF", (unsigned) pfd.revents, len);
            return false;
        }
        len += (size_t) n;
        if (buf[len - 1] == '\n')
            break;
    }
    buf[len] = '\0';
    return len > 0 && buf[len - 1] == '\n';
}

static void *spawn_worker_body(void *arg)
{
    struct spawn_worker *w = arg;

    (void) pthread_barrier_wait(w->bar);
    for (int r = 0; r < SPAWN_ROUNDS; r++) {
        posix_spawn_file_actions_t fa;
        char tag[32], env[48], line[64];
        char *argv[] = {"syscall_testing", "--spawn-echo-child", tag, NULL};
        char *envp[] = {env, NULL};
        pid_t pid = -1;
        int status = -1;

        /* Clear this round's pid before publishing the round, so a reader that
         * sees the new round never takes a stale pid for it.
         */
        __atomic_store_n(&w->pids[r], -1, __ATOMIC_RELAXED);
        w->round = r;
        w->phase = 1; /* spawning */
        snprintf(tag, sizeof tag, "%d:%d", w->idx, r);
        snprintf(env, sizeof env, "SPAWN_TAG=%s", tag);
        int rc = posix_spawn_file_actions_init(&fa);

        if (!rc) {
            rc = posix_spawn_file_actions_adddup2(&fa, w->pipefd[1], SPAWN_REPORT_FD);
            if (!rc)
                rc = posix_spawn(&pid, "/bin/syscall_testing", &fa, NULL, argv, envp);
            (void) posix_spawn_file_actions_destroy(&fa);
        }
        if (rc) {
            w->bad_spawn++;
            if (!w->spawn_err) {
                w->spawn_err = rc;
                w->spawn_round = r;
            }
            continue;
        }
        __atomic_store_n(&w->pids[r], pid, __ATOMIC_RELAXED);
        w->phase = 2; /* reaping */
        /* Reap first: the child writes its line before it exits, so the line is
         * in the pipe by then however slowly the child ran. Reading first gave
         * up after the poll timeout on a loaded hart and then took the late
         * line as the next round's report.
         */
        if (waitpid(pid, &status, 0) != pid || !WIFEXITED(status) ||
            WEXITSTATUS(status) != 0)
            w->bad_exit++;
        w->phase = 3; /* reading the report */
        bool read_ok = spawn_read_report(w->pipefd[0], line, sizeof line);

        if (!read_ok || strncmp(line, tag, strlen(tag)) ||
            line[strlen(tag)] != ' ' ||
            strtol(line + strlen(tag) + 1, NULL, 10) != (long) pid) {
            if (!w->bad_report++) {
                w->bad_round = r;
                w->bad_read = !read_ok;
                snprintf(w->bad_line, sizeof w->bad_line, "%s", line);
                w->bad_line[strcspn(w->bad_line, "\n")] = '\0';
            }
        }
    }
    w->done = 1;
    return NULL;
}

enum { SPAWN_STALL_S = 30 }; /* seconds without progress that count as a stall */

/* Print every thread of pid with its kernel state, priority and policy, the
 * sync object it waits on, its last hart and how long it has been blocked. With
 * busy_only, skip threads waiting for a message, so a server's dump shows only
 * the threads doing or waiting on something. Run on a stall or failure only.
 */
static void spawn_dump_threads(pid_t pid, bool busy_only)
{
    char path[32];
    procfs_info info;
    int fd;

    snprintf(path, sizeof path, "/proc/%d", (int) pid);
    if ((fd = open(path, O_RDONLY)) < 0) {
        printf("  pid %d: %s\n", (int) pid, strerror(errno));
        return;
    }
    memset(&info, 0, sizeof info);
    if (devctl(fd, DEVCTL_PROC_INFO, &info, sizeof info, NULL) == EOK)
        printf("  pid %d: flags %#x threads %u parent %d\n", (int) pid,
               info.flags, info.num_threads, (int) info.parent);
    for (int tid = 1, n = 0; n < 64; tid++, n++) {
        procfs_status st;

        memset(&st, 0, sizeof st);
        st.tid = tid;
        if (devctl(fd, DEVCTL_PROC_TIDSTATUS, &st, sizeof st, NULL) != EOK ||
            (int) st.tid < tid)
            break;
        tid = (int) st.tid;
        if (busy_only && st.state == STATE_RECEIVE)
            continue;
        printf("  pid %d tid %d state %u prio %u/%u pol %u sync %#lx cpu %u "
               "blocked %llu ms\n",
               (int) pid, tid, st.state, st.priority, st.real_priority, st.policy,
               (unsigned long) st.blocked.sync.sync, st.last_cpu,
               (unsigned long long) (st.nsec_since_block / 1000000));
    }
    (void) close(fd);
}

/* The child a worker is reaping or reading from, or -1. The worker may be
 * moving on as this runs, so every field is loaded atomically.
 */
static pid_t spawn_cur_pid(const struct spawn_worker *w)
{
    /* Phase before round: a phase of 2 then belongs to this round or an earlier
     * one, whose pid is already stored, and a newer round reads the -1 its
     * worker stored before publishing it.
     */
    int phase = w->phase;
    int round = w->round;

    return phase >= 2 ? __atomic_load_n(&w->pids[round], __ATOMIC_RELAXED) : -1;
}

static void spawn_dump_stall(const struct spawn_worker *w)
{
    const pthread_barrier_t *b = w[0].bar;

    /* A stalled worker may still wake and write these: load them atomically. */
    printf("  barrier %p: lock owner %#lx count %#lx, cond owner %#lx, "
           "waiting %u gen %lu\n",
           (const void *) b,
           (unsigned long) __atomic_load_n(&b->lock.owner, __ATOMIC_RELAXED),
           (unsigned long) __atomic_load_n(&b->lock.count, __ATOMIC_RELAXED),
           (unsigned long) __atomic_load_n(&b->cond.owner, __ATOMIC_RELAXED),
           __atomic_load_n(&b->waiting, __ATOMIC_RELAXED),
           __atomic_load_n(&b->generation, __ATOMIC_RELAXED));
    spawn_dump_threads(getpid(), false);
    for (int i = 0; i < SPAWN_WORKERS; i++) {
        pid_t pid = spawn_cur_pid(&w[i]); /* read once: the worker moves on */

        if (!w[i].done && pid > 0)
            spawn_dump_threads(pid, false);
    }
    spawn_dump_threads(ROOTTASK_PID, false);
}

/* Join the team, naming every worker's round and phase when none has moved for
 * SPAWN_STALL_S seconds, so a hang says whether it is a spawn, a reap or a
 * read.
 */
static void spawn_join(struct spawn_worker *w)
{
    static const char *const phase[] = {"start", "spawn", "reap", "read"};
    long last = -1;
    int still = 0;
    bool dumped = false; /* thread dump printed for the current stall */

    for (;;) {
        long sum = 0;
        int done = 0;

        for (int i = 0; i < SPAWN_WORKERS; i++) {
            sum += w[i].round * 4 + w[i].phase;
            done += w[i].done;
        }
        if (done == SPAWN_WORKERS)
            break;
        still = sum == last ? still + 1 : 0;
        if (!still)
            dumped = false;
        last = sum;
        if (still == SPAWN_STALL_S) {
            still = 0;
            printf("spawn_concurrent: no progress for %d s:", SPAWN_STALL_S);
            for (int i = 0; i < SPAWN_WORKERS; i++)
                printf(" w%d=%d:%s%s pid=%d", i, w[i].round, phase[w[i].phase & 3],
                       w[i].done ? "(done)" : "",
                       (int) spawn_cur_pid(&w[i]));
            printf("\n");
            if (!dumped)
                spawn_dump_stall(w);
            dumped = true;
        }
        sleep(1);
    }
    for (int i = 0; i < SPAWN_WORKERS; i++)
        (void) pthread_join(w[i].th, NULL);
}

static int spawn_count_dup_pids(const struct spawn_worker *w)
{
    int dup = 0;

    for (int a = 0; a < SPAWN_WORKERS * SPAWN_ROUNDS; a++)
        for (int b = a + 1; b < SPAWN_WORKERS * SPAWN_ROUNDS; b++) {
            pid_t pa = w[a / SPAWN_ROUNDS].pids[a % SPAWN_ROUNDS];
            pid_t pb = w[b / SPAWN_ROUNDS].pids[b % SPAWN_ROUNDS];

            if (pa > 0 && pa == pb)
                dup++;
        }
    return dup;
}

/* Print roottask's heap use and the free physical memory after a failed run, so
 * an ENOMEM spawn shows which of the two ran short.
 */
static void spawn_concurrent_report_memory(void)
{
    char snap[4096];
    int fd = open("/sys/kstats", O_RDONLY);

    if (fd < 0)
        return;
    if (kstats_read(fd, snap, sizeof snap) > 0)
        printf("spawn_concurrent: roottask.heap_used=%ld pa.free_bytes=%ld\n",
               kstats_get(snap, "roottask.heap_used"),
               kstats_get(snap, "pa.free_bytes"));
    (void) close(fd);
}

void test_spawn_concurrent(void)
{
    static struct spawn_worker w[SPAWN_WORKERS];
    pthread_barrier_t bar;
    unsigned fds_before, fds_after;
    int piped = 0, created = 0, bad_spawn = 0, bad_report = 0, bad_exit = 0;
    int dup_pid, spawn_err = 0;
    char detail[160];

    /* Static for its size, so clear it: --laps runs the test again. */
    memset(w, 0, sizeof w);
    fds_before = spawn_count_open_fds();
    if (pthread_barrier_init(&bar, NULL, SPAWN_WORKERS) != 0) {
        test_fail("spawn_concurrent fixture", strerror(errno));
        return;
    }
    for (int i = 0; i < SPAWN_WORKERS; i++, piped++) {
        w[i].idx = i;
        w[i].bar = &bar;
        if (pipe(w[i].pipefd) != 0)
            break;
        (void) fcntl(w[i].pipefd[0], F_SETFD, FD_CLOEXEC);
        (void) fcntl(w[i].pipefd[1], F_SETFD, FD_CLOEXEC);
    }
    if (piped == SPAWN_WORKERS)
        for (; created < SPAWN_WORKERS; created++)
            if (pthread_create(&w[created].th, NULL, spawn_worker_body,
                               &w[created]) != 0)
                break;
    if (created != SPAWN_WORKERS)
        test_fail("spawn_concurrent fixture", strerror(errno));
    /* An incomplete team never releases the barrier: do not join it. */
    if (created == SPAWN_WORKERS)
        spawn_join(w);
    for (int i = 0; i < piped && created == SPAWN_WORKERS; i++) {
        bad_spawn += w[i].bad_spawn;
        bad_report += w[i].bad_report;
        bad_exit += w[i].bad_exit;
        if (!spawn_err)
            spawn_err = w[i].spawn_err;
    }
    dup_pid = created == SPAWN_WORKERS ? spawn_count_dup_pids(w) : 0;
    if (created == SPAWN_WORKERS) {
        for (int i = 0; i < piped; i++) {
            (void) close(w[i].pipefd[0]);
            (void) close(w[i].pipefd[1]);
        }
        (void) pthread_barrier_destroy(&bar);
        fds_after = spawn_count_open_fds();
        snprintf(detail, sizeof detail,
                 "spawn=%d (%s) report=%d exit=%d dup_pid=%d fds %u->%u", bad_spawn,
                 spawn_err ? strerror(spawn_err) : "-", bad_report, bad_exit, dup_pid,
                 fds_before, fds_after);
        for (int i = 0; i < piped && bad_spawn; i++)
            if (w[i].bad_spawn)
                printf("spawn_concurrent: w%d %d failed spawn(s), first in round "
                       "%d: %s\n",
                       i, w[i].bad_spawn, w[i].spawn_round,
                       strerror(w[i].spawn_err));
        if (bad_spawn || bad_report || bad_exit)
            spawn_concurrent_report_memory();
        for (int i = 0; i < piped && bad_report; i++)
            if (w[i].bad_report)
                printf("spawn_concurrent: w%d %d bad report(s), first in round "
                       "%d: %s \"%s\" (pid %d)\n",
                       i, w[i].bad_report, w[i].bad_round,
                       w[i].bad_read ? "read failed" : "read", w[i].bad_line,
                       (int) w[i].pids[w[i].bad_round]);
        check(!bad_spawn && !bad_report && !bad_exit && !dup_pid &&
                  fds_before == fds_after,
              "spawn_concurrent 8x100 exact argv/envp, unique pids, no fd leak",
              detail);

        /* After the verdict, so the worker detail above stays next to it: the
         * roottask threads not idle in receive, which shows a stuck cleaner and
         * whatever it waits on.
         */
        if (bad_spawn || bad_report || bad_exit)
            spawn_dump_threads(ROOTTASK_PID, true);
    }
}

/* Print roottask's heap use and the free physical memory with "residents" idle
 * processes alive, so a run shows how close the storm came to exhausting either
 * (K26).
 */
static void spawn_storm_report_memory(unsigned residents)
{
    char snap[4096];
    int fd = open("/sys/kstats", O_RDONLY);

    if (fd < 0)
        return;
    if (kstats_read(fd, snap, sizeof snap) > 0)
        printf("spawn_storm: residents=%u roottask.heap_used=%ld pa.free_bytes=%ld\n",
               residents, kstats_get(snap, "roottask.heap_used"),
               kstats_get(snap, "pa.free_bytes"));
    (void) close(fd);
}

/* The middle process of the orphan test: leave a grandchild blocked on stdin,
 * report its pid on stdout, and exit before it. Stdin is the gate the test
 * closes, stdout the report pipe.
 */
int orphan_exit_mid(void)
{
    posix_spawn_file_actions_t fa;
    char *argv[] = {"cat", NULL};
    char *envp[] = {NULL};
    char buf[32];
    pid_t g;
    int n, rc;

    /* The grandchild keeps neither the report pipe nor stdout. */
    rc = posix_spawn_file_actions_init(&fa);
    if (rc)
        return EXIT_FAILURE;
    rc = posix_spawn_file_actions_addclose(&fa, SPAWN_REPORT_FD);
    if (!rc)
        rc = posix_spawn_file_actions_addopen(&fa, STDOUT_FILENO, "/dev/null",
                                              O_WRONLY, 0);
    if (!rc)
        rc = posix_spawn(&g, "/bin/cat", &fa, NULL, argv, envp);
    (void) posix_spawn_file_actions_destroy(&fa);
    if (rc)
        return EXIT_FAILURE;
    n = snprintf(buf, sizeof buf, "%d\n", (int) g);
    return write(SPAWN_REPORT_FD, buf, (size_t) n) == n ? EXIT_SUCCESS
                                                      : EXIT_FAILURE;
}

/* True if procfs lists pid. */
static bool proc_listed(pid_t pid)
{
    char path[32];
    int fd;

    snprintf(path, sizeof path, "/proc/%d", (int) pid);
    if ((fd = open(path, O_RDONLY)) < 0)
        return false;
    (void) close(fd);
    return true;
}

/* A child that outlives its parent must still finish exiting. Roottask kept
 * the parent pid recorded at spawn, so once that parent was gone the orphan's
 * exit retried for ever to lock it, holding a pool thread and never leaving the
 * process table.
 */
void test_orphan_exit(void)
{
    char *argv[] = {"syscall_testing", "--orphan-exit-mid", NULL};
    posix_spawn_file_actions_t fa;
    int gate[2] = {-1, -1}, rep[2] = {-1, -1}, status = 0, rc, gone = 0;
    char line[32];
    pid_t mid = -1, g = -1;
    bool reaped = false;

    /* Only the middle process and its grandchild may hold the gate's read end,
     * and only the test its write end, or EOF would never come.
     */
    rc = pipe(gate) != 0 || pipe(rep) != 0;
    for (int i = 0; !rc && i < 2; i++)
        rc = fcntl(gate[i], F_SETFD, FD_CLOEXEC) != 0 ||
             fcntl(rep[i], F_SETFD, FD_CLOEXEC) != 0;
    if (rc) {
        test_fail("orphan_exit fixture", strerror(errno));
        goto out;
    }
    rc = posix_spawn_file_actions_init(&fa);
    if (!rc) {
        rc = posix_spawn_file_actions_adddup2(&fa, gate[0], STDIN_FILENO);
        if (!rc)
            rc = posix_spawn_file_actions_adddup2(&fa, rep[1], SPAWN_REPORT_FD);
        if (!rc)
            rc = posix_spawn(&mid, "/bin/syscall_testing", &fa, NULL, argv, NULL);
        (void) posix_spawn_file_actions_destroy(&fa);
    }
    (void) close(gate[0]);
    gate[0] = -1;
    (void) close(rep[1]);
    rep[1] = -1;
    if (rc) {
        test_fail("orphan_exit spawn", strerror(rc));
        goto out;
    }
    if (!spawn_read_report(rep[0], line, sizeof line) ||
        (g = (pid_t) strtol(line, NULL, 10)) <= 0) {
        test_fail("orphan_exit fixture", "middle process did not report");
        goto out;
    }
    reaped = waitpid(mid, &status, 0) == mid;
    if (!reaped || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        test_fail("orphan_exit fixture", "middle process did not exit cleanly");
        goto out;
    }
    if (!proc_listed(g)) {
        test_fail("orphan_exit fixture", "the orphan is not in /proc");
        goto out;
    }
    (void) close(gate[1]); /* EOF: the orphan reads it and exits */
    gate[1] = -1;
    /* kill() already says ESRCH once a process is exiting: ask procfs, which
     * lists the orphan until roottask has destroyed it.
     */
    for (int i = 0; i < 100 && !gone; i++) {
        gone = !proc_listed(g);
        if (!gone)
            sleep_ms(50);
    }
    check(gone, "orphan_exit a child whose parent is gone finishes exiting",
          "the orphan stayed in the process table");
    if (!gone)
        (void) kill(g, SIGKILL);
out:
    if (mid > 0 && !reaped) {
        (void) kill(mid, SIGKILL);
        (void) waitpid(mid, &status, 0);
    }
    for (int i = 0; i < 2; i++) {
        if (gate[i] >= 0)
            (void) close(gate[i]);
        if (rep[i] >= 0)
            (void) close(rep[i]);
    }
}

#define SPAWN_HEAP_ROUNDS 2

/* Run the spawn storm with residents idle processes alive across it: each is
 * /bin/cat (the small toolbox binary) blocked reading a pipe until EOF. The
 * kernel's pshut and irqoff lines (CONFIG_INSTRUMENTATION) printed before the
 * "storm done" marker cover the storm's exits at that process count.
 */
void test_spawn_storm(unsigned residents)
{
    static pid_t pids[SPAWN_RESIDENTS_MAX];
    char *argv[] = {"cat", NULL};
    char *envp[] = {NULL};
    unsigned spawned = 0, bad_exit = 0;
    char detail[96];
    int p[2];

    if (residents > SPAWN_RESIDENTS_MAX || pipe(p) != 0) {
        test_fail("spawn_storm residents fixture",
                  residents > SPAWN_RESIDENTS_MAX ? "too many" : strerror(errno));
        return;
    }
    /* A resident that kept the write end open would never see EOF. */
    if (fcntl(p[0], F_SETFD, FD_CLOEXEC) != 0 || fcntl(p[1], F_SETFD, FD_CLOEXEC) != 0) {
        test_fail("spawn_storm residents fixture", strerror(errno));
        (void) close(p[0]);
        (void) close(p[1]);
        return;
    }
    spawn_storm_report_memory(0);
    for (; spawned < residents; spawned++) {
        posix_spawn_file_actions_t fa;

        if (spawned && spawned % 50 == 0)
            spawn_storm_report_memory(spawned);
        int rc = posix_spawn_file_actions_init(&fa);

        if (!rc) {
            rc = posix_spawn_file_actions_adddup2(&fa, p[0], STDIN_FILENO);
            if (!rc)
                rc = posix_spawn_file_actions_addopen(&fa, STDOUT_FILENO, "/dev/null",
                                                      O_WRONLY, 0);
            if (!rc)
                rc = posix_spawn(&pids[spawned], "/bin/cat", &fa, NULL, argv, envp);
            (void) posix_spawn_file_actions_destroy(&fa);
        }
        if (rc) {
            snprintf(detail, sizeof detail, "resident %u: %s", spawned, strerror(rc));
            test_fail("spawn_storm residents", detail);
            break;
        }
    }
    printf("spawn_storm: residents=%u\n", spawned);
    spawn_storm_report_memory(spawned);
    if (spawned == residents) {
        /* Spawns with 32 and 64 KiB arguments after each concurrent storm:
         * the storm leaves roottask's fixed heap holding blocks of many
         * sizes, and these needed new backing the heap could no longer give
         * (K43).
         */
        for (int round = 0; round < SPAWN_HEAP_ROUNDS; round++) {
            test_spawn_concurrent();
            spawn_storm_report_memory(spawned);
            test_spawn_large_arguments();
            spawn_storm_report_memory(spawned);
        }
        sleep(2); /* let one more pshut/irqoff report land before the marker */
        printf("spawn_storm: storm done residents=%u\n", spawned);
    }
    (void) close(p[1]);
    (void) close(p[0]);
    for (unsigned i = 0; i < spawned; i++) {
        int status;

        if (waitpid(pids[i], &status, 0) != pids[i] || !WIFEXITED(status) ||
            WEXITSTATUS(status) != 0)
            bad_exit++;
    }
    if (residents) {
        snprintf(detail, sizeof detail, "%u of %u residents exited badly", bad_exit,
                 spawned);
        check(spawned == residents && !bad_exit, "spawn_storm residents spawn and exit",
              detail);
    }
}

void test_vfs(void)
{
    test_system_db_r();
    test_fcntl_dupfd_cloexec();
    test_dup_dup2();
    test_iodup_fd_lifetime();
    test_strerror_r();
    test_asctime_ctime_r();
    test_mqueue_lifecycle();
    test_mqueue_transfer();
    test_mqueue_blocking();
    test_pipe_read_write_transport();
    test_native_file_io();
    test_posix_io_size_overflow_guards();
    test_posix_memalign_contract();
    test_posix_umask_contract();
    test_posix_memlock_contract();
    test_posix_shm_contract();
    test_posix_stubs_fail_closed();
    test_statvfs_wrappers();
    test_nvme_negative_lseek();
    test_nvme_raw_perm();
    test_sys_kstats();
    test_sys_log_read();
    test_spawn_concurrent();
    test_orphan_exit();
    test_malloc_growth();
    test_posix_errno_corners();
    test_posix_access_and_path_corners();
    test_posix_creat();
    test_posix_unlinkat_contract();
    test_posix_opendir_wrong_type();
    test_posix_open_directory_write_is_eisdir();
    test_posix_open_namespace_directory_write_is_eisdir();
    test_posix_open_readonly_fs_write_is_erofs();
    test_posix_open_readonly_fs_create_precedence();
    test_posix_open_trailing_slash_non_directory();
    test_posix_open_romfs_write_semantics();
    test_posix_open_capfs_write_semantics();
    test_posix_open_creat_excl_existing();
    test_posix_fstat_file_types();
    test_posix_poll_readiness();
    test_posix_poll_null_devices();
    test_posix_poll_service_faults();
    test_posix_poll_blocking();
    test_posix_poll_hup();
    test_posix_select();
    test_posix_at_fd_relative();
    test_posix_pipe_access_mode();
    test_posix_lstat();
    test_posix_rewinddir();
    test_posix_readdir_r();
    test_posix_rand_r();
    test_posix_isatty();
    test_posix__exit();
    test_posix_crypt();
    /* Before the suite's own setsid: its children are then still in the session
     * the suite was started in, which for a boot-time starter is the system
     * session.
     */
    test_session_rule();
    test_posix_session_jobcontrol();
    test_posix_fsync();
    test_posix_sync();
    test_sync_waiter_cap();
    test_posix_sleep();
    test_posix_getopt();
    test_posix_exec_family();
    test_posix_credentials();
    test_posix_getlogin();
    test_posix_ttyname();
    test_posix_reentrant_helpers();
    test_posix_file_locking();
    test_posix_fcntl_flock_crossproc();
    test_posix_fcntl_flock_setlkw_block();
    test_posix_fpathconf_contract();
    test_posix_pathconf_contract();
    test_posix_confstr_contract();
    test_uname_matches_syspage();
    test_namespace_stats_sysfs();
    test_cap_self_introspection();
    test_cap_self_native_messages();
    test_endpoint_cap_lifecycle_handles();
    test_endpoint_single_consumer();
    test_logger_native_messages();
    test_namespace_version();
    test_namespace_open_race();
    test_namespace_open_entry_generation_race();
    test_namespace_uncacheable_bypass();
    test_memobj_metadata_devmem();
    test_memobj_metadata_typed();
    test_memobj_handle_backed_shared_mapping();
    test_memobj_handle_backed_typed_lifecycle();
    test_memobj_readonly_shared_write_denied();
    test_posix_msync_contract();
    test_memobj_stale_mapping_faults_after_munmap();
    test_ioserv_reply_status_taxonomy();
    test_ioserv_accounting_sysfs();
    test_ioserv_accounting_create_destroy();
    test_ioserv_accounting_admission_limit();
    test_namespace_symlink_loop_is_bounded();
    test_namespace_symlink_mutual_loop_is_bounded();
    test_transport_does_not_roundtrip_namespace();
    test_at_syscalls_at_fdcwd();
    test_namespace_sandbox_conformance();
    test_proc_fd_directory_shape();
    test_proc_fd_reopen();
    test_proc_fd_closed_descriptor_hidden();
    test_spawn_cloexec_fd_not_inherited();
    test_proc_wait_handle_shape();
    test_spawn_returns_wait_handle();
    test_spawn_missing_binary_is_unobservable();
    test_spawn_fault_reports_sigsegv();
    test_mprotect_readonly_rejects_write();
    test_spawn_file_actions_addopen();
    test_spawn_file_actions_close_dup2();
    test_spawn_malformed_messages_fail_closed();
    test_spawn_file_actions_close_dup2_spawn();
    test_spawnp_absolute_path();
    test_spawnattr_flags();
    test_spawnattr_resetids_runtime();
    test_spawnattr_setsid();
    test_spawn_file_actions_chdir();
    test_spawn_large_arguments();
    test_getsid_cases();
    test_pgrp_model();
    test_proc_lifecycle_burst_wait();
    test_proc_wait_handle_stale_after_reap();
    test_kill_zero_after_reap();
    test_named_channel_registry();
    test_name_unlink_open_race();
    test_named_endpoint_destroy_stress();
    test_endpoint_sender_identity_from_kernel();
    test_name_attach_rejects_unknown_flags();
    test_name_detach_savedpp_preserves_channel();
    test_pthread_null_key_destructor();
    test_pthread_cancel_contract();
    test_pthread_identity_and_tls();
    test_pthread_attr_setters();
}
