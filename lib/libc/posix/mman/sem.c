/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <mqueue.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <devctl.h>
#include <devctl/misc.h>
#include <sys/ftype.h>
#include <sys/iomsg.h>
#include <sys/rtmux-core.h>

#include "connect.h"
#include "signal-state.h"

#define SEM_PREFIX "/dev/sem"

static int sem_ret(int ret)
{
    if (ret == EOK)
        return 0;

    errno = ret < 0 ? -ret : ret;
    return -1;
}

static int sem_named_fd(sem_t *sem)
{
    if (!sem || sem->owner != RTMUX_SYNC_NAMED_SEM) {
        errno = EINVAL;
        return -1;
    }

    return (int) sem->count;
}

static bool sem_name_valid(const char *name)
{
    if (!name || name[0] != '/' || name[1] == '\0' || strchr(name + 1, '/')) {
        errno = EINVAL;
        return false;
    }
    return true;
}

static char *sem_path(const char *name)
{
    char *path;

    if (!sem_name_valid(name))
        return NULL;

    path = malloc(sizeof(SEM_PREFIX) + strlen(name));
    if (!path) {
        errno = ENOMEM;
        return NULL;
    }
    (void) strcpy(path, SEM_PREFIX);
    (void) strcat(path, name);
    return path;
}

sem_t *sem_open(const char *name, int oflag, ...)
{
    sem_t *sem;
    unsigned initial = 0;
    mode_t mode = 0;
    int fd;

    if (!sem_name_valid(name))
        return SEM_FAILED;

    if (oflag & O_CREAT) {
        va_list ap;

        va_start(ap, oflag);
        mode = (mode_t) va_arg(ap, int);
        initial = va_arg(ap, unsigned);
        va_end(ap);
        if (initial > SEM_VALUE_MAX) {
            errno = EINVAL;
            return SEM_FAILED;
        }
    }

    sem = malloc(sizeof(*sem));
    if (!sem) {
        errno = ENOMEM;
        return SEM_FAILED;
    }

    fd = _connect_object(name, SEM_PREFIX, mode, oflag | O_RDWR, _FTYPE_SEM,
                         _IO_CONNECT_EXTRA_SEM, sizeof(initial), &initial);
    if (fd == -1) {
        free(sem);
        return SEM_FAILED;
    }

    sem->count = fd;
    sem->owner = RTMUX_SYNC_NAMED_SEM;
    return sem;
}

int sem_close(sem_t *sem)
{
    int fd = sem_named_fd(sem);

    if (fd == -1)
        return -1;

    if (close(fd) == -1)
        return -1;
    free(sem);
    return 0;
}

int sem_unlink(const char *name)
{
    char *path = sem_path(name);
    int ret;

    if (!path)
        return -1;

    ret = unlink(path);
    free(path);
    return ret;
}

int sem_init(sem_t *sem, int pshared, unsigned value)
{
    if (!sem || pshared || value > SEM_VALUE_MAX) {
        errno = pshared ? ENOTSUP : EINVAL;
        return -1;
    }

    sem->count = (long) value;
    return sem_ret(sync_type_create_r(RTMUX_SYNC_SEM, sem, NULL));
}

int sem_destroy(sem_t *sem)
{
    if (!sem) {
        errno = EINVAL;
        return -1;
    }

    return sem_ret(sync_destroy_r(sem));
}

int sem_getvalue(sem_t *sem, int *value)
{
    int fd;
    struct mq_attr attr;

    if (!sem || !value || sem->owner != RTMUX_SYNC_SEM) {
        fd = sem_named_fd(sem);
        if (fd == -1)
            return -1;
        if (!value) {
            errno = EINVAL;
            return -1;
        }
        if (devctl(fd, DEVCTL_MQ_GET_ATTR, &attr, sizeof(attr), NULL) != EOK) {
            errno = ENOSYS;
            return -1;
        }
        *value = (int) attr.mq_curmsgs;
        return 0;
    }

    *value = sem->count > 0 ? (int) sem->count : 0;
    return 0;
}

int sem_post(sem_t *sem)
{
    int fd;

    if (sem && sem->owner == RTMUX_SYNC_NAMED_SEM) {
        fd = (int) sem->count;
        return write(fd, NULL, 0) == 0 ? 0 : -1;
    }

    if (!sem || sem->owner != RTMUX_SYNC_SEM) {
        errno = EINVAL;
        return -1;
    }
    if (sem->count >= SEM_VALUE_MAX) {
        errno = EOVERFLOW;
        return -1;
    }

    return sem_ret(sync_sem_post_r(sem));
}

int sem_trywait(sem_t *sem)
{
    int fd;
    int flags;
    int ret;

    if (sem && sem->owner == RTMUX_SYNC_NAMED_SEM) {
        fd = (int) sem->count;
        flags = fcntl(fd, F_GETFL);
        if (flags == -1)
            return -1;
        if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1)
            return -1;
        ret = read(fd, NULL, 0) == 0 ? 0 : -1;
        (void) fcntl(fd, F_SETFL, flags);
        return ret;
    }

    if (!sem || sem->owner != RTMUX_SYNC_SEM) {
        errno = EINVAL;
        return -1;
    }

    return sem_ret(sync_sem_wait_r(sem, 1));
}

int sem_wait(sem_t *sem)
{
    libc_waiter_t w;
    int ret;

    if (sem && sem->owner == RTMUX_SYNC_NAMED_SEM)
        return read((int) sem->count, NULL, 0) == 0 ? 0 : -1;

    if (!sem || sem->owner != RTMUX_SYNC_SEM) {
        errno = EINVAL;
        return -1;
    }

    __libc_wait_enter(&w);
    ret = sync_sem_wait_r(sem, 0);
    __libc_wait_exit(&w);
    return sem_ret(ret);
}

int sem_timedwait(sem_t *sem, const struct timespec *abs_timeout)
{
    libc_waiter_t w;
    uint64_t timeout;
    int ret;

    if (sem && sem->owner == RTMUX_SYNC_NAMED_SEM) {
        if (!TIMESPEC_VALID(abs_timeout)) {
            errno = EINVAL;
            return -1;
        }
        ret = sem_trywait(sem);
        if (ret == 0 || errno != EAGAIN)
            return ret;
        timeout = timespec2nsec(abs_timeout);
        ret = __timer_timeout_r(CLOCK_REALTIME,
                                TIMER_ABSTIME | RTMUX_TIMEOUT_REPLY, NULL,
                                &timeout, NULL);
        if (ret != EOK)
            return sem_ret(ret);
        ret = sem_wait(sem);
        (void) __timer_timeout_r(CLOCK_REALTIME, 0, NULL, NULL, NULL);
        if (ret == -1 && errno == EINTR) {
            /* The timeout unblocks the wait with EINTR; so can a caught signal.
             * Report a timeout only once the deadline has passed.
             */
            struct timespec now;

            if (clock_gettime(CLOCK_REALTIME, &now) == 0 &&
                (now.tv_sec > abs_timeout->tv_sec ||
                 (now.tv_sec == abs_timeout->tv_sec &&
                  now.tv_nsec >= abs_timeout->tv_nsec)))
                errno = ETIMEDOUT;
        }
        return ret;
    }

    if (!sem || sem->owner != RTMUX_SYNC_SEM || !TIMESPEC_VALID(abs_timeout)) {
        errno = EINVAL;
        return -1;
    }

    ret = sync_sem_wait_r(sem, 1);
    if (ret != -EAGAIN)
        return sem_ret(ret);

    timeout = timespec2nsec(abs_timeout);
    ret = __timer_timeout_r(CLOCK_REALTIME, TIMER_ABSTIME | RTMUX_TIMEOUT_SEM,
                            NULL, &timeout, NULL);
    if (ret != EOK)
        return sem_ret(ret);

    __libc_wait_enter(&w);
    ret = sync_sem_wait_r(sem, 0);
    __libc_wait_exit(&w);
    (void) __timer_timeout_r(CLOCK_REALTIME, 0, NULL, NULL, NULL);
    return sem_ret(ret);
}
