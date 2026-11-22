/*
 * Host-side test for the serial driver's reply-then-consume rule.
 *
 * The window between the liveness check and the reply (a reader dying there)
 * cannot be hit on demand from a guest test, so this compiles the shared
 * serial service and stubs the IPC calls: a failed msg_reply must leave the
 * cooked byte for the next reader, a successful one must consume it. It also
 * drives the _IO_NOTIFY handler: readiness is level-sensitive, an arm is woken
 * once by input, a repeated entry asking for nothing keeps it, and a close
 * drops it. The shared notification lists (lib/libc/ioserv/notify-list.c) are
 * linked in.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <assert.h>
#include <stdio.h>

#include <stdint.h>

/* The hooks the service expects from its driver, declared before the include. */
static void tx_byte(uint8_t c);
static int rxbuf_get(void);
static void tx_drain(void);
static int tx_break(void);

#include <sys/priority-bands.h>
static int notification_coid;
#define NOTIFICATION_CODE_RX (_NOTIFICATION_CODE_MINAVAIL + 1)
#include "serial-service.h"

/* Serial timer delivery is exercised by the guest test. */
int timer_create(clockid_t id, struct sigevent *event, timer_t *timer)
{
    (void) id;
    (void) event;
    *timer = 1;
    return 0;
}

int timer_settime(timer_t timer,
                  int flags,
                  const struct itimerspec *spec,
                  struct itimerspec *old)
{
    (void) timer;
    (void) flags;
    (void) spec;
    (void) old;
    return 0;
}

static int rx_next = -1;      /* byte rxbuf_get() hands out once, or -1 */
static int reply_result;      /* what the stubbed msg_reply returns */
static int reply_calls;
static int reply_byte = -1;

static void tx_byte(uint8_t c) { (void) c; }
static void tx_drain(void) {}
static int tx_break(void) { return EOK; }

static int rxbuf_get(void)
{
    int c = rx_next;

    rx_next = -1;
    return c;
}

int msg_info(int rcvid, struct _msg_info *info)
{
    (void) rcvid;
    memset(info, 0, sizeof *info);
    info->scoid = 7;
    return 0;
}

int msg_error(int rcvid, int err)
{
    (void) rcvid;
    (void) err;
    return 0;
}

/* The job-control query goes to roottask; no session is bound in this test, so
 * the stub answers like a roottask with nothing to report.
 */
int msg_sendnc(int coid, const void *smsg, size_t sbytes, void *rmsg, size_t rbytes)
{
    (void) coid;
    (void) smsg;
    (void) sbytes;
    memset(rmsg, 0, rbytes);
    return 0;
}

static int host_errno;
int *__get_errno_ptr(void)
{
    return &host_errno;
}

int msg_reply(int rcvid, int status, const void *msg, int bytes)
{
    (void) rcvid;
    (void) status;
    assert(bytes == 1);
    reply_byte = *(const unsigned char *) msg;
    reply_calls++;
    return reply_result;
}

static int deliver_calls;
static int deliver_rcvid = -1;

int msg_deliver_event(int rcvid, const struct sigevent *event)
{
    (void) event;
    deliver_rcvid = rcvid;
    deliver_calls++;
    return 0;
}

/* One _IO_NOTIFY from client thread rcvid on open coid; returns the handler's
 * status and leaves the satisfied conditions in *sat.
 */
static int notify(int rcvid, int coid, int action, unsigned cond, unsigned *sat)
{
    ioserv_context_t ctp;
    io_notify_t msg;
    int rc;

    memset(&ctp, 0, sizeof ctp);
    memset(&msg, 0, sizeof msg);
    ctp.rcvid = rcvid;
    ctp.info.scoid = 7;
    ctp.info.coid = coid;
    msg.i.type = _IO_NOTIFY;
    msg.i.action = action;
    msg.i.flags = cond;
    SIGEV_NOTIFICATION_INIT(&msg.i.event, 5, -1, 0, 0);
    rc = ser_notify(&ctp, &msg, NULL);
    *sat = msg.o.flags;
    return rc;
}

static void close_open(int coid)
{
    ioserv_context_t ctp;

    memset(&ctp, 0, sizeof ctp);
    ctp.info.scoid = 7;
    ctp.info.coid = coid;
    (void) ser_close_dup(&ctp, NULL, NULL);
}

/* Nodes on the input list: all of them, or only those still armed. */
static int input_nodes(int armed_only)
{
    const ioserv_notify_event_t *e;
    int n = 0;

    for (e = ser_notify_lists[IOSERV_NOTIFY_INPUT].list; e; e = e->next)
        if (!armed_only || SIGEV_GET_TYPE(&e->event) != SIGEV_NONE)
            n++;
    return n;
}

static void test_notify(void)
{
    unsigned sat;

    serial_flush_input();

    /* Output never waits; idle input arms and answers EAGAIN. */
    assert(notify(10, 3, _NOTIFY_ACTION_CONDARM, _NOTIFY_COND_OUTPUT, &sat) !=
               EAGAIN &&
           sat == _NOTIFY_COND_OUTPUT);
    assert(notify(10, 3, _NOTIFY_ACTION_CONDARM, _NOTIFY_COND_INPUT, &sat) ==
           EAGAIN);
    assert(input_nodes(1) == 1);

    /* A repeated entry for the descriptor asking for nothing keeps the arm. */
    assert(notify(10, 3, _NOTIFY_ACTION_CONDARM, 0, &sat) == EAGAIN);
    assert(input_nodes(1) == 1);

    /* A byte wakes the poller once and disarms it. */
    rx_next = 'p';
    rx_notification_handler(NULL, 0, 0, NULL);
    assert(deliver_calls == 1 && deliver_rcvid == 10);
    assert(input_nodes(1) == 0);

    /* Data already queued is reported at once: no arm, no notification. */
    assert(notify(11, 3, _NOTIFY_ACTION_CONDARM, _NOTIFY_COND_INPUT, &sat) !=
               EAGAIN &&
           (sat & _NOTIFY_COND_INPUT));
    assert(input_nodes(1) == 0);
    serial_flush_input();

    /* A partial canonical line is not readable; its newline makes it so. */
    ser_termios.c_lflag |= ICANON;
    assert(notify(12, 3, _NOTIFY_ACTION_CONDARM, _NOTIFY_COND_INPUT, &sat) ==
           EAGAIN);
    rx_next = 'q';
    rx_notification_handler(NULL, 0, 0, NULL);
    assert(deliver_calls == 1 && input_nodes(1) == 1);
    rx_next = '\n';
    rx_notification_handler(NULL, 0, 0, NULL);
    assert(deliver_calls == 2 && deliver_rcvid == 12);
    ser_termios.c_lflag &= ~(tcflag_t) ICANON;
    serial_flush_input();

    /* The POLL pass disarms; a close drops only its own open's arms. */
    assert(notify(13, 3, _NOTIFY_ACTION_CONDARM, _NOTIFY_COND_INPUT, &sat) ==
           EAGAIN);
    (void) notify(13, 3, _NOTIFY_ACTION_POLL, _NOTIFY_COND_INPUT, &sat);
    assert(sat == 0 && input_nodes(1) == 0);
    assert(notify(14, 3, _NOTIFY_ACTION_CONDARM, _NOTIFY_COND_INPUT, &sat) ==
           EAGAIN);
    assert(notify(15, 4, _NOTIFY_ACTION_CONDARM, _NOTIFY_COND_INPUT, &sat) ==
           EAGAIN);
    assert(input_nodes(1) == 2);
    close_open(4);
    assert(input_nodes(1) == 1 &&
           ser_notify_lists[IOSERV_NOTIFY_INPUT].list->coid == 3);
    close_open(3);
    assert(input_nodes(0) == 0);

    /* The poll pass and a ready reply allocate nothing, so they cannot fail
     * with ENOMEM: only recording an arm does.
     */
    assert(notify(16, 5, _NOTIFY_ACTION_POLL, _NOTIFY_COND_INPUT, &sat) !=
               ENOMEM &&
           input_nodes(0) == 0);
    assert(notify(16, 5, _NOTIFY_ACTION_CONDARM, _NOTIFY_COND_OUTPUT, &sat) !=
               EAGAIN &&
           sat == _NOTIFY_COND_OUTPUT &&
           ser_notify_lists[IOSERV_NOTIFY_OUTPUT].list == NULL);

    /* A disarmed arm no longer counts as a waiter. */
    assert(notify(17, 5, _NOTIFY_ACTION_CONDARM, _NOTIFY_COND_INPUT, &sat) ==
           EAGAIN);
    assert(ioserv_notify_armed(ser_notify_lists, IOSERV_NOTIFY_INPUT, 1));
    (void) notify(17, 5, _NOTIFY_ACTION_POLL, _NOTIFY_COND_INPUT, &sat);
    assert(!ioserv_notify_armed(ser_notify_lists, IOSERV_NOTIFY_INPUT, 1));
    close_open(5);
    assert(input_nodes(0) == 0);
    rx_next = 'r';
    rx_notification_handler(NULL, 0, 0, NULL);
    assert(deliver_calls == 2);
    serial_flush_input();
}

static void park_reader(void)
{
    pending_size = 1;
    pending_rcvid = 42;
    pending_scoid = 7;
    pending_coid = 3;
}

int main(void)
{
    serial_termios_init();
    ser_termios.c_lflag &= ~(tcflag_t) (ICANON | ECHO);

    /* Reply fails: the reader is gone, the byte survives for the next one. */
    park_reader();
    rx_next = 'a';
    reply_result = -1;
    rx_notification_handler(NULL, 0, 0, NULL);
    assert(reply_calls == 1 && reply_byte == 'a');
    assert(pending_rcvid == -1);
    assert(ser_input.tail != ser_input.head &&
           ser_input.queue[ser_input.tail % TTY_QUEUE] == 'a');

    /* The next reader gets that same byte, and a good reply consumes it. */
    park_reader();
    reply_result = 0;
    rx_notification_handler(NULL, 0, 0, NULL);
    assert(reply_calls == 2 && reply_byte == 'a');
    assert(ser_input.head == ser_input.tail);

    /* A later byte is not skipped or duplicated. */
    park_reader();
    rx_next = 'b';
    rx_notification_handler(NULL, 0, 0, NULL);
    assert(reply_calls == 3 && reply_byte == 'b');
    assert(ser_input.head == ser_input.tail);

    test_notify();

    printf("serial-reply: ok\n");
    return 0;
}
