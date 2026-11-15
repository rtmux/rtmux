/*
 * Small shared helpers for RTMux LKL domain servers.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/dispatch.h>
#include <sys/ioserv.h>
#include <sys/iomsg.h>
#include <sys/rtmux-core.h>
#include <unistd.h>

#include <lkl.h>
#include <lkl_host.h>

#include "server.h"

static pthread_mutex_t admission_lock = PTHREAD_MUTEX_INITIALIZER;

void rtmux_lkl_status(const char *s)
{
    (void) write(STDERR_FILENO, s, strlen(s));
}

int rtmux_lkl_start_kernel(const char *name,
                           const char *stack,
                           const char *cmdline)
{
    int ret;

    fprintf(stderr, "%s: start %s\n", name, stack);
    ret = lkl_start_kernel(cmdline);
    fprintf(stderr, "%s: lkl_start_kernel returned\n", name);
    if (ret < 0) {
        fprintf(stderr, "%s: lkl_start_kernel: %s\n", name, lkl_strerror(ret));
        return -1;
    }
    return 0;
}

size_t rtmux_lkl_text_format(char *buf, size_t bufsz, const char *fmt, ...)
{
    int n;
    va_list ap;

    va_start(ap, fmt);
    n = vsnprintf(buf, bufsz, fmt, ap);
    va_end(ap);
    if (n < 0) {
        if (bufsz != 0)
            buf[0] = '\0';
        return 0;
    }
    if ((size_t) n >= bufsz)
        return bufsz == 0 ? 0 : bufsz - 1;
    return (size_t) n;
}

/* Whether ctp holds a lone _IO_NOTIFY (not part of a combined message). A
 * request that finds the admission lock taken is refused with EAGAIN, and an
 * _IO_NOTIFY client takes EAGAIN for "armed", so poll would sleep with nothing
 * armed or report a busy descriptor as POLLERR. Waiting for the lock instead
 * would stall a poll behind a blocking read. A notify handler therefore runs
 * without the admission lock and guards what it reads with a lock of its own. A
 * lone message is at offset 0 and carries no combine flag.
 */
static int lone_notify(const ioserv_context_t *ctp)
{
    const ioserv_msg_t *cur = ctp->msg;

    return ctp->offset == 0 && cur->type == _IO_NOTIFY &&
           !(cur->combine.combine_len & _IO_COMBINE_FLAG);
}

/* Set on the dispatch thread whose lock call skipped the admission lock for a
 * lone _IO_NOTIFY. The unlock cannot look at the message again: the handler
 * writes its reply over it in place (io_notify_t is a union), and a read reply
 * can leave bytes there that look like a lone _IO_NOTIFY.
 */
static pthread_key_t notify_skip_key;
static pthread_once_t notify_skip_once = PTHREAD_ONCE_INIT;
static int notify_skip_ok;

static void notify_skip_init(void)
{
    notify_skip_ok = pthread_key_create(&notify_skip_key, NULL) == 0;
}

int rtmux_lkl_admission_lock(ioserv_context_t *ctp, void *msg, void *ocb)
{
    (void) msg;
    (void) ocb;
    if (lone_notify(ctp)) {
        (void) pthread_once(&notify_skip_once, notify_skip_init);
        if (notify_skip_ok &&
            pthread_setspecific(notify_skip_key, (void *) 1) == 0)
            return EOK;
    }
    return pthread_mutex_trylock(&admission_lock) == EOK ? EOK : EAGAIN;
}

int rtmux_lkl_admission_wait(ioserv_context_t *ctp, void *msg, void *ocb)
{
    (void) ctp;
    (void) msg;
    (void) ocb;

    pthread_mutex_lock(&admission_lock);
    return EOK;
}

int rtmux_lkl_admission_unlock(ioserv_context_t *ctp, void *msg, void *ocb)
{
    (void) ctp;
    (void) msg;
    (void) ocb;
    if (notify_skip_ok && pthread_getspecific(notify_skip_key)) {
        (void) pthread_setspecific(notify_skip_key, NULL);
        return EOK;
    }
    pthread_mutex_unlock(&admission_lock);
    return EOK;
}

static ioserv_context_t *lkl_pool_block(ioserv_context_t *ctp)
{
    return (ioserv_context_t *) dispatch_block((dispatch_context_t *) ctp);
}

static int lkl_pool_handler(ioserv_context_t *ctp)
{
    return dispatch_handler((dispatch_context_t *) ctp);
}

static ioserv_context_t *lkl_pool_context_alloc(dispatch_t *dpp)
{
    return (ioserv_context_t *) dispatch_context_alloc(dpp);
}

static void lkl_pool_context_free(ioserv_context_t *ctp)
{
    dispatch_context_free((dispatch_context_t *) ctp);
}

int rtmux_lkl_dispatch_pool_start(dispatch_t *dpp, const char *name)
{
    thread_pool_attr_t attr = {
        .handle = dpp,
        .block_func = lkl_pool_block,
        .handler_func = lkl_pool_handler,
        .context_alloc = lkl_pool_context_alloc,
        .context_free = lkl_pool_context_free,
        .lo_water = 1,
        .increment = 1,
        .hi_water = 2,
        .maximum = 2,
    };
    thread_pool_t *pool;

    pool = thread_pool_create(&attr, 0);
    if (!pool) {
        fprintf(stderr, "%s: thread_pool_create: %s\n", name, strerror(errno));
        return -1;
    }
    if (thread_pool_start(pool) != EOK) {
        fprintf(stderr, "%s: thread_pool_start: %s\n", name, strerror(errno));
        return -1;
    }
    for (;;)
        pause();
}

dispatch_t *rtmux_lkl_dispatch_begin(const char *name)
{
    dispatch_t *dpp = dispatch_create();

    if (!dpp)
        fprintf(stderr, "%s: dispatch_create: %s\n", name, strerror(errno));
    return dpp;
}

int rtmux_lkl_dispatch_serve(dispatch_t *dpp, const char *name)
{
    if (rtmux_detach(EXIT_SUCCESS) != EOK) {
        fprintf(stderr, "%s: rtmux_detach: %s\n", name, strerror(errno));
        return -1;
    }
    return rtmux_lkl_dispatch_pool_start(dpp, name);
}
