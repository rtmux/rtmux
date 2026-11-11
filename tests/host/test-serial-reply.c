/*
 * Host-side test for the serial driver's reply-then-consume rule.
 *
 * The window between the liveness check and the reply (a reader dying there)
 * cannot be hit on demand from a guest test, so this compiles the shared
 * serial service and stubs the IPC calls: a failed msg_reply must leave the
 * cooked byte for the next reader, a successful one must consume it.
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

#include "serial-service.h"

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

static void park_reader(void)
{
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
    assert(cooked_peek() == 'a');

    /* The next reader gets that same byte, and a good reply consumes it. */
    park_reader();
    reply_result = 0;
    rx_notification_handler(NULL, 0, 0, NULL);
    assert(reply_calls == 2 && reply_byte == 'a');
    assert(cooked_peek() == -1);

    /* A later byte is not skipped or duplicated. */
    park_reader();
    rx_next = 'b';
    rx_notification_handler(NULL, 0, 0, NULL);
    assert(reply_calls == 3 && reply_byte == 'b');
    assert(cooked_peek() == -1);

    printf("serial-reply: ok\n");
    return 0;
}
