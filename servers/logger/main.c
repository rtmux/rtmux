/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * system logger service
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "externs.h"

static int slog_open(ioserv_context_t *ctp,
                     io_open_t *msg,
                     struct slogdev *trp,
                     void *extra);
static int console_open(ioserv_context_t *ctp,
                        io_open_t *msg,
                        struct slogdev *trp,
                        void *extra);
static int slog_unlink(ioserv_context_t *ctp,
                       io_unlink_t *msg,
                       struct slogdev *trp,
                       void *extra);
static int slog_close_ocb(ioserv_context_t *ctp,
                          void *reserved,
                          struct slog_ocb *ocb);
static void parse_options(int argc, char *argv[]);
static void logger_init(struct slogdev *trp);

static struct slogdev SlogDev;

static const ioserv_connect_ops_t SlogConnectFuncs = {
    .nfuncs = IOSERV_CONNECT_NFUNCS,
    .open = slog_open,
    .unlink = slog_unlink,
};

static const ioserv_connect_ops_t ConsoleConnectFuncs = {
    .nfuncs = IOSERV_CONNECT_NFUNCS,
    .open = console_open,
};

static const ioserv_ops_t SlogIoFuncs = {
    .nfuncs = IOSERV_IO_NFUNCS,
    .read = io_read,
    .write = io_write,
    .close_ocb = slog_close_ocb,
    .notify = io_notify,
    .unblock = io_unblock,
    .msg = io_msg,
};

static const ioserv_ops_t ConsoleIoFuncs = {
    .nfuncs = IOSERV_IO_NFUNCS,
    .write = io_console_write,
    .close_ocb = slog_close_ocb,
};



int main(int argc, char *argv[])
{
    dispatch_t *dpp;
    ioserv_context_t *ctp;
    struct slogdev *trp;

    if (argc > 0 && argv[0])
        logger_program_name = argv[0];

    parse_options(argc, argv);

    /* Buffer allocation. The design puts all the data structures in one big
     * contiguious chunk of memory. Makes it easy to analyze if it was in a
     * memory mapped SRAM card which survives a crash or power failure.
     */
    trp = &SlogDev;
    trp->beg = malloc((size_t) NumInts * sizeof(int));
    if (!trp->beg) {
        fprintf(stderr, "%s: Insufficient memory to allocate buffers.\n",
                logger_program_name);
        exit(EXIT_FAILURE);
    }
    trp->end = trp->beg + NumInts;
    logger_init(trp);
    for (int i = 0; i < SLOG_NOTIFY_COUNT; i++)
        trp->notify[i].cnt = INT_MAX;

    /* Create a dispatch context to receive messages. */
    if (!(dpp = dispatch_create())) {
        fprintf(stderr, "%s: Unable to allocate dispatch context\n",
                logger_program_name);
        exit(EXIT_FAILURE);
    }
    LoggerDpp = dpp;

    /* All slogf/slogb calls should fit in the initial receive. The payload sits
     * right after the io_write header (that is the offset _io_write_log() reads
     * from), so the buffer must hold the whole header plus a maximal event; the
     * old "_SLOG_HDRINTS + sizeof(io_msg_t)" slack was 5 bytes short of that
     * and truncated a maximal event.
     */
    ioserv_attr_t res_attr = {
        .nparts_max = 2,
        .msg_max_size = sizeof(io_write_t) + _SLOG_MAXSIZE,
    };

    /* Create /dev/slog in the pathname space. At this point we can get opens.
     */
    if ((trp->id = ioserv_attach(dpp, &res_attr, "/dev/slog", _FTYPE_ANY,
                                 IOSERV_FLAG_SELF, &SlogConnectFuncs,
                                 &SlogIoFuncs, trp)) == -1) {
        fprintf(stderr, "%s: Unable to allocate device %s (%s)\n",
                logger_program_name, "/dev/slog", strerror(errno));
        exit(EXIT_FAILURE);
    }

    /* Create /dev/console in the pathname space. At this point we can get
     * opens.
     */
    if (AttachConsole &&
        ioserv_attach(dpp, &res_attr, "/dev/console", _FTYPE_ANY, 0,
                      &ConsoleConnectFuncs, &ConsoleIoFuncs, trp) == -1) {
        fprintf(stderr, "%s: Unable to allocate device %s (%s)\n",
                logger_program_name, "/dev/console", strerror(errno));
        exit(EXIT_FAILURE);
    }
    /* If a logfile was specified then create a thread to write it */
    if (LogFname) {
        sigset_t signalset;

        if (pthread_create(0, NULL, &logger, NULL) != 0) {
            fprintf(stderr, "%s: Unable to create logger thread.\n",
                    logger_program_name);
            exit(EXIT_FAILURE);
        }
        /* We want the logger thread to catch signals. Not us! */
        sigemptyset(&signalset);
        sigfillset(&signalset);
        sigprocmask(SIG_BLOCK, &signalset, NULL);
    }
    /* The logger service is single-threaded. */
    if (!(ctp = ioserv_context_alloc(dpp))) {
        fprintf(stderr, "%s: Unable to allocate I/O service context (%s)\n",
                logger_program_name, strerror(errno));
        exit(EXIT_FAILURE);
    }

    /* Detach from parent only after all fallible startup work has completed. */
    if (rtmux_detach(EXIT_SUCCESS) != EOK) {
        fprintf(stderr, "%s: Unable to detach from parent (%s)\n",
                logger_program_name, strerror(errno));
        exit(EXIT_FAILURE);
    }

    for (;;) {
        if (!(ctp = ioserv_block(ctp)))
            exit(EXIT_FAILURE);

        ioserv_handler(ctp);
    }

    exit(EXIT_SUCCESS);
}


static void parse_options(int argc, char *argv[])
{
    int opt;
    char *cp;

    /* Set defaults. */
    NumInts = 4096;           /* Works out to a 16K buffer */
    LogFsize = 0;             /* Default is to grow and grow... */
    FilterLog = _SLOG_DEBUG1; /* Log everything */
    LogFflags = 0;            /* Default no flags set */

    while ((opt = getopt(argc, argv, "Ccf:l:s:tvx")) != -1) {
        switch (opt) {
        case 'C':
            AttachConsole = false;
            break;
        case 'x':
            CrashOnWrite = true;
            break;
        case 't': /* Tee all text events to stdout */
            TeeStdout = 1;
            break;
        case 'c': /* Commit modifications as per O_SYNC */
            LogFflags |= LOGF_FLAG_OSYNC;
            break;
        case 'f':
            FilterLog = atoi(optarg);
            break;

        case 'l':
            LogFname = optarg;
            if ((cp = strchr(LogFname, ','))) {
                LogFsize = atoi(cp + 1) * 1024;
                cp[0] = '\0';
            }
            break;

        case 's':
            NumInts = atoi(optarg) * (1024 / 4);
            if (NumInts < 1024)
                NumInts = 1024;
            break;

        case 'v':
            ++Verbose;
            break;

        default:
            exit(EXIT_FAILURE);
        }
    }
}


static void logger_init(struct slogdev *trp)
{
    memset(trp->beg, 0, (size_t) NumInts * sizeof(trp->beg[0]));
    trp->get = trp->put = trp->beg;
    trp->cnt = 0;
}


static int slog_ocb_alloc(ioserv_context_t *ctp,
                          struct slogdev *trp,
                          const ioserv_ops_t *funcs)
{
    struct ocbs *ocbl;
    struct slog_ocb *ocb;

    if ((ocb = calloc(1, sizeof(*ocb)))) {
        ocb->dev = trp;
        ocb->ioflag = ((io_open_t *) ctp->msg)->connect.ioflag &
                      (~(unsigned) _IO_FLAG_MASK |
                       ((io_open_t *) ctp->msg)->connect.access);

        /* Only keep a list of open for reads on /dev/slog. The trp->id contains
         * the id for /dev/slog.
         */
        if (trp->id != ctp->id ||
            (((io_open_t *) ctp->msg)->connect.ioflag & _IO_FLAG_RD) == 0) {
            if (ioserv_open_bind(ctp, ocb, funcs) != -1)
                return EOK;
            free(ocb);
            return errno;
        }

        /* We keep a list of readers which is needed to check for overflow. */
        if ((ocbl = calloc(1, sizeof(*ocbl)))) {
            ocbl->ocb = ocb;
            ocbl->next = trp->ocbs;
            trp->ocbs = ocbl;
            if (ioserv_open_bind(ctp, ocb, funcs) != -1)
                return EOK;
            trp->ocbs = ocbl->next;
            free(ocbl);
        }

        free(ocb);
    }

    return ENFILE;
}


static int slog_open_common(ioserv_context_t *ctp,
                            io_open_t *msg,
                            struct slogdev *trp,
                            const ioserv_ops_t *funcs)
{
    if (msg->connect.subtype != _IO_CONNECT_OPEN &&
        msg->connect.subtype != _IO_CONNECT_COMBINE &&
        msg->connect.subtype != _IO_CONNECT_COMBINE_CLOSE)
        return ENOSYS;
    if (msg->connect.file_type != _FTYPE_ANY)
        return ENOSYS;
    if ((msg->connect.ioflag & (O_CREAT | O_EXCL)) == (O_CREAT | O_EXCL))
        return EEXIST;

    return slog_ocb_alloc(ctp, trp, funcs);
}


static int slog_open(ioserv_context_t *ctp,
                     io_open_t *msg,
                     struct slogdev *trp,
                     void *extra)
{
    (void) extra;
    return slog_open_common(ctp, msg, trp, &SlogIoFuncs);
}


static int console_open(ioserv_context_t *ctp,
                        io_open_t *msg,
                        struct slogdev *trp,
                        void *extra)
{
    (void) extra;
    return slog_open_common(ctp, msg, trp, &ConsoleIoFuncs);
}


static int slog_unlink(ioserv_context_t *ctp,
                       io_unlink_t *msg,
                       struct slogdev *trp,
                       void *extra)
{
    (void) ctp;
    (void) msg;
    (void) trp;
    (void) extra;
    return ENOSYS;
}


static int slog_close_ocb(ioserv_context_t *ctp,
                          void *reserved,
                          struct slog_ocb *ocb)
{
    struct slogdev *trp = ocb->dev;
    struct ocbs *cur, *prev;

    (void) reserved;
    slog_notify_remove(ctp, trp);
    prev = (struct ocbs *) &trp->ocbs;
    for (; (cur = prev->next); prev = cur)
        if (cur->ocb == ocb) {
            prev->next = cur->next;
            free(cur);
            break;
        }
    free(ocb);
    return EOK;
}
