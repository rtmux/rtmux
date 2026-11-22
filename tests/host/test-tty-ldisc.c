/* SPDX-License-Identifier: Apache-2.0 */
#include <assert.h>
#include <errno.h>
#include <string.h>
#include <sys/ioserv-ldisc.h>

int main(void)
{
    struct ioserv_ldisc ld = {0};
    struct termios t = {.c_lflag = ICANON | ECHOE};
    struct tty_read_clock clock = {0};
    struct tty_echo echo;
    unsigned used;
    char bytes[8];
    t.c_cc[VERASE] = 8;
    t.c_cc[VKILL] = 21;
    t.c_cc[VEOL] = '!';
    t.c_cc[VEOF] = 4;
    const char *s = "ab\bc!x\025d\004\004";
    while (*s) {
        ioserv_ldisc_feed(&ld, &t, *s++, &echo);
        assert(!echo.repeat);
    }
    assert(ioserv_ldisc_read(&ld, &t, &clock, 0, 0, bytes, 8, &used) == 3);
    assert(!memcmp(bytes, "ac!", 3));
    ld.tail += used;
    assert(ioserv_ldisc_read(&ld, &t, &clock, 0, 0, bytes, 8, &used) == 1 &&
           bytes[0] == 'd');
    ld.tail += used;
    assert(!ioserv_ldisc_read(&ld, &t, &clock, 0, 0, bytes, 8, &used) &&
           used == 1);
    ld.tail += used;

    ioserv_ldisc_feed(&ld, &t, 'x', &echo);
    struct termios raw = {.c_lflag = 0};
    raw.c_cc[VMIN] = 3;
    raw.c_cc[VTIME] = 1;
    ioserv_ldisc_set(&ld, &t, &raw, 0);
    assert(ioserv_ldisc_read(&ld, &raw, &clock, 0, 0, bytes, 8, &used) ==
           -EAGAIN);
    assert(clock.deadline == 100000000);
    ioserv_ldisc_feed(&ld, &raw, 'y', &echo);
    assert(ioserv_ldisc_read(&ld, &raw, &clock, 90000000, 0, bytes, 8, &used) ==
           -EAGAIN);
    assert(clock.deadline == 190000000);
    assert(ioserv_ldisc_read(&ld, &raw, &clock, 190000000, 0, bytes, 8,
                             &used) == 2);
    assert(used == 2 && !memcmp(bytes, "xy", 2));
    ioserv_ldisc_set(&ld, &raw, &t, 1);
    assert(ld.tail == ld.head && !ld.editing);
}
