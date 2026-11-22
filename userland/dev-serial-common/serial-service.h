/*
 * Shared serial I/O service handlers and line discipline.
 *
 * The including driver must provide:
 *   static void tx_byte(uint8_t c);
 *   static int rxbuf_get(void);
 *   static void tx_drain(void);   // block until the transmitter is idle
 *   static int tx_break(void);    // assert a line break; EOK or an errno
 *
 * Include exactly once per driver translation unit. The file-scope static state
 * below assumes one dispatch thread: ser_read, ser_notify and
 * rx_notification_handler run serially through the same dispatcher.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef RTMUX_DEVC_COMMON_SERIAL_SERVICE_H
#define RTMUX_DEVC_COMMON_SERIAL_SERVICE_H

#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/iomsg.h>
#include <sys/ioserv.h>
#include <sys/ioserv-tty.h>
#include <sys/dispatch.h>
#include <sys/rtmux-core.h>
#include <sys/procmsg.h>
#include <signal.h>
#include <termios.h>
#include <sys/ioserv-ldisc.h>
#include <devctl/common.h>
#include <devctl/tty.h>

static struct termios ser_termios;
static pid_t ser_pgrp;
static pid_t ser_sid; /* leader of the session this is the terminal of, or 0 */

/* The one parked reader. pending_scoid names its client connection, so a
 * disconnect (synthesized _IO_CLOSE) can drop a reader that died while parked;
 * pending_coid names the open it read from, so closing another open of the same
 * client leaves it parked. All three are -1 when no reader is parked.
 */
static int pending_rcvid = -1;
static int pending_scoid = -1;
static int pending_coid = -1;

static struct ioserv_ldisc ser_input;
static struct tty_read_clock pending_clock;
static unsigned pending_size;
static timer_t read_timer;
static int read_timer_created;

/* Armed readiness notifications (_IO_NOTIFY), one list per condition. Like the
 * rings, the lists are only touched by the dispatch thread, which also runs
 * rx_notification_handler: that serialization is the lock that keeps arming and
 * the RX edge from racing. Every condition fires on a single byte.
 */
static ioserv_notify_t ser_notify_lists[IOSERV_NOTIFY_COUNT];

/* Ask roottask a job control question about a client (see sys/procmsg.h).
 * Returns 0 or an errno; *out is filled for _PROC_JOBCTL_QUERY.
 */
static int ser_jobctl(unsigned subtype,
                      pid_t pid,
                      int arg,
                      struct _proc_jobctl_reply *out)
{
    struct _proc_jobctl msg;
    struct _proc_jobctl_reply rep;

    memset(&msg, 0, sizeof msg);
    msg.type = _PROC_JOBCTL;
    msg.subtype = (uint16_t) subtype;
    msg.pid = pid;
    msg.arg = arg;
    /* ipc-send: dev-serial -> roottask-core */
    if (msg_sendnc(ROOTTASK_COID, &msg, sizeof msg, &rep, sizeof rep) == -1)
        return errno;
    if (out)
        *out = rep;
    return 0;
}

/* Job control gate for a read (is_write = 0) or a write (is_write = 1) by the
 * client. Only the client's controlling terminal is policed, and only for a
 * background process; a write is policed only under TOSTOP.
 *
 * A background read gets EIO when its group is orphaned or it blocks/ignores
 * SIGTTIN; otherwise the group is sent SIGTTIN and the read fails with EINTR. A
 * write gets the same with SIGTTOU, except that a blocked/ignored SIGTTOU lets
 * it through.
 *
 * Returns 0 to proceed, else the errno to reply.
 */
static int ser_jobctl_gate(pid_t pid, int is_write)
{
    struct _proc_jobctl_reply q;
    unsigned hold = is_write ? _JOBCTL_HOLD_TTOU : _JOBCTL_HOLD_TTIN;

    if (is_write && !(ser_termios.c_lflag & TOSTOP))
        return 0;
    if (ser_jobctl(_PROC_JOBCTL_QUERY, pid, 0, &q) != 0 ||
        !(q.flags & _JOBCTL_CTTY) || (q.flags & _JOBCTL_FG))
        return 0;
    if (is_write && (q.flags & hold))
        return 0;
    if ((q.flags & _JOBCTL_ORPHAN) || (q.flags & hold))
        return EIO;
    (void) ser_jobctl(_PROC_JOBCTL_SIGBG, pid, is_write ? SIGTTOU : SIGTTIN,
                      NULL);
    return EINTR;
}

static void serial_flush_input(void);
static void ser_finish_read(void);

/* A termios signal character typed on a terminal that has a session: echo it,
 * drop what was typed ahead (unless NOFLSH), and signal the session's
 * foreground group.
 *
 * Returns nonzero when c was consumed as a signal.
 */
static int ser_signal_char(int c)
{
    int sig = 0;

    if (!ser_sid || !(ser_termios.c_lflag & ISIG) || c <= 0 || c > 0xff)
        return 0;
    if (c == ser_termios.c_cc[VINTR])
        sig = SIGINT;
    else if (c == ser_termios.c_cc[VQUIT])
        sig = SIGQUIT;
    else if (c == ser_termios.c_cc[VSUSP])
        sig = SIGTSTP;
    if (!sig)
        return 0;
    if ((ser_termios.c_lflag & ECHO) && c < ' ') {
        tx_byte('^');
        tx_byte((uint8_t) (c + '@'));
    }
    if (!(ser_termios.c_lflag & NOFLSH))
        serial_flush_input();

    /* A session that is gone leaves nobody to signal; stop claiming the
     * characters so they reach whoever reads next.
     */
    if (ser_jobctl(_PROC_JOBCTL_SIGFG, ser_sid, sig, NULL) == ESRCH)
        ser_sid = 0;
    return 1;
}

static void serial_termios_init(void)
{
    ser_termios.c_iflag = ICRNL | BRKINT;
    ser_termios.c_oflag = OPOST | ONLCR;
    ser_termios.c_cflag = CS8 | CREAD | HUPCL;
    ser_termios.c_ispeed = ser_termios.c_ospeed = B38400;
    ser_termios.c_lflag = ECHO | ECHOE | ECHOK | ICANON | ISIG;
    ser_termios.c_cc[VMIN] = 1;
    ser_termios.c_cc[VEOF] = 0x04;
    ser_termios.c_cc[VEOL] = 0;
    ser_termios.c_cc[VERASE] = 0x08;
    ser_termios.c_cc[VINTR] = 0x03;
    ser_termios.c_cc[VKILL] = 0x15;
    ser_termios.c_cc[VQUIT] = 0x1c;
    ser_termios.c_cc[VSUSP] = 0x1a;
    ser_termios.c_cc[VSTART] = 0x11;
    ser_termios.c_cc[VSTOP] = 0x13;
}

static int ser_open(ioserv_context_t *ctp,
                    io_open_t *msg,
                    void *handle,
                    void *extra)
{
    (void) handle;
    (void) extra;
    if (ioserv_open_bind(ctp, (void *) (uintptr_t) msg->connect.ioflag, 0) ==
        -1)
        return errno;
    return EOK;
}

static void ser_pump(void)
{
    int c;
    while (ser_input.head - ser_input.tail <= TTY_QUEUE - TTY_LINE &&
           (c = rxbuf_get()) >= 0) {
        struct tty_echo echo;
        if (ser_signal_char(c))
            continue;
        ioserv_ldisc_feed(&ser_input, &ser_termios, (unsigned char) c, &echo);
        for (unsigned i = 0; i < echo.repeat; i++)
            for (unsigned j = 0; j < echo.len; j++)
                tx_byte((uint8_t) echo.bytes[j]);
    }
}

static int ser_arm_read(uint64_t deadline)
{
    if (!deadline)
        return 0;
    if (!read_timer_created) {
        struct sigevent ev;
        SIGEV_NOTIFICATION_INIT(&ev, notification_coid, RTMUX_PRIO_DEVICE_IST,
                                NOTIFICATION_CODE_RX, 0);
        if (timer_create(CLOCK_MONOTONIC, &ev, &read_timer) == -1)
            return errno;
        read_timer_created = 1;
    }
    struct itimerspec spec = {0};
    spec.it_value.tv_sec = (time_t) (deadline / 1000000000);
    spec.it_value.tv_nsec = (long) (deadline % 1000000000);
    return timer_settime(read_timer, TIMER_ABSTIME, &spec, NULL) ? errno : 0;
}

/* Forget the parked reader. A nonzero err is replied to it; zero means the
 * reader is gone (or already answered), so nothing is sent and the rcvid is
 * never used again.
 */
static void ser_cancel_pending(int err)
{
    int rcvid = pending_rcvid;

    pending_rcvid = -1;
    pending_scoid = -1;
    pending_coid = -1;
    if (err && rcvid != -1)
        msg_error(rcvid, err);
}

/* Drop the parked reader if it can no longer take a byte: its rcvid is dead or
 * reused by another client (EOK, no reply), or it was signalled (EINTR).
 * Returns nonzero while a live reader is still parked.
 */
static int ser_pending_live(void)
{
    struct _msg_info info;

    if (pending_rcvid == -1)
        return 0;
    if (msg_info(pending_rcvid, &info) == -1 || info.scoid != pending_scoid) {
        ser_cancel_pending(0);
        return 0;
    }
    if (info.flags & RTMUX_MI_UNBLOCK_REQ) {
        ser_cancel_pending(EINTR);
        return 0;
    }
    return 1;
}

static int ser_read(ioserv_context_t *ctp, io_read_t *msg, void *ocb)
{
    unsigned ioflag = (unsigned) (uintptr_t) ocb;
    int c;

    if (!(ioflag & _IO_FLAG_RD))
        return EBADF;

    if (msg->i.xtype & _IO_XFLAG_NONBLOCK)
        ioflag |= O_NONBLOCK;
    if (msg->i.xtype & _IO_XFLAG_BLOCK)
        ioflag &= ~(unsigned) O_NONBLOCK;

    switch (msg->i.xtype & _IO_XTYPE_MASK) {
    case _IO_XTYPE_NONE:
        break;
    default:
        return ENOSYS;
    }

    if (!msg->i.nbytes) {
        _IO_SET_READ_NBYTES(ctp, 0);
        return EOK;
    }

    if (ioserv_tty_hung_up(&ser_termios)) { /* end of file */
        _IO_SET_READ_NBYTES(ctp, 0);
        return EOK;
    }
    if ((c = ser_jobctl_gate(ctp->info.pid, 0)) != 0)
        return c;

    unsigned size = msg->i.nbytes < TTY_QUEUE ? msg->i.nbytes : TTY_QUEUE;
    if (size > ctp->msg_max_size)
        size = (unsigned) ctp->msg_max_size;
    struct tty_read_clock clock = {0};
    unsigned consume;
    ser_pump();
    c = ioserv_ldisc_read(&ser_input, &ser_termios, &clock, ioserv_tty_now(),
                          ioflag & O_NONBLOCK, (char *) msg, size, &consume);
    if (c >= 0) {
        ser_input.tail += consume;
        _IO_SET_READ_NBYTES(ctp, c);
        return c ? IOSERV_PTR(ctp, msg, c) : EOK;
    }
    if (ioflag & O_NONBLOCK)
        return EAGAIN;
    if (ser_pending_live())
        return EBUSY;
    if ((c = ser_arm_read(clock.deadline)) != 0)
        return c;
    pending_size = size;
    pending_clock = clock;
    pending_rcvid = ctp->rcvid;
    pending_scoid = ctp->info.scoid;
    pending_coid = ctp->info.coid;
    return IOSERV_NOREPLY;
}

static int ser_write(ioserv_context_t *ctp, io_write_t *msg, void *ocb)
{
    unsigned ioflag = (unsigned) (uintptr_t) ocb;
    size_t header = sizeof msg->i;
    size_t payload;
    char *data;
    int err;

    if (!(ioflag & _IO_FLAG_WR))
        return EBADF;

    switch (msg->i.xtype & _IO_XTYPE_MASK) {
    case _IO_XTYPE_NONE:
        break;
    default:
        return ENOSYS;
    }

    if ((err = ioserv_tty_write_err(&ser_termios)) != 0)
        return err;
    if ((err = ser_jobctl_gate(ctp->info.pid, 1)) != 0)
        return err;

    /* Reject malformed writes and clamp to the received payload size.
     * msg->i.nbytes is caller-supplied, so a short message claiming a longer
     * write must not expose stale bytes in the dispatch buffer.
     */
    if (msg->i.nbytes < 0 || ctp->info.msglen < header)
        return EINVAL;
    payload = ctp->info.msglen - header;
    if ((size_t) msg->i.nbytes > payload)
        msg->i.nbytes = (int32_t) payload;

    data = (char *) (&msg->i) + header;
    for (int i = 0; i < msg->i.nbytes; i++) {
        char c = data[i];

        if ((ser_termios.c_oflag & (OPOST | ONLCR)) == (OPOST | ONLCR) &&
            c == '\n') {
            tx_byte('\r');
        }
        tx_byte((uint8_t) c);
    }

    _IO_SET_WRITE_NBYTES(ctp, msg->i.nbytes);
    return EOK;
}

static dev_t ser_devno;
static ino_t ser_inode;

static int ser_stat(ioserv_context_t *ctp, io_stat_t *msg, void *ocb)
{
    (void) ocb;
    memset(&msg->o, 0, sizeof msg->o);
    msg->o.st_dev = msg->o.st_rdev = ser_devno;
    msg->o.st_ino = ser_inode;
    msg->o.st_size = 1;
    msg->o.st_ctime = msg->o.st_mtime = msg->o.st_atime = time(0);
    msg->o.st_mode = 0666 | S_IFCHR;
    msg->o.st_nlink = 1;
    return IOSERV_PTR(ctp, &msg->o, sizeof msg->o);
}

/* Discard unread input: any partial canonical line, the cooked buffer, and
 * whatever raw bytes are still queued. Output is transmitted synchronously, so
 * there is no output queue to flush.
 */
static void serial_flush_input(void)
{
    while (rxbuf_get() >= 0)
        ;
    ser_input.editing = 0;
    ser_input.tail = ser_input.head;
}

static int ser_devctl(ioserv_context_t *ctp, io_devctl_t *msg, void *ocb)
{
    union {
        int oflag;
        int qsel;
        int flow;
        struct termios tios;
        pid_t pgrp;
    } *data = (void *) (msg + 1);
    unsigned ioflag = (unsigned) (uintptr_t) ocb;
    size_t nbytes = 0;

    switch (msg->i.devctl_cmd) {
    case DEVCTL_GET_FLAGS: {
        /* I/O-service ioflag carries 1-based _IO_FLAG_RD/_IO_FLAG_WR bits.
         * Convert them to O_RDONLY/O_WRONLY/O_RDWR without underflowing ioflag
         * == 0.
         */
        unsigned acc = ioflag & (unsigned) O_ACCMODE;
        data->oflag = (int) (ioflag & ~(unsigned) O_ACCMODE);
        if (acc != 0)
            data->oflag |= (int) ((acc - 1) & (unsigned) O_ACCMODE);
        nbytes = sizeof data->oflag;
        break;
    }
    case DEVCTL_SET_FLAGS:
        return ENOSYS;
    case DEVCTL_TTY_ISATTY:
        break;
    case DEVCTL_TTY_GET_ATTR:
        memcpy(data, &ser_termios, sizeof(struct termios));
        nbytes = sizeof(struct termios);
        break;
    case DEVCTL_TTY_SET_ATTR:
    case DEVCTL_TTY_SET_ATTR_DRAIN:
    case DEVCTL_TTY_SET_ATTR_FLUSH:
        /* Reject a short payload before consuming it as termios state: check
         * both the caller's claimed nbytes and the bytes actually received
         * (ctp->size), so a forged short message cannot make us read stale
         * dispatch-buffer bytes.
         */
        if (msg->i.nbytes < (int32_t) sizeof(struct termios) ||
            (size_t) ctp->size < sizeof(*msg) + sizeof(struct termios))
            return EINVAL;
        int flush = msg->i.devctl_cmd == DEVCTL_TTY_SET_ATTR_FLUSH;
        if (flush)
            serial_flush_input();
        ioserv_ldisc_set(&ser_input, &ser_termios,
                         (const struct termios *) data, flush);
        memcpy(&ser_termios, data, sizeof(struct termios));
        pending_clock = (struct tty_read_clock) {0};
        ioserv_tty_hangup_wake(ser_notify_lists, &ser_termios);

        /* A reader parked on a line that is now hung up gets end of file. */
        if (ioserv_tty_hung_up(&ser_termios) && ser_pending_live()) {
            int rcvid = pending_rcvid;

            ser_cancel_pending(0);
            (void) msg_reply(rcvid, 0, NULL, 0);
        } else {
            ser_finish_read();
        }
        break;
    case DEVCTL_TTY_GET_PGRP:
        *(pid_t *) data = ser_pgrp;
        nbytes = sizeof(pid_t);
        break;
    case DEVCTL_TTY_GET_SID:
        *(pid_t *) data = ser_sid;
        nbytes = sizeof(pid_t);
        break;
    case DEVCTL_TTY_SET_PGRP: {
        pid_t pgrp;
        int err;

        if (msg->i.nbytes < (int32_t) sizeof(pid_t) ||
            (size_t) ctp->size < sizeof(*msg) + sizeof(pid_t))
            return EINVAL;

        /* Roottask checks that this is the caller's controlling terminal and
         * that the group is in the caller's session, and records it.
         */
        pgrp = *(pid_t *) data;
        if ((err = ser_jobctl(_PROC_JOBCTL_SETPGRP, ctp->info.pid, pgrp,
                              NULL)) != 0)
            return err;
        ser_pgrp = pgrp;
        break;
    }
    case DEVCTL_TTY_SET_SID:
        /* Roottask binds the terminal to a session; the leader's group leads
         * (POSIX 7.1.1.3). Nobody else may rebind it.
         */
        if (ctp->info.pid != ROOTTASK_PID)
            return EPERM;
        if (msg->i.nbytes < (int32_t) sizeof(pid_t) ||
            (size_t) ctp->size < sizeof(*msg) + sizeof(pid_t) ||
            *(pid_t *) data < 0)
            return EINVAL; /* 0 unbinds the terminal */
        ser_sid = ser_pgrp = *(pid_t *) data;
        break;
    case DEVCTL_TTY_FLUSH:
        /* nbytes is caller-supplied; reject a short payload (by claim and by
         * received extent) before reading the queue selector.
         */
        if (msg->i.nbytes < (int32_t) sizeof data->qsel ||
            (size_t) ctp->size < sizeof(*msg) + sizeof data->qsel)
            return EINVAL;
        if (data->qsel == TCIFLUSH || data->qsel == TCIOFLUSH)
            serial_flush_input();
        break;
    case DEVCTL_TTY_DRAIN:
        /* tx_byte only guarantees FIFO space, not that the last byte has left
         * the wire, so block on the driver until the transmitter is idle.
         */
        tx_drain();
        break;
    case DEVCTL_TTY_SENDBREAK:
        return tx_break();
    case DEVCTL_TTY_FLOW:
        if (msg->i.nbytes < (int32_t) sizeof data->flow ||
            (size_t) ctp->size < sizeof(*msg) + sizeof data->flow)
            return EINVAL;
        switch (data->flow) {
        case TCIOFF:
            tx_byte(ser_termios.c_cc[VSTOP]);
            break;
        case TCION:
            tx_byte(ser_termios.c_cc[VSTART]);
            break;
        case TCOOFF:
        case TCOON:
            /* Serial writes are transmitted synchronously through tx_byte();
             * there is no service-side output queue to suspend or resume.
             */
            break;
        default:
            return EINVAL;
        }
        break;
    default:
        return ENOSYS;
    }

    if (nbytes) {
        msg->o.ret_val = 0;
        return IOSERV_PTR(ctp, &msg->o, sizeof msg->o + nbytes);
    }
    return EOK;
}

/* Conditions in want satisfied now. Input means a read would return a byte at
 * once: raw bytes are run through the line discipline first, so a partial
 * canonical line is not readable and data that arrived before any poller armed
 * is seen. That pass is what the next read would do anyway, and is bounded by
 * the raw ring; it is skipped when input is not asked about. Output is
 * transmitted synchronously by tx_byte(), with no queue to fill, so a write
 * never parks.
 */
static unsigned ser_ready(unsigned want)
{
    unsigned sat = _NOTIFY_COND_OUTPUT;

    if (ioserv_tty_hung_up(&ser_termios))
        return 0; /* POLLHUP, never writable (POSIX 2.10.6) */
    if (want & _NOTIFY_COND_INPUT) {
        ser_pump();
        if (ser_input.head != ser_input.tail)
            sat |= _NOTIFY_COND_INPUT;
    }
    return sat & want;
}

/* A poller may be waiting for input: wake it once a byte is readable. Callers
 * have just run the raw ring through the line discipline.
 */
static void ser_notify_input(void)
{
    if (ser_input.head != ser_input.tail)
        ioserv_notify_trigger(ser_notify_lists, 1, IOSERV_NOTIFY_INPUT);
}

/* _IO_NOTIFY for poll() and select(). Readiness is level-sensitive: the reply
 * carries what is satisfied now, and a notification is armed only when nothing
 * requested is. A byte that reaches the raw ring after this check raises an RX
 * pulse that this same dispatch thread handles after the arm is recorded, so
 * the edge cannot be lost between the check and the arm. A repeated pollfd
 * entry asking only for conditions a terminal never raises arms only those, and
 * leaves the arm the descriptor's other entry made.
 */
static int ser_notify(ioserv_context_t *ctp, io_notify_t *msg, void *ocb)
{
    (void) ocb;
    return ioserv_notify(
        ctp, msg, ser_notify_lists,
        ser_ready(msg->i.flags &
                  (unsigned) (_NOTIFY_COND_INPUT | _NOTIFY_COND_OUTPUT)),
        ioserv_tty_hup(&ser_termios), NULL);
}

static void ser_finish_read(void)
{
    ser_pump();
    if (ser_pending_live()) {
        char reply[TTY_QUEUE];
        unsigned consume;
        int n = ioserv_ldisc_read(&ser_input, &ser_termios, &pending_clock,
                                  ioserv_tty_now(), 0, reply, pending_size,
                                  &consume);
        if (n >= 0) {
            int rcvid = pending_rcvid;
            ser_cancel_pending(0);
            if (msg_reply(rcvid, n, reply, n) != -1)
                ser_input.tail += consume;
        } else {
            int err = ser_arm_read(pending_clock.deadline);
            if (err)
                ser_cancel_pending(err);
        }
    }
    /* Whatever the reader left is readable by the pollers. */
    ser_notify_input();
}

static int rx_notification_handler(message_context_t *ctp,
                                   int code,
                                   unsigned flags,
                                   void *handle)
{
    (void) ctp;
    (void) code;
    (void) flags;
    (void) handle;
    ser_finish_read();
    return 0;
}

/* A signalled or killed reader parked in ser_read(): reply EINTR if it is the
 * parked one. Any other rcvid is not ours, so leave it to the default action.
 */
static int ser_unblock(ioserv_context_t *ctp, io_notification_t *msg, void *ocb)
{
    (void) msg;
    (void) ocb;

    if (pending_rcvid == -1 || ctp->rcvid != pending_rcvid)
        return IOSERV_DEFAULT;
    ser_cancel_pending(0);
    return EINTR; /* the framework replies it to ctp->rcvid */
}

/* Close of a client open, including the close synthesized when a client process
 * dies. The parked reader is matched by client connection (scoid) and by the
 * open it read from (coid; the OCB is only the open flags), so closing one open
 * leaves a read parked on another open of the same client alone. A match is
 * replied EINTR when still blocked, not left hanging. When the whole client is
 * dying its rcvid is dead and the reply is dropped by the kernel; the msg_info
 * check keeps a reused rcvid from being answered by mistake.
 */
static int ser_close_dup(ioserv_context_t *ctp, io_close_t *msg, void *ocb)
{
    (void) msg;
    (void) ocb;

    if (pending_rcvid != -1 && ctp->info.scoid == pending_scoid &&
        ctp->info.coid == pending_coid) {
        struct _msg_info info;
        int live =
            msg_info(pending_rcvid, &info) != -1 && info.scoid == pending_scoid;

        ser_cancel_pending(live ? EINTR : 0);
    }
    ioserv_notify_remove(ctp, ser_notify_lists);
    return EOK;
}

/* _IO_OPENFD: roottask opens a second connection to the terminal when it makes
 * it a session's controlling terminal (_PROC_SESSION_TCSETSID).
 */
static int ser_openfd(ioserv_context_t *ctp, io_openfd_t *msg, void *ocb)
{
    uintptr_t source = (uintptr_t) ocb;
    uintptr_t requested = msg->i.ioflag;

    if (msg->i.xtype != _IO_OPENFD_NONE)
        return ENOSYS;
    if ((requested & _IO_FLAG_MASK & ~source) != 0)
        return EACCES;
    if (ioserv_open_bind(ctp, (void *) requested, 0) == -1)
        return errno;
    return EOK;
}

static const ioserv_ops_t ser_io_funcs = {
    .nfuncs = IOSERV_IO_NFUNCS,
    .read = ser_read,
    .write = ser_write,
    .stat = ser_stat,
    .devctl = ser_devctl,
    .notify = ser_notify,
    .openfd = ser_openfd,
    .unblock = ser_unblock,
    .close_dup = ser_close_dup,
};

static const ioserv_connect_ops_t ser_connect_funcs = {
    .nfuncs = IOSERV_CONNECT_NFUNCS,
    .open = ser_open,
};

#endif
