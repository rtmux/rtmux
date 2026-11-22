/* SPDX-License-Identifier: Apache-2.0 */
#ifndef RTMUX_REPLY_CLIENT_H
#define RTMUX_REPLY_CLIENT_H

/* The client can be blocked on the received connection or on its virtual
 * connection. Snapshot blocked_on once because cancellation can clear it.
 * The caller retains its state, queue and delivery-claim checks. */
static inline connect_t *reply_client_connection(thread_t *client,
                                                 connect_t *received)
{
    connect_t *bound = __atomic_load_n(&client->blocked_on, __ATOMIC_ACQUIRE);
    if (bound == received)
        return bound;
    return bound && (bound->flags & COF_VCONNECT) &&
                   bound->un.lcl.cop == received
               ? bound
               : NULL;
}

/* The claim on a reply-blocked client (message-core.c). */
connect_t *reply_client_lock(connect_t *received,
                             thread_t *client,
                             channel_t **chpp);
bool reply_client_claim_locked(thread_t *client);
int reply_client_claim(thread_t *act,
                       connect_t **copp,
                       thread_t *client,
                       channel_t **chpp);
void msg_claim_release(channel_t *chp, thread_t *peer);

/* Called after publishing the new client and dropping the channel/connection
 * lock. Each caller applies the critical-thread policy for its receive path. */
static inline void reply_client_priority(thread_t *server,
                                         thread_t *client,
                                         kdispatch_t *dispatch)
{
    if (client) {
        thread_lend_msg(
            server, __atomic_load_n(&client->priority, __ATOMIC_ACQUIRE), true);
        (void) thread_end_orphan_loan(server);
        (void) thread_prio_update_dpp(server, dispatch, 1);
    } else {
        thread_lend_msg(server, 0, false);
        thread_prio_update(server);
    }
}
#endif
