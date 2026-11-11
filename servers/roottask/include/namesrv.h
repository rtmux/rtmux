/*
 * RTMux namesrv extraction boundary.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef _NAMESRV_H
#define _NAMESRV_H

int namesrv_resolve_io(ioserv_context_t *ctp, struct _io_connect *connect);
int namesrv_change_root_cwd(message_context_t *ctp,
                            struct _io_connect *connect);

#endif
