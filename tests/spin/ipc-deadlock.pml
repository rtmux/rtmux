/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Q43 abstract model: wait-for deadlock freedom of blocking synchronous sends.
 *
 * The send graph comes from "edges.inc", which scripts/check-ipc-deadlock.sh
 * generates from docs/ipc-send-ranks.tsv (the shipped graph) or reads from
 * tests/ipc-deadlock/ (the planted cyclic graph).  It defines NNODES and
 * EDGE_SET (statements setting edge[from * NNODES + to]).
 *
 * Every node is a single-threaded process, the worst case: a node with no
 * incoming edge is a client that sends once along each of its edges; every
 * other node is a server that loops receiving a request, forwarding to all
 * of its successors in turn (a blocking rendezvous Send, then waits for the
 * Reply), and replying to the requester.  A server is only allowed to rest at
 * its receive (label `end`), so a state where every live process is blocked in
 * a send or a reply wait is an invalid end state -- Spin's deadlock report.
 * Reply and notifications never block and are not modeled as edges.
 *
 * Every node of the graph is modeled, including sinks (nodes with no outgoing
 * edge: they receive and reply).  Servers always run; of the clients (nodes with
 * no incoming edge) only those in CLIENT_MASK (bit i = node i, set by the gate
 * with -D) run.  The gate checks every non-empty subset of the clients, so any
 * number of clients contend for the same servers.
 *
 * Over the rank-ordered shipped graph the search must report errors: 0.  Over
 * the planted cyclic graph it must find an invalid end state.
 */

#include "edges.inc"

bool edge[NNODES * NNODES];
bool has_pred[NNODES];

chan req[NNODES] = [0] of { byte }; /* rendezvous: sender id */
chan rep[NNODES] = [0] of { bit };  /* reply to a blocked sender */

proctype node(byte me) {
    byte from, j;
    bit ack;

    if
    :: !has_pred[me] ->
        /* client: one request along each outgoing edge */
        for (j : 0 .. NNODES - 1) {
            if
            :: edge[me * NNODES + j] ->
                req[j] ! me;
                rep[me] ? ack
            :: else -> skip
            fi
        }
    :: else ->
        do
        :: 1 ->
end:        req[me] ? from;
            for (j : 0 .. NNODES - 1) {
                if
                :: edge[me * NNODES + j] ->
                    req[j] ! me;
                    rep[me] ? ack
                :: else -> skip
                fi
            }
            rep[from] ! 1
        od
    fi
}

init {
    byte i, j;

    EDGE_SET
    for (i : 0 .. NNODES - 1) {
        for (j : 0 .. NNODES - 1) {
            if
            :: edge[i * NNODES + j] -> has_pred[j] = true
            :: else -> skip
            fi
        }
    }
    atomic {
        for (i : 0 .. NNODES - 1) {
            /* servers always run; of the clients only those in CLIENT_MASK */
            if
            :: has_pred[i] || ((CLIENT_MASK >> i) & 1) -> run node(i)
            :: else -> skip
            fi
        }
    }
}
