/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <pthread.h>
#include <sys/ioserv-private.h>
#include "internal.h"

struct link *_ioserv_link_list;
struct _ioserv_handle_table _ioserv_io_table = {
    .vector = NULL,
    .free_list = NULL,
    .mutex = PTHREAD_MUTEX_INITIALIZER,
    .cond = PTHREAD_COND_INITIALIZER,
    .nentries = 0,
    .total = 0,
    .free = 0,
    .min = _IOSERV_CLIENT_MIN * _IOSERV_CLIENT_FD_MIN,
    .free_buckets = NULL,
    .total_buckets = 0,
    .nfree_buckets = 0,
    .min_buckets = _IOSERV_CLIENT_MIN,
    .nlists_max = _IOSERV_CLIENT_FD_MAX / 4,
};

pthread_key_t _ioserv_thread_key;
