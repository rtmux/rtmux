/*
 * pthread_attr scheduling and guard-size accessors.
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 RTMux contributors
 */

#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <sys/sched-ops.h>

static int valid_policy(int policy)
{
    return policy >= RTMUX_SCHED_NOCHANGE && policy <= SCHED_SPORADIC;
}

int pthread_attr_getguardsize(const pthread_attr_t *attr, size_t *guardsize)
{
    if (!attr || !guardsize)
        return EINVAL;

    *guardsize = attr->guardsize;
    return EOK;
}

int pthread_attr_setguardsize(pthread_attr_t *attr, size_t guardsize)
{
    if (!attr)
        return EINVAL;

    attr->guardsize = (unsigned) guardsize;
    return EOK;
}

int pthread_attr_getinheritsched(const pthread_attr_t *attr, int *inheritsched)
{
    if (!attr || !inheritsched)
        return EINVAL;

    *inheritsched = attr->flags & PTHREAD_INHERITSCHED_MASK;
    return EOK;
}

int pthread_attr_setinheritsched(pthread_attr_t *attr, int inheritsched)
{
    if (!attr)
        return EINVAL;

    switch (inheritsched) {
    case PTHREAD_INHERIT_SCHED:
    case PTHREAD_EXPLICIT_SCHED:
        attr->flags &= ~PTHREAD_INHERITSCHED_MASK;
        attr->flags |= inheritsched;
        return EOK;
    default:
        return EINVAL;
    }
}

int pthread_attr_getschedparam(const pthread_attr_t *attr,
                               struct sched_param *param)
{
    if (!attr || !param)
        return EINVAL;

    *param = attr->sched_param;
    return EOK;
}

int pthread_attr_setschedparam(pthread_attr_t *attr,
                               const struct sched_param *param)
{
    if (!attr || !param)
        return EINVAL;

    attr->sched_param = *param;
    return EOK;
}

int pthread_attr_getschedpolicy(const pthread_attr_t *attr, int *policy)
{
    if (!attr || !policy)
        return EINVAL;

    *policy = attr->policy;
    return EOK;
}

int pthread_attr_setschedpolicy(pthread_attr_t *attr, int policy)
{
    if (!attr || !valid_policy(policy))
        return EINVAL;

    attr->policy = policy;
    return EOK;
}

int pthread_attr_getscope(const pthread_attr_t *attr, int *contentionscope)
{
    if (!attr || !contentionscope)
        return EINVAL;

    *contentionscope = attr->flags & PTHREAD_CONTENTIONSCOPE_MASK;
    return EOK;
}

int pthread_attr_setscope(pthread_attr_t *attr, int contentionscope)
{
    if (!attr)
        return EINVAL;

    switch (contentionscope) {
    case PTHREAD_SCOPE_SYSTEM:
        attr->flags &= ~PTHREAD_CONTENTIONSCOPE_MASK;
        return EOK;
    case PTHREAD_SCOPE_PROCESS:
        return ENOTSUP;
    default:
        return EINVAL;
    }
}
