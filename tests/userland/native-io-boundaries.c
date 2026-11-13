/*
 * Native memory-file IPC boundary and malformed-message regressions.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/dispatch.h>
#include <sys/iomsg.h>
#include <sys/rtmux-core.h>
#include <sys/stat.h>

#include "test.h"

struct native_read_at {
    struct _io_read read;
    struct _xtype_offset offset;
};

struct native_write_at {
    struct _io_write write;
    struct _xtype_offset offset;
};

enum { NATIVE_BYTES = 64, NATIVE_START = 17, NATIVE_FUZZ_CASES = 4096 };
#define NATIVE_FUZZ_SEED 0x6d656d31U

static unsigned char native_data[NATIVE_BYTES];
static unsigned native_probes;

/* Build in aligned locals, then copy to the possibly unaligned wire address. */
static size_t native_header(void *wire,
                            int writing,
                            unsigned xtype,
                            uint64_t count,
                            int64_t offset)
{
    int positioned = (xtype & _IO_XTYPE_MASK) == _IO_XTYPE_OFFSET;

    if (writing) {
        struct native_write_at msg = {0};
        size_t length = positioned ? sizeof msg : sizeof msg.write;

        msg.write.type = _IO_WRITE;
        msg.write.combine_len = (uint16_t) length;
        msg.write.nbytes = (int32_t) (uint32_t) count;
        msg.write.xtype = xtype;
        msg.offset.offset = offset;
        memcpy(wire, &msg, length);
        return length;
    } else {
        struct native_read_at msg = {0};
        size_t length = positioned ? sizeof msg : sizeof msg.read;

        msg.read.type = _IO_READ;
        msg.read.combine_len = (uint16_t) length;
        msg.read.nbytes = (size_t) count;
        msg.read.xtype = (uint16_t) xtype;
        msg.offset.offset = offset;
        memcpy(wire, &msg, length);
        return length;
    }
}

static void native_frame(void *wire, unsigned length)
{
    uint16_t frame = (uint16_t) length;

    memcpy((unsigned char *) wire + sizeof(uint16_t), &frame, sizeof frame);
}

static int native_seek(int fd, uint64_t offset, int whence, uint64_t *position)
{
    io_lseek_t msg = {0};

    msg.i.type = _IO_LSEEK;
    msg.i.combine_len = sizeof msg.i;
    msg.i.whence = (int16_t) whence;
    msg.i.offset = offset;
    return msg_send(fd, &msg.i, sizeof msg.i, position, sizeof *position);
}

static int native_state(int fd, uint64_t position)
{
    unsigned char contents[NATIVE_BYTES];
    struct stat st;
    uint64_t actual;

    return native_seek(fd, 0, SEEK_CUR, &actual) == 0 && actual == position &&
           fstat(fd, &st) == 0 && st.st_size == NATIVE_BYTES &&
           pread(fd, contents, sizeof contents, 0) == sizeof contents &&
           memcmp(contents, native_data, sizeof contents) == 0;
}

/* Check observable results, the entire reply including canaries, and every
 * fixture byte. An incomplete type (0/1 bytes) can be rejected by dispatch,
 * before read/write is identifiable; error == -1 permits that routing error.
 */
static int native_probe(int fd,
                        const void *wire,
                        size_t length,
                        uint64_t start,
                        int result,
                        int error,
                        size_t reply_length,
                        int reply_offset,
                        uint64_t end,
                        char *detail,
                        size_t detail_size)
{
    unsigned char reply[16], expected[sizeof reply];
    uint64_t position;
    int rc, saved, reply_ok, state_ok, error_ok;

    memset(reply, 0xa5, sizeof reply);
    memcpy(expected, reply, sizeof expected);
    if (reply_offset >= 0 && result > 0)
        memcpy(expected, native_data + reply_offset, (size_t) result);
    if (native_seek(fd, start, SEEK_SET, &position) != 0 || position != start) {
        snprintf(detail, detail_size, "fixture seek failed: errno=%d", errno);
        return 0;
    }
    errno = 0;
    native_probes++;
    rc = msg_send(fd, wire, length, reply, reply_length);
    saved = errno;
    error_ok = result != -1 || saved == error ||
               (error == -1 &&
                (saved == EINVAL || saved == ENOSYS || saved == EBADMSG));
    reply_ok = memcmp(reply, expected, sizeof reply) == 0;
    state_ok = native_state(fd, end);
    if (rc == result && error_ok && reply_ok && state_ok)
        return 1;
    snprintf(detail, detail_size,
             "len=%zu rc=%d errno=%d expected=%d/%d reply=%d state=%d", length,
             rc, saved, result, error, reply_ok, state_ok);
    return 0;
}

static int native_reject(int fd,
                         const void *wire,
                         size_t length,
                         int error,
                         char *detail,
                         size_t size)
{
    return native_probe(fd, wire, length, NATIVE_START, -1, error, 8, -1,
                        NATIVE_START, detail, size);
}

static int native_zero(int fd,
                       const void *wire,
                       size_t length,
                       char *detail,
                       size_t size)
{
    return native_probe(fd, wire, length, NATIVE_START, 0, 0, 8, -1,
                        NATIVE_START, detail, size);
}

static void native_truncations(int fd)
{
    for (int writing = 0; writing <= 1; writing++) {
        for (int positioned = 0; positioned <= 1; positioned++) {
            for (int combined = 0; combined <= 1; combined++) {
                _Alignas(8) unsigned char wire[96] = {0};
                size_t prefix = combined ? sizeof(struct _io_write) : 0;
                size_t header;
                char detail[180] = "", name[100];
                int ok = 1;

                if (combined) {
                    (void) native_header(wire, 1, _IO_XTYPE_NONE, 0, 0);
                    native_frame(wire, (unsigned) prefix | _IO_COMBINE_FLAG);
                }
                header = native_header(
                    wire + prefix, writing,
                    positioned ? _IO_XTYPE_OFFSET : _IO_XTYPE_NONE, 0, 0);
                for (size_t cut = 0; cut < header; cut++) {
                    if (!native_reject(fd, wire, prefix + cut,
                                       cut < 2 ? -1 : EBADMSG, detail,
                                       sizeof detail)) {
                        ok = 0;
                        break;
                    }
                }
                snprintf(name, sizeof name,
                         "native every %s%s header truncation%s (0..%zu)",
                         positioned ? "positioned " : "",
                         writing ? "write" : "read",
                         combined ? " after prefix" : "", header - 1);
                check(ok, name, detail);
                check(native_zero(fd, wire, prefix + header, detail,
                                  sizeof detail),
                      "native complete header after truncation sweep", detail);
            }
        }
    }
}

static void native_payload_truncations(int fd)
{
    for (int positioned = 0; positioned <= 1; positioned++) {
        for (int combined = 0; combined <= 1; combined++) {
            unsigned char wire[sizeof(struct native_write_at) + NATIVE_BYTES +
                               sizeof(struct _io_read)];
            unsigned xtype = positioned ? _IO_XTYPE_OFFSET : _IO_XTYPE_NONE;
            char detail[180] = "", name[100];
            size_t header, length;
            int ok = 1;

            for (size_t cut = 0; cut < NATIVE_BYTES; cut++) {
                header = native_header(wire, 1, xtype, NATIVE_BYTES, 0);
                memcpy(wire + header, native_data, NATIVE_BYTES);
                length = header + cut;
                if (combined) {
                    native_frame(wire, (unsigned) length | _IO_COMBINE_FLAG);
                    length +=
                        native_header(wire + length, 0, _IO_XTYPE_NONE, 0, 0);
                }
                if (!native_reject(fd, wire, length, EBADMSG, detail,
                                   sizeof detail)) {
                    ok = 0;
                    break;
                }
            }
            snprintf(name, sizeof name,
                     "native every %swrite payload truncation%s (0..63)",
                     positioned ? "positioned " : "",
                     combined ? " in frame" : "");
            check(ok, name, detail);
            header = native_header(wire, 1, xtype, NATIVE_BYTES, 0);
            memcpy(wire + header, native_data, NATIVE_BYTES);
            length = header + NATIVE_BYTES;
            if (combined) {
                native_frame(wire, (unsigned) length | _IO_COMBINE_FLAG);
                length += native_header(wire + length, 0, _IO_XTYPE_NONE, 0, 0);
            }
            check(native_probe(fd, wire, length, positioned ? NATIVE_START : 0,
                               combined ? 0 : NATIVE_BYTES, 0, 8, -1,
                               positioned ? NATIVE_START : NATIVE_BYTES, detail,
                               sizeof detail),
                  "native full payload after truncation sweep", detail);
        }
    }
}

static void native_alignments(int fd)
{
    for (int writing = 0; writing <= 1; writing++) {
        for (int positioned = 0; positioned <= 1; positioned++) {
            unsigned xtype = positioned ? _IO_XTYPE_OFFSET : _IO_XTYPE_NONE;
            size_t alignment = positioned
                                   ? (writing ? _Alignof(struct native_write_at)
                                              : _Alignof(struct native_read_at))
                                   : (writing ? _Alignof(struct _io_write)
                                              : _Alignof(struct _io_read));
            char detail[180] = "", name[100];
            int source_ok = 1, frame_ok = 1;

            for (unsigned residue = 0; residue < 8; residue++) {
                _Alignas(8) unsigned char wire[96] = {0};
                size_t header = native_header(wire + residue, writing, xtype, 1,
                                              NATIVE_START);

                wire[residue + header] = native_data[NATIVE_START];
                if (!native_probe(fd, wire + residue,
                                  header + (unsigned) writing, NATIVE_START, 1,
                                  0, 8, writing ? -1 : NATIVE_START,
                                  positioned ? NATIVE_START : NATIVE_START + 1,
                                  detail, sizeof detail)) {
                    source_ok = 0;
                    break;
                }
            }
            snprintf(name, sizeof name, "native %s%s sender alignment 0..7",
                     positioned ? "positioned " : "",
                     writing ? "write" : "read");
            check(source_ok, name, detail);
            for (unsigned residue = 0; residue < 8; residue++) {
                _Alignas(8) unsigned char wire[96] = {0};
                size_t prefix = sizeof(struct _io_write) + residue;
                size_t header;
                int ok;

                (void) native_header(wire, 1, _IO_XTYPE_NONE, 0, 0);
                native_frame(wire, (unsigned) prefix | _IO_COMBINE_FLAG);
                header = native_header(wire + prefix, writing, xtype, 1,
                                       NATIVE_START);
                wire[prefix + header] = native_data[NATIVE_START];
                ok = residue % alignment == 0
                         ? native_probe(
                               fd, wire, prefix + header + (unsigned) writing,
                               NATIVE_START, 1, 0, 8,
                               writing ? -1 : NATIVE_START,
                               positioned ? NATIVE_START : NATIVE_START + 1,
                               detail, sizeof detail)
                         : native_reject(fd, wire,
                                         prefix + header + (unsigned) writing,
                                         EBADMSG, detail, sizeof detail);
                if (!ok) {
                    char reason[128];
                    memcpy(reason, detail, sizeof reason);
                    reason[sizeof reason - 1] = '\0';
                    snprintf(detail, sizeof detail, "residue=%u: %s", residue,
                             reason);
                    frame_ok = 0;
                    break;
                }
            }
            snprintf(name, sizeof name, "native %s%s frame alignment 0..7",
                     positioned ? "positioned " : "",
                     writing ? "write" : "read");
            check(frame_ok, name, detail);
        }
    }
}

static void native_integer_boundaries(int fd)
{
    const uint64_t counts[] = {0,
                               1,
                               INT32_MAX - 1U,
                               INT32_MAX,
                               (uint64_t) INT32_MAX + 1U,
                               UINT32_MAX,
                               (uint64_t) UINT32_MAX + 1U,
                               INT64_MAX,
                               SIZE_MAX};
    const int32_t write_counts[] = {INT32_MIN, INT32_MIN + 1, -1,       0,
                                    1,         INT32_MAX - 1, INT32_MAX};
    const int64_t offsets[] = {INT64_MIN,
                               -1,
                               0,
                               1,
                               NATIVE_BYTES - 1,
                               NATIVE_BYTES,
                               NATIVE_BYTES + 1,
                               INT64_MAX - 1,
                               INT64_MAX};
    char detail[180] = "";
    int ok = 1;

    for (int positioned = 0; positioned <= 1; positioned++) {
        unsigned xtype = positioned ? _IO_XTYPE_OFFSET : _IO_XTYPE_NONE;

        for (size_t i = 0; i < sizeof counts / sizeof counts[0]; i++) {
            unsigned char wire[sizeof(struct native_read_at)];
            size_t header =
                native_header(wire, 0, xtype, counts[i], NATIVE_START);
            int n = counts[i] > 7 ? 7 : (int) counts[i];

            ok =
                counts[i] > INT32_MAX
                    ? native_reject(fd, wire, header, EINVAL, detail,
                                    sizeof detail)
                    : native_probe(
                          fd, wire, header, NATIVE_START, n, 0, 7, NATIVE_START,
                          NATIVE_START + (positioned ? 0U : (unsigned) n),
                          detail, sizeof detail);
            if (!ok)
                break;
        }
        check(ok,
              positioned ? "native pread count INT32_MAX-1/MAX/MAX+1/SIZE_MAX"
                         : "native read count INT32_MAX-1/MAX/MAX+1/SIZE_MAX",
              detail);
        for (size_t i = 0; i < sizeof write_counts / sizeof write_counts[0];
             i++) {
            unsigned char wire[sizeof(struct native_write_at) + 1];
            int32_t count = write_counts[i];
            size_t header = native_header(
                wire, 1, xtype, (uint64_t) (int64_t) count, NATIVE_START);

            wire[header] = native_data[NATIVE_START];
            ok = count < 0 || count > 1
                     ? native_reject(fd, wire, header + 1,
                                     count < 0 ? EINVAL : EBADMSG, detail,
                                     sizeof detail)
                     : native_probe(
                           fd, wire, header + 1, NATIVE_START, count, 0, 8, -1,
                           NATIVE_START + (positioned ? 0U : (unsigned) count),
                           detail, sizeof detail);
            if (!ok)
                break;
        }
        check(ok,
              positioned ? "native pwrite count INT32_MIN/-1/0/1/MAX-1/MAX"
                         : "native write count INT32_MIN/-1/0/1/MAX-1/MAX",
              detail);
    }
    for (int writing = 0; writing <= 1; writing++) {
        for (size_t i = 0; i < sizeof offsets / sizeof offsets[0]; i++) {
            unsigned char wire[sizeof(struct native_read_at)];
            int64_t offset = offsets[i];
            size_t header = native_header(wire, writing, _IO_XTYPE_OFFSET,
                                          writing ? 0 : 1, offset);
            int n = !writing && offset >= 0 && offset < NATIVE_BYTES;

            ok = offset < 0 ? native_reject(fd, wire, header, EINVAL, detail,
                                            sizeof detail)
                            : native_probe(fd, wire, header, NATIVE_START, n, 0,
                                           8, n ? (int) offset : -1,
                                           NATIVE_START, detail, sizeof detail);
            if (!ok)
                break;
        }
        check(ok,
              writing ? "native zero write offset INT64_MIN..INT64_MAX"
                      : "native read offset INT64_MIN..INT64_MAX",
              detail);
    }
    for (unsigned n = 1; n <= 8; n++) {
        unsigned char wire[sizeof(struct _io_write) + 8] = {0};
        uint64_t start = UINT64_MAX - n + 1U;
        size_t header = native_header(wire, 1, _IO_XTYPE_NONE, n, 0);

        ok = native_probe(fd, wire, header + n, start, -1, EOVERFLOW, 8, -1,
                          start, detail, sizeof detail);
        if (!ok)
            break;
    }
    check(ok, "native position plus write count crosses UINT64_MAX", detail);
}

static void native_frame_boundaries(int fd)
{
    char detail[180] = "";
    int ok = 1;

    for (unsigned i = 0; i <= sizeof(struct _io_write) + 2; i++) {
        _Alignas(8) unsigned char wire[64] = {0};
        size_t header = native_header(wire, 1, _IO_XTYPE_NONE, 0, 0);
        size_t length =
            header + native_header(wire + header, 0, _IO_XTYPE_NONE, 0, 0);
        unsigned frame = i <= header ? i : 0x7ffeU + i - (unsigned) header - 1U;

        native_frame(wire, frame | _IO_COMBINE_FLAG);
        ok = frame == header
                 ? native_zero(fd, wire, length, detail, sizeof detail)
                 : native_reject(fd, wire, length, EBADMSG, detail,
                                 sizeof detail);
        if (!ok)
            break;
    }
    check(ok, "native combine length 0..header/0x7ffe/0x7fff", detail);
}

static uint32_t native_random(uint32_t *state)
{
    uint32_t x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return x;
}

/* Every mutation preserves a known-invalid property. Keep the type read/write
 * so random bytes cannot close the handle or invoke a different service.
 */
static void native_malformed_fuzz(int fd)
{
    uint32_t state = NATIVE_FUZZ_SEED;
    unsigned classes[12] = {0};
    char detail[220] = "";
    int ok = 1;

    for (unsigned iteration = 0; iteration < NATIVE_FUZZ_CASES; iteration++) {
        _Alignas(8) unsigned char wire[128];
        unsigned kind = native_random(&state) % 12U;
        int writing = (int) (native_random(&state) & 1U);
        size_t header, length;
        int error = EBADMSG;

        for (size_t j = 0; j < sizeof wire; j++)
            wire[j] = (unsigned char) native_random(&state);
        header = native_header(wire, writing, _IO_XTYPE_NONE, 0, 0);
        length = header;
        classes[kind]++;
        switch (kind) {
        case 0: /* Fixed header is incomplete. */
            length = 2U + native_random(&state) % (header - 2U);
            break;
        case 1: /* Offset extension is incomplete. */
            length =
                header + native_random(&state) % sizeof(struct _xtype_offset);
            (void) native_header(wire, writing, _IO_XTYPE_OFFSET, 0, 0);
            break;
        case 2: /* Signed write count is negative. */
            length = native_header(
                wire, 1, _IO_XTYPE_NONE,
                (uint64_t) -(int64_t) (1U + native_random(&state) % INT32_MAX),
                0);
            error = EINVAL;
            break;
        case 3: /* Read count exceeds the reply status representation. */
            length = native_header(
                wire, 0, _IO_XTYPE_NONE,
                (uint64_t) INT32_MAX + 1U + native_random(&state), 0);
            error = EINVAL;
            break;
        case 4: /* Unknown xtype with random upper flag bits. */
            length = native_header(wire, writing,
                                   (native_random(&state) & ~255U) | 3U, 0, 0);
            error = ENOSYS;
            break;
        case 5: /* Positioned offset is negative. */
            length = native_header(wire, writing, _IO_XTYPE_OFFSET, 0,
                                   -(int64_t) (1ULL + native_random(&state)));
            error = EINVAL;
            break;
        case 6: { /* Write data is missing from the sender. */
            size_t payload = native_random(&state) % 16U;
            header =
                native_header(wire, 1, _IO_XTYPE_NONE,
                              payload + 1U + native_random(&state) % 16U, 0);
            length = header + payload;
            break;
        }
        case 7: { /* Write consumes bytes from the following frame. */
            size_t payload = native_random(&state) % 16U;
            size_t frame;
            header = native_header(wire, 1, _IO_XTYPE_NONE, payload + 1U, 0);
            frame = header + payload;
            native_frame(wire, (unsigned) frame | _IO_COMBINE_FLAG);
            length =
                frame + native_header(wire + frame, 0, _IO_XTYPE_NONE, 0, 0);
            break;
        }
        case 8: /* Maximum declared frame exceeds the sender's bytes. */
            native_frame(wire, 0xffffU);
            break;
        case 9: /* Zero-length nonterminal frame. */
            native_frame(wire, _IO_COMBINE_FLAG);
            break;
        case 10: { /* Misaligned read frame after a valid no-op write. */
            size_t prefix =
                sizeof(struct _io_write) + 1U + native_random(&state) % 7U;
            (void) native_header(wire, 1, _IO_XTYPE_NONE, 0, 0);
            native_frame(wire, (unsigned) prefix | _IO_COMBINE_FLAG);
            length =
                prefix + native_header(wire + prefix, 0, _IO_XTYPE_NONE, 0, 0);
            break;
        }
        case 11: /* Offset bytes exist in IPC but exceed their frame. */
            header = native_header(wire, writing, _IO_XTYPE_NONE, 0, 0);
            (void) native_header(wire, writing, _IO_XTYPE_OFFSET, 0, 0);
            native_frame(wire, (unsigned) header | _IO_COMBINE_FLAG);
            length =
                header + native_header(wire + header, 0, _IO_XTYPE_NONE, 0, 0);
            break;
        }
        if (!native_reject(fd, wire, length, error, detail, sizeof detail)) {
            char reason[128];
            memcpy(reason, detail, sizeof reason);
            reason[sizeof reason - 1] = '\0';
            snprintf(detail, sizeof detail,
                     "seed=0x%x iteration=%u class=%u %s", NATIVE_FUZZ_SEED,
                     iteration, kind, reason);
            ok = 0;
            break;
        }
    }
    check(ok, "native malformed IPC fuzz (4096 cases, seed 0x6d656d31)",
          detail);
    ok = 1;
    for (size_t i = 0; i < sizeof classes / sizeof classes[0]; i++)
        if (classes[i] == 0)
            ok = 0;
    check(ok, "native fuzz exercises all 12 mutation classes", "missing class");
}

void test_native_io_boundaries(void)
{
    const char path[] = "/dev/shmem/rtmux-io-boundaries";
    int fd;

    for (size_t i = 0; i < sizeof native_data; i++)
        native_data[i] = (unsigned char) (i * 37U + 1U);
    (void) unlink(path);
    fd = open(path, O_CREAT | O_EXCL | O_RDWR, 0600);
    if (fd == -1) {
        test_fail("native boundary fixture", strerror(errno));
        return;
    }
    if (write(fd, native_data, sizeof native_data) != sizeof native_data) {
        test_fail("native boundary fixture write", strerror(errno));
        goto done;
    }
    native_probes = 0;
    native_truncations(fd);
    native_payload_truncations(fd);
    native_alignments(fd);
    native_integer_boundaries(fd);
    native_frame_boundaries(fd);
    /* Isolate fuzzing from fixture corruption detected in earlier matrices. */
    (void) close(fd);
    (void) unlink(path);
    fd = open(path, O_CREAT | O_EXCL | O_RDWR, 0600);
    if (fd == -1 ||
        write(fd, native_data, sizeof native_data) != sizeof native_data) {
        test_fail("native fuzz fixture reset", strerror(errno));
        goto done;
    }
    native_malformed_fuzz(fd);
    printf("native boundary probes: %u (fuzz %u, seed 0x%x)\n", native_probes,
           NATIVE_FUZZ_CASES, NATIVE_FUZZ_SEED);
done:
    if (fd != -1)
        (void) close(fd);
    check(unlink(path) == 0, "native boundary cleanup", "unlink fixture");
}
