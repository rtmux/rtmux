/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <sys/rtmux-core.h>
#include <atomic.h>
#include <stddef.h>
#include <stdlib.h>
#include <sys/iomsg.h>
#include <sys/ioserv-private.h>
#include <sys/path-registry.h>
#include <sys/dispatch.h>
#include <string.h>
#include <pthread.h>
#include "state.h"
#include "sleepon.h"
#include "../ioserv/api.h"
#include "../ioserv/internal.h"


/* TODO: Big re-write of this function to make it block until we are all
 * disconnected ...
 */
int ioserv_backend_detach(dispatch_t *dpp, int id, unsigned flags)
{
    ioserv_context_t ctp;
    struct link *linkl, *callerlink;
    int lastcount;

    /* We don't want to increment our link count here ... */
    linkl = _ioserv_link_query(id, 0);
    if ((!linkl) || (linkl->flags & _IOSERV_LINK_HALFOPEN)) {
        /* The _IOSERV_LINK_HALFOPEN check is to prevent a detach from happening
         * at the same time that service attach is still in progress. We
         * probably don't need both this bit _and_ the _IOSERV_LINK_DETACHWAIT
         * one - they can both use the same one.
         */
        errno = ENOENT;
        return -1;
    }

    /* Just turn off the extra-handler flag on this connection, but don't
     * invalidate the dpp function pointer since there might be other
     * mountpoints sharing this dpp.
     */
    if (linkl->flags & _IOSERV_LINK_OTHERFUNC) {
        linkl->flags &= ~(unsigned) _IOSERV_LINK_OTHERFUNC;
    }

    /* Rip the pathname out of the space so that no further connections are
     * forwarded from proc. There are still possibly connections "in flight" se
     * we don't rip this request out since the id might get accidentally re-used
     * by some other _ioserv_link_alloc().
     */
    if (_ioserv_link_free(id, IOSERV_DETACH_PATHNAME) == -1) {
        errno = ENOENT;
        return -1;
    }
    /* If all we wanted to do was detach the pathname ... then so be it */
    if (flags & IOSERV_DETACH_PATHNAME) {
        return 0;
    }

    /* Setting this flag will prevent any further requests to
     * ioserv_link_query() to fail on this id. There may be requests who are
     * already in the connect() functions which will be holding a valid link
     * handle pointer.
     *
     * We need to atomically test and set this flag since we want to make sure
     * that there is only one of us who is doing the detach at a time. If we set
     * the flag and find it is already set then we bail with ENOENT just as if
     * we didn't get the link value. This lets the previous guy handle the
     * detach.
     */
    if ((atomic_or_value(&linkl->flags, _IOSERV_LINK_DETACHWAIT)) &
        _IOSERV_LINK_DETACHWAIT) {
        errno = ENOENT;
        return -1;
    }

    /* We do a useless lookup on the id in order to prevent a race between this
     * code and the _ioserv_link_query() code which actually does the count
     * increment. This is also a sanity test.
     */
    if (_ioserv_link_query(id, 0)) {
#ifndef NDEBUG
        errno = EINVAL;
        return -1;
#endif
    }

    /* We wait for the system to go "quiet" in that all of the people who were
     * using the link structure are no longer using it. Don't count ourselves if
     * we were in a connect function.
     */
    callerlink = pthread_getspecific(_ioserv_thread_key);
    lastcount = (callerlink == linkl) ? 1 : 0;

    /* TODO: Use a dedicated sleepon. Fix _ioserv_link_return */
    _sleepon_lock(&_sleepon_default);
    while (linkl->count > lastcount) {
        _sleepon_wait(&_sleepon_default, linkl, 0);
    }
    _sleepon_unlock(&_sleepon_default);

    /* Invalidate the link so it won't be used again by accident */
    if (lastcount) {
        pthread_setspecific(_ioserv_thread_key, NULL);
    }

    memset(&ctp, 0, sizeof(ctp));
    (void) _ioserv_detach_id(&ctp, id, flags & IOSERV_DETACH_CLOSE);

    /* This will actually rip the link entry out and free it. There should be no
     * active requests in the system at this point and the id will get re-used
     * by some other attach.
     */
    if (_ioserv_link_free(id, 0) == -1) {
        return -1;
    }

    return 0;
}
