/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * What the terminal services (the console and the serial drivers) share about
 * a hung-up line.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef RTMUX_SYS_IOSERV_TTY_H
#define RTMUX_SYS_IOSERV_TTY_H

#include <errno.h>
#include <limits.h>
#include <termios.h>
#include <sys/poll.h>
#include <sys/ioserv.h>

/* A line is hung up while its output speed is B0 (tcsetattr, POSIX 11.2.5),
 * until a speed is set again. A read on it returns end of file and a write
 * fails with EIO (POSIX 11.1.10), and a poll reports POLLHUP and never
 * POLLOUT. The caller holds whatever lock guards t.
 */
static inline int ioserv_tty_hung_up(const struct termios *t)
{
    return t->c_ospeed == B0;
}

/* The hangup code ioserv_notify takes for the line t: POLLHUP while it is hung
 * up, else none.
 */
static inline unsigned ioserv_tty_hup(const struct termios *t)
{
    return ioserv_tty_hung_up(t) ? POLLHUP : 0;
}

/* The errno of a write to the line t: EIO while it is hung up, else 0. */
static inline int ioserv_tty_write_err(const struct termios *t)
{
    return ioserv_tty_hung_up(t) ? EIO : 0;
}

/* After a new termios was stored in t: a line that is now hung up wakes every
 * poller armed on lists, a hangup-only one included, to see POLLHUP.
 */
static inline void ioserv_tty_hangup_wake(ioserv_notify_t *lists,
                                          const struct termios *t)
{
    if (!ioserv_tty_hung_up(t))
        return;
    for (int i = 0; i < IOSERV_NOTIFY_COUNT; i++)
        ioserv_notify_trigger(lists, INT_MAX, i);
}

#endif /* RTMUX_SYS_IOSERV_TTY_H */
