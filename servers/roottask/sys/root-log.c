/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * root-log.c -- Roottask logging subsystem implementation.
 *
 * Provides a ring buffer for diagnostic messages and optional console echo.
 * This is the ONLY file in roottask that calls dbgprintf (the U-mode
 * replacement for the kernel's kprintf — slot __KSYS_DBGPRINT, formatted
 * variant in lib/libc/support/dbgprintf.c).
 *
 * Ring buffer layout:
 *   A circular char array of CONFIG_ROOTTASK_SYSLOG_SIZE bytes.
 *   log_head is the next write position (0..SIZE-1).
 *   log_seq is the absolute count of bytes ever written (64-bit, never wraps
 *   in practice); byte N lives at log_buf[N % SIZE] while N >= log_seq - SIZE.
 *   The ring retains the most recent CONFIG_ROOTTASK_SYSLOG_SIZE bytes;
 *   older data is overwritten in place. root_log_read() reads by absolute
 *   sequence, so a reader detects overwritten data instead of mixing eras.
 *
 * This file also exposes:
 *   root_crash(file, line, fmt, ...) — record a fatal in the ring,
 *       echo via dbgprintf, then syscall_priv(__KSYS_PRIV_REBOOT).
 *       Used by the crash() / CRASHCHECK macros (see root-log.h).
 *   root_console_write(buf, len) — raw byte-level console write
 *       (kernel UART), used by roottask's /dev/text I/O service.
 *   root_console_getchar() — single-character read from kernel UART.
 */

#include "externs.h"
#include "root-log.h"
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <sys/rtmux-core.h> /* dbgprint, dbgprintf */

/* Log levels (internal) */
#define LVL_ERR 0
#define LVL_WARN 1
#define LVL_INFO 2
#define LVL_DBG 3
#define LVL_TRACE 4

static char log_buf[CONFIG_ROOTTASK_SYSLOG_SIZE];
static size_t log_head;  /* next write position in ring */
static uint64_t log_seq; /* absolute bytes ever written */
static pthread_mutex_t log_lock = PTHREAD_MUTEX_INITIALIZER;

static const char *level_prefix[] = {
    "RT:E ", /* LVL_ERR */
    "RT:W ", /* LVL_WARN */
    "RT   ", /* LVL_INFO */
    "RT:D ", /* LVL_DBG */
    "RT:T ", /* LVL_TRACE */
};

/* Append raw bytes to the ring buffer. Caller must hold log_lock. */
static void ring_write(const char *buf, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        log_buf[log_head] = buf[i];
        log_head++;
        if (log_head >= CONFIG_ROOTTASK_SYSLOG_SIZE)
            log_head = 0;
    }
    log_seq += len;
}

/* Sequence of the oldest byte still in the ring. Caller must hold log_lock. */
static uint64_t ring_oldest(void)
{
    return log_seq > CONFIG_ROOTTASK_SYSLOG_SIZE
               ? log_seq - CONFIG_ROOTTASK_SYSLOG_SIZE
               : 0;
}

/* root_log_bounds -- report the sequence range the ring currently holds.
 *
 * @oldest: receives the sequence of the oldest retained byte
 * @end: receives the sequence one past the newest byte (total bytes written)
 */
void root_log_bounds(uint64_t *oldest, uint64_t *end)
{
    pthread_mutex_lock(&log_lock);
    *end = log_seq;
    *oldest = ring_oldest();
    pthread_mutex_unlock(&log_lock);
}

/* root_log_read -- copy ring bytes starting at absolute sequence *seq.
 *
 * Copies at most min(n, ROOT_LOG_READ_MAX, bytes up to @limit) bytes and
 * advances *seq by the amount copied. @limit is an absolute end sequence, so a
 * reader can bound itself to the data present when it opened.
 * Return value: bytes copied (0 at @limit), -EOVERFLOW if *seq was already
 *               overwritten (the caller's window is lost; *seq is unchanged),
 *               -EINVAL if *seq is beyond the data written so far.
 */
ssize_t root_log_read(char *dst, uint64_t *seq, uint64_t limit, size_t n)
{
    uint64_t pos;
    size_t first;

    if (n > ROOT_LOG_READ_MAX)
        n = ROOT_LOG_READ_MAX;
    pthread_mutex_lock(&log_lock);
    if (*seq < ring_oldest()) {
        pthread_mutex_unlock(&log_lock);
        return -EOVERFLOW;
    }
    if (*seq > log_seq) {
        pthread_mutex_unlock(&log_lock);
        return -EINVAL;
    }
    if (limit > log_seq)
        limit = log_seq;
    if (*seq >= limit) {
        pthread_mutex_unlock(&log_lock);
        return 0;
    }
    if ((uint64_t) n > limit - *seq)
        n = (size_t) (limit - *seq);
    pos = *seq % CONFIG_ROOTTASK_SYSLOG_SIZE;
    first = CONFIG_ROOTTASK_SYSLOG_SIZE - (size_t) pos;
    if (first > n)
        first = n;
    memcpy(dst, log_buf + pos, first);
    memcpy(dst + first, log_buf, n - first); /* wrapped tail, if any */
    *seq += n;
    pthread_mutex_unlock(&log_lock);
    return (ssize_t) n;
}

/* root_emit -- format, buffer, and optionally echo a log message.
 *
 * @level: LVL_ERR..LVL_TRACE @fmt: printf-style format string @ap: argument
 * list
 */
static void root_emit(int level, const char *fmt, va_list ap)
{
    char body[256];
    char buf[512];
    const char *pfx = level_prefix[level];
    size_t plen = strlen(pfx);
    int rc;

    /* Format the message body (no prefix yet) so we can split it into lines and
     * emit the level prefix only on non-empty lines. Empty lines (a bare "\n")
     * get printed as just "\n" — useful for visually separating sections of log
     * output without the "RT " prefix taking up the line.
     */
    rc = vsnprintf(body, sizeof(body), fmt, ap);
    if (rc < 0)
        rc = 0;
    if ((size_t) rc >= sizeof(body))
        rc = (int) sizeof(body) - 1;
    size_t blen = (size_t) rc;

    /* Ensure the body ends with a newline so the final line gets emitted. */
    if (blen == 0 || body[blen - 1] != '\n') {
        if (blen < sizeof(body) - 1)
            body[blen++] = '\n';
    }

    /* Walk body line-by-line, prepending pfx only to non-empty lines. */
    size_t total = 0;
    const char *p = body;
    const char *end = body + blen;
    while (p < end) {
        const char *nl = memchr(p, '\n', (size_t) (end - p));
        size_t linelen = nl ? (size_t) (nl - p) : (size_t) (end - p);

        if (linelen == 0) {
            /* Empty line — just the newline, no prefix. */
            if (total + 1 < sizeof(buf))
                buf[total++] = '\n';
        } else {
            /* Non-empty line — prefix + content + newline. */
            if (total + plen + linelen + 1 < sizeof(buf)) {
                memcpy(buf + total, pfx, plen);
                total += plen;
                memcpy(buf + total, p, linelen);
                total += linelen;
                buf[total++] = '\n';
            }
        }
        p = nl ? nl + 1 : end;
    }
    buf[total] = '\0';

    /* Write to ring buffer (always, regardless of level) */
    pthread_mutex_lock(&log_lock);
    ring_write(buf, total);
    pthread_mutex_unlock(&log_lock);

    /* Console echo -- the ONE place in roottask that calls dbgprintf (the
     * U-mode kprintf replacement, syscall slot __KSYS_DBGPRINT). Goes via
     * dbgprint (raw bytes) since "buf" is already a fully formatted line — no
     * need to re-run a format step.
     */
#ifdef CONFIG_ROOTTASK_SYSLOG_STDOUT
    {
        int echo = 0;
        switch (level) {
        case LVL_ERR:
        case LVL_WARN:
        case LVL_INFO:
            echo = 1;
            break;
        case LVL_DBG:
            echo = (roottask_loglevel >= 1);
            break;
        case LVL_TRACE:
            echo = (roottask_loglevel >= 2);
            break;
        }
        if (echo)
            dbgprint(buf, total);
    }
#endif
}


/* root_err / root_warn / root_log / root_dbg / root_trace — the leveled logging
 * API. Each formats a printf-style message at its level and hands it to
 * root_emit (ring buffer plus console echo gated by roottask_loglevel).
 * Parameters:
 *   fmt, ... : printf-style format string and arguments
 */
void root_err(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    root_emit(LVL_ERR, fmt, ap);
    va_end(ap);
}

void root_warn(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    root_emit(LVL_WARN, fmt, ap);
    va_end(ap);
}

void root_log(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    root_emit(LVL_INFO, fmt, ap);
    va_end(ap);
}

void root_dbg(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    root_emit(LVL_DBG, fmt, ap);
    va_end(ap);
}

void root_trace(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    root_emit(LVL_TRACE, fmt, ap);
    va_end(ap);
}


/* root_crash -- fatal diagnostic. Used by the crash() and CRASHCHECK macros
 * (see root-log.h). Records the message in the ring buffer via root_emit,
 * echoes a final "RT:CRASH" line directly to console (in case the ring buffer
 * pthread mutex is wedged), then asks the kernel to reboot. Never returns.
 */
void root_crash(const char *file, int line, const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    int n;

    /* Best-effort entry into the ring buffer. */
    va_start(ap, fmt);
    {
        char inner[200];
        size_t plen;
        (void) vsnprintf(inner, sizeof(inner), fmt ? fmt : "", ap);
        plen = (size_t) snprintf(buf, sizeof(buf), "CRASH at %s:%d — %s\n",
                                 file, line, inner);
        if (plen < sizeof(buf))
            root_err("%s", buf);
    }
    va_end(ap);

    /* Direct dbgprint after the ring-buffer write so the operator sees the
     * message even if console echo was disabled by the Kconfig option or by a
     * wedged log mutex.
     */
    n = snprintf(buf, sizeof(buf), "RT:CRASH %s:%d (roottask halting)\n", file,
                 line);
    if (n > 0)
        dbgprint(buf, (size_t) n);

    /* Ask the kernel to reboot abnormally. syscall_priv path runs the reboot
     * kernel extension under BKL. If for any reason it returns, fall through to
     * a hard wfi loop.
     */
    (void) syscall_priv(__KSYS_PRIV_REBOOT, (void *) (uintptr_t) 1);
    for (;;)
        ;
}


/* root_console_write -- raw byte-level write to the kernel console. Used by
 * roottask's /dev/text I/O service (path/devtext.c) to forward user-supplied
 * bytes verbatim. This is data passthrough, not logging — it intentionally
 * bypasses the ring buffer.
 */
void root_console_write(const char *buf, size_t len)
{
    if (len > 0)
        dbgprint(buf, len);
}


/* root_console_getchar -- block until a character is available on the kernel
 * console, return it. Wraps __KSYS_PRIV_CON_GETC.
 */
int root_console_getchar(void)
{
    return (int) syscall_priv(__KSYS_PRIV_CON_GETC, NULL);
}
