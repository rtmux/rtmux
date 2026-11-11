/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <core/globals.h>

void rdecl connect_detach(connect_t *cop, unsigned prio)
{
    channel_t *chp;

    if (cop->process && cop->cap != 0) {
        (void) rtmux_cap_revoke(&cop->process->endpoint_caps, cop->cap, NULL);
        cop->cap = 0;
    }

    if ((cop->flags & COF_VCONNECT) == 0) {
        /* Inform the server that the connection has been dropped via a
         * notification.
         */
        chp = cop->channel;
        if (chp) {
            if (chp->flags & RTMUX_FLG_CHF_DISCONNECT) {
                /* Tell the server. */
                _TRACE_COMM_EMIT_SNOTIFICATION_DIS(
                    cop, cop->scoid | RTMUX_SIDE_CHANNEL,
                    prio | _NOTIFICATION_PRIO_BOOST);
                notification_deliver(
                    chp, prio | _NOTIFICATION_PRIO_BOOST,
                    _NOTIFICATION_CODE_DISCONNECT, 0,
                    cop->scoid | (int) RTMUX_SIDE_CHANNEL,
                    cop->nresv ? NOTIFICATION_DELIVER_RESERVED_FLAG : 0);

                /* The reserved soul has been popped (or the server is gone);
                 * the reservation has done its job either way.
                 */
                connect_unreserve(cop);

                /* The client is no longer associated with the connection.
                 * Setting process to NULL will allow lookup_rcvid() to fail if
                 * further operations are attempted on the rcvid.
                 */
                cop->process = 0;
                return;
            } else {
                /* Remove the connection from the servers channels vector. */
                vector_rem(&chp->process->chancons, cop->scoid);
            }
        }
    } else {
        LINK1_REM(cop->un.lcl.cop->next, cop, connect_t);
        if (connect_links_drop(cop->un.lcl.cop)) {
            connect_detach(cop->un.lcl.cop, prio);
        }
    }

    /* cop is unpublished above (vector_rem from chancons in the non-VCONNECT
     * path, or LINK1_REM from the vconnect chain); drain RCU, revoke every cap
     * naming it, and recycle the soul. The owner-only revoke at the top of this
     * function misses a cap that was IPC-transferred to another process, so the
     * fan-out inside connect_soul_free is what actually closes that hole.
     */
    connect_soul_free(cop->process, cop);
}



/* Drop a client reference to a connection: the fd slot of ksys_connect_detach,
 * or the reference of a send that did not block.
 *
 * links counts the fd slots and one reference per blocked or in-flight sender
 * (ksys_msg_sendv takes its reference with connect_link_get() before it
 * publishes the sender, so a running sibling's send either holds a reference or
 * sees links == 0 and fails). connect_links_drop() is therefore true for
 * exactly one party: this call, or the reply-side drop of the last blocked
 * sender, which runs connect_detach(). What the last drop does:
 * - channel already destroyed (cop->channel NULL): channel_destroy unpublished
 *    the server side and nothing else names cop, so return 1 and the caller
 *    frees the soul (connect_soul_free synchronizes, so not inside a read
 *    section);
 * - CHF_DISCONNECT channel: send the disconnect notice; the server's
 *    ConnectDetach of its scoid then frees the soul (links == 0);
 * - anything else (no notice wanted, shared VCONNECT or remote connection):
 *    nothing, as before; channel_destroy / process exit reap it.
 *    connect_detach's vector_rem on the server's chancons is not safe from a
 *    live client.
 * The read section keeps chp and cop valid across the notice: the server may
 * free cop as soon as the notice is queued, and that free waits for readers.
 * channel_destroy writes cop->channel and reads cop->next with no preemption
 * point between, so a reaped cop is never freed under that walk.
 */
int rdecl connect_client_drop(connect_t *cop, unsigned prio)
{
    channel_t *chp;
    int reap = 0;

    rcu_read_lock(&connobj_rcu);
    if (connect_links_drop(cop) &&
        !(cop->flags & (COF_VCONNECT | COF_NETCON))) {
        chp = __atomic_load_n(&cop->channel, __ATOMIC_ACQUIRE);
        if (!chp) {
            reap = 1;
        } else if ((chp->flags & RTMUX_FLG_CHF_DISCONNECT) &&
                   !(chp->flags & RTMUX_FLG_CHF_GLOBAL)) {
            _TRACE_COMM_EMIT_SNOTIFICATION_DIS(cop,
                                               cop->scoid | RTMUX_SIDE_CHANNEL,
                                               prio | _NOTIFICATION_PRIO_BOOST);
            notification_deliver(
                chp, prio | _NOTIFICATION_PRIO_BOOST,
                _NOTIFICATION_CODE_DISCONNECT, 0,
                cop->scoid | (int) RTMUX_SIDE_CHANNEL,
                cop->nresv ? NOTIFICATION_DELIVER_RESERVED_FLAG : 0);
            connect_unreserve(cop);

            /* No longer associated with the client: lookup_rcvid() must fail.
             */
            cop->process = 0;
        }
    }
    rcu_read_unlock(&connobj_rcu);
    return reap;
}



void rdecl connect_coid_disconnect(connect_t *cop,
                                   channel_t *chp,
                                   unsigned prio)
{
    unsigned i, flag = 0;
    connect_t *cop2;
    vector_t *vec;

    if ((chp->flags & RTMUX_FLG_CHF_COID_DISCONNECT) == 0)
        return;

    for (vec = &chp->process->fdcons;;) {
        for (i = 0; i < vec->nentries; ++i) {
            if (VECP2(cop2, vec, i) == cop) {
                _TRACE_COMM_EMIT_SNOTIFICATION_DEA(cop, (-1), prio);
                notification_deliver(chp, prio, _NOTIFICATION_CODE_COIDDEATH,
                                     (unsigned) i | (unsigned) flag, -1, 0);
            }
        }

        if (vec == &chp->process->chancons)
            break;

        vec = &chp->process->chancons;
        flag = RTMUX_SIDE_CHANNEL;
    }
}
