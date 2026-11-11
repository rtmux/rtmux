/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef SERVERS_LOGGER_SERVICE_H
#define SERVERS_LOGGER_SERVICE_H

void *logger(void *dummy);

int io_read(ioserv_context_t *ctp, io_read_t *msg, struct slog_ocb *ocb);
int io_write(ioserv_context_t *ctp, io_write_t *msg, struct slog_ocb *ocb);
int io_notify(ioserv_context_t *ctp, io_notify_t *msg, struct slog_ocb *ocb);
void slog_notify_trigger(struct slogdev *trp, int index);
void slog_notify_remove(ioserv_context_t *ctp, struct slogdev *trp);
int io_unblock(ioserv_context_t *ctp,
               io_notification_t *msg,
               struct slog_ocb *ocb);
int io_console_write(ioserv_context_t *ctp,
                     io_write_t *msg,
                     struct slog_ocb *ocb);
int io_msg(ioserv_context_t *ctp, io_msg_t *msg, struct slog_ocb *ocb);

#endif
