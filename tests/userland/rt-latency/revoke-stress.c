/*
 * revoke-stress - endpoint detach latency under sender churn.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/dispatch.h>
#include <sys/rtmux-core.h>

#define DEFAULT_CLIENTS 4
#define DEFAULT_ITERS 64
#define DEFAULT_SAMPLES 5
#define PROBE_ITERS 128
#define WAIT_LOOPS 20000

struct revoke_args {
    const char *name;
    int chid;
    int max_iters;
    atomic_int stop;
    atomic_int ready;
    atomic_int sends;
    atomic_int failures;
    atomic_int clients_done;
    atomic_int server_done;
};

struct probe_args {
    int chid;
    int coid;
    atomic_int stop;
    atomic_int ready;
    atomic_int sends;
    atomic_int failures;
    atomic_int priority_ok;
    atomic_int client_done;
    atomic_int server_done;
    atomic_ullong max_roundtrip_ns;
};

static int monotonic_ns(uint64_t *out)
{
    return __clock_time(CLOCK_MONOTONIC, NULL, out);
}

static void record_max(atomic_ullong *target, unsigned long long value)
{
    unsigned long long old =
        atomic_load_explicit(target, memory_order_relaxed);

    while (old < value && !atomic_compare_exchange_weak_explicit(
                              target, &old, value, memory_order_relaxed,
                              memory_order_relaxed)) {
    }
}

static void *revoke_server(void *arg)
{
    struct revoke_args *args = arg;
    union {
        int msg;
        struct _notification n;
    } buf;

    atomic_store_explicit(&args->ready, 1, memory_order_release);
    while (!atomic_load_explicit(&args->stop, memory_order_acquire)) {
        int rcvid = msg_receive(args->chid, &buf, sizeof(buf), NULL);

        if (rcvid < 0)
            break;
        if (rcvid == 0) {
            /* The name_attach channel is CHF_DISCONNECT: the last client
             * name_close sends a notice that the server must answer by
             * detaching the scoid, then keep serving. It is not a stop signal.
             */
            if (buf.n.code == _NOTIFICATION_CODE_DISCONNECT)
                (void) __connect_detach((uint64_t) buf.n.scoid);
            continue;
        }
        (void) msg_reply(rcvid, EOK, &buf.msg, sizeof(buf.msg));
    }
    atomic_store_explicit(&args->server_done, 1, memory_order_release);
    return NULL;
}

static void *revoke_client(void *arg)
{
    struct revoke_args *args = arg;

    for (int i = 0; i < args->max_iters &&
                    !atomic_load_explicit(&args->stop, memory_order_acquire);
         i++) {
        int fd = name_open(args->name, 0);
        int msg = 0x72566f6b;
        int reply = 0;

        if (fd == -1) {
            atomic_fetch_add_explicit(&args->failures, 1,
                                      memory_order_relaxed);
            break;
        }
        if (msg_send(fd, &msg, sizeof(msg), &reply, sizeof(reply)) == 0 &&
            reply == msg) {
            atomic_fetch_add_explicit(&args->sends, 1, memory_order_relaxed);
        } else {
            atomic_fetch_add_explicit(&args->failures, 1,
                                      memory_order_relaxed);
            (void) name_close(fd);
            break;
        }
        (void) name_close(fd);
        (void) __sched_yield();
    }
    atomic_fetch_add_explicit(&args->clients_done, 1, memory_order_release);
    return NULL;
}

static void *probe_server(void *arg)
{
    struct probe_args *args = arg;
    int msg;

    atomic_store_explicit(&args->ready, 1, memory_order_release);
    while (!atomic_load_explicit(&args->stop, memory_order_acquire)) {
        int rcvid = msg_receive(args->chid, &msg, sizeof(msg), NULL);

        if (rcvid <= 0)
            break;
        (void) msg_reply(rcvid, EOK, &msg, sizeof(msg));
    }
    atomic_store_explicit(&args->server_done, 1, memory_order_release);
    return NULL;
}

static void *probe_client(void *arg)
{
    struct probe_args *args = arg;
    struct sched_param sp = {.sched_priority = 40};

    if (__sched_set(0, 0, SCHED_FIFO, &sp) == EOK)
        atomic_store_explicit(&args->priority_ok, 1, memory_order_release);
    else
        atomic_fetch_add_explicit(&args->failures, 1, memory_order_relaxed);

    for (int i = 0; i < PROBE_ITERS &&
                    !atomic_load_explicit(&args->stop, memory_order_acquire);
         i++) {
        uint64_t start, end;
        int msg = 0x70726f62;
        int reply = 0;

        if (monotonic_ns(&start) == -1)
            start = 0;
        if (msg_send(args->coid, &msg, sizeof(msg), &reply, sizeof(reply)) == 0
            && reply == msg) {
            atomic_fetch_add_explicit(&args->sends, 1, memory_order_relaxed);
            if (start != 0 && monotonic_ns(&end) != -1 && end >= start)
                record_max(&args->max_roundtrip_ns, end - start);
        } else {
            if (!atomic_load_explicit(&args->stop, memory_order_acquire))
                atomic_fetch_add_explicit(&args->failures, 1,
                                          memory_order_relaxed);
            break;
        }
        (void) __sched_yield();
    }
    atomic_store_explicit(&args->client_done, 1, memory_order_release);
    return NULL;
}

static void stop_probe(struct probe_args *args)
{
    atomic_store_explicit(&args->stop, 1, memory_order_release);
    (void) __connect_detach((uint64_t) args->coid);
    (void) channel_destroy(args->chid);
    for (int i = 0;
         i < WAIT_LOOPS &&
         !atomic_load_explicit(&args->client_done, memory_order_acquire);
         i++)
        (void) __sched_yield();
    for (int i = 0;
         i < WAIT_LOOPS &&
         !atomic_load_explicit(&args->server_done, memory_order_acquire);
         i++)
        (void) __sched_yield();
}

static int fail(const char *what)
{
    fprintf(stderr, "revoke-stress: %s: %s\n", what, strerror(errno));
    return EXIT_FAILURE;
}

static int cmp_u64(const void *a, const void *b)
{
    uint64_t av = *(const uint64_t *) a;
    uint64_t bv = *(const uint64_t *) b;

    return (av > bv) - (av < bv);
}

static void report(const char *name, uint64_t *samples, int n, int clients,
                   int iters, int sends, int failures)
{
    qsort(samples, (size_t) n, sizeof(samples[0]), cmp_u64);
    printf("revoke-stress: %s samples=%d clients=%d iters=%d sends=%d "
           "failures=%d p50-ns=%llu p99-ns=%llu max-ns=%llu\n",
           name, n, clients, iters, sends, failures,
           (unsigned long long) samples[n / 2],
           (unsigned long long) samples[((n - 1) * 99) / 100],
           (unsigned long long) samples[n - 1]);
}

static int run_once(int nclients, int iters, uint64_t *detach_ns,
                    uint64_t *ipc_ns, int *revoke_sends, int *revoke_failures,
                    int *ipc_sends, int *ipc_failures)
{
    static atomic_int seq;
    char name[64];
    char path[96];
    struct revoke_args rargs;
    struct probe_args pargs;
    name_attach_t *attach = NULL;
    pthread_t server, pserver, pclient;
    pthread_t *clients = NULL;
    uint64_t start = 0, end = 0;
    int launched = 0;
    int server_started = 0;
    int pserver_started = 0;
    int pclient_started = 0;
    int ok = 0;
    int id;

    clients = calloc((size_t) nclients, sizeof(clients[0]));
    if (!clients)
        return fail("calloc");

    id = atomic_fetch_add_explicit(&seq, 1, memory_order_relaxed);
    snprintf(name, sizeof(name), "rtmux-revoke-stress-%d-%d", getpid(), id);
    snprintf(path, sizeof(path), "/dev/name/local/%s", name);

    memset(&rargs, 0, sizeof(rargs));
    rargs.name = name;
    rargs.max_iters = iters;

    memset(&pargs, 0, sizeof(pargs));
    pargs.chid = -1;
    pargs.coid = -1;

    (void) unlink(path);
    attach = name_attach(NULL, name, 0);
    if (!attach)
        goto done;
    rargs.chid = attach->chid;

    pargs.chid = channel_create(0);
    if (pargs.chid == -1)
        goto done;
    pargs.coid = connect_attach(0, getpid(), pargs.chid, RTMUX_SIDE_CHANNEL, 0);
    if (pargs.coid == -1)
        goto done;

    if ((errno = pthread_create(&pserver, NULL, probe_server, &pargs)) != EOK)
        goto done;
    pserver_started = 1;
    while (!atomic_load_explicit(&pargs.ready, memory_order_acquire))
        (void) __sched_yield();
    if ((errno = pthread_create(&pclient, NULL, probe_client, &pargs)) != EOK)
        goto done;
    pclient_started = 1;

    if ((errno = pthread_create(&server, NULL, revoke_server, &rargs)) != EOK)
        goto done;
    server_started = 1;
    while (!atomic_load_explicit(&rargs.ready, memory_order_acquire))
        (void) __sched_yield();

    for (int i = 0; i < nclients; i++) {
        if ((errno = pthread_create(&clients[i], NULL, revoke_client,
                                    &rargs)) != EOK)
            goto done;
        launched++;
    }

    for (int i = 0; i < WAIT_LOOPS &&
                    atomic_load_explicit(&rargs.sends,
                                         memory_order_relaxed) < nclients;
         i++)
        (void) __sched_yield();
    if (atomic_load_explicit(&rargs.sends, memory_order_relaxed) < nclients) {
        errno = ETIMEDOUT;
        goto done;
    }

    if (monotonic_ns(&start) != EOK)
        goto done;
    if (name_detach(attach, 0) != EOK)
        goto done;
    attach = NULL;
    if (monotonic_ns(&end) != EOK)
        goto done;

    atomic_store_explicit(&rargs.stop, 1, memory_order_release);
    for (int i = 0; i < WAIT_LOOPS; i++) {
        if (atomic_load_explicit(&rargs.clients_done, memory_order_acquire) ==
            nclients)
            break;
        (void) __sched_yield();
    }

    stop_probe(&pargs);
    pargs.coid = -1;
    pargs.chid = -1;
    if (pclient_started)
        (void) pthread_join(pclient, NULL);
    if (pserver_started)
        (void) pthread_join(pserver, NULL);
    for (int i = 0; i < launched; i++)
        (void) pthread_join(clients[i], NULL);
    if (server_started)
        (void) pthread_join(server, NULL);
    pclient_started = 0;
    pserver_started = 0;
    launched = 0;
    server_started = 0;
    for (int i = 0; i < WAIT_LOOPS; i++) {
        if (atomic_load_explicit(&rargs.server_done, memory_order_acquire) == 1)
            break;
        (void) __sched_yield();
    }

    *detach_ns = end - start;
    *ipc_ns = atomic_load_explicit(&pargs.max_roundtrip_ns,
                                   memory_order_relaxed);
    *revoke_sends = atomic_load_explicit(&rargs.sends, memory_order_relaxed);
    *revoke_failures =
        atomic_load_explicit(&rargs.failures, memory_order_relaxed);
    *ipc_sends = atomic_load_explicit(&pargs.sends, memory_order_relaxed);
    *ipc_failures =
        atomic_load_explicit(&pargs.failures, memory_order_relaxed);

    if (atomic_load_explicit(&rargs.clients_done, memory_order_acquire) !=
            nclients ||
        atomic_load_explicit(&pargs.sends, memory_order_relaxed) == 0 ||
        atomic_load_explicit(&pargs.priority_ok, memory_order_acquire) != 1 ||
        atomic_load_explicit(&rargs.server_done, memory_order_acquire) != 1 ||
        atomic_load_explicit(&pargs.client_done, memory_order_acquire) != 1 ||
        atomic_load_explicit(&pargs.server_done, memory_order_acquire) != 1 ||
        atomic_load_explicit(&pargs.failures, memory_order_relaxed) != 0) {
        errno = EIO;
        goto done;
    }
    ok = 1;

done:
    atomic_store_explicit(&rargs.stop, 1, memory_order_release);
    if (pargs.coid != -1 || pargs.chid != -1)
        stop_probe(&pargs);
    pargs.coid = -1;
    pargs.chid = -1;
    if (attach) {
        (void) name_detach(attach, 0);
        attach = NULL;
    }
    for (int i = 0; i < WAIT_LOOPS; i++) {
        if (atomic_load_explicit(&rargs.clients_done, memory_order_acquire) >=
                launched &&
            atomic_load_explicit(&rargs.server_done, memory_order_acquire) == 1)
            break;
        (void) __sched_yield();
    }
    for (int i = 0; i < WAIT_LOOPS; i++) {
        if ((pargs.coid == -1 && pargs.chid == -1) ||
            (atomic_load_explicit(&pargs.client_done, memory_order_acquire) ==
                 1 &&
             atomic_load_explicit(&pargs.server_done, memory_order_acquire) ==
                 1))
            break;
        (void) __sched_yield();
    }
    for (int i = 0; i < launched; i++)
        (void) pthread_join(clients[i], NULL);
    if (server_started)
        (void) pthread_join(server, NULL);
    if (pclient_started)
        (void) pthread_join(pclient, NULL);
    if (pserver_started)
        (void) pthread_join(pserver, NULL);
    if (!ok)
        fprintf(stderr,
                "revoke-stress: FAIL launched=%d sends=%d failures=%d "
                "clients_done=%d server_done=%d probe_sends=%d "
                "probe_failures=%d probe_client_done=%d probe_server_done=%d: "
                "%s\n",
                launched,
                atomic_load_explicit(&rargs.sends, memory_order_relaxed),
                atomic_load_explicit(&rargs.failures, memory_order_relaxed),
                atomic_load_explicit(&rargs.clients_done,
                                     memory_order_acquire),
                atomic_load_explicit(&rargs.server_done, memory_order_acquire),
                atomic_load_explicit(&pargs.sends, memory_order_relaxed),
                atomic_load_explicit(&pargs.failures, memory_order_relaxed),
                atomic_load_explicit(&pargs.client_done, memory_order_acquire),
                atomic_load_explicit(&pargs.server_done, memory_order_acquire),
                strerror(errno));
    free(clients);
    return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}

int main(int argc, char **argv)
{
    uint64_t *detach_samples;
    uint64_t *ipc_samples;
    int nclients = DEFAULT_CLIENTS;
    int iters = DEFAULT_ITERS;
    int samples = DEFAULT_SAMPLES;
    int total_revoke_sends = 0;
    int total_revoke_failures = 0;
    int total_ipc_sends = 0;
    int total_ipc_failures = 0;

    if (argc > 1)
        nclients = atoi(argv[1]);
    if (argc > 2)
        iters = atoi(argv[2]);
    if (argc > 3)
        samples = atoi(argv[3]);
    if (nclients <= 0 || iters <= 0 || samples <= 0) {
        fprintf(stderr, "usage: revoke-stress [clients [iters [samples]]]\n");
        return EXIT_FAILURE;
    }

    detach_samples = calloc((size_t) samples, sizeof(detach_samples[0]));
    ipc_samples = calloc((size_t) samples, sizeof(ipc_samples[0]));
    if (!detach_samples || !ipc_samples)
        return fail("calloc");

    for (int i = 0; i < samples; i++) {
        int revoke_sends = 0;
        int revoke_failures = 0;
        int ipc_sends = 0;
        int ipc_failures = 0;

        if (run_once(nclients, iters, &detach_samples[i], &ipc_samples[i],
                     &revoke_sends, &revoke_failures, &ipc_sends,
                     &ipc_failures) != EXIT_SUCCESS) {
            free(detach_samples);
            free(ipc_samples);
            return EXIT_FAILURE;
        }
        total_revoke_sends += revoke_sends;
        total_revoke_failures += revoke_failures;
        total_ipc_sends += ipc_sends;
        total_ipc_failures += ipc_failures;
    }

    report("endpoint-detach", detach_samples, samples, nclients, iters,
           total_revoke_sends, total_revoke_failures);
    report("unrelated-ipc", ipc_samples, samples, nclients, iters,
           total_ipc_sends, total_ipc_failures);
    puts("revoke-stress: PASS");
    free(detach_samples);
    free(ipc_samples);
    return EXIT_SUCCESS;
}
