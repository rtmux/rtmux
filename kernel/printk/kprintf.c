/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * kprintf.c — Unified kernel printf (startup + kernel, single binary)
 *
 * Full kvsnprintf formatter with width, alignment, and LP64 support. Output
 * goes through a pluggable print_char function pointer, set early by board init
 * (set_print_char).
 *
 * License: Apache 2.0
 */

#include <stdarg.h>
#include <stdatomic.h>
#include <bitops.h>
#include <core/globals.h>
#include <cpu-inline.h>
#include <sbi.h> /* sbi_dbcn_write — atomic console output */

/* Output character function pointer. Set by set_print_char() during board init
 * (before any kprintf call).
 */
static void dummy_print_char(int c)
{
    (void) c;
}
static void (*print_char)(int) = dummy_print_char;

/* Spinlock for serializing kprintf output across CPUs. Zero-initialized, so
 * harmless before SMP is active.
 */
atomic_uint kprintf_slock;
static int at_bol = 1; /* at beginning of line — print [cpu] prefix */
int cpunum_ready; /* set after cpu_set_cpunum; suppresses prefix before that */

#ifdef CONFIG_STALL_DETECTOR

/* Forward-progress sequence counter. Bumped under kprintf_slock on every output
 * call, so an observer (the stall detector in clock_handler) can detect "no
 * output for N ticks" == likely kernel stall.
 */
volatile unsigned long kprintf_seq;
#endif

/* Disable interrupts BEFORE acquiring the spinlock to prevent priority
 * inversion: if a thread holds kprintf_slock and gets preempted, any other
 * thread (or the same CPU's interrupt handler) spinning on the lock would
 * deadlock on uniprocessor.
 *
 * Per-CPU saved sstatus so SMP kprintf calls don't clobber each other.
 *
 * Re-entrancy on the same CPU: a synchronous trap (page fault, etc.) INSIDE a
 * kprintf body re-enters strap_handler_c, whose diagnostic paths themselves
 * call kprintf. Without the per-CPU in_kprintf counter below, the inner
 * kprintf_lock() would spin forever on the lock the outer call already holds —
 * a self-deadlock. The counter lets the outermost call own the lock, lets
 * nested calls fall through to the same con_write path, and only releases the
 * lock and restores interrupts when the outermost call returns. Output from
 * nested calls is interleaved with the outer's output — that is better than
 * deadlock and lets the inner-fault diagnostic finish printing.
 */
static unsigned long kprintf_saved_sstatus[CONFIG_PROCESSORS_MAX];
static unsigned in_kprintf[CONFIG_PROCESSORS_MAX];

/* Per-CPU crash buffer + rendezvous (fixes torn crash dumps).
 *
 * When this CPU enters "crash mode" via kprintf_crash_mode_on(), every
 * subsequent kprintf()/kprintf_raw() on this CPU appends into its own
 * page-aligned BSS buffer instead of going through the global kprintf lock +
 * console. No contention, no re-entry, no interleaving with other CPUs that
 * might be crashing in parallel.
 *
 * crash_rendezvous() is called once the diagnostic dump is complete. It joins
 * the rendezvous (atomic counter), waits briefly for siblings to finish their
 * own dumps, then exactly one CPU wins a printer slot and walks every CPU's
 * buffer in CPU-id order, emitting them via the already-lock-free con_write
 * path. Every other CPU just halts.
 *
 * Why a per-CPU buffer instead of fixing the kprintf lock contention: the
 * failure modes we've actually seen aren't "lock contention slows things down"
 * but "two crashing CPUs each emit a 2 KiB diagnostic which interleaves
 * character-by-character on the console, producing unreadable output." The
 * rendezvous turns this into a clean per-CPU group dump where each CPU's story
 * is contiguous.
 *
 * Buffer is one page per CPU = CONFIG_PROCESSORS_MAX × 4 KiB BSS. Page-aligned
 * so the array start sits at a page boundary; with row size == 4096 every
 * per-CPU row is page-aligned too.
 */
#define CRASH_BUF_SIZE 4096
static char crash_buf[CONFIG_PROCESSORS_MAX][CRASH_BUF_SIZE]
    __attribute__((aligned(4096)));
static unsigned crash_buf_used[CONFIG_PROCESSORS_MAX];
static unsigned char crash_mode_active[CONFIG_PROCESSORS_MAX];
/* The CPUs that have joined the rendezvous. */
static _Atomic uint64_t crash_arrived_mask;

/* The CPUs online or committed to join when the first CPU began crashing, the
 * ones that get IPI_CRASH; 0 until then.
 */
static _Atomic uint64_t crash_peers_at_entry;

/* The CPUs that have entered crash handling; the rendezvous waits for these
 * too. A mask, so the wait loop reads one word instead of every CPU's flag.
 */
static _Atomic uint64_t crash_entered_mask;
static atomic_uint crash_printer_picked;

static inline void kprintf_lock(void)
{
    unsigned long s = r_sstatus();
    w_sstatus(s & ~SSTATUS_SIE); /* disable interrupts (idempotent if nested) */

    unsigned cpu = get_cpunum();
    if (in_kprintf[cpu]++ == 0) {
        /* Outermost call on this CPU — spin to acquire the global lock. */
        while (
            atomic_exchange_explicit(&kprintf_slock, 1, memory_order_acquire))
            __asm__ volatile("" ::: "memory");
        kprintf_saved_sstatus[cpu] = s;
    }

    /* Nested call (in_kprintf was non-zero) — outer holds the lock, fall
     * through. Do NOT touch kprintf_saved_sstatus: the outer already saved the
     * pre-disable state we'll need at outer-exit.
     */
}

static inline void kprintf_unlock(void)
{
    unsigned cpu = get_cpunum();
    if (--in_kprintf[cpu] == 0) {
        /* Outermost call ending — release the lock and restore interrupts. */
        unsigned long s = kprintf_saved_sstatus[cpu];
        atomic_store_explicit(&kprintf_slock, 0, memory_order_release);
        if (s & SSTATUS_SIE)
            w_sstatus(r_sstatus() | SSTATUS_SIE);
    }
    /* Nested unlock — outer still owns the lock; do nothing. */
}

/* kprintf_force_release — emergency reset of the per-CPU and global kprintf
 * bookkeeping. Used by the S-mode crash dump path in trap-handler.c when we
 * need to print from a CPU that may have been mid-way through a kprintf call
 * when it died. Resets the in_kprintf nesting counter to zero AND clears the
 * global slock. Subsequent kprintf calls on this CPU will see in_kprintf==0
 * (outermost) and try to acquire the slock fresh. Output may interleave with
 * other CPUs that grab the slock after our release — acceptable cost for making
 * the crash dump always print.
 */
void kprintf_force_release(void)
{
    in_kprintf[get_cpunum()] = 0;
    atomic_store_explicit(&kprintf_slock, 0, memory_order_release);
}

/* cpu_tag — the console prefix character for a CPU: 0-9, a-z, A-Z, then '?'. */
static char cpu_tag(unsigned cpu)
{
    if (cpu <= 9)
        return (char) ('0' + cpu);
    if (cpu <= 35)
        return (char) ('a' + cpu - 10);
    if (cpu <= 61)
        return (char) ('A' + cpu - 36);
    return '?';
}

/* Print [cpu] prefix if we're at the beginning of a line. Must be called while
 * holding kprintf_slock.
 */
static void emit_prefix(void)
{
    if (at_bol) {
        if (cpunum_ready && num_processors > 1) {
            unsigned cpu = get_cpunum();
            const char pfx[] = {
                [0] = '[',
                [1] = cpu_tag(cpu),
                [2] = ']',
                [3] = ' ',
            };
            con_write(pfx, 4);
        }
        at_bol = 0;
    }
}

/* Update at_bol by scanning the last character of the output. Must be called
 * while holding kprintf_slock.
 */
static void update_bol(const char *buf, size_t len)
{
    if (len > 0)
        at_bol = (buf[len - 1] == '\n');
}

/* Install the per-character output function. Parameters:
 *   prt : function called with each output character.
 */
void set_print_char(void (*prt)(int))
{
    print_char = prt;
}

/* Low-level display: output text via SBI DBCN write (one ECALL per buffer,
 * atomic from the guest's perspective — no byte-level interleaving even when
 * called concurrently with sbi_ecall_console_* from sibling harts).
 *
 * No \r insertion — the SBI console (OpenSBI) already converts \n to \r\n.
 * Adding \r here produces double carriage returns.
 *
 * The print_char function pointer (set via set_print_char) is no longer the
 * canonical output path — DBCN replaces it. print_char is retained for callers
 * that explicitly want a per-character API (none in-tree today).
 */
void con_write(const char *text, size_t len)
{
    if (len != 0)
        sbi_dbcn_write(text, (unsigned long) len);
}

/* Write a NUL-terminated string to the console. Parameters:
 *   text : string to write.
 */
void con_puts(const char *text)
{
    con_write(text, strlen(text));
}

/* Read one character from the console.
 * Return value: the character read.
 */
int con_getchar(void)
{
    return cpu_console_getc();
}

/* Display text on the console; if len is 0 the string length is used.
 * Parameters:
 *   text : text to display.
 *   len  : byte count, or 0 to use strlen(text).
 */
void dbug_display(const char *text, size_t len)
{
    if (len == 0)
        len = strlen(text);
    con_write(text, len);
}


/* kvsnprintf — full-featured kernel vsnprintf.
 *
 * Supported format specifiers:
 *   %d, %ld     — signed decimal (int / long)
 *   %u, %lu     — unsigned decimal (unsigned / unsigned long)
 *   %x, %lx, %X — hex (unsigned / unsigned long)
 *   %p          — pointer (16 hex digits, zero-padded)
 *   %P          — physical address (paddr_t)
 *   %s          — string (NULL → "(NULL)")
 *   %c          — character
 *   %o          — octal
 *   %b          — binary
 *   %%          — literal %
 *
 * Flags: '-' (left-align), '0' (zero-fill), '*' (dynamic width) Width: numeric
 * (e.g. %16x, %2d)
 */
size_t kvsnprintf(char *buf, size_t buflen, const char *fmt, va_list ap)
{
    size_t len;

    for (buf--, fmt--, len = 0; len < buflen && (*++buf = *++fmt); len++) {
        if (*buf == '%') {
            register char *str = 0;
            char fill = ' ';
            char leadchr = 0;
            int width = 0;
            int left = 0;
            int ignore = 0;
            int longflag = 0;
            size_t strwidth = 0;
            unsigned radix = 0;
            paddr_t value;
            char temp[35];

            do {
                value = 0;
                switch (*++fmt) {
                case '\0':
                    fmt--;
                    /* fall through */
                case '%':
                    str = buf;
                    break;

                case 'd':
                    radix = 10;
                    if (longflag) {
                        long li = va_arg(ap, long);
                        if (li < 0) {
                            leadchr = '-';

                            /* Negate after the conversion, not before: -li is
                             * undefined for LONG_MIN, and unsigned negation is
                             * defined and yields the same magnitude.
                             */
                            value = -(unsigned long) li;
                        } else {
                            value = (unsigned long) li;
                        }
                    } else {
                        int i = va_arg(ap, int);
                        if (i < 0) {
                            leadchr = '-';

                            /* Same as the long case above: -i is undefined for
                             * INT_MIN.
                             */
                            value = -(unsigned) i;
                        } else {
                            value = (unsigned) i;
                        }
                    }
                    break;

                case 'u':
                    radix = 10;
                    value = longflag ? va_arg(ap, unsigned long)
                                     : va_arg(ap, unsigned);
                    break;

                case 'b':
                    radix = 2;
                    value = va_arg(ap, unsigned);
                    break;

                case 'o':
                    radix = 8;
                    value = longflag ? va_arg(ap, unsigned long)
                                     : va_arg(ap, unsigned);
                    break;

                case 'P':
                    radix = 16;
                    value = va_arg(ap, paddr_t);
                    break;

                case 'p':
                    width = sizeof(void *) * 2;
                    fill = '0';
                    radix = 16;
                    value = (uintptr_t) va_arg(ap, void *);
                    break;

                case 'X':
                case 'x':
                    radix = 16;
                    value = longflag ? va_arg(ap, unsigned long)
                                     : va_arg(ap, unsigned);
                    break;

                case 'c':
                    str = temp;
                    str[0] = (char) va_arg(ap, int);
                    str[1] = '\0';
                    strwidth = 1;
                    break;

                case 's':
                    str = va_arg(ap, char *);
                    if (!str) {
                        str = "(NULL)";
                    }
                    break;

                case '-':
                    left = 1;
                    break;

                case '0':
                    fill = *fmt;
                    break;

                case 'l':
                    longflag++;
                    break;

                case '*':
                    width = va_arg(ap, int);
                    if (width < 0) {
                        width = -width;
                        left = 1;
                    }
                    ignore = 1;
                    break;

                default:
                    if (!ignore) {
                        if (*fmt >= '0' && *fmt <= '9') {
                            while (*fmt >= '0' && *fmt <= '9') {
                                width = (width * 10) + *fmt++ - '0';
                            }
                            fmt--;
                            ignore = 1;
                        }
                    }
                    break;
                }
                if (radix) {
                    unsigned shift = (((*fmt == 'X') ? 'A' : 'a') - '9') - 1;
                    paddr_t c;

                    str = &temp[sizeof temp - 1];
                    *str-- = '\0';

                    /* The enclosing "if (radix)" is what makes the divisions
                     * below safe, and a prover does not carry that across the
                     * loop boundary on its own.
                     */
                    /*@ loop invariant radix != 0; */
                    do {
                        c = value % radix + '0';
                        if (c > '9')
                            c += shift;
                        *str-- = (char) c;
                        value /= radix;
                    } while (value);
                    if (leadchr) {
                        *str = leadchr;
                    } else {
                        str++;
                    }
                    strwidth = (size_t) (&temp[sizeof temp - 1] - str);
                }
            } while (!str);
            if (str != buf) {
                len--;
                buf--;
                if (width) {
                    if (!strwidth) {
                        strwidth = strlen(str);
                    }
                    if ((size_t) width < strwidth) {
                        width = (int) strwidth;
                    }
                    if (!left) {
                        while (++len < buflen && width > strwidth) {
                            *++buf = fill;
                            strwidth++;
                        }
                        len--;
                    }
                }
                for (str--; ++len < buflen && (*++buf = *++str);) {
                    /* nothing to do */
                }
                if (*buf == '\0') {
                    buf--;
                }
                len--;
                while (++len < buflen && width > strwidth) {
                    *++buf = fill;
                    strwidth++;
                }
                len--;
            }
        }
    }
    return len;
}


/* kvsnprintf_unsigned — minimal "unsigned to decimal ASCII" helper. Used only
 * by the crash-dump printer (crash_rendezvous), which must not depend on the
 * full kvsnprintf formatter at the most fragile moment — a stray IRQ waking a
 * wfi-halted sibling could otherwise race against the formatter's internal
 * state.
 *
 * Returns bytes written (NOT including a NUL — the caller passes the length to
 * con_write).
 */
static size_t kvsnprintf_unsigned(char *buf, size_t buflen, unsigned val)
{
    char tmp[12];
    int ti = 0;
    if (val == 0) {
        if (buflen >= 1)
            buf[0] = '0';
        return 1;
    }
    while (val && ti < (int) sizeof tmp) {
        tmp[ti++] = (char) ('0' + (val % 10));
        val /= 10;
    }
    size_t out = 0;
    while (ti > 0 && out < buflen)
        buf[out++] = tmp[--ti];
    return out;
}

/* crash_buf_append — copy "n" bytes into this CPU's crash buffer. Caller is in
 * crash mode (no lock); appends are bounded by remaining space; overflow bytes
 * are dropped (not wrapped). No interleaving: each CPU writes only its own row.
 */
static void crash_buf_append(unsigned cpu, const char *src, size_t n)
{
    unsigned u = crash_buf_used[cpu];
    if (u >= CRASH_BUF_SIZE)
        return;
    unsigned space = CRASH_BUF_SIZE - u;
    unsigned copy = (n < space) ? (unsigned) n : space;
    for (unsigned i = 0; i < copy; i++)
        crash_buf[cpu][u + i] = src[i];
    crash_buf_used[cpu] = u + copy;
}

/* kprintf_flush_deferred — write out text kprintf_bounded() parked in this
 * CPU's crash buffer while the console lock was busy, and empty the buffer.
 * Caller holds the kprintf lock and is not in crash mode, so the row is private
 * to this CPU.
 */
static void kprintf_flush_deferred(unsigned cpu)
{
    unsigned used = crash_buf_used[cpu];

    if (!used)
        return;
    emit_prefix();
    con_write(crash_buf[cpu], used);
    update_bol(crash_buf[cpu], used);
    crash_buf_used[cpu] = 0;
}

/* kprintf_emit_locked — console write for one formatted line, then release the
 * kprintf lock. Caller holds it (kprintf_lock or kprintf_trylock_bounded).
 */
static void kprintf_emit_locked(const char *buffer, size_t len)
{
    kprintf_flush_deferred(get_cpunum());
    if (buffer[0] != '\n')
        emit_prefix();
    con_write(buffer, len);
    update_bol(buffer, len);
#ifdef CONFIG_STALL_DETECTOR
    kprintf_seq++;
#endif
    kprintf_unlock();
}

/* kprintf_emit — send one formatted line to the console.
 *
 * In crash mode (per-CPU) the line goes to this CPU's staging buffer instead:
 * no global lock, no console, and the rendezvous printer dumps every CPU's
 * buffer in order, so output cannot interleave. Otherwise it is written under
 * the kprintf lock with a [cpu] line prefix.
 */
static void kprintf_emit(const char *buffer, size_t len)
{
    unsigned cpu = get_cpunum();

    if (crash_mode_active[cpu]) {
        crash_buf_append(cpu, buffer, len);
        return;
    }
    kprintf_lock();
    kprintf_emit_locked(buffer, len);
}

/* Formatted kernel output to the console.
 *
 * Formats into a fixed buffer and hands it to kprintf_emit. Parameters:
 *   fmt : printf-style format string, followed by its arguments.
 * Return value: number of characters formatted.
 */
int kprintf(const char *fmt, ...)
{
    char buffer[256];
    va_list ap;
    size_t len;

    va_start(ap, fmt);
    len = kvsnprintf(buffer, sizeof buffer, fmt, ap);
    va_end(ap);

    kprintf_emit(buffer, len);
    return (int) len;
}

/* kprintf_rl_admit — decide whether a rate-limited report prints; see
 * KPRINTF_RATELIMITED in log.h.
 *
 * The first KPRINTF_RL_BURST events print. After that an event prints only when
 * it is a multiple of KPRINTF_RL_INTERVAL past the burst and no line from this
 * site printed in the last second. A site that fires N times in a boot thus
 * prints at most KPRINTF_RL_BURST + N / KPRINTF_RL_INTERVAL lines, and once
 * past the burst no more than one a second however hard it is driven.
 *
 * No lock: the count is one relaxed fetch-add, and the once-a-second slot is
 * claimed with a compare-exchange, so of two harts racing for it one prints and
 * the other drops. The second is between admissions, not console writes: a
 * writer delayed for longer than that can land next to the following one, and
 * its suppressed count then reads 0. Before the timebase is known (early
 * startup) only the count applies. Parameters:
 *   rl   : the call site's state, zero-initialized.
 *   seen : out, this event's number at the site, counted from 1; may be NULL.
 *   last : out, the number of the previous printed event (0 if none); may be
 *          NULL.
 * Return value: true when the caller should print.
 */
bool kprintf_rl_admit(struct kprintf_ratelimit *rl,
                      unsigned long *seen,
                      unsigned long *last)
{
    unsigned long n = __atomic_add_fetch(&rl->events, 1, __ATOMIC_RELAXED);
    unsigned long now, next, prev;

    if (n > KPRINTF_RL_BURST) {
        if ((n - KPRINTF_RL_BURST) % KPRINTF_RL_INTERVAL != 0)
            return false;
        now = r_time();
        next = __atomic_load_n(&rl->next_tick, __ATOMIC_RELAXED);
        if ((long) (now - next) < 0 ||
            !__atomic_compare_exchange_n(&rl->next_tick, &next,
                                         now + kopt.timebase_freq, false,
                                         __ATOMIC_RELAXED, __ATOMIC_RELAXED))
            return false;
    }

    /* Advance printed_at monotonically: an admitted event delayed behind a
     * later one must not move it back.
     */
    prev = __atomic_load_n(&rl->printed_at, __ATOMIC_RELAXED);
    while (prev < n &&
           !__atomic_compare_exchange_n(&rl->printed_at, &prev, n, false,
                                        __ATOMIC_RELAXED, __ATOMIC_RELAXED))
        ;
    if (seen)
        *seen = n;
    if (last)
        *last = prev;
    return true;
}

/* kprintf_rl_print — print an event kprintf_rl_admit admitted.
 *
 * The last burst line says further reports are rate-limited; each later line
 * carries the number dropped since the previous one and the running total.
 * Parameters:
 *   seen, last : as kprintf_rl_admit returned them.
 *   fmt        : printf-style format string, followed by its arguments.
 */
void kprintf_rl_print(unsigned long seen,
                      unsigned long last,
                      const char *fmt,
                      ...)
{
    /* Room kept for the appended note: 64 characters fit it even with both
     * counts at 20 digits.
     */
    enum { NOTE_ROOM = 64 };
    char buffer[256]; /* kprintf's size: no deeper trap-stack use */
    va_list ap;
    size_t len;

    va_start(ap, fmt);
    len = kvsnprintf(buffer, sizeof buffer - NOTE_ROOM, fmt, ap);
    va_end(ap);

    if (seen >= KPRINTF_RL_BURST && len && buffer[len - 1] == '\n')
        len--;
    if (seen == KPRINTF_RL_BURST)
        len += (size_t) ksnprintf(buffer + len, NOTE_ROOM,
                                  " (further reports rate-limited)\n");
    else if (seen > KPRINTF_RL_BURST)
        len += (size_t) ksnprintf(buffer + len, NOTE_ROOM,
                                  " (%lu suppressed, %lu total)\n",
                                  seen > last ? seen - last - 1 : 0, seen);
    kprintf_emit(buffer, len);
}

#ifdef CONFIG_STALL_DETECTOR
/* kprintf_trylock_bounded — kprintf_lock() that gives up after "spins" failed
 * attempts instead of waiting for ever.
 *
 * Returns true with the lock held (the caller must release it through
 * kprintf_unlock), false with the lock NOT held and the interrupt state
 * restored. It never clears or steals another hart's hold on kprintf_slock
 * (contrast stall_dump()).
 */
static bool kprintf_trylock_bounded(unsigned spins)
{
    unsigned long s = r_sstatus();
    unsigned cpu;

    w_sstatus(s & ~SSTATUS_SIE);
    cpu = get_cpunum();
    if (in_kprintf[cpu]++ == 0) {
        while (
            atomic_load_explicit(&kprintf_slock, memory_order_relaxed) ||
            atomic_exchange_explicit(&kprintf_slock, 1, memory_order_acquire)) {
            if (spins-- == 0) {
                in_kprintf[cpu]--;
                if (s & SSTATUS_SIE)
                    w_sstatus(r_sstatus() | SSTATUS_SIE);
                return false;
            }
            __asm__ volatile("" ::: "memory");
        }
        kprintf_saved_sstatus[cpu] = s;
    }
    return true;
}

/* kprintf_flush_parked — write out a line parked by kprintf_bounded() if the
 * console lock is free now, without waiting for another kprintf on this CPU.
 * Same context rules as kprintf_bounded(). Costs a load when nothing is parked.
 */
void kprintf_flush_parked(unsigned spins)
{
    unsigned cpu = get_cpunum();

    if (!crash_buf_used[cpu] || crash_mode_active[cpu] ||
        !kprintf_trylock_bounded(spins))
        return;
    kprintf_flush_deferred(cpu);
    kprintf_unlock();
}

/* kprintf_bounded — kprintf() that cannot block behind a wedged console owner.
 * If the kprintf lock is not won within "spins" attempts, the formatted line is
 * parked in this CPU's crash buffer and written out under the lock by this
 * CPU's next kprintf()/kprintf_raw()/kprintf_bounded() (bounded by
 * CRASH_BUF_SIZE; overflow is dropped). The caller runs with interrupts
 * disabled (timer-trap context), so nothing on this CPU can touch the buffer
 * meanwhile. Parameters:
 *   spins : lock acquisition attempts before parking the line.
 *   fmt   : printf-style format string, followed by its arguments.
 * Return value: number of characters formatted.
 */
int kprintf_bounded(unsigned spins, const char *fmt, ...)
{
    char buffer[256];
    va_list ap;
    size_t len;
    unsigned cpu = get_cpunum();

    va_start(ap, fmt);
    len = kvsnprintf(buffer, sizeof buffer, fmt, ap);
    va_end(ap);

    if (crash_mode_active[cpu] || !kprintf_trylock_bounded(spins))
        crash_buf_append(cpu, buffer, len);
    else
        kprintf_emit_locked(buffer, len);
    return (int) len;
}

/* kprintf_emergency — kprintf_bounded() for a line that must reach the console
 * even if the lock owner never lets go (a hart wedged mid-print). With the lock
 * won in "spins" attempts it is an ordinary locked print. Otherwise the line
 * goes straight to the console in one SBI write, with a leading newline so it
 * does not continue the owner's half-written line and a "[cpu] " prefix,
 * without the lock, the parked buffer, or any shared console state (at_bol is
 * the lock owner's to update, so it is left alone): it can interleave with, but
 * not corrupt, what the owner prints, and does not allocate. Same context rules
 * as kprintf_bounded(). Parameters:
 *   spins : lock acquisition attempts before the lock-free write.
 *   fmt   : printf-style format string, followed by its arguments.
 * Return value: number of characters formatted.
 */
int kprintf_emergency(unsigned spins, const char *fmt, ...)
{
    char buffer[256];
    char out[sizeof buffer + 8];
    va_list ap;
    size_t len, n = 0;
    unsigned cpu = get_cpunum();

    va_start(ap, fmt);
    len = kvsnprintf(buffer, sizeof buffer, fmt, ap);
    va_end(ap);

    if (!crash_mode_active[cpu] && kprintf_trylock_bounded(spins)) {
        kprintf_emit_locked(buffer, len);
        return (int) len;
    }
    out[n++] = '\n';
    if (cpunum_ready && num_processors > 1) {
        out[n++] = '[';
        out[n++] = cpu_tag(cpu);
        out[n++] = ']';
        out[n++] = ' ';
    }
    for (size_t i = 0; i < len && n < sizeof out; i++)
        out[n++] = buffer[i];
    con_write(out, n);
    return (int) len;
}

#endif /* CONFIG_STALL_DETECTOR */

#ifdef CONFIG_DEBUG_INJECT
/* kprintf_inject_hold — hold the console lock for "ms" milliseconds with
 * interrupts off, then release it: the stalled-lock-owner fixture for
 * kprintf_emergency(). Runs in the IPI_DEBUG_SPIN handler (trap context, SIE
 * already off). The two "inject: console lock" lines bracket the hold, so a
 * host check can tell a report printed during it from one parked until after
 * it. A debug injection only; other harts that print meanwhile wait.
 */
void kprintf_inject_hold(unsigned ms)
{
    uint64_t end = r_time() + (uint64_t) ms * (qtimeptr->cycles_per_sec / 1000);

    kprintf("inject: console lock held for %u ms\n", ms);
    kprintf_lock();
    while (r_time() < end)
        __asm__ volatile("" ::: "memory");
    kprintf_unlock();
    kprintf("inject: console lock released\n");
}
#endif /* CONFIG_DEBUG_INJECT */

/* kprintf_raw — output a pre-formatted buffer under kprintf's lock. Used by
 * ksys_dbgprint to avoid format-string parsing overhead.
 */
void kprintf_raw(const char *buf, size_t len)
{
    unsigned cpu = get_cpunum();
    if (crash_mode_active[cpu]) {
        crash_buf_append(cpu, buf, len);
        return;
    }

    kprintf_lock();
    kprintf_flush_deferred(cpu);
    emit_prefix();
    con_write(buf, len);
    update_bol(buf, len);
#ifdef CONFIG_STALL_DETECTOR
    kprintf_seq++;
#endif
    kprintf_unlock();
}

/* Format into a caller-supplied buffer (1000-byte cap). Parameters:
 *   buffer : destination buffer.
 *   fmt    : printf-style format string, followed by its arguments.
 * Return value: number of characters written.
 */
int ksprintf(char *buffer, const char *fmt, ...)
{
    va_list ap;
    int len;

    va_start(ap, fmt);
    len = (int) kvsnprintf(buffer, 1000, fmt, ap);
    va_end(ap);
    return len;
}

/* Format into a caller-supplied buffer with an explicit size bound. Parameters:
 *   buffer : destination buffer.
 *   len    : buffer size in bytes.
 *   fmt    : printf-style format string, followed by its arguments.
 * Return value: number of characters written.
 */
int ksnprintf(char *buffer, size_t len, const char *fmt, ...)
{
    va_list ap;
    int ret;

    va_start(ap, fmt);
    ret = (int) kvsnprintf(buffer, len, fmt, ap);
    va_end(ap);
    return ret;
}


/* crash_mark_entered — set this CPU's crash mode flag and record it in
 * crash_entered_mask, before it signals a sibling or joins the rendezvous.
 */
static void crash_mark_entered(unsigned cpu)
{
    __atomic_store_n(&crash_mode_active[cpu], 1, __ATOMIC_RELEASE);
    atomic_fetch_or_explicit(&crash_entered_mask, 1ULL << cpu,
                             memory_order_release);
}

/* crash_enter_self — set THIS CPU's crash-mode flag and broadcast IPI_CRASH to
 * every sibling. Internal helper used by both crash_recursive_check
 * (no-recursive path) and kprintf_crash_mode_on (entry from crash_halt).
 * Atomic-flag-then-broadcast in one place closes the race window where a
 * sibling could see this CPU's flag as 0 between the recursive check and the
 * broadcast.
 *
 * Idempotent: re-entry just returns without re-broadcasting.
 */
static void crash_enter_self(void)
{
    unsigned me = get_cpunum();

    if (crash_mode_active[me])
        return;

    crash_mark_entered(me);

    /* The first CPU in fixes the set of CPUs the rendezvous is for; a later one
     * signals that same set.
     */
    uint64_t set = 0, mine = cpu_online_snapshot() | (1ULL << me);

    /* A committed AP will reach its interrupt handler even if its online bit
     * has not been published yet. Dropped and parked CPUs cannot.
     */
    for (unsigned cpu = 0; cpu < num_processors; ++cpu)
        if (__atomic_load_n(&smp_slot[cpu], __ATOMIC_ACQUIRE) ==
            SMP_SLOT_JOINED)
            mine |= 1ULL << cpu;
    mine &= __atomic_load_n(&legal_cpu_bitmask, __ATOMIC_ACQUIRE);
    mine |= 1ULL << me;
    if (atomic_compare_exchange_strong_explicit(&crash_peers_at_entry, &set,
                                                mine, memory_order_acq_rel,
                                                memory_order_acquire))
        set = mine;

    /* Broadcast quiesce IPI to every sibling. Each peer's IPI handler calls
     * crash_quiesce_self(), which appends a brief snapshot to its own per-CPU
     * buffer and joins the rendezvous — the printer then dumps every CPU's
     * buffer in CPU-id order.
     */
    for (unsigned cpu = 0; cpu < num_processors; cpu++) {
        if (cpu != me && cpu_in_mask(set, cpu))
            send_ipi(cpu, IPI_CRASH);
    }
}

/* crash_recursive_check — re-entry guard for the S-mode crash dump.
 *
 * Call once at the very top of any large diagnostic block — BEFORE the
 * diagnostic torrent of kprintfs and BEFORE kprintf_crash_mode_on.
 *
 * Two paths:
 *   - Recursive trap: this CPU's crash-mode flag is already set →
 *     append a one-line marker (the inner trap's scause/sepc/stval —
 *     enough to identify it) and dive straight into crash_rendezvous,
 *     so the outer's accumulated diagnostic still gets dumped by the
 *     rendezvous printer with the marker tucked in.  Never returns.
 *
 *   - First entry on this CPU: set the flag + broadcast IPI_CRASH to
 *     every sibling RIGHT NOW (closing the race window where a peer
 *     could see flag=0 between this check and a later set).  Then
 *     return to the caller, which proceeds with the diagnostic.  The
 *     subsequent kprintf_crash_mode_on() call is a no-op.
 */
void crash_recursive_check(unsigned long scause,
                           unsigned long sepc,
                           unsigned long stval)
{
    if (crash_mode_active[get_cpunum()]) {
        kprintf(
            "\n!! recursive trap on cpu %u during crash dump: "
            "scause=%lu sepc=%lx stval=%lx !!\n",
            get_cpunum(), scause, sepc, stval);
        crash_rendezvous();
        /* NOTREACHED */
    }
    crash_enter_self();
}

/* kprintf_crash_mode_on — flip THIS CPU into per-CPU buffered mode AND
 * broadcast IPI_CRASH to every sibling. Idempotent — re-entry returns without
 * re-broadcasting. Without the broadcast, sibling CPUs keep running user
 * threads, hit related broken state, fault, and can trip a thread_destroy →
 * intrevent_flush self-deadlock.
 */
void kprintf_crash_mode_on(void)
{
    crash_enter_self();
}

/* crash_quiesce_self — IPI_CRASH handler entry point.
 *
 * Called from the riscv ipi_handler when a sibling CPU is crashing and
 * broadcast IPI_CRASH to us. We:
 *   1. Note we've been quiesced (set our per-CPU crash_mode flag —
 *      kprintfs from now on go to our per-CPU buffer, not the
 *      console).  Skip the IPI re-broadcast that
 *      kprintf_crash_mode_on() would otherwise do (set the flag
 *      directly).
 *   2. Snapshot a brief identity line into our buffer: where we
 *      were when the IPI hit (sepc), and what thread was current.
 *   3. Join the rendezvous and halt.  The printer (whichever CPU
 *      wins the cmpxchg in crash_rendezvous) dumps every per-CPU
 *      buffer in order.
 *
 * If we are ourselves already in a crash dump (e.g. two CPUs crashed
 * near-simultaneously and IPI'd each other), just join the rendezvous directly
 * — our buffer already has our own dump.
 */
void crash_quiesce_self(void)
{
    unsigned cpu = get_cpunum();

    if (crash_mode_active[cpu]) {
        crash_rendezvous();
        /* NOTREACHED */
    }
    crash_mark_entered(cpu);

    /* Brief identity snapshot. sepc CSR still holds the PC at IPI entry — we
     * haven't touched it. The active thread, if any, was the dispatched thread
     * on this CPU.
     */
    {
        unsigned long pc = (unsigned long) r_sepc();
        kprintf("\n*** cpu %u quiesced by sibling crash IPI ***\n", cpu);
        kprintf("  sepc at IPI = 0x%lx\n", pc);

        thread_t *act = get_active_thread(cpu);
        if (act) {
            kprintf("  current thread: tid=%d pid=%d state=%d flags=0x%x\n",
                    (int) act->tid, act->process ? (int) act->process->pid : -1,
                    (int) act->state, (unsigned) act->flags);
        } else {
            kprintf("  current thread: <none>\n");
        }
    }

    crash_rendezvous();
    /* NOTREACHED */
}

/* The CPUs the rendezvous waits for: those online or committed to join when the
 * first CPU began crash handling and still eligible to run, plus every CPU that
 * has entered crash handling. The two sets differ: a CPU that crashes before it
 * joins was never online, and one that joins after the first crash is never
 * signalled and not waited for. Each CPU records itself in crash_entered_mask
 * before it signals a sibling or joins, so a waiter sees every CPU that can
 * arrive.
 */
static uint64_t crash_participants(void)
{
    return (atomic_load_explicit(&crash_peers_at_entry, memory_order_acquire) &
            __atomic_load_n(&legal_cpu_bitmask, __ATOMIC_ACQUIRE)) |
           atomic_load_explicit(&crash_entered_mask, memory_order_acquire);
}

/* Write a decimal number to the console. */
static void crash_put_uint(unsigned v)
{
    char dec[16];
    int dlen = (int) kvsnprintf_unsigned(dec, sizeof dec, v);

    con_write(dec, (size_t) dlen);
}

/* How long a crashing CPU waits for the other participants, and the iteration
 * cap that ends the wait whatever the time counter says.
 */
#define CRASH_RENDEZVOUS_MS 1000
#define CRASH_RENDEZVOUS_MAX_SPINS 500000000UL

/* crash_rendezvous — join the SMP crash rendezvous, then halt.
 *
 * Called once this CPU has finished accumulating its diagnostic into its
 * per-CPU crash_buf. Protocol:
 *
 *   1. set this CPU's bit in crash_arrived_mask — announce arrival.
 *   2. Spin so siblings still finishing their dumps catch up (bounded by
 *      this hart's time counter and an iteration cap, with no dependency
 *      on the timer subsystem, which itself may be broken at crash time).
 *   3. cmpxchg(crash_printer_picked, 0 -> 1).  Whoever wins is the
 *      printer; the rest just halt.  Their bytes still get printed —
 *      the printer walks every CPU's buffer.
 *   4. Printer: emit a header, then for each CPU with a non-empty
 *      buffer emit "--- cpu N (M bytes) ---" + the bytes.  Output
 *      goes through con_write (lock-free), so the rendezvous
 *      serialization is the only thing keeping the channel single-
 *      writer — which is exactly what we want.
 *   5. cpu_halt — never returns.
 *
 * Failure-tolerance:
 *   - A CPU that never arrives: the arrival count never reaches
 *     crash_participants(), but the bounded spin still expires; this CPU
 *     then becomes the printer of the buffers that are filled.
 *     Diagnostic still prints.
 *   - Every participant arrives: the spin exits early; first CPU to win
 *     the printer cmpxchg dumps for everyone.
 *   - One CPU is wedged and never reaches the rendezvous: its buffer
 *     is empty (used == 0) and the printer skips it.  The other CPUs'
 *     dumps still print.
 */
void crash_rendezvous(void)
{
    uint64_t set, arrived;

    /* Every caller has already marked this CPU entered (crash_enter_self,
     * crash_quiesce_self or kprintf_crash_mode_on), so the participant set
     * below includes it.
     */
    atomic_fetch_or_explicit(&crash_arrived_mask, 1ULL << get_cpunum(),
                             memory_order_acq_rel);

    /* Bounded spin — long enough that any sibling crashing in the same window
     * will reach this rendezvous. A lone crasher is its whole participant set
     * and does not wait at all. The bound is CRASH_RENDEZVOUS_MS of this hart's
     * own time counter and at least 50M iterations: an iteration count alone is
     * tens of milliseconds, which a sibling whose virtual CPU the host has
     * descheduled can miss, and its dump is then printed as absent. The hard
     * iteration cap ends the wait when the counter does not advance. When the
     * clock rate is not known yet the time bound is zero, and the wait is the
     * 50M iterations alone.
     */
    uint64_t cps = qtimeptr ? qtimeptr->cycles_per_sec : 0;
    uint64_t wait = cps / 1000 * CRASH_RENDEZVOUS_MS;
    uint64_t t0 = r_time();

    arrived = atomic_load_explicit(&crash_arrived_mask, memory_order_acquire);
    set = crash_participants();
    for (volatile unsigned long i = 0;
         (arrived & set) != set &&
         (i < 50000000UL ||
          (i < CRASH_RENDEZVOUS_MAX_SPINS && r_time() - t0 < wait));
         i++) {
        __asm__ volatile("" ::: "memory");
        arrived =
            atomic_load_explicit(&crash_arrived_mask, memory_order_acquire);
        set = crash_participants();
    }

    if (atomic_exchange_explicit(&crash_printer_picked, 1,
                                 memory_order_acquire) == 0) {
        /* Emit the per-CPU header pieces character-by-character via print_char
         * (which is what con_write does) — but compose the header inline rather
         * than via ksnprintf, so a stray IRQ on another (supposedly halted) CPU
         * that wakes from its wfi and touches the console can't race against a
         * single ksnprintf sprintf-then-write sequence. Each con_write call
         * emits a short fixed string atomically from this CPU's perspective.
         */
        con_write("\r\n=== CRASH DUMP ===\r\n", 22);
        /* Walk every row, not num_processors: that is still 0 while startup
         * runs, so a startup crash() would print an empty dump. Rows nobody
         * wrote are skipped below.
         */
        for (unsigned cpu = 0; cpu < CONFIG_PROCESSORS_MAX; cpu++) {
            unsigned u = crash_buf_used[cpu];
            if (u == 0)
                continue;

            /* "--- cpu N (M bytes) ---\r\n" composed inline */
            con_write("\r\n--- cpu ", 10);
            crash_put_uint(cpu);
            con_write(" (", 2);
            crash_put_uint(u);
            con_write(" bytes) ---\r\n", 13);

            con_write(crash_buf[cpu], u);
        }
        /* How many CPUs this wait was for, and how many had arrived */
        con_write("\r\ncrash rendezvous: ", 20);
        crash_put_uint(popcount64(arrived & set));
        con_write(" arrived, ", 10);
        crash_put_uint(popcount64(set));
        con_write(" expected", 9);
        con_write("\r\n=== END CRASH DUMP ===\r\n", 26);
    }
    cpu_halt();
}

/* Print a final diagnostic message and halt the system.
 *
 * Enters per-CPU crash mode, appends the formatted message and the tracelogger
 * ring dump to this CPU's crash buffer, then joins the SMP crash rendezvous
 * (which dumps every CPU's buffer and halts). Never returns. Parameters:
 *   fmt : printf-style format string (may be NULL), then its arguments.
 */
void crash_halt(const char *fmt, ...)
{
    /* Enter crash mode: subsequent kprintf calls on this CPU append to the
     * per-CPU staging buffer instead of going to the console. Idempotent — the
     * trap-handler diagnostic block may already have flipped us into crash mode
     * before reaching here.
     */
    kprintf_crash_mode_on();

    if (fmt) {
        char buffer[256];
        size_t len;
        va_list ap;
        va_start(ap, fmt);
        len = kvsnprintf(buffer, sizeof buffer, fmt, ap);
        va_end(ap);
        crash_buf_append(get_cpunum(), buffer, len);
        crash_buf_append(get_cpunum(), "\r\n", 2);
    } else {
        const char *m = "CRASH (no message)\r\n";
        crash_buf_append(get_cpunum(), m, 20);
    }

    /* Dump the tracelogger ring buffer. Mirrors the trap-handler crash path —
     * without this, direct crash() calls from kernel invariant checks
     * (pril_validate, etc.) lose the recent trace context that's critical for
     * diagnosing SMP races.
     */
    {
        extern void tr_dump_to_console(void);
        tr_dump_to_console();
    }

    crash_rendezvous();
    /* NOTREACHED */
}
