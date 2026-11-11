/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "externs.h"
#include <fcntl.h>
#include <stdalign.h>
#include <termios.h>
#include <devctl/common.h>
#include <devctl/tty.h>
#include <sys/procmsg.h>
#include "pinode.h"

static pthread_mutex_t text_mutex = PTHREAD_MUTEX_INITIALIZER;
static dev_t text_devno;

/* Terminal state for /dev/console (minimal termios support for early userland)
 */
static struct termios text_termios;
static pid_t text_pgrp;
static pid_t text_sid; /* leader of the session this is the terminal of, or 0 */

/* Job control gate for a read (is_write = 0) or a write (is_write = 1) by the
 * caller. Only the caller's controlling terminal is policed, and only for a
 * background process; a write is policed only under TOSTOP.
 *
 * A background read gets EIO when its group is orphaned or it blocks/ignores
 * SIGTTIN; otherwise the group is sent SIGTTIN and the read fails with EINTR. A
 * write gets the same with SIGTTOU, except that a blocked/ignored SIGTTOU lets
 * it through.
 *
 * Returns EOK to proceed, else the errno to reply.
 */
static int text_jobctl_gate(ioserv_context_t *ctp, int is_write)
{
    struct _proc_jobctl_reply q;
    unsigned hold = is_write ? _JOBCTL_HOLD_TTOU : _JOBCTL_HOLD_TTIN;

    if (is_write && !(text_termios.c_lflag & TOSTOP))
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
#define TEXT_AHEAD 128
static int text_ahead[TEXT_AHEAD];
static unsigned text_ahead_head, text_ahead_tail;

/* Static stack: the watcher is made with __thread_create before the message
 * pool runs, as pthread_create cannot get a stack from a roottask worker.
 */
#define TEXT_WATCHER_STACK 8192
static alignas(16) char text_watcher_stack[TEXT_WATCHER_STACK];

/* Take the next input character: from the ring (to_ring = 0), else from the
 * console. With to_ring set, data characters are queued instead of returned and
 * the call ends when the console is empty or the ring full. A signal character
 * is echoed, flushes what was typed ahead (unless NOFLSH), signals the
 * session's foreground group, and is never returned.
 *
 * Returns the character, or -1.
 */
static int text_next(int to_ring)
{
    for (;;) {
        int c, sig = 0;

        pthread_mutex_lock(&text_mutex);
        if (!to_ring && text_ahead_head != text_ahead_tail) {
            c = text_ahead[text_ahead_tail++ % TEXT_AHEAD];
            pthread_mutex_unlock(&text_mutex);
            return c;
        }
        if (to_ring && text_ahead_head - text_ahead_tail >= TEXT_AHEAD) {
            pthread_mutex_unlock(&text_mutex);
            return -1;
        }
        c = root_console_getchar();
        if (c < 0) {
            pthread_mutex_unlock(&text_mutex);
            return -1;
        }
        if (!text_sid || !(sig = text_signal_char(c))) {
            if (to_ring) {
                text_ahead[text_ahead_head++ % TEXT_AHEAD] = c;
                pthread_mutex_unlock(&text_mutex);
                continue;
            }
            pthread_mutex_unlock(&text_mutex);
            return c;
        }
        if (!(text_termios.c_lflag & NOFLSH)) {
            text_ahead_tail = text_ahead_head;
            while (root_console_getchar() >= 0)
                ;
        }
        if ((text_termios.c_lflag & ECHO) && c < ' ') {
            char echo[2] = {'^', (char) (c + '@')};

            root_console_write(echo, 2);
        }
        pthread_mutex_unlock(&text_mutex);
        if (proc_jobctl_signal_fg(ROOTTASK_PID, text_sid, sig) == ESRCH) {
            text_sid = 0; /* the session is gone: stop claiming the keys */
        }
    }
}

static void *text_watcher(void *arg)
{
    (void) arg;
    for (;;) {
        struct timespec ts = {.tv_nsec = 50 * 1000000L};

        nanosleep(&ts, NULL);
        if (text_sid)
            (void) text_next(1);
    }
    return NULL;
}

/* Read handler for /dev/text — return one character from the console.
 *
 * Blocks (or returns EAGAIN in non-blocking mode) until a character is
 * available, applies ICRNL and ECHO termios processing, and replies with at
 * most one byte. Parameters:
 *   ctp : I/O service context
 *   msg : the io_read request / reply buffer
 *   ocb : open control block holding the io flags
 * Return value: a I/O service reply on success, or an errno on failure.
 */
static int text_read(ioserv_context_t *ctp, io_read_t *msg, void *ocb)
{
    unsigned ioflag = (unsigned) (uintptr_t) ocb;
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

    if ((c = text_jobctl_gate(ctp, 0)) != EOK) {
        return c;
    }

    for (;;) {
        struct _msg_info info;
        uint64_t to;

        c = text_next(0);
        if (c >= 0) {
            break;
        }
        if (ioflag & O_NONBLOCK) {
            return EAGAIN;
        }
        to = 100 * 1000000ULL;
        (void) __timer_timeout_r(CLOCK_REALTIME, RTMUX_TIMEOUT_NANOSLEEP, 0,
                                 &to, NULL);
        if (msg_info(ctp->rcvid, &info) != -1 &&
            (info.flags & RTMUX_MI_UNBLOCK_REQ)) {
            return EINTR;
        }
    }
    if (c > 0xff) {
        /* Non-ascii so return EOF */
        _IO_SET_READ_NBYTES(ctp, 0);
        return EOK;
    }

    /* ICRNL: convert CR to NL (standard termios input processing) */
    if ((text_termios.c_iflag & ICRNL) && c == '\r')
        c = '\n';

    /* ECHO: terminal driver responsibility per POSIX */
    if (text_termios.c_lflag & ECHO) {
        char ch = (char) c;
        pthread_mutex_lock(&text_mutex);
        if (ch == '\r' || ch == '\n')
            root_console_write("\r\n", 2);
        else if (ch >= ' ')
            root_console_write(&ch, 1);
        pthread_mutex_unlock(&text_mutex);
    }

    *(char *) msg = (char) c;
    _IO_SET_READ_NBYTES(ctp, 1);
    return IOSERV_PTR(ctp, msg, 1);
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

    if ((err = text_jobctl_gate(ctp, 1)) != EOK) {
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
    pthread_mutex_lock(&text_mutex);
    root_console_write((char *) (&msg->i) + sizeof msg->i,
                       (size_t) msg->i.nbytes);
    pthread_mutex_unlock(&text_mutex);
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
        memcpy(data, &text_termios, sizeof(struct termios));
        nbytes = sizeof(struct termios);
        break;

    case DEVCTL_TTY_SET_ATTR:
    case DEVCTL_TTY_SET_ATTR_DRAIN:
    case DEVCTL_TTY_SET_ATTR_FLUSH:
        if (msg->i.nbytes < (int32_t) sizeof(struct termios))
            return EINVAL;
        memcpy(&text_termios, data, sizeof(struct termios));
        break;

    case DEVCTL_TTY_GET_PGRP:
        *(pid_t *) data = text_pgrp;
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
        if (msg->i.nbytes < (int32_t) sizeof(pid_t) || *(pid_t *) data <= 0)
            return EINVAL;
        text_sid = text_pgrp = *(pid_t *) data;
        break;

    case DEVCTL_TTY_FLUSH:
        /* Discard pending input: the console has no software queue, so drain
         * whatever bytes are already available from the hardware. Output is
         * written synchronously, so an output flush has nothing to discard.
         * nbytes is caller-supplied, so reject a payload too short to hold the
         * queue selector rather than reading past it.
         */
        if (msg->i.nbytes < (int32_t) sizeof data->qsel)
            return EINVAL;
        if (data->qsel == TCIFLUSH || data->qsel == TCIOFLUSH) {
            pthread_mutex_lock(&text_mutex);
            text_ahead_tail = text_ahead_head;
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
            char ch = (char) text_termios.c_cc[VSTOP];

            pthread_mutex_lock(&text_mutex);
            root_console_write(&ch, 1);
            pthread_mutex_unlock(&text_mutex);
            break;
        }
        case TCION: {
            char ch = (char) text_termios.c_cc[VSTART];

            pthread_mutex_lock(&text_mutex);
            root_console_write(&ch, 1);
            pthread_mutex_unlock(&text_mutex);
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
    .openfd = text_openfd,
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
    /* Sensible terminal defaults so the shell gets ECHO | ICANON */
    text_termios.c_iflag = ICRNL | BRKINT;
    text_termios.c_oflag = OPOST | ONLCR;
    text_termios.c_cflag = CS8 | CREAD | HUPCL;
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
