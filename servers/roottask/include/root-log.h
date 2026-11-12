/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * root-log.h -- Roottask logging subsystem.
 *
 * All roottask diagnostic output goes through these functions. Messages are
 * stored in a ring buffer (CONFIG_ROOTTASK_SYSLOG_SIZE bytes) and optionally
 * echoed to the system console (CONFIG_ROOTTASK_SYSLOG_STDOUT).
 *
 * Level semantics:
 *   root_err   -- error, always logged (prefixed "RT:E ")
 *   root_warn  -- warning, always logged (prefixed "RT:W ")
 *   root_log   -- informational, always logged (prefixed "RT   ")
 *   root_dbg   -- debug, logged when roottask_loglevel >= 1 (-v)
 *   root_trace -- trace, logged when roottask_loglevel >= 2 (-vv)
 *
 * All levels always write to the ring buffer. Console echo respects
 * roottask_loglevel for root_dbg and root_trace. roottask_loglevel is set from
 * -v / -vv / -vvv on the boot command line, parsed by roottask_main.
 */

#ifndef _ROOTTASK_ROOT_LOG_H
#define _ROOTTASK_ROOT_LOG_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include <generated/autoconf.h>

/* Defaults if Kconfig values are not yet generated */
#ifndef CONFIG_ROOTTASK_SYSLOG_SIZE
#define CONFIG_ROOTTASK_SYSLOG_SIZE (1024 * 1024)
#endif

#ifndef CONFIG_ROOTTASK_SYSLOG_STDOUT
#define CONFIG_ROOTTASK_SYSLOG_STDOUT 1
#endif

/* Logging functions. All accept printf-style format strings (using the kernel's
 * kvsnprintf formatter).
 */
void root_err(const char *fmt, ...);
void root_warn(const char *fmt, ...);
void root_log(const char *fmt, ...);
void root_dbg(const char *fmt, ...);
void root_trace(const char *fmt, ...);

/* The ring is also readable by absolute byte sequence (served as /sys/log). A
 * single root_log_read returns at most ROOT_LOG_READ_MAX bytes.
 */
#define ROOT_LOG_READ_MAX 4096
#define ROOT_LOG_MARK_MAX 128 /* longest _SYS_CMD_LOG_MARK text */
void root_log_bounds(uint64_t *oldest, uint64_t *end);
ssize_t root_log_read(char *dst, uint64_t *seq, uint64_t limit, size_t n);

/* root_crash -- fatal diagnostic, never returns. Used by the crash() and
 * CRASHCHECK macros below. Records the message in the ring buffer via root_err,
 * dbgprints a final fail-safe line, and asks the kernel to reboot.
 */
__attribute__((noreturn)) void root_crash(const char *file,
                                          int line,
                                          const char *fmt,
                                          ...);

/* Console I/O for roottask's /dev/text I/O service. These bypass the ring
 * buffer (data passthrough, not logging) and go straight to the kernel UART via
 * the dbgprint syscall and __KSYS_PRIV_CON_GETC.
 */
void root_console_write(const char *buf, size_t len);
int root_console_getchar(void);


/* Macro overrides — kill every kernel-named log/crash macro that would
 * otherwise emit "kprintf" or "crash_halt" from roottask text. The K-prefixed
 * names are kernel-internal; roottask is U-mode, so everything routes through
 * the ring buffer (and onward to console via the single dbgprintf call inside
 * root-log.c).
 *
 * Side effect: every CRASHCHECK / crash / LOGF_* / pr_* / DBGOUT /
 * STRLCPY-overflow message is now retained in the ring buffer, even if console
 * echo was off.
 */
#include <log.h> /* kernel/include/log.h — pulls in the
                             K-named macros we are about to override */

#undef LOGF_E
#undef LOGF_W
#undef LOGF_I
#undef LOGF_D
#undef LOGF_T
#undef DBGOUT
#undef pr_err
#undef pr_warn
#undef pr_info
#undef pr_debug
#undef pr_alert
#undef pr_crit
#undef ultra_verbose
#undef crash

#define LOGF_E(fmt, ...) root_err(fmt, ##__VA_ARGS__)
#define LOGF_W(fmt, ...) root_warn(fmt, ##__VA_ARGS__)
#define LOGF_I(fmt, ...) root_log(fmt, ##__VA_ARGS__)
#define LOGF_D(fmt, ...) root_dbg(fmt, ##__VA_ARGS__)
#define LOGF_T(fmt, ...) root_trace(fmt, ##__VA_ARGS__)

#define pr_err(fmt, ...) root_err(fmt, ##__VA_ARGS__)
#define pr_warn(fmt, ...) root_warn(fmt, ##__VA_ARGS__)
#define pr_info(fmt, ...) root_log(fmt, ##__VA_ARGS__)
#define pr_debug(fmt, ...) root_dbg(fmt, ##__VA_ARGS__)
#define pr_alert(fmt, ...) root_err("ALERT: " fmt, ##__VA_ARGS__)
#define pr_crit(fmt, ...) root_err("CRITICAL: " fmt, ##__VA_ARGS__)

#define ultra_verbose(fmt, ...) root_trace(fmt, ##__VA_ARGS__)

#ifndef NDEBUG
#define DBGOUT(fmt, ...) root_trace(fmt, ##__VA_ARGS__)
#else
#define DBGOUT(fmt, ...) ((void) 0)
#endif

#define crash(...) root_crash(__FILE__, __LINE__, "" __VA_ARGS__)

/* STR_OVERFLOW (kernel/include/decls.h, used by STRLCPY) emits a kprintf —
 * override to use root_warn so the truncation report lands in the ring buffer.
 */
#undef STR_OVERFLOW
#ifndef NDEBUG
#define STR_OVERFLOW(f, line, s, d, l)                          \
    root_warn(                                                  \
        "%s:%u - strncpy overflow, src truncated to %d bytes\n" \
        "\tsrc - '%s'\n\tdst  - '%s'\n",                        \
        (f), (line), (l) - 1, (s), (d))
#else
#define STR_OVERFLOW(f, line, s, d, l)
#endif

#endif /* _ROOTTASK_ROOT_LOG_H */
