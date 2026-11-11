/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * (c) 2026 RTMux contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <termios.h>

static bool termios_speed_valid(speed_t speed)
{
    static const speed_t valid[] = {
        B0,     B50,    B75,    B110,   B134,    B150,  B200,
        B300,   B600,   B1200,  B1800,  B2400,   B4800, B9600,
        B19200, B38400, B57600, B76800, B115200,
    };

    for (size_t i = 0; i < sizeof valid / sizeof valid[0]; i++) {
        if (valid[i] == speed)
            return true;
    }
    return false;
}

speed_t cfgetispeed(const struct termios *termios_p)
{
    return termios_p->c_ispeed;
}

speed_t cfgetospeed(const struct termios *termios_p)
{
    return termios_p->c_ospeed;
}

int cfsetispeed(struct termios *termios_p, speed_t speed)
{
    if (!termios_speed_valid(speed)) {
        errno = EINVAL;
        return -1;
    }
    termios_p->c_ispeed = speed;
    return 0;
}

int cfsetospeed(struct termios *termios_p, speed_t speed)
{
    if (!termios_speed_valid(speed)) {
        errno = EINVAL;
        return -1;
    }
    termios_p->c_ospeed = speed;
    return 0;
}
