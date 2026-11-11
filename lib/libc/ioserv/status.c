/*
 * RTMux I/O-service reply status taxonomy.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <stdatomic.h>
#include <stdio.h>
#include <sys/ioserv.h>
#include "internal.h"

struct ioserv_reply_atomic_counters {
    atomic_ullong sends;
    atomic_ullong receives;
    atomic_ullong denied_sends;
    atomic_ullong timeouts;
    atomic_ullong queue_full;
    atomic_ullong max_depth;
    atomic_ullong disconnects;
};

static struct ioserv_reply_atomic_counters counters;

static void ioserv_reply_counters_sample_depth(uint64_t queue_depth)
{
    unsigned long long old_depth;

    old_depth = atomic_load_explicit(&counters.max_depth, memory_order_relaxed);
    while (queue_depth > old_depth &&
           !atomic_compare_exchange_weak_explicit(
               &counters.max_depth, &old_depth,
               (unsigned long long) queue_depth, memory_order_relaxed,
               memory_order_relaxed)) {
    }
}

ioserv_reply_status_t ioserv_reply_status_from_errno(int error)
{
    switch (error) {
    case EOK:
        return IOSERV_REPLY_OK;
    case EACCES:
    case EPERM:
        return IOSERV_REPLY_DENIED;
    case ETIMEDOUT:
        return IOSERV_REPLY_TIMEOUT;
    case EBUSY:
    case EAGAIN:
        return IOSERV_REPLY_BUSY;
    case ENOENT:
    case ENODEV:
    case ENXIO:
    case EBADF:
    case ESRCH:
    case EPIPE:
        return IOSERV_REPLY_UNAVAILABLE;
    case EIO:
    case ENOSPC:
    case EROFS:
        return IOSERV_REPLY_IO_FAILURE;
    case EFAULT:
        return IOSERV_REPLY_FAULTED;
    default:
        return IOSERV_REPLY_BAD_REQUEST;
    }
}

const char *ioserv_reply_status_name(ioserv_reply_status_t status)
{
    static const char *const names[] = {
        [IOSERV_REPLY_OK] = "OK",
        [IOSERV_REPLY_DENIED] = "DENIED",
        [IOSERV_REPLY_UNAVAILABLE] = "UNAVAILABLE",
        [IOSERV_REPLY_TIMEOUT] = "TIMEOUT",
        [IOSERV_REPLY_BAD_REQUEST] = "BAD_REQUEST",
        [IOSERV_REPLY_IO_FAILURE] = "IO_FAILURE",
        [IOSERV_REPLY_FAULTED] = "FAULTED",
        [IOSERV_REPLY_BUSY] = "BUSY",
    };

    if ((unsigned) status >= sizeof(names) / sizeof(names[0]) || !names[status])
        return "BAD_REQUEST";
    return names[status];
}

void ioserv_reply_counters_record_receive(uint64_t queue_depth)
{
    atomic_fetch_add_explicit(&counters.receives, 1, memory_order_relaxed);
    ioserv_reply_counters_sample_depth(queue_depth);
}

void ioserv_reply_counters_record_disconnect(void)
{
    atomic_fetch_add_explicit(&counters.disconnects, 1, memory_order_relaxed);
}

void ioserv_reply_counters_record(int error, uint64_t queue_depth)
{
    ioserv_reply_status_t status = ioserv_reply_status_from_errno(error);

    atomic_fetch_add_explicit(&counters.sends, 1, memory_order_relaxed);
    if (status == IOSERV_REPLY_DENIED)
        atomic_fetch_add_explicit(&counters.denied_sends, 1,
                                  memory_order_relaxed);
    else if (status == IOSERV_REPLY_TIMEOUT)
        atomic_fetch_add_explicit(&counters.timeouts, 1, memory_order_relaxed);
    else if (status == IOSERV_REPLY_BUSY)
        atomic_fetch_add_explicit(&counters.queue_full, 1,
                                  memory_order_relaxed);
    ioserv_reply_counters_sample_depth(queue_depth);
}

void ioserv_reply_counters_snapshot(ioserv_reply_counters_t *out)
{
    if (!out)
        return;
    out->sends = atomic_load_explicit(&counters.sends, memory_order_relaxed);
    out->receives =
        atomic_load_explicit(&counters.receives, memory_order_relaxed);
    out->denied_sends =
        atomic_load_explicit(&counters.denied_sends, memory_order_relaxed);
    out->timeouts =
        atomic_load_explicit(&counters.timeouts, memory_order_relaxed);
    out->queue_full =
        atomic_load_explicit(&counters.queue_full, memory_order_relaxed);
    out->max_depth =
        atomic_load_explicit(&counters.max_depth, memory_order_relaxed);
    out->disconnects =
        atomic_load_explicit(&counters.disconnects, memory_order_relaxed);
}

int ioserv_reply_counters_format(char *buf, size_t size)
{
    ioserv_reply_counters_t c;
    int n;

    if (!buf || size == 0)
        return EINVAL;
    ioserv_reply_counters_snapshot(&c);
    n = snprintf(
        buf, size,
        "ioserv_sends %llu\n"
        "ioserv_receives %llu\n"
        "ioserv_denied_sends %llu\n"
        "ioserv_timeouts %llu\n"
        "ioserv_queue_full %llu\n"
        "ioserv_max_depth %llu\n",
        (unsigned long long) c.sends, (unsigned long long) c.receives,
        (unsigned long long) c.denied_sends, (unsigned long long) c.timeouts,
        (unsigned long long) c.queue_full, (unsigned long long) c.max_depth);
    if (n < 0 || (size_t) n >= size)
        return ENOSPC;
    return EOK;
}
