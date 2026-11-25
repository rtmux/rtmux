/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "root-mutex.h"
#include <sys/ioserv-ldisc.h>
#include "externs.h"
#include <fcntl.h>
#include <limits.h>
#include <stdalign.h>
#include <sys/poll.h>
#include <termios.h>
#include <sys/ioserv-tty.h>
#include <devctl/common.h>
#include <devctl/tty.h>
#include <sys/procmsg.h>
#include "pinode.h"

/* Both locks inherit priority; devtext_init() sets them up. The watcher thread
 * runs below the dispatch workers and holds text_mutex again each time its
 * timed wait ends, so with a plain mutex a preempted watcher stalls every
 * worker that closes /dev/text, and each blocked worker makes the pool start
 * another.
 *
 * text_mutex guards the typed-ahead ring, the armed notifications and the
 * termios state. text_out_mutex orders console output only, and no path writes
 * to the console while holding text_mutex, so a worker that closes or polls the
 * device never waits for a console write.
 */
static pthread_mutex_t text_mutex;
static pthread_mutex_t text_out_mutex;

/* Write to the console in order with every other writer. */
static void text_console_write(const char *buf, size_t len)
{
    root_mutex_lock(&text_out_mutex);
    root_console_write(buf, len);
    pthread_mutex_unlock(&text_out_mutex);
}

static dev_t text_devno;

/* Terminal state for /dev/console (minimal termios support for early userland)
 */
static struct termios text_termios;
static pid_t text_pgrp;
static pid_t text_sid; /* leader of the session this is the terminal of, or 0 */

/* A copy of the terminal state, taken under text_mutex: tcsetattr writes the
 * termios on any thread, so a read of it is made under the same lock. *hangups,
 * when not NULL, gets the number of times the line was hung up: a reader that
 * waits sees one that came and went while it slept.
 */
static unsigned text_hangups;

static struct termios text_attr(unsigned *hangups)
{
    struct termios t;

    root_mutex_lock(&text_mutex);
    t = text_termios;
    if (hangups)
        *hangups = text_hangups;
    pthread_mutex_unlock(&text_mutex);
    return t;
}

/* Job control gate for a read (is_write = 0) or a write (is_write = 1) by the
 * caller. Only the caller's controlling terminal is policed, and only for a
 * background process; a write is policed only under TOSTOP, which lflag (the
 * c_lflag the caller read under the lock) says.
 *
 * A background read gets EIO when its group is orphaned or it blocks/ignores
 * SIGTTIN; otherwise the group is sent SIGTTIN and the read fails with EINTR. A
 * write gets the same with SIGTTOU, except that a blocked/ignored SIGTTOU lets
 * it through.
 *
 * Returns EOK to proceed, else the errno to reply.
 */
static int text_jobctl_gate(ioserv_context_t *ctp, int is_write, tcflag_t lflag)
{
    struct _proc_jobctl_reply q;
    unsigned hold = is_write ? _JOBCTL_HOLD_TTOU : _JOBCTL_HOLD_TTIN;

    if (is_write && !(lflag & TOSTOP))
        return EOK;
    if (proc_jobctl_query(ROOTTASK_PID, ctp->info.pid, &q) != EOK ||
        !(q.flags & _JOBCTL_CTTY) || (q.flags & _JOBCTL_FG))
        return EOK;
    if (is_write && (q.flags & hold))
        return EOK;
    if ((q.flags & _JOBCTL_ORPHAN) || (q.flags & hold))
        return EIO;
    (void) proc_jobctl_signal_bg(ROOTTASK_PID, ctp->info.pid,
                                 is_write ? SIGTTOU : SIGTTIN);
    return EINTR;
}

/* Whether c is one of the termios signal characters, and which signal it
 * raises. The character must be enabled (not _POSIX_VDISABLE) and ISIG set.
 */
static int text_signal_char(int c)
{
    if (!(text_termios.c_lflag & ISIG) || c <= 0 || c > 0xff)
        return 0;
    if (c == text_termios.c_cc[VINTR])
        return SIGINT;
    if (c == text_termios.c_cc[VQUIT])
        return SIGQUIT;
    if (c == text_termios.c_cc[VSUSP])
        return SIGTSTP;
    return 0;
}


/* Typed-ahead input. A terminal signal character must be noticed when typed,
 * even while no process is reading (a foreground job that does not read can
 * still be interrupted), so a watcher thread moves what arrives into this ring
 * for the next reader, acting on signal characters as it goes. Both sides run
 * under text_mutex, which keeps the characters in order.
 */
static struct ioserv_ldisc text_input;

/* Armed readiness notifications (_IO_NOTIFY), under text_mutex like the ring.
 * The console raises no interrupt, so the watcher thread looks for input while
 * a poller is armed and wakes it once a read can return.
 */
static ioserv_notify_t text_notify_lists[IOSERV_NOTIFY_COUNT];

/* Signalled when a poller arms, so the watcher starts looking at once. */
static pthread_cond_t text_watch_cond;

/* Static stack: the watcher is made with __thread_create before the message
 * pool runs, as pthread_create cannot get a stack from a roottask worker.
 */
#define TEXT_WATCHER_STACK 8192
static alignas(16) char text_watcher_stack[TEXT_WATCHER_STACK];

/* Drain available console input through the line discipline. Deliver signal
 * characters to the foreground group and emit echo outside text_mutex.
 */
static void text_pump(void)
{
    for (;;) {
        int c, sig = 0;
        char echo[2];
        size_t echo_len = 0;

        root_mutex_lock(&text_mutex);
        if (text_input.head - text_input.tail > TTY_QUEUE - TTY_LINE) {
            pthread_mutex_unlock(&text_mutex);
            return;
        }
        c = root_console_getchar();
        if (c < 0) {
            pthread_mutex_unlock(&text_mutex);
            return;
        }
        if (!text_sid || !(sig = text_signal_char(c))) {
            struct tty_echo output;
            ioserv_ldisc_feed(&text_input, &text_termios, (unsigned char) c,
                              &output);
            pthread_mutex_unlock(&text_mutex);
            for (unsigned i = 0; i < output.repeat; i++)
                text_console_write(output.bytes, output.len);
            continue;
        }
        if (!(text_termios.c_lflag & NOFLSH)) {
            text_input.tail = text_input.head;
            text_input.editing = 0;
            while (root_console_getchar() >= 0)
                ;
        }
        if ((text_termios.c_lflag & ECHO) && c < ' ') {
            echo[0] = '^';
            echo[1] = (char) (c + '@');
            echo_len = sizeof echo;
        }
        pthread_mutex_unlock(&text_mutex);
        if (echo_len)
            text_console_write(echo, echo_len);
        if (proc_jobctl_signal_fg(ROOTTASK_PID, text_sid, sig) == ESRCH) {
            text_sid = 0; /* the session is gone: stop claiming the keys */
        }
    }
}

/* How often the watcher looks at the console: a poller waiting on it is woken
 * within TEXT_WATCH_ARMED_MS of a key press. The console raises no interrupt.
 */
#define TEXT_WATCH_MS 50
#define TEXT_WATCH_ARMED_MS 10

static void *text_watcher(void *arg)
{
    int armed = 0;

    (void) arg;
    root_mutex_lock(&text_mutex);
    for (;;) {
        struct timespec until;
        long ms = armed ? TEXT_WATCH_ARMED_MS : TEXT_WATCH_MS;

        clock_gettime(CLOCK_MONOTONIC, &until);
        until.tv_nsec += ms * 1000000L;
        if (until.tv_nsec >= 1000000000L) {
            until.tv_sec++;
            until.tv_nsec -= 1000000000L;
        }
        (void) pthread_cond_timedwait(&text_watch_cond, &text_mutex, &until);
        armed = ioserv_notify_armed(text_notify_lists, IOSERV_NOTIFY_INPUT, 1);
        if (!text_sid && !armed)
            continue;
        pthread_mutex_unlock(&text_mutex);
        text_pump();
        root_mutex_lock(&text_mutex);

        /* A poller may have armed while the lock was dropped; its signal was
         * missed, so look again to pick the short interval for it.
         */
        armed = ioserv_notify_armed(text_notify_lists, IOSERV_NOTIFY_INPUT, 1);
        if (armed && text_input.head != text_input.tail)
            ioserv_notify_trigger(text_notify_lists, 1, IOSERV_NOTIFY_INPUT);
    }
    return NULL;
}

/* A blocked console read periodically pumps input and checks for cancellation,
 * hangup and the per-read termios deadline.
 */
static int text_read(ioserv_context_t *ctp, io_read_t *msg, void *ocb)
{
    unsigned ioflag = (unsigned) (uintptr_t) ocb;
    unsigned hangups;
    int c;

    if (!(ioflag & _IO_FLAG_RD)) {
        return EBADF;
    }

    if (ioflag & O_NONBLOCK) {
        if (msg->i.xtype & _IO_XFLAG_BLOCK) {
            ioflag &= ~(unsigned) O_NONBLOCK;
        }
    } else {
        if (msg->i.xtype & _IO_XFLAG_NONBLOCK) {
            ioflag |= O_NONBLOCK;
        }
    }

    switch (msg->i.xtype & _IO_XTYPE_MASK) {
    case _IO_XTYPE_NONE:
        break;
    default:
        /* We don't handle this xtype, return error */
        return ENOSYS;
    }

    if (!msg->i.nbytes) {
        /* Nothing requested so return EOF */
        _IO_SET_READ_NBYTES(ctp, 0);
        return EOK;
    }

    if ((c = text_jobctl_gate(ctp, 0, 0)) != EOK) {
        return c;
    }

    /* At most the receive buffer, so it fits an unsigned. */
    unsigned size =
        (unsigned) (msg->i.nbytes < ctp->msg_max_size ? msg->i.nbytes
                                                      : ctp->msg_max_size);
    struct tty_read_clock clock = {0};
    (void) text_attr(&hangups);
    for (;;) {
        struct _msg_info info;
        uint64_t to;

        /* A line that is hung up, or was since the read began, is at end of
         * file, for a reader that waits too.
         */
        text_pump();
        root_mutex_lock(&text_mutex);
        if (ioserv_tty_hung_up(&text_termios) || text_hangups != hangups) {
            pthread_mutex_unlock(&text_mutex);
            _IO_SET_READ_NBYTES(ctp, 0);
            return EOK;
        }
        unsigned consume;
        uint64_t now = ioserv_tty_now();
        c = ioserv_ldisc_read(&text_input, &text_termios, &clock, now,
                              ioflag & O_NONBLOCK, (char *) msg, size,
                              &consume);
        text_input.tail += consume;
        pthread_mutex_unlock(&text_mutex);
        if (c >= 0)
            break;
        if (ioflag & O_NONBLOCK)
            return EAGAIN;
        to = TEXT_WATCH_ARMED_MS * 1000000ULL;
        if (clock.deadline > now && clock.deadline - now < to)
            to = clock.deadline - now;
        (void) __timer_timeout_r(CLOCK_REALTIME, RTMUX_TIMEOUT_NANOSLEEP, 0,
                                 &to, NULL);
        if (msg_info(ctp->rcvid, &info) != -1 &&
            (info.flags & RTMUX_MI_UNBLOCK_REQ)) {
            return EINTR;
        }
    }
    _IO_SET_READ_NBYTES(ctp, c);
    return c ? IOSERV_PTR(ctp, msg, c) : EOK;
}

/* Write handler for /dev/text — emit bytes to the console.
 *
 * Parameters:
 *   ctp : I/O service context
 *   msg : the io_write request (data follows the header)
 *   ocb : open control block holding the io flags
 * Return value: EOK on success, or an errno on failure.
 */
static int text_write(ioserv_context_t *ctp, io_write_t *msg, void *ocb)
{
    unsigned ioflag = (unsigned) (uintptr_t) ocb;
    struct termios t;
    int err;

    if (!(ioflag & _IO_FLAG_WR)) {
        return EBADF;
    }

    switch (msg->i.xtype & _IO_XTYPE_MASK) {
    case _IO_XTYPE_NONE:
        break;
    default:
        /* We don't handle this xtype, return error */
        return ENOSYS;
    }

    t = text_attr(NULL);
    if ((err = ioserv_tty_write_err(&t)) != 0)
        return err;
    if ((err = text_jobctl_gate(ctp, 1, t.c_lflag)) != EOK) {
        return err;
    }

    /* nbytes is a signed int32 copied from the untrusted client. The clamp
     * below only catches a too-large positive value; a negative nbytes would
     * pass it and then sign-extend through (size_t) into a huge length, walking
     * roottask memory past the message buffer (console info-leak, then a fault
     * that takes down pid 1). Reject it up front. /dev/text is world-writable.
     */
    if (msg->i.nbytes < 0)
        return EINVAL;
    if (msg->i.nbytes > (int32_t) (ctp->msg_max_size - sizeof msg->i)) {
        msg->i.nbytes = (int32_t) (ctp->msg_max_size - sizeof msg->i);
    }
    text_console_write((char *) (&msg->i) + sizeof msg->i,
                       (size_t) msg->i.nbytes);
    _IO_SET_WRITE_NBYTES(ctp, msg->i.nbytes);
    return EOK;
}

/* Stat handler for /dev/text — fill in a character-device stat buffer.
 *
 * Parameters:
 *   ctp : I/O service context
 *   msg : the io_stat request / reply buffer
 *   ocb : open control block (unused)
 * Return value: a I/O service reply with the stat structure.
 */
static int text_stat(ioserv_context_t *ctp, io_stat_t *msg, void *ocb)
{
    memset(&msg->o, 0x00, sizeof msg->o);
    msg->o.st_ino = PINO_TEXT;
    msg->o.st_size = 1;
    msg->o.st_ctime = msg->o.st_mtime = msg->o.st_atime = time(0);
    msg->o.st_mode = 0666 | S_IFCHR;
    msg->o.st_nlink = 1;
    msg->o.st_dev = (ctp->info.srcnd << RTMUX_NODE_BITS) | path_devno;
    msg->o.st_rdev = (ctp->info.srcnd << RTMUX_NODE_BITS) | text_devno;
    return IOSERV_PTR(ctp, &msg->o, sizeof msg->o);
}

/* Devctl handler for /dev/text — minimal termios and flag support.
 *
 * Handles the GETFLAGS/SETFLAGS, ISATTY, TCGETATTR/TCSETATTR*,
 * TCGETPGRP/TCSETPGRP, TCGETSID, and TCFLUSH/TCDRAIN/TCFLOW commands.
 * Parameters:
 *   ctp : I/O service context
 *   msg : the io_devctl request / reply buffer (data follows header)
 *   ocb : open control block holding the io flags
 * Return value: a I/O service reply or EOK on success, ENOSYS for unknown
 *               commands.
 */
static int text_devctl(ioserv_context_t *ctp, io_devctl_t *msg, void *ocb)
{
    union {
        int oflag;
        int mountflag;
        int qsel;
        int flow;
        struct termios tios;
        pid_t pgrp;
    } *data = (void *) (msg + 1);
    unsigned ioflag = (unsigned) (uintptr_t) ocb;
    size_t nbytes = 0;

    switch ((unsigned) msg->i.devctl_cmd) {
    case DEVCTL_GET_FLAGS:
        data->oflag = (int) (ioflag & ~(unsigned) O_ACCMODE) |
                      (int) ((ioflag - 1) & (unsigned) O_ACCMODE);
        nbytes = sizeof data->oflag;
        break;

    case DEVCTL_SET_FLAGS:
        break;

    case DEVCTL_TTY_ISATTY:
        break;

    case DEVCTL_TTY_GET_ATTR:
        root_mutex_lock(&text_mutex);
        memcpy(data, &text_termios, sizeof(struct termios));
        pthread_mutex_unlock(&text_mutex);
        nbytes = sizeof(struct termios);
        break;

    case DEVCTL_TTY_SET_ATTR:
    case DEVCTL_TTY_SET_ATTR_DRAIN:
    case DEVCTL_TTY_SET_ATTR_FLUSH:
        if (msg->i.nbytes < (int32_t) sizeof(struct termios))
            return EINVAL;
        root_mutex_lock(&text_mutex);
        int flush = msg->i.devctl_cmd == DEVCTL_TTY_SET_ATTR_FLUSH;
        ioserv_ldisc_set(&text_input, &text_termios,
                         (const struct termios *) data, flush);
        if (flush)
            while (root_console_getchar() >= 0)
                ;
        memcpy(&text_termios, data, sizeof(struct termios));
        ioserv_tty_hangup_wake(text_notify_lists, &text_termios);
        if (ioserv_tty_hung_up(&text_termios))
            text_hangups++;
        pthread_mutex_unlock(&text_mutex);
        break;

    case DEVCTL_TTY_GET_PGRP:
        *(pid_t *) data = text_pgrp;
        nbytes = sizeof(pid_t);
        break;

    case DEVCTL_TTY_GET_SID:
        *(pid_t *) data = text_sid;
        nbytes = sizeof(pid_t);
        break;

    case DEVCTL_TTY_SET_PGRP: {
        int err;
        pid_t pgrp;

        if (msg->i.nbytes < (int32_t) sizeof(pid_t))
            return EINVAL;

        /* Roottask checks that this is the caller's controlling terminal and
         * that the group is in the caller's session, and records it.
         */
        pgrp = *(pid_t *) data;
        if ((err = proc_jobctl_setpgrp(ROOTTASK_PID, ctp->info.pid, pgrp)) !=
            EOK)
            return err;
        text_pgrp = pgrp;
        break;
    }

    case DEVCTL_TTY_SET_SID:
        /* Roottask binds the terminal to a session; the leader's group leads
         * (POSIX 7.1.1.3).
         */
        if (ctp->info.pid != ROOTTASK_PID)
            return EPERM; /* only roottask binds the terminal */
        if (msg->i.nbytes < (int32_t) sizeof(pid_t) || *(pid_t *) data < 0)
            return EINVAL; /* 0 unbinds the terminal */
        text_sid = text_pgrp = *(pid_t *) data;
        break;

    case DEVCTL_TTY_FLUSH:
        /* Discard the edited line, queued records and whatever bytes are
         * already available from the hardware. Output is written synchronously,
         * so an output flush has nothing to discard. nbytes is caller-supplied,
         * so reject a payload too short to hold the queue selector rather than
         * reading past it.
         */
        if (msg->i.nbytes < (int32_t) sizeof data->qsel)
            return EINVAL;
        if (data->qsel == TCIFLUSH || data->qsel == TCIOFLUSH) {
            root_mutex_lock(&text_mutex);
            text_input.tail = text_input.head;
            text_input.editing = 0;
            while (root_console_getchar() >= 0)
                ;
            pthread_mutex_unlock(&text_mutex);
        }
        break;

    case DEVCTL_TTY_DRAIN:
        /* Console output is written synchronously, so anything already written
         * has been transmitted — there is nothing to wait for.
         */
        break;

    case DEVCTL_TTY_SENDBREAK:
        /* The SBI console is not an asynchronous serial line, so POSIX has
         * tcsendbreak() take no action and succeed.
         */
        break;

    case DEVCTL_TTY_FLOW:
        if (msg->i.nbytes < (int32_t) sizeof data->flow)
            return EINVAL;
        switch (data->flow) {
        case TCIOFF: {
            char ch = (char) text_attr(NULL).c_cc[VSTOP];

            text_console_write(&ch, 1);
            break;
        }
        case TCION: {
            char ch = (char) text_attr(NULL).c_cc[VSTART];

            text_console_write(&ch, 1);
            break;
        }
        case TCOOFF:
        case TCOON:
            /* /dev/text output is synchronous; there is no queued output to
             * suspend or resume.
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

/* _IO_NOTIFY for poll() and select(). Input is ready once a read would return a
 * character at once: what the console holds is moved into the ring first, so a
 * character typed before the poller armed is seen. Output is written
 * synchronously, so it is always ready. The ring is checked and the arm made
 * under text_mutex, which the watcher also takes before it wakes pollers, so a
 * character it queues after the check still finds the arm.
 */
static int text_notify(ioserv_context_t *ctp, io_notify_t *msg, void *ocb)
{
    unsigned trig = _NOTIFY_COND_OUTPUT;
    int ret;

    (void) ocb;
    if (msg->i.flags & (unsigned) _NOTIFY_COND_INPUT)
        text_pump();
    root_mutex_lock(&text_mutex);
    if (ioserv_tty_hung_up(&text_termios)) /* POLLHUP, never writable */
        trig = 0;
    else if (text_input.head != text_input.tail)
        trig |= _NOTIFY_COND_INPUT;
    ret = ioserv_notify(ctp, msg, text_notify_lists, trig,
                        ioserv_tty_hup(&text_termios), NULL);
    if (ret == EAGAIN)
        pthread_cond_signal(&text_watch_cond);
    pthread_mutex_unlock(&text_mutex);
    return ret;
}

/* A closed open takes its arms with it. */
static int text_close_dup(ioserv_context_t *ctp, io_close_t *msg, void *ocb)
{
    (void) msg;
    (void) ocb;
    root_mutex_lock(&text_mutex);
    ioserv_notify_remove(ctp, text_notify_lists);
    pthread_mutex_unlock(&text_mutex);
    return EOK;
}

/* _IO_OPENFD: roottask opens a second connection to the terminal when it makes
 * it a session's controlling terminal (_PROC_SESSION_TCSETSID).
 */
static int text_openfd(ioserv_context_t *ctp, io_openfd_t *msg, void *ocb)
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

static const ioserv_ops_t text_io_funcs = {
    .nfuncs = IOSERV_IO_NFUNCS,
    .read = text_read,
    .write = text_write,
    .stat = text_stat,
    .devctl = text_devctl,
    .notify = text_notify,
    .openfd = text_openfd,
    .close_dup = text_close_dup,
};

/* Open handler for /dev/text — bind the connection with its io flags.
 *
 * Parameters:
 *   ctp    : I/O service context
 *   msg    : the io_open request
 *   handle : connect handle (unused)
 *   extra  : extra open data (unused)
 * Return value: EOK on success, or errno on bind failure.
 */
static int text_open(ioserv_context_t *ctp,
                     io_open_t *msg,
                     void *handle,
                     void *extra)
{
    root_trace("text_open: rcvid=0x%x ioflag=0x%x\n", ctp->rcvid,
               msg->connect.ioflag);
    if (ioserv_open_bind(ctp, (void *) (uintptr_t) msg->connect.ioflag, 0) ==
        -1) {
        root_log("text_open: ioserv_open_bind FAILED errno=%d\n", errno);
        return errno;
    }
    return EOK;
}

static const ioserv_connect_ops_t text_connect_funcs = {
    .nfuncs = IOSERV_CONNECT_NFUNCS,
    .open = text_open,
};

/* Initialize the /dev/text device.
 *
 * Sets up sensible terminal defaults, attaches the I/O service at /dev/text,
 * and allocates its device number.
 */
void devtext_init(void)
{
    /* Priority inheritance: see the comment at text_mutex. */
    root_mutex_init_pi(&text_mutex);
    root_mutex_init_pi(&text_out_mutex);
    /* Sensible terminal defaults so the shell gets ECHO | ICANON */
    text_termios.c_iflag = ICRNL | BRKINT;
    text_termios.c_oflag = OPOST | ONLCR;
    text_termios.c_cflag = CS8 | CREAD | HUPCL;
    text_termios.c_ispeed = text_termios.c_ospeed = B38400;
    text_termios.c_lflag = ECHO | ECHOE | ECHOK | ICANON | ISIG;
    text_termios.c_cc[VEOF] = 0x04; /* ^D */
    text_termios.c_cc[VEOL] = 0;
    text_termios.c_cc[VERASE] = 0x08; /* ^H */
    text_termios.c_cc[VINTR] = 0x03;  /* ^C */
    text_termios.c_cc[VKILL] = 0x15;  /* ^U */
    text_termios.c_cc[VMIN] = 1;
    text_termios.c_cc[VQUIT] = 0x1c;  /* ^\ */
    text_termios.c_cc[VSUSP] = 0x1a;  /* ^Z */
    text_termios.c_cc[VSTART] = 0x11; /* ^Q */
    text_termios.c_cc[VSTOP] = 0x13;  /* ^S */
    text_termios.c_cc[VTIME] = 0;

    {
        struct _thread_attr attr;
        pthread_condattr_t cattr;

        pthread_condattr_init(&cattr);
        pthread_condattr_setclock(&cattr, CLOCK_MONOTONIC);
        pthread_cond_init(&text_watch_cond, &cattr);
        pthread_condattr_destroy(&cattr);
        memset(&attr, 0, sizeof attr);
        attr.stackaddr = text_watcher_stack;
        attr.stacksize = sizeof text_watcher_stack;
        attr.flags = PTHREAD_CREATE_DETACHED;
        (void) __thread_create(0, text_watcher, NULL, &attr);
    }

    ioserv_attach(dpp, NULL, "/dev/text", 0, 0, &text_connect_funcs,
                  &text_io_funcs, 0);
    hwres_proc_devno("dev", &text_devno, -1, 0);
}
