/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef RTMUX_LKL_DOMAIN_H
#define RTMUX_LKL_DOMAIN_H

#include <stdbool.h>

#include <lkl.h>
#include <lkl_host.h>

#define RTMUX_LKL_FSD_CMDLINE "mem=16M"
#define RTMUX_LKL_NETD_CMDLINE "mem=16M"
#define RTMUX_LKL_NETD_STACK_SIZE (768U * 1024U)
#define RTMUX_LKL_DEFAULT_STACK_SIZE (512U * 1024U)

/* The LKL domain scheduling budget, declared here rather than left as a magic
 * number in host.c. A device IST runs SCHED_RR at this priority so hardware
 * wakeups preempt the dispatch worker; the worker itself has no fixed priority
 * -- it inherits the waiting client's priority per message (receive PI).
 */
#define RTMUX_LKL_IST_PRIORITY 21

struct rtmux_lkl_host_stats {
    unsigned long long page_current;
    unsigned long long page_high;
    unsigned long long page_total;
    unsigned long long dma_current;
    unsigned long long dma_high;
    unsigned long long dma_total;
    unsigned long long net_bounce_rx_bytes;
    unsigned long long net_bounce_tx_bytes;
    unsigned long long lkl_block_read_bytes;
    unsigned long long lkl_block_write_bytes;
    unsigned long long thread_count;
    unsigned long long thread_stack_bytes;
};

extern struct lkl_host_operations lkl_host_ops;
extern struct lkl_dev_blk_ops lkl_dev_blk_ops;

/* Map a negated LKL/Linux errno to an RTMux POSIX errno (EOK on success, EIO
 * for unmapped codes). Defined once in host.c; both servers share it so no raw
 * LKL errno leaks past the service boundary.
 */
int lkl_errno(long ret);

int rtmux_lkl_host_selftest(void);
void rtmux_lkl_host_stats(struct rtmux_lkl_host_stats *out);
void rtmux_lkl_host_stats_net_bounce_rx(size_t bytes);
void rtmux_lkl_host_stats_net_bounce_tx(size_t bytes);
void rtmux_lkl_host_stats_lkl_block_read(size_t bytes);
void rtmux_lkl_host_stats_lkl_block_write(size_t bytes);
void rtmux_lkl_block_crash_after_request(bool enabled);
struct lkl_netdev *rtmux_lkl_netdev_create(void);
void rtmux_lkl_netdev_set_delegated_mmio(bool enabled);
void rtmux_lkl_netdev_crash_after_tx(bool enabled);
void rtmux_lkl_netdev_destroy(struct lkl_netdev *nd);

#endif
