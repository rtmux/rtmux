/* SPDX-License-Identifier: Apache-2.0 */
#ifndef RTMUX_IOSERV_LDISC_H
#define RTMUX_IOSERV_LDISC_H

#include <stdint.h>
#include <stddef.h>
#include <termios.h>
#include <time.h>

#define TTY_LINE 256
#define TTY_QUEUE 512

struct ioserv_ldisc {
    unsigned head, tail, editing;
    unsigned char line[TTY_LINE];
    uint16_t queue[TTY_QUEUE];
};

struct tty_echo {
    char bytes[3];
    unsigned len, repeat;
};

struct tty_read_clock {
    uint64_t deadline;
    unsigned available;
};

static inline uint64_t ioserv_tty_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t) ts.tv_sec * 1000000000 + (uint64_t) ts.tv_nsec;
}

void ioserv_ldisc_set(struct ioserv_ldisc *,
                      const struct termios *,
                      const struct termios *,
                      int flush);
void ioserv_ldisc_feed(struct ioserv_ldisc *,
                       const struct termios *,
                       unsigned char,
                       struct tty_echo *);
int ioserv_ldisc_read(const struct ioserv_ldisc *,
                      const struct termios *,
                      struct tty_read_clock *,
                      uint64_t now,
                      int nonblock,
                      char *out,
                      unsigned size,
                      unsigned *consume);

#endif
