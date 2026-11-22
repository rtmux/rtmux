/* SPDX-License-Identifier: Apache-2.0 */
#include <sys/ioserv-ldisc.h>
#include <errno.h>
#include <string.h>

#define TTY_END 256
#define TTY_EOF 512

static void append(struct ioserv_ldisc *ld, unsigned value)
{
    if (ld->head - ld->tail < TTY_QUEUE)
        ld->queue[ld->head++ % TTY_QUEUE] = (uint16_t) value;
}

void ioserv_ldisc_set(struct ioserv_ldisc *ld,
                      const struct termios *old,
                      const struct termios *next,
                      int flush)
{
    if (flush) {
        ld->tail = ld->head;
        ld->editing = 0;
    } else if ((old->c_lflag & ICANON) && !(next->c_lflag & ICANON)) {
        for (unsigned i = 0; i < ld->editing; i++)
            append(ld, ld->line[i]);
        ld->editing = 0;
    }
}

void ioserv_ldisc_feed(struct ioserv_ldisc *ld,
                       const struct termios *t,
                       unsigned char c,
                       struct tty_echo *echo)
{
    int canon = t->c_lflag & ICANON;
    *echo = (struct tty_echo) {0};
    if (c == '\r' && (t->c_iflag & ICRNL))
        c = '\n';
    if (canon && c && (c == t->c_cc[VERASE] || c == t->c_cc[VKILL])) {
        unsigned erase = c == t->c_cc[VKILL] ? ld->editing : !!ld->editing;
        ld->editing -= erase;
        if ((t->c_lflag & ECHO) && erase) {
            if (t->c_lflag & ECHOE)
                *echo = (struct tty_echo) {"\b \b", 3, erase};
            else if (c == t->c_cc[VKILL] && (t->c_lflag & ECHOK))
                *echo = (struct tty_echo) {"\r\n", 2, 1};
            else
                *echo = (struct tty_echo) {{(char) c}, 1, 1};
        }
        return;
    }
    int eof = canon && c && c == t->c_cc[VEOF];
    if (canon && (eof || c == '\n' || (c && c == t->c_cc[VEOL]))) {
        if (!eof)
            ld->line[ld->editing++] = c;
        if (!ld->editing)
            append(ld, TTY_EOF);
        else
            for (unsigned i = 0; i < ld->editing; i++)
                append(ld, ld->line[i] | (i + 1 == ld->editing ? TTY_END : 0));
        ld->editing = 0;
    } else if (canon) {
        if (ld->editing == TTY_LINE - 1)
            return;
        ld->line[ld->editing++] = c;
    } else {
        append(ld, c);
    }
    if ((t->c_lflag & ECHO) && !eof) {
        if (c == '\n')
            *echo = (struct tty_echo) {"\r\n", 2, 1};
        else if (c >= ' ' && c != 127)
            *echo = (struct tty_echo) {{(char) c}, 1, 1};
    }
}

/* Inspect a read without removing input. The service commits consume only
 * after a successful reply, so cancellation leaves the next reader its data. */
int ioserv_ldisc_read(const struct ioserv_ldisc *ld,
                      const struct termios *t,
                      struct tty_read_clock *clock,
                      uint64_t now,
                      int nonblock,
                      char *out,
                      unsigned size,
                      unsigned *consume)
{
    unsigned avail = ld->head - ld->tail;
    unsigned min = t->c_cc[VMIN] < size ? t->c_cc[VMIN] : size;
    uint64_t interval = (uint64_t) t->c_cc[VTIME] * 100000000;
    *consume = 0;
    if (!size)
        return 0;
    if (t->c_lflag & ICANON) {
        clock->deadline = 0;
        if (!avail)
            return -EAGAIN;
    } else if (nonblock) {
        if (!avail)
            return -EAGAIN;
    } else {
        if (interval && (!min || avail) &&
            (!clock->deadline || (min && clock->available != avail)))
            clock->deadline = now + interval;
        clock->available = avail;
        if (avail < (min ? min : !!interval) &&
            (!clock->deadline || now < clock->deadline))
            return -EAGAIN;
    }
    unsigned n = 0;
    while (n < size && *consume < avail) {
        unsigned v = ld->queue[(ld->tail + *consume) % TTY_QUEUE];
        if (v == TTY_EOF && n)
            break;
        ++*consume;
        if (v == TTY_EOF)
            break;
        out[n++] = (char) v;
        if (v & TTY_END)
            break;
    }
    return (int) n;
}
