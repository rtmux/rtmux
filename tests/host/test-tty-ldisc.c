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

    /* POSIX XBD 11.1.7: in case B (MIN>0, TIME=0) "the timer plays no role",
     * so a read timer a case-C read started must not end a read that a
     * tcsetattr has since made case B.
     */
    struct tty_read_clock c2 = {0};
    struct termios rt = {.c_lflag = 0};
    rt.c_cc[VMIN] = 0;
    rt.c_cc[VTIME] = 1;
    assert(ioserv_ldisc_read(&ld, &rt, &c2, 0, 0, bytes, 8, &used) == -EAGAIN);
    rt.c_cc[VMIN] = 2;
    rt.c_cc[VTIME] = 0;
    assert(ioserv_ldisc_read(&ld, &rt, &c2, 200000000, 0, bytes, 8, &used) ==
           -EAGAIN);
    ioserv_ldisc_feed(&ld, &rt, 'q', &echo);
    assert(ioserv_ldisc_read(&ld, &rt, &c2, 300000000, 0, bytes, 8, &used) ==
           -EAGAIN);
    ld.tail = ld.head;

    /* Case A: TIME is an inter-byte timer "activated after the first byte is
     * received". Once the byte that started it is gone (another reader took
     * it), the read waits for a byte again instead of returning nothing.
     */
    c2 = (struct tty_read_clock) {0};
    rt.c_cc[VMIN] = 2;
    rt.c_cc[VTIME] = 1;
    ioserv_ldisc_feed(&ld, &rt, 'a', &echo);
    assert(ioserv_ldisc_read(&ld, &rt, &c2, 0, 0, bytes, 8, &used) == -EAGAIN &&
           c2.deadline == 100000000);
    ld.tail = ld.head; /* the other reader */
    assert(ioserv_ldisc_read(&ld, &rt, &c2, 150000000, 0, bytes, 8, &used) ==
           -EAGAIN);
    assert(ioserv_ldisc_read(&ld, &rt, &c2, 500000000, 0, bytes, 8, &used) ==
           -EAGAIN);
    ioserv_ldisc_feed(&ld, &rt, 'b', &echo);
    assert(ioserv_ldisc_read(&ld, &rt, &c2, 550000000, 0, bytes, 8, &used) ==
           -EAGAIN && c2.deadline == 650000000);
    assert(ioserv_ldisc_read(&ld, &rt, &c2, 650000000, 0, bytes, 8, &used) == 1 &&
           bytes[0] == 'b');
    ld.tail += used;

    /* XBD 11.1.6: a canonical read returns "at most one line". A line that does
     * not fit in a nearly full queue is cut short (what is kept past
     * {MAX_INPUT} is implementation-defined) but keeps its delimiter, so the
     * next line is not glued onto it.
     */
    struct termios ct = {.c_lflag = ICANON};
    char big[TTY_QUEUE];
    for (int line = 0; line < 3; line++) {
        for (int i = 0; i < 199; i++)
            ioserv_ldisc_feed(&ld, &ct, 'x', &echo);
        ioserv_ldisc_feed(&ld, &ct, '\n', &echo);
    }
    assert(ld.head - ld.tail == TTY_QUEUE);
    for (int line = 0; line < 2; line++) {
        assert(ioserv_ldisc_read(&ld, &ct, &clock, 0, 0, big, sizeof big, &used) == 200 &&
               big[199] == '\n');
        ld.tail += used;
    }
    ioserv_ldisc_feed(&ld, &ct, 'a', &echo);
    ioserv_ldisc_feed(&ld, &ct, 'b', &echo);
    ioserv_ldisc_feed(&ld, &ct, '\n', &echo);
    int n = ioserv_ldisc_read(&ld, &ct, &clock, 0, 0, big, sizeof big, &used);
    assert(n == TTY_QUEUE - 400 && big[n - 1] == '\n' && big[n - 2] == 'x');
    ld.tail += used;
    assert(ioserv_ldisc_read(&ld, &ct, &clock, 0, 0, big, sizeof big, &used) == 3 &&
           !memcmp(big, "ab\n", 3));
    ld.tail += used;

    /* A queue with no room at all drops the line rather than half of it. */
    for (int line = 0; line < 2; line++) {
        for (int i = 0; i < 255; i++)
            ioserv_ldisc_feed(&ld, &ct, 'y', &echo);
        ioserv_ldisc_feed(&ld, &ct, '\n', &echo);
    }
    assert(ld.head - ld.tail == TTY_QUEUE);
    ioserv_ldisc_feed(&ld, &ct, 'z', &echo);
    ioserv_ldisc_feed(&ld, &ct, '\n', &echo);
    assert(ld.head - ld.tail == TTY_QUEUE && !ld.editing);

    /* A line ended by EOF has no delimiter: what fits is its first bytes. */
    ld.tail = ld.head - (TTY_QUEUE - 3); /* three free slots */
    unsigned mark = ld.head;
    struct termios et = ct;
    et.c_cc[VEOF] = 4;
    for (const char *p = "abcdef\004"; *p; p++)
        ioserv_ldisc_feed(&ld, &et, (unsigned char) *p, &echo);
    ld.tail = mark;
    assert(ioserv_ldisc_read(&ld, &et, &clock, 0, 0, big, sizeof big, &used) == 3 &&
           !memcmp(big, "abc", 3));

    /* Canonical markers must not count as raw input or end raw reads, even
     * when compaction crosses the end of the queue. */
    ld = (struct ioserv_ldisc) {.head = TTY_QUEUE - 2, .tail = TTY_QUEUE - 2};
    for (const char *p = "\004a\n\004b\n\004c"; *p; p++)
        ioserv_ldisc_feed(&ld, &et, (unsigned char) *p, &echo);
    rt.c_cc[VMIN] = 6;
    rt.c_cc[VTIME] = 0;
    ioserv_ldisc_set(&ld, &et, &rt, 0);
    assert(ld.head - ld.tail == 5);
    assert(ioserv_ldisc_read(&ld, &rt, &clock, 0, 0, bytes, sizeof bytes, &used) ==
           -EAGAIN);
    ioserv_ldisc_feed(&ld, &rt, 'd', &echo);
    assert(ioserv_ldisc_read(&ld, &rt, &clock, 0, 0, bytes, sizeof bytes, &used) == 6 &&
           used == 6 && !memcmp(bytes, "a\nb\ncd", 6));
    ld.tail += used;
    ioserv_ldisc_set(&ld, &rt, &et, 0);
    ioserv_ldisc_feed(&ld, &et, 4, &echo);
    ioserv_ldisc_set(&ld, &et, &rt, 0);
    assert(ioserv_ldisc_read(&ld, &rt, &clock, 0, 1, bytes, sizeof bytes, &used) ==
           -EAGAIN);
}
