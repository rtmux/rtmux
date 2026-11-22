/*
 * kernel/log.h — kernel logging macros and kprintf prototypes.
 *
 * Provides LOGF_E/W/I/D/T macros for structured logging at different verbosity
 * levels. Safe to include from kernel, roottask, and libc.
 *
 * E/I always print. W prints regardless of level but is rate-limited per call
 * site, so a warning repeated in a burst may drop lines. D prints with -v
 * (kopt.loglevel >= 1). T prints with -vv (kopt.loglevel >= 2).
 *
 * kopt.loglevel is set by '-v' options in common-options.c.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef _KERNEL_LOG_H_INCLUDED
#define _KERNEL_LOG_H_INCLUDED

#include <stdbool.h>
#include <stddef.h>
#include <stdarg.h>
#include <options.h>

#ifndef STRINGIZE
#define STRINGIZE_(x) #x
#define STRINGIZE(x) STRINGIZE_(x)
#endif

/* kprintf family — authoritative prototypes (defined in
 * kernel/printk/kprintf.c).
 */
int kprintf(const char *fmt, ...);
void kprintf_raw(const char *buf, size_t len);

/* Per-call-site rate limit for reports a caller can trigger at will (a bad
 * system call, a user fault, a recovered copy fault). A synchronous console
 * write costs hundreds of microseconds (the kcall-dbgprint row in
 * docs/rt-budgets.md), so an unthrottled report is both a console flood and a
 * latency hazard. The first KPRINTF_RL_BURST reports print; after that one in
 * KPRINTF_RL_INTERVAL does, at most once a second, carrying the count
 * suppressed. Assumption class: tuning. Revisit trigger: a debugging session
 * that needs the full stream.
 */
#define KPRINTF_RL_BURST 16
#define KPRINTF_RL_INTERVAL 1000

struct kprintf_ratelimit {
    unsigned long events;     /* reports seen at this site; atomic */
    unsigned long printed_at; /* events when the last line printed */
    unsigned long next_tick;  /* rdtime before which no line prints */
};

bool kprintf_rl_admit(struct kprintf_ratelimit *rl,
                      unsigned long *seen,
                      unsigned long *last);
void kprintf_rl_print(unsigned long seen,
                      unsigned long last,
                      const char *fmt,
                      ...);

/* KPRINTF_RATELIMITED(fmt, ...) — kprintf with a static limiter private to this
 * call site. The arguments are evaluated only for a report that prints, so a
 * dropped one costs the admit check alone.
 */
#define KPRINTF_RATELIMITED(fmt, ...)                                 \
    do {                                                              \
        static struct kprintf_ratelimit kprintf_rl_site_;             \
        unsigned long kprintf_rl_seen_, kprintf_rl_last_;             \
        if (kprintf_rl_admit(&kprintf_rl_site_, &kprintf_rl_seen_,    \
                             &kprintf_rl_last_))                      \
            kprintf_rl_print(kprintf_rl_seen_, kprintf_rl_last_, fmt, \
                             ##__VA_ARGS__);                          \
    } while (0)
#ifdef CONFIG_STALL_DETECTOR
/* kprintf that parks the line (flushed by this CPU's next kprintf) instead of
 * spinning for ever on a busy console lock. Interrupt-context callers only.
 */
int kprintf_bounded(unsigned spins, const char *fmt, ...);
void kprintf_flush_parked(unsigned spins);

/* kprintf_bounded() whose fallback is a lock-free console write, not a parked
 * line: for a report that must come out while the console owner is wedged.
 */
int kprintf_emergency(unsigned spins, const char *fmt, ...);
#endif
#ifdef CONFIG_DEBUG_INJECT
void kprintf_inject_hold(unsigned ms); /* hold the console lock, irqs off */
#endif
int ksprintf(char *buffer, const char *fmt, ...);
int ksnprintf(char *buffer, size_t len, const char *fmt, ...);
size_t kvsnprintf(char *buf, size_t buflen, const char *fmt, va_list ap);
void dbug_display(const char *buffer, size_t len);
void con_write(const char *buffer, size_t len);
int con_getchar(void);
void con_puts(const char *buffer);

/* crash_halt(fmt, ...) — print message and halt (defined in kprintf.c).
 *
 * crash() with no args expands to crash_halt with file:line. crash("fmt", ...)
 * forwards arguments to crash_halt (used by startup).
 */
void crash_halt(const char *fmt, ...);
#define crash(...)                                                          \
    crash_halt("CRASH: " __FILE__ ":" STRINGIZE(__LINE__) __VA_OPT__(" — ") \
                   __VA_ARGS__)

/* SMP crash-dump rendezvous (kprintf.c).
 *
 * kprintf_crash_mode_on()
 *   Switch this CPU into per-CPU buffered mode: every subsequent
 *   kprintf/kprintf_raw on this CPU appends to its private 4 KiB
 *   staging buffer instead of going to the global console.  No lock,
 *   no console contention with sibling CPUs that may be crashing in
 *   parallel.  Idempotent.  Call this at the very top of any large
 *   diagnostic block that might race against another CPU's diagnostic.
 *
 * crash_rendezvous()
 *   Join the SMP rendezvous and halt.  All CPUs that called this
 *   funnel through one printer (chosen by cmpxchg); the printer walks
 *   every CPU's staging buffer in CPU-id order and emits each as a
 *   contiguous chunk, headed by "--- cpu N (M bytes) ---".  No tear-up.
 *   Lone crasher case is handled by a bounded spin before printer
 *   selection — the lone crasher just becomes its own printer.
 *
 * crash_halt(fmt, ...) calls both internally; bare crash() / crash_halt sites
 * need no source change to benefit. Use the explicit entries only when you need
 * to accumulate a multi-line diagnostic before the final crash() (see
 * trap-handler.c S-mode dump for the canonical usage).
 */
void kprintf_crash_mode_on(void);
void crash_rendezvous(void) __attribute__((noreturn));

/* crash_recursive_check — re-entry guard.
 * Returns normally if this CPU is NOT already in crash mode. If this CPU IS
 * already in crash mode (recursive trap during a crash dump), appends a
 * one-line marker to the per-CPU buffer and calls crash_rendezvous() — does NOT
 * return in that case.
 */
void crash_recursive_check(unsigned long scause,
                           unsigned long sepc,
                           unsigned long stval);

/* crash_quiesce_self — IPI_CRASH handler entry (sibling is crashing).
 *
 * Called from the arch IPI handler when a sibling CPU broadcast IPI_CRASH at
 * us. Snapshots a brief identity line into this CPU's per-CPU crash buffer
 * (sepc at IPI entry, current thread tid/pid) and joins the rendezvous via
 * crash_rendezvous() — never returns.
 */
void crash_quiesce_self(void) __attribute__((noreturn));

/* DBGOUT — throwaway debug-only kprintf. Compiled out with NDEBUG. Use
 * LOGF_D/LOGF_T for permanent instrumentation.
 */
#ifndef NDEBUG
#define DBGOUT(fmt, ...) LOGF_T(fmt, ##__VA_ARGS__)
#else
#define DBGOUT(fmt, ...) ((void) 0)
#endif

/* Structured logging macros */
#define LOGF_E(fmt, ...) kprintf("ERROR: " fmt, ##__VA_ARGS__)

/* Rate-limited per call site by the KPRINTF_RATELIMITED limiter: a warning that
 * fires in a loop prints its burst, then at most one line a second.
 */
#define LOGF_W(fmt, ...) KPRINTF_RATELIMITED("WARNING: " fmt, ##__VA_ARGS__)
#define LOGF_I(fmt, ...) kprintf(fmt, ##__VA_ARGS__)
#define LOGF_D(fmt, ...)                 \
    do {                                 \
        if (kopt.loglevel >= 1)          \
            kprintf(fmt, ##__VA_ARGS__); \
    } while (0)
#define LOGF_T(fmt, ...)                 \
    do {                                 \
        if (kopt.loglevel >= 2)          \
            kprintf(fmt, ##__VA_ARGS__); \
    } while (0)

/* MARK_POTN_INF_LOOP() — count each (rate-limited) pass of a KERNEL loop that
 * could spin forever, and remember where the last one was, so a hang inside
 * such a loop is visible in a crash or hang dump (scripts/gdb/dump-state.gdb
 * prints both) instead of being a silent busy-spin.
 *
 * It writes nothing to the console. A console marker from a spinning hart lands
 * in the middle of whatever line another hart or user space is printing, and
 * every log pattern anchored to that line then had to strip it first.
 *
 * One relaxed atomic add and a pointer store, so it is safe inside any spin
 * loop. Debug-only: NDEBUG strips it.
 *
 * Defined only for KERNEL_INTERNAL builds.
 *
 * For a TIGHT spin (millions of iters/sec) gate the call with a counter so a
 * normal transient wait counts nothing and only a true infinite spin climbs,
 * e.g.:
 *     while (cond) { if ((++spins & 0xFFFFF) == 0) MARK_POTN_INF_LOOP(); ... }
 */
#ifdef KERNEL_INTERNAL
#ifndef NDEBUG
extern unsigned long potn_inf_loop_marks; /* kernel/core/globals.c */
extern const char *potn_inf_loop_site;
#define MARK_POTN_INF_LOOP()                                                   \
    do {                                                                       \
        __atomic_fetch_add(&potn_inf_loop_marks, 1UL, __ATOMIC_RELAXED);       \
        __atomic_store_n(&potn_inf_loop_site,                                  \
                         __FILE__ ":" STRINGIZE(__LINE__), __ATOMIC_RELAXED);  \
    } while (0)
#else
#define MARK_POTN_INF_LOOP() ((void) 0)
#endif
#endif /* KERNEL_INTERNAL */

#endif /* _KERNEL_LOG_H_INCLUDED */
