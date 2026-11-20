/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * sysconf-test.c — value sanity for sysconf() and confstr().
 *
 * This suite pins the page size from three independent sources (sysconf, getpagesize, the loader's AT_PAGESZ),
 * holds every _SC_* value libc or roottask answers to a range, sweeps every
 * name for a value with garbage in its upper 32 bits, and checks every seeded
 * _CS_* string.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/auxv.h>
#include <sys/conf.h>
#include <sys/rtmux-core.h>
#include <sys/sysmsg.h>
#include <unistd.h>

#include "test.h"

#define RTMUX_PAGE_SIZE 4096L
#define SC_SWEEP_MAX 200 /* above the highest _SC_* in <confname.h> */
#define POISON 0x5a /* fill byte that shows a reply wrote into a buffer */

/* The loader's AT_PAGESZ, read from the vector that follows the initial envp,
 * or 0 when the vector or the entry is missing.
 */
static unsigned long auxv_pagesz(void)
{
    char **ep = test_initial_envp;
    const uint64_t *av;

    if (!ep)
        return 0;
    while (*ep)
        ep++;
    for (av = (const uint64_t *) (ep + 1); av[0] != AT_NULL; av += 2) {
        if (av[0] == AT_PAGESZ)
            return (unsigned long) av[1];
    }
    return 0;
}

static void test_pagesize(void)
{
    long sc = sysconf(_SC_PAGESIZE);
    char detail[96];

    snprintf(detail, sizeof detail, "sysconf=%#lx getpagesize=%#x AT_PAGESZ=%#lx", sc,
             (unsigned) getpagesize(), auxv_pagesz());
    check(sc == RTMUX_PAGE_SIZE, "sysconf(_SC_PAGESIZE) == 4096", detail);
    check(sysconf(_SC_PAGE_SIZE) == sc, "_SC_PAGE_SIZE aliases _SC_PAGESIZE", detail);
    check(getpagesize() == RTMUX_PAGE_SIZE, "getpagesize() == 4096", detail);
    check(auxv_pagesz() == RTMUX_PAGE_SIZE, "AT_PAGESZ == 4096", detail);
}

struct sc_range {
    int name;
    const char *label;
    long lo, hi;
};

#define SC_RANGE(n, lo, hi) {(n), #n, (lo), (hi)}
#define SC_EXACT(n, v) SC_RANGE(n, v, v)

/* Every _SC_* name that libc's or roottask's static table answers, plus the
 * ones namedsem seeds at boot. Exact values come from the same headers the
 * tables use; ranges run from the POSIX minimum to a bound no RTMux build
 * comes near, so a value with garbage in its upper half falls outside.
 */
static const struct sc_range sc_ranges[] = {
    SC_EXACT(_SC_CHAR_BIT, CHAR_BIT),
    SC_EXACT(_SC_CHAR_MAX, CHAR_MAX),
    SC_EXACT(_SC_CHAR_MIN, CHAR_MIN),
    SC_EXACT(_SC_INT_MAX, INT_MAX),
    SC_EXACT(_SC_INT_MIN, INT_MIN),
    SC_EXACT(_SC_LONG_BIT, 64),
    SC_EXACT(_SC_SCHAR_MAX, SCHAR_MAX),
    SC_EXACT(_SC_SCHAR_MIN, SCHAR_MIN),
    SC_EXACT(_SC_SHRT_MAX, SHRT_MAX),
    SC_EXACT(_SC_SHRT_MIN, SHRT_MIN),
    SC_EXACT(_SC_SSIZE_MAX, LONG_MAX),
    SC_EXACT(_SC_UCHAR_MAX, UCHAR_MAX),
    SC_EXACT(_SC_UINT_MAX, (long) UINT_MAX),
    SC_EXACT(_SC_ULONG_MAX, (long) ULONG_MAX),
    SC_EXACT(_SC_USHRT_MAX, USHRT_MAX),
    SC_EXACT(_SC_WORD_BIT, 32),
    SC_RANGE(_SC_MB_LEN_MAX, 1, 16),
    SC_RANGE(_SC_THREAD_DESTRUCTOR_ITERATIONS, 4, 1024),
    SC_EXACT(_SC_VERSION, _POSIX_VERSION),
    SC_RANGE(_SC_ARG_MAX, 4096, 1L << 30),
    SC_RANGE(_SC_CHILD_MAX, 25, 1L << 20),
    SC_RANGE(_SC_CLK_TCK, 1, 1000000),
    SC_RANGE(_SC_NGROUPS_MAX, 8, 65536),
    SC_RANGE(_SC_OPEN_MAX, 20, 1L << 20),
    SC_EXACT(_SC_SAVED_IDS, 1),
    SC_RANGE(_SC_NZERO, 20, 64),
    SC_RANGE(_SC_RTSIG_MAX, 8, 64),
    SC_EXACT(_SC_THREAD_STACK_MIN, PTHREAD_STACK_MIN),
    SC_RANGE(_SC_THREAD_THREADS_MAX, 64, 1L << 20),
    SC_RANGE(_SC_NPROCESSORS_CONF, 1, 4096),
    SC_RANGE(_SC_NPROCESSORS_ONLN, 1, 4096),
    SC_RANGE(_SC_DELAYTIMER_MAX, 32, INT_MAX),
    SC_RANGE(_SC_SEMAPHORES, 1, INT_MAX),
    SC_EXACT(_SC_SEM_NSEMS_MAX, -1),
    SC_RANGE(_SC_SEM_VALUE_MAX, 32767, INT_MAX),
#if defined(TZNAME_MAX)
    SC_RANGE(_SC_TZNAME_MAX, 6, 255),
#endif
};

static void test_sc_ranges(void)
{
    size_t i;

    for (i = 0; i < sizeof sc_ranges / sizeof sc_ranges[0]; i++) {
        const struct sc_range *r = &sc_ranges[i];
        long v = sysconf(r->name);
        char name[64], detail[96];

        snprintf(name, sizeof name, "%s in range", r->label);
        snprintf(detail, sizeof detail, "got %ld (%#lx), want [%ld, %ld]", v, v, r->lo, r->hi);
        check(v >= r->lo && v <= r->hi, name, detail);
    }
    check(sysconf(_SC_NPROCESSORS_ONLN) <= sysconf(_SC_NPROCESSORS_CONF),
          "_SC_NPROCESSORS_ONLN <= _SC_NPROCESSORS_CONF", "more online than configured");
}

/* Every name the headers define, and the gaps between them: an answer other
 * than "unset" (-1) must fit in 32 bits, except the three limits that are
 * 64-bit by definition and checked exactly above. A narrow field loaded as a
 * long shows up here as a value with a nonzero upper half.
 */
static void test_sc_sweep(void)
{
    char detail[96] = "";
    int name, bad = 0;

    for (name = 0; name <= SC_SWEEP_MAX; name++) {
        long v = sysconf(name);

        if (name == _SC_SSIZE_MAX || name == _SC_UINT_MAX || name == _SC_ULONG_MAX)
            continue;
        if (v != -1 && (v < INT32_MIN || v > INT32_MAX)) {
            if (!bad++)
                snprintf(detail, sizeof detail, "sysconf(%d) = %#lx", name, v);
        }
    }
    check(!bad, "every sysconf value fits its width", detail);
}

#define ERRNO_SENTINEL 0x7e57

/* POSIX: an invalid name returns -1 with EINVAL; a valid name with no limit
 * returns -1 and leaves errno alone; a valid name with a value leaves errno
 * alone too.
 */
static void test_sc_errno(void)
{
    static const int invalid[] = {-1, INT_MIN, 0, 9, 12, 42, 47, 160, 1000, INT_MAX};
    static const int unset[] = {_SC_AIO_MAX, _SC_ASYNCHRONOUS_IO};
    size_t i;
    char name[64], detail[96];

    for (i = 0; i < sizeof invalid / sizeof invalid[0]; i++) {
        long v;

        errno = ERRNO_SENTINEL;
        v = sysconf(invalid[i]);
        snprintf(name, sizeof name, "sysconf(%d) fails with EINVAL", invalid[i]);
        snprintf(detail, sizeof detail, "got %ld errno %d", v, errno);
        check(v == -1 && errno == EINVAL, name, detail);
    }
    for (i = 0; i < sizeof unset / sizeof unset[0]; i++) {
        long v;

        errno = ERRNO_SENTINEL;
        v = sysconf(unset[i]);
        snprintf(name, sizeof name, "sysconf(%d) with no limit leaves errno alone", unset[i]);
        snprintf(detail, sizeof detail, "got %ld errno %d", v, errno);
        check(v == -1 && errno == ERRNO_SENTINEL, name, detail);
    }
    errno = ERRNO_SENTINEL;
    check(sysconf(_SC_PAGESIZE) == RTMUX_PAGE_SIZE && sysconf(_SC_CHAR_BIT) == CHAR_BIT &&
              errno == ERRNO_SENTINEL,
          "a valid sysconf name leaves errno alone", "success must not touch errno");
}

static const struct {
    int name;
    const char *label;
} cs_names[] = {
    {_CS_PATH, "_CS_PATH"},       {_CS_HOSTNAME, "_CS_HOSTNAME"},
    {_CS_RELEASE, "_CS_RELEASE"}, {_CS_VERSION, "_CS_VERSION"},
    {_CS_MACHINE, "_CS_MACHINE"}, {_CS_ARCHITECTURE, "_CS_ARCHITECTURE"},
};

/* strlen bounded by n, for a buffer a broken reply may leave unterminated. */
static size_t bounded_len(const char *s, size_t n)
{
    const char *nul = memchr(s, '\0', n);

    return nul ? (size_t) (nul - s) : n;
}

/* Each seeded string reports one length however it is asked, comes back
 * NUL-terminated at that length, and truncates into a short buffer.
 */
static void test_confstr_values(void)
{
    size_t i;

    for (i = 0; i < sizeof cs_names / sizeof cs_names[0]; i++) {
        char buf[256], small[4], name[64], detail[96];
        size_t need, got, trunc, len;

        memset(buf, POISON, sizeof buf);
        memset(small, POISON, sizeof small);
        need = confstr(cs_names[i].name, NULL, 0);
        got = confstr(cs_names[i].name, buf, sizeof buf);
        trunc = confstr(cs_names[i].name, small, sizeof small);
        len = bounded_len(buf, sizeof buf);

        snprintf(name, sizeof name, "confstr(%s) length consistent", cs_names[i].label);
        snprintf(detail, sizeof detail, "need=%zu got=%zu trunc=%zu len=%zu", need, got,
                 trunc, len);
        check(need >= 2 && need <= sizeof buf && got == need && trunc == need && len == need - 1,
              name, detail);
        snprintf(name, sizeof name, "confstr(%s) truncates", cs_names[i].label);
        check(bounded_len(small, sizeof small) == sizeof small - 1 &&
                  !memcmp(small, buf, sizeof small - 1),
              name, "a short buffer must hold a NUL-terminated prefix");
    }
}

static int untouched(const char *buf, size_t n)
{
    while (n--)
        if (buf[n] != POISON)
            return 0;
    return 1;
}

/* Ask roottask for an unseeded name with a raw request carrying `size` as the
 * buffer size, and report whether it answered "no match, nothing copied".
 * Roottask used to echo the size back, so a negative one became a reply length
 * near SIZE_MAX and a positive one copied that many stale bytes of its receive
 * buffer to the caller.
 */
static int confstr_raw_unmatched(int64_t size, char *detail, size_t detail_len)
{
    sys_conf_t msg;
    char buf[64];
    iov_t iov[2];
    int rc;

    memset(&msg, 0, sizeof msg);
    memset(buf, POISON, sizeof buf);
    msg.i.type = _SYS_CONF;
    msg.i.subtype = _SYS_SUB_GET;
    msg.i.cmd = _CONF_STR;
    msg.i.name = _CS_VENDOR;
    msg.i.value = size;
    SETIOV(iov + 0, &msg.o, sizeof msg.o);
    SETIOV(iov + 1, buf, sizeof buf);
    rc = msg_sendvnc_r(ROOTTASK_COID, (iov_t *) &msg.i, -(ssize_t) sizeof msg.i, iov, 2);
    snprintf(detail, detail_len, "size=%lld rc=%d match=%d value=%lld", (long long) size, rc,
             (int) msg.o.match, (long long) msg.o.value);
    return rc == 0 && msg.o.match == -1 && msg.o.value == 0 && untouched(buf, sizeof buf);
}

static void test_confstr_unmatched(void)
{
    char buf[64], detail[96];

    check(confstr_raw_unmatched(-1, detail, sizeof detail),
          "confstr request with a negative size copies nothing", detail);
    check(confstr_raw_unmatched((int64_t) sizeof buf, detail, sizeof detail),
          "confstr request for an unseeded name copies nothing", detail);

    memset(buf, POISON, sizeof buf);
    check(confstr(_CS_VENDOR, buf, sizeof buf) == 0 && untouched(buf, sizeof buf),
          "confstr(unseeded) returns 0 and leaves the buffer alone",
          "an unseeded vendor name must report no value");
}

void test_sysconf(void)
{
    test_pagesize();
    test_sc_ranges();
    test_sc_sweep();
    test_sc_errno();
    test_confstr_values();
    test_confstr_unmatched();
}
