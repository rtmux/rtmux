/*
 * Small shared helpers for RTMux LKL domain servers.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef RTMUX_LKL_SERVER_H
#define RTMUX_LKL_SERVER_H

#include <stddef.h>
#include <sys/dispatch.h>
#include <sys/ioserv.h>

struct _io_connect;

void rtmux_lkl_status(const char *s);
int rtmux_lkl_start_kernel(const char *name,
                           const char *stack,
                           const char *cmdline);
size_t rtmux_lkl_text_format(char *buf, size_t bufsz, const char *fmt, ...);
int rtmux_lkl_connect_path_ok(size_t received, const struct _io_connect *c);
int rtmux_lkl_admission_lock(ioserv_context_t *ctp, void *msg, void *ocb);
int rtmux_lkl_admission_wait(ioserv_context_t *ctp, void *msg, void *ocb);
int rtmux_lkl_admission_unlock(ioserv_context_t *ctp, void *msg, void *ocb);
int rtmux_lkl_dispatch_pool_start(dispatch_t *dpp, const char *name);

/* Create a dispatch handle, or print a name-tagged error and return NULL. The
 * caller attaches its personality resource paths, then hands the handle to
 * rtmux_lkl_dispatch_serve to detach from the parent and run the pool.
 */
dispatch_t *rtmux_lkl_dispatch_begin(const char *name);
int rtmux_lkl_dispatch_serve(dispatch_t *dpp, const char *name);

#endif
