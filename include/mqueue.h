/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* mqueue.h: message queue attribute layout shared with named semaphores */

#if !defined _MQUEUE_H_INCLUDED
#define _MQUEUE_H_INCLUDED

#ifndef __PLATFORM_H_INCLUDED
#include <sys/platform.h>
#endif

#if !defined __TYPES_H_INCLUDED
#include <sys/types.h>
#endif

__BEGIN_DECLS

struct mq_attr {
    long mq_maxmsg;  /* maximum number of messages stored */
    long mq_msgsize; /* maximum message length */
    long mq_flags;
    long mq_curmsgs;  /* current number of messages stored */
    long mq_sendwait; /* number of processes waiting to send */
    long mq_recvwait; /* number of processes waiting to receive */
};

/* A message-queue descriptor. RTMux backs a queue with a shared-memory object
 * (POSIX permits mapping message queues onto existing IPC); the descriptor is
 * the object's file descriptor and (mqd_t) -1 is the error return.
 */
typedef int mqd_t;

struct timespec;

extern mqd_t mq_open(const char *__name, int __oflag, ...);
extern int mq_close(mqd_t __mqdes);
extern int mq_unlink(const char *__name);
extern int mq_send(mqd_t __mqdes,
                   const char *__msg,
                   size_t __len,
                   unsigned __prio);
extern ssize_t mq_receive(mqd_t __mqdes,
                          char *__msg,
                          size_t __len,
                          unsigned *__prio);
extern int mq_timedsend(mqd_t __mqdes,
                        const char *__msg,
                        size_t __len,
                        unsigned __prio,
                        const struct timespec *__abstime);
extern ssize_t mq_timedreceive(mqd_t __mqdes,
                               char *__msg,
                               size_t __len,
                               unsigned *__prio,
                               const struct timespec *__abstime);
extern int mq_getattr(mqd_t __mqdes, struct mq_attr *__attr);
extern int mq_setattr(mqd_t __mqdes,
                      const struct mq_attr *__attr,
                      struct mq_attr *__oattr);

__END_DECLS
#endif
