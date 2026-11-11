/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * system logger service
 *
 * io_write / io_console_write handlers — insert events into circular buffer.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "externs.h"

#define _SLOG_MINSIZE (3 * sizeof(int))

static void check_overrun(struct slogdev *trp, int *ptr, int cnt);

static int _io_write_log(ioserv_context_t *ctp,
                         io_write_t *msg,
                         struct slog_ocb *ocb,
                         unsigned cnt,
                         int txt)
{
    unsigned n;
    int *ptr;
    int *ptr2;
    struct slogdev *trp;
    struct waiting *wap;
    struct timeval tval;
    int txt_hdr[_SLOG_HDRINTS] = {
        [0] = _SLOG_TEXTBIT,
    };

    /* The /dev/console entrypoint doesn't add the HDRINTS, so if this is a
     * /dev/console entry, we'll have to add the space for the headers here.
     */
    if (txt)
        cnt += _SLOG_HDRINTS;

    /* If no room, remove events to make room. This is the normal case after we
     * have been running for a while.
     */
    trp = ocb->dev;
    while (((unsigned) NumInts - trp->cnt) < cnt) {
        n = (unsigned) _SLOG_GETCOUNT(*trp->get) + _SLOG_HDRINTS;
        check_overrun(trp, trp->get, (int) n);
        trp->cnt -= n;
        trp->get += n;
        if (trp->get >= trp->end)
            trp->get = trp->beg + (trp->get - trp->end);
    }

    /* Patch in the size of the event and the time. */
    if (txt) {
        ptr = txt_hdr;
        ptr2 = (int *) (sizeof(msg->i) + (char *) &msg->i);
    } else {
        ptr = (int *) (sizeof(msg->i) + (char *) &msg->i);
        ptr2 = ptr + _SLOG_HDRINTS;
    }
    gettimeofday(&tval, NULL);
    {
        unsigned msecs = (unsigned) (tval.tv_usec / 1000);
        ptr[0] = (int) ((((unsigned) *ptr & ~0x00fffff0u) | (msecs << 4)) |
                        ((cnt - _SLOG_HDRINTS) << 16));
        ptr[2] = (int) tval.tv_sec;
    }

    /* Echo text events to stdout if -t flag is set */
    if (TeeStdout && (ptr[0] & _SLOG_TEXTBIT)) {
        int major = _SLOG_GETMAJOR(ptr[1]);
        int sev = _SLOG_GETSEVERITY(ptr[0]);
        const char *text = (const char *) ptr2;
        printf("[%d.%d] %s", major, sev, text);
        if (text[0] && text[strlen(text) - 1] != '\n')
            putchar('\n');
        fflush(stdout);
    }

    if (Verbose >= 3)
        printf("Add %5ld %5u Put: %p, beg: %p end: %p\n", trp->put - trp->beg,
               cnt, (void *) trp->put, (void *) trp->beg, (void *) trp->end);

    /* Insert first the hdrints, and then the buffer. Check for wrap in the
     * circular buffer.
     */
    if (trp->put + _SLOG_HDRINTS >= trp->end) {
        n = (unsigned) (trp->end - trp->put);
        memcpy(trp->put, ptr, n * sizeof(int));
        memcpy(trp->beg, ptr + n, (_SLOG_HDRINTS - n) * sizeof(int));
        trp->put = trp->beg + _SLOG_HDRINTS - n;
    } else {
        memcpy(trp->put, ptr, _SLOG_HDRINTS * sizeof(int));
        trp->put += _SLOG_HDRINTS;
    }
    cnt -= _SLOG_HDRINTS;
    trp->cnt += _SLOG_HDRINTS;

    /* Second part: the payload data */
    if (trp->put + cnt >= trp->end) {
        n = (unsigned) (trp->end - trp->put);
        memcpy(trp->put, ptr2, n * sizeof(int));
        memcpy(trp->beg, ptr2 + n, (cnt - n) * sizeof(int));
        trp->put = trp->beg + cnt - n;
    } else {
        memcpy(trp->put, ptr2, cnt * sizeof(int));
        trp->put += cnt;
    }
    trp->cnt += cnt;

    /* PR 8029: ensure put != get (except at start), since read side uses that
     * condition for "empty buffer".
     */
    if (trp->put == trp->get) {
        n = (unsigned) _SLOG_GETCOUNT(*trp->get) + _SLOG_HDRINTS;
        check_overrun(trp, trp->get, (int) n);
        trp->cnt -= n;
        trp->get += n;
        if (trp->get >= trp->end)
            trp->get = trp->beg + (trp->get - trp->end);
    }

    /* PR 26878: wake up readers in priority order. */
    while ((wap = trp->waiting)) {
        trp->waiting = wap->next;
        ctp->rcvid = wap->rcvid;
        free(wap);
        if (ioserv_msg_again(ctp, ctp->rcvid) == -1) {
            if (Verbose >= 3)
                printf("ioserv_msg_again failed\n");
        }
    }
    if (trp->notify[SLOG_NOTIFY_INPUT].cnt <= (int) trp->cnt)
        slog_notify_trigger(trp, SLOG_NOTIFY_INPUT);

    return EOK;
}


int io_write(ioserv_context_t *ctp, io_write_t *msg, struct slog_ocb *ocb)
{
    unsigned cnt;
    size_t nbytes;

    if ((ocb->ioflag & _IO_FLAG_WR) == 0)
        return EBADF;

    if ((msg->i.xtype & _IO_XTYPE_MASK) != _IO_XTYPE_NONE)
        return EINVAL;

    /* Round up to int boundary */
    nbytes = ((size_t) msg->i.nbytes + (sizeof(int) - 1)) & ~(sizeof(int) - 1);
    if (nbytes < _SLOG_MINSIZE || nbytes > _SLOG_MAXSIZE)
        return EINVAL;

    /* msg->i.nbytes is the client's DECLARED length. _io_write_log() reads that
     * many bytes straight from the receive buffer, but only ctp->size bytes
     * were actually transferred; reading past them copies stale buffer contents
     * (earlier clients' payloads / uninitialized memory) into the shared slog
     * ring, where any /dev/slog reader can see it. Cap to the payload that was
     * actually received, keeping the int alignment the ring format needs.
     */
    {
        size_t recvd = ctp->size > 0 ? (size_t) ctp->size : 0;
        size_t avail = recvd > sizeof(msg->i)
                           ? (recvd - sizeof(msg->i)) & ~(sizeof(int) - 1)
                           : 0;
        if (nbytes > avail)
            nbytes = avail;
    }
    if (nbytes < _SLOG_MINSIZE)
        return EINVAL;

    cnt = (unsigned) (nbytes / sizeof(int));

    /* PR 26878: reply immediately to avoid priority inversion. */
    msg_reply(ctp->rcvid, (int) nbytes, NULL, 0);

    _io_write_log(ctp, msg, ocb, cnt, 0);
    if (CrashOnWrite) {
        (void) ioserv_detach(LoggerDpp, ocb->dev->id, 0);
        abort();
    }

    return IOSERV_NOREPLY;
}


static void check_overrun(struct slogdev *trp, int *ptr, int cnt)
{
    struct ocbs *list;
    struct slog_ocb *ocb;
    int *get;

    if (cnt == 0)
        return;

    for (list = trp->ocbs; list; list = list->next) {
        if ((ocb = list->ocb) && (get = OCBGET(ocb))) {
            if (!ptr && cnt == -1) {
                if (Verbose >= 2)
                    printf("Reset blocked reader\n");
                OCBGET(ocb) = NULL;
            } else if (get == ptr && ptr != trp->put) {
                if (Verbose >= 1)
                    printf("Overrun %ld %d\n", get - trp->beg, cnt);
                get += cnt;
                if (get >= trp->end)
                    get = trp->beg + (get - trp->end);
                OCBGET(ocb) = get;
            }
        }
    }
}


/* io_console_write handles IO_WRITE messages to /dev/console. These get put
 * into the slog buffer without caller-provided headers.
 */
int io_console_write(ioserv_context_t *ctp,
                     io_write_t *msg,
                     struct slog_ocb *ocb)
{
    int ret;
    size_t off, len, n, have;
    unsigned cnt;
    char *cptr;

    ret = EOK;
    cptr = (char *) (sizeof(msg->i) + (char *) &msg->i);
    off = 0;

    if ((ocb->ioflag & _IO_FLAG_WR) == 0)
        return EBADF;

    if ((msg->i.xtype & _IO_XTYPE_MASK) != _IO_XTYPE_NONE)
        return EINVAL;

    /* nbytes is a signed int32 from the client; a negative value would
     * sign-extend into a huge len. The per-chunk cap below keeps it safe, but
     * reject it up front like io_write()/devtext do rather than treating it as
     * a multi-gigabyte write.
     */
    if (msg->i.nbytes < 0)
        return EINVAL;
    len = (size_t) msg->i.nbytes;

    /* Payload bytes actually present in cptr for this iteration. The first
     * chunk is the inline data received with the message (ctp->size beyond the
     * io_write header); each later chunk is what msg_read() actually returned.
     * Capping n to this stops a client that declares a large nbytes but
     * transfers little from getting stale receive-buffer bytes echoed to the
     * console / slog ring.
     */
    have = ctp->size > (int) sizeof(msg->i)
               ? (size_t) ctp->size - sizeof(msg->i)
               : 0;

    while (off < len) {
        /* Limit write to max payload. */
        n = len - off;
        if (n > _SLOG_MAXSIZE - _SLOG_HDRINTS * sizeof(int) - 1)
            n = _SLOG_MAXSIZE - _SLOG_HDRINTS * sizeof(int) - 1;

        /* Ensure cptr holds up to n valid payload bytes. The first chunk is the
         * inline data received with the message; once that is consumed — or
         * when none arrived inline — pull the next chunk from the client rather
         * than treating an empty inline buffer as end-of-data (which would
         * truncate a legitimate multi-part write).
         */
        if (have == 0) {
            ret = msg_read(ctp->rcvid, cptr, (int) n,
                           (int) (off + sizeof(msg->i)));
            if (ret <= 0)
                break;
            have = (size_t) ret;
        }
        if (n > have)
            n = have;

        /* NUL terminate the string and figure out how many ints we're writing.
         */
        cptr[n] = '\0';
        cnt = (unsigned) (n / sizeof(int)) + 1;

        ret = _io_write_log(ctp, msg, ocb, cnt, 1);
        if (ret != EOK)
            break;

        off += n;
        have = 0; /* consumed this chunk; the next iteration re-fetches */
    }

    _IO_SET_WRITE_NBYTES(ctp, (int) off);
    return ret;
}
