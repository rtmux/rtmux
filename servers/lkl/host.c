/*
 * RTMux host operations shared by RTMux LKL domain servers.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdalign.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/rtmux-core.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/trace.h>
#include <time.h>
#include <unistd.h>

#include <lkl.h>
#include <lkl_host.h>
#include <lkl/asm-generic/errno.h>

#include <generated/autoconf.h>

#include "domain.h"
#include "virtio.h"

/* Translate an LKL/Linux return value into an RTMux POSIX errno.
 *
 * LKL syscalls return a negated Linux (asm-generic) errno on failure. RTMux
 * errno is QNX-derived: values 1..34 happen to match Linux, but everything
 * above diverges (Linux ENOSYS 38 vs RTMux 89, Linux socket errnos 88..113 vs
 * RTMux 236..276, ...). A raw negate-and-return therefore mistranslates every
 * error above ERANGE, which is why both servers must funnel replies through
 * this one helper. Unknown/unmapped codes fail closed as EIO with the source
 * value logged, so no server ever hands a raw LKL errno back to libc.
 */
int lkl_errno(long ret)
{
    if (ret >= 0)
        return EOK;
    if (ret < -4095) /* not an errno-shaped return */
        return EIO;

    int e = (int) -ret;

    /* errno 1..34 are numerically identical to RTMux; LKL_EDEADLK (35) is the
     * first divergent value.
     */
    if (e < LKL_EDEADLK)
        return e;

    switch (e) {
    /* filesystem / VFS / generic */
    case LKL_EDEADLK:
        return EDEADLK;
    case LKL_ENAMETOOLONG:
        return ENAMETOOLONG;
    case LKL_ENOLCK:
        return ENOLCK;
    case LKL_ENOSYS:
        return ENOSYS;
    case LKL_ENOTEMPTY:
        return ENOTEMPTY;
    case LKL_ELOOP:
        return ELOOP;
    case LKL_ENOMSG:
        return ENOMSG;
    case LKL_EIDRM:
        return EIDRM;
    case LKL_EREMOTE:
        return EREMOTE;
    case LKL_ENOLINK:
        return ENOLINK;
    case LKL_EPROTO:
        return EPROTO;
    case LKL_EBADMSG:
        return EBADMSG;
    case LKL_EOVERFLOW:
        return EOVERFLOW;
    case LKL_EILSEQ:
        return EILSEQ;
    case LKL_ERESTART:
        return ERESTART;
    case LKL_EOWNERDEAD:
        return EOWNERDEAD;
    case LKL_EDQUOT:
        return EDQUOT;
    case LKL_ECANCELED:
        return ECANCELED;
    case LKL_ESTALE:
        return ESTALE;
    /* socket / network */
    case LKL_ENOTSOCK:
        return ENOTSOCK;
    case LKL_EDESTADDRREQ:
        return EDESTADDRREQ;
    case LKL_EMSGSIZE:
        return EMSGSIZE;
    case LKL_EPROTOTYPE:
        return EPROTOTYPE;
    case LKL_ENOPROTOOPT:
        return ENOPROTOOPT;
    case LKL_EPROTONOSUPPORT:
        return EPROTONOSUPPORT;
    case LKL_ESOCKTNOSUPPORT:
        return ESOCKTNOSUPPORT;
    case LKL_EOPNOTSUPP:
        return EOPNOTSUPP;
    case LKL_EPFNOSUPPORT:
        return EPFNOSUPPORT;
    case LKL_EAFNOSUPPORT:
        return EAFNOSUPPORT;
    case LKL_EADDRINUSE:
        return EADDRINUSE;
    case LKL_EADDRNOTAVAIL:
        return EADDRNOTAVAIL;
    case LKL_ENETDOWN:
        return ENETDOWN;
    case LKL_ENETUNREACH:
        return ENETUNREACH;
    case LKL_ENETRESET:
        return ENETRESET;
    case LKL_ECONNABORTED:
        return ECONNABORTED;
    case LKL_ECONNRESET:
        return ECONNRESET;
    case LKL_ENOBUFS:
        return ENOBUFS;
    case LKL_EISCONN:
        return EISCONN;
    case LKL_ENOTCONN:
        return ENOTCONN;
    case LKL_ESHUTDOWN:
        return ESHUTDOWN;
    case LKL_ETOOMANYREFS:
        return ETOOMANYREFS;
    case LKL_ETIMEDOUT:
        return ETIMEDOUT;
    case LKL_ECONNREFUSED:
        return ECONNREFUSED;
    case LKL_EHOSTDOWN:
        return EHOSTDOWN;
    case LKL_EHOSTUNREACH:
        return EHOSTUNREACH;
    case LKL_EALREADY:
        return EALREADY;
    case LKL_EINPROGRESS:
        return EINPROGRESS;
    default:
        fprintf(stderr, "lkl: unmapped errno %d -> EIO\n", e);
        return EIO;
    }
}

#define VIRTNET_RXQ 0
#define VIRTNET_TXQ 1

/* 32-entry ring caps inbound burst; bump if the single dispatch thread can't
 * drain fast enough and the netdev starts dropping.
 */
#define VIRTNET_Q_SIZE VIRTIO_QUEUE_MAX_SIZE
#define VIRTNET_BUF_SIZE 2048U

/* virtio-net header ahead of every frame: 10 bytes on a legacy (mmio v1)
 * device, 12 bytes once VIRTIO_F_VERSION_1 is negotiated (mmio v2), where the
 * num_buffers field is always present. hdr_size below picks per device.
 */
#define VIRTNET_LEGACY_HDR_SIZE 10U
#define VIRTNET_MODERN_HDR_SIZE 12U

/* Bounded wait for a free TX descriptor. A wedged device (lost or suppressed
 * TX-completion IRQ) must fail the netdev, not hang the LKL tx thread forever.
 * Raise if a legitimately slow backend trips it.
 */
#define VIRTNET_TX_STALL_NS (2ULL * 1000 * 1000 * 1000)
#define VIRTNET_IOMMU_RANGE_MAX (2U + 2U * 3U + 2U * VIRTIO_QUEUE_MAX_SIZE)

struct lkl_sem {
    pthread_mutex_t lock;
    pthread_cond_t cond;
    int count;
};

struct lkl_mutex {
    pthread_mutex_t mutex;
};

struct lkl_tls_key {
    pthread_key_t key;
};

struct lkl_thread_start {
    void (*fn)(void *);
    void *arg;
    unsigned long stack_size;
};

struct lkl_timer {
    void (*fn)(void);
    pthread_mutex_t lock;
    pthread_cond_t cond;
    pthread_t thread;
    struct timespec deadline;
    uint64_t generation;
    bool armed;
    bool quit;
    bool self_free;
    bool thread_started;
};

struct rtmux_lkl_netdev {
    struct lkl_netdev nd;
    pthread_mutex_t lock;
    pthread_cond_t event;
    struct rtmux_virtio_mmio vio;
    int version;
    struct rtmux_virtio_queue *rxq;
    struct rtmux_virtio_queue *txq;
    struct rtmux_virtio_dma_pool rx_pool;
    struct rtmux_virtio_dma_pool tx_pool;
    unsigned hdr_size;
    bool tx_free[VIRTNET_Q_SIZE];
    unsigned tx_inflight;
    bool hup;
};

static pthread_mutex_t lkl_host_stats_lock = PTHREAD_MUTEX_INITIALIZER;
static struct rtmux_lkl_host_stats lkl_host_stats_state;
static bool lkl_block_crash_after_request;
static bool lkl_netdev_crash_after_tx;

#if defined(CONFIG_INSTRUMENTATION)
static void lkl_host_trace(unsigned event, unsigned op, uintptr_t value)
{
    unsigned data[5] = {
        RTMUX_TRACE_INSERTEVENT, _TRACE_USER_C | event, 0, op, (unsigned) value,
    };

    (void) trace_event(data);
}
#else
static void lkl_host_trace(unsigned event, unsigned op, uintptr_t value)
{
    (void) event;
    (void) op;
    (void) value;
}
#endif

static void host_stats_add(unsigned long long *current,
                           unsigned long long *high,
                           unsigned long long *total,
                           unsigned long long size)
{
    /* Saturate rather than wrap: a wrapped counter makes /stats lie. */
    if (ULLONG_MAX - *current < size)
        *current = ULLONG_MAX;
    else
        *current += size;
    if (ULLONG_MAX - *total < size)
        *total = ULLONG_MAX;
    else
        *total += size;
    if (*current > *high)
        *high = *current;
}

static void host_stats_sub(unsigned long long *current, unsigned long long size)
{
    if (*current >= size)
        *current -= size;
    else
        *current = 0;
}

static void host_stats_count(unsigned long long *counter,
                             unsigned long long size)
{
    if (ULLONG_MAX - *counter < size)
        *counter = ULLONG_MAX;
    else
        *counter += size;
}

static void host_stats_page_add(unsigned long size)
{
    pthread_mutex_lock(&lkl_host_stats_lock);
    host_stats_add(&lkl_host_stats_state.page_current,
                   &lkl_host_stats_state.page_high,
                   &lkl_host_stats_state.page_total, (unsigned long long) size);
    pthread_mutex_unlock(&lkl_host_stats_lock);
}

static void host_stats_page_sub(unsigned long size)
{
    pthread_mutex_lock(&lkl_host_stats_lock);
    host_stats_sub(&lkl_host_stats_state.page_current,
                   (unsigned long long) size);
    pthread_mutex_unlock(&lkl_host_stats_lock);
}

void rtmux_lkl_host_stats_dma_add(size_t size)
{
    pthread_mutex_lock(&lkl_host_stats_lock);
    host_stats_add(&lkl_host_stats_state.dma_current,
                   &lkl_host_stats_state.dma_high,
                   &lkl_host_stats_state.dma_total, (unsigned long long) size);
    pthread_mutex_unlock(&lkl_host_stats_lock);
}

void rtmux_lkl_host_stats_dma_sub(size_t size)
{
    pthread_mutex_lock(&lkl_host_stats_lock);
    host_stats_sub(&lkl_host_stats_state.dma_current,
                   (unsigned long long) size);
    pthread_mutex_unlock(&lkl_host_stats_lock);
}

void rtmux_lkl_host_stats_net_bounce_rx(size_t bytes)
{
    pthread_mutex_lock(&lkl_host_stats_lock);
    host_stats_count(&lkl_host_stats_state.net_bounce_rx_bytes,
                     (unsigned long long) bytes);
    pthread_mutex_unlock(&lkl_host_stats_lock);
}

void rtmux_lkl_host_stats_net_bounce_tx(size_t bytes)
{
    pthread_mutex_lock(&lkl_host_stats_lock);
    host_stats_count(&lkl_host_stats_state.net_bounce_tx_bytes,
                     (unsigned long long) bytes);
    pthread_mutex_unlock(&lkl_host_stats_lock);
}

void rtmux_lkl_host_stats_lkl_block_read(size_t bytes)
{
    pthread_mutex_lock(&lkl_host_stats_lock);
    host_stats_count(&lkl_host_stats_state.lkl_block_read_bytes,
                     (unsigned long long) bytes);
    pthread_mutex_unlock(&lkl_host_stats_lock);
}

void rtmux_lkl_host_stats_lkl_block_write(size_t bytes)
{
    pthread_mutex_lock(&lkl_host_stats_lock);
    host_stats_count(&lkl_host_stats_state.lkl_block_write_bytes,
                     (unsigned long long) bytes);
    pthread_mutex_unlock(&lkl_host_stats_lock);
}

void rtmux_lkl_block_crash_after_request(bool enabled)
{
    __atomic_store_n(&lkl_block_crash_after_request, enabled, __ATOMIC_RELEASE);
}

static void host_stats_thread_add(unsigned long stack_size)
{
    pthread_mutex_lock(&lkl_host_stats_lock);
    lkl_host_stats_state.thread_count++;
    lkl_host_stats_state.thread_stack_bytes += (unsigned long long) stack_size;
    pthread_mutex_unlock(&lkl_host_stats_lock);
}

static void host_stats_thread_sub(unsigned long stack_size)
{
    pthread_mutex_lock(&lkl_host_stats_lock);
    if (lkl_host_stats_state.thread_count > 0)
        lkl_host_stats_state.thread_count--;
    host_stats_sub(&lkl_host_stats_state.thread_stack_bytes,
                   (unsigned long long) stack_size);
    pthread_mutex_unlock(&lkl_host_stats_lock);
}

void rtmux_lkl_host_stats(struct rtmux_lkl_host_stats *out)
{
    if (!out)
        return;
    pthread_mutex_lock(&lkl_host_stats_lock);
    *out = lkl_host_stats_state;
    pthread_mutex_unlock(&lkl_host_stats_lock);
}

static void lkl_netd_print(const char *str, int len)
{
    if (len <= 0)
        return;
    (void) write(STDERR_FILENO, str, (size_t) len);
}

static void lkl_netd_panic(void)
{
    lkl_netd_print("lkl-host: panic\n", 16);
    abort();
}

static lkl_thread_t lkl_netd_native_thread_handle(void)
{
    return (lkl_thread_t) (unsigned int) pthread_self();
}

static pthread_once_t lkl_netd_stack_key_once = PTHREAD_ONCE_INIT;
static pthread_key_t lkl_netd_stack_key;
static int lkl_netd_stack_key_ok;

static void lkl_netd_stack_key_init(void)
{
    lkl_netd_stack_key_ok =
        pthread_key_create(&lkl_netd_stack_key, NULL) == EOK;
}

static struct lkl_sem *lkl_netd_sem_alloc(int count)
{
    struct lkl_sem *sem = calloc(1, sizeof(*sem));

    if (!sem)
        return NULL;
    sem->count = count;
    if (pthread_mutex_init(&sem->lock, NULL) != EOK) {
        free(sem);
        return NULL;
    }
    if (pthread_cond_init(&sem->cond, NULL) != EOK) {
        (void) pthread_mutex_destroy(&sem->lock);
        free(sem);
        return NULL;
    }
    return sem;
}

static void lkl_netd_sem_free(struct lkl_sem *sem)
{
    if (!sem)
        return;
    (void) pthread_cond_destroy(&sem->cond);
    (void) pthread_mutex_destroy(&sem->lock);
    free(sem);
}

static void lkl_netd_sem_up(struct lkl_sem *sem)
{
    (void) pthread_mutex_lock(&sem->lock);

    /* Guard the signed counter: wrapping past INT_MAX would flip it negative
     * and strand every waiter in lkl_netd_sem_down().
     */
    if (sem->count < INT_MAX)
        sem->count++;
    if (sem->count > 0)
        (void) pthread_cond_signal(&sem->cond);
    (void) pthread_mutex_unlock(&sem->lock);
}

static void lkl_netd_sem_down(struct lkl_sem *sem)
{
    lkl_host_trace(RTMUX_TRACE_USER_LKL_HOST_ENTER,
                   RTMUX_TRACE_LKL_HOST_SEM_DOWN, (uintptr_t) sem);
    (void) pthread_mutex_lock(&sem->lock);
    while (sem->count <= 0)
        (void) pthread_cond_wait(&sem->cond, &sem->lock);
    sem->count--;
    (void) pthread_mutex_unlock(&sem->lock);
    lkl_host_trace(RTMUX_TRACE_USER_LKL_HOST_EXIT,
                   RTMUX_TRACE_LKL_HOST_SEM_DOWN, (uintptr_t) sem);
}

static struct lkl_mutex *lkl_netd_mutex_alloc(int recursive)
{
    pthread_mutexattr_t attr;
    struct lkl_mutex *mutex = calloc(1, sizeof(*mutex));

    if (!mutex)
        return NULL;
    if (pthread_mutexattr_init(&attr) != EOK) {
        free(mutex);
        return NULL;
    }
    if (recursive)
        attr.flags |= PTHREAD_RECURSIVE_ENABLE;
    if (pthread_mutex_init(&mutex->mutex, &attr) != EOK) {
        (void) pthread_mutexattr_destroy(&attr);
        free(mutex);
        return NULL;
    }
    (void) pthread_mutexattr_destroy(&attr);
    return mutex;
}

static void lkl_netd_mutex_free(struct lkl_mutex *mutex)
{
    if (!mutex)
        return;
    (void) pthread_mutex_destroy(&mutex->mutex);
    free(mutex);
}

static void lkl_netd_mutex_lock(struct lkl_mutex *mutex)
{
    lkl_host_trace(RTMUX_TRACE_USER_LKL_HOST_ENTER,
                   RTMUX_TRACE_LKL_HOST_MUTEX_LOCK, (uintptr_t) mutex);
    (void) pthread_mutex_lock(&mutex->mutex);
    lkl_host_trace(RTMUX_TRACE_USER_LKL_HOST_EXIT,
                   RTMUX_TRACE_LKL_HOST_MUTEX_LOCK, (uintptr_t) mutex);
}

static void lkl_netd_mutex_unlock(struct lkl_mutex *mutex)
{
    (void) pthread_mutex_unlock(&mutex->mutex);
}

static void *lkl_netd_thread_trampoline(void *arg)
{
    struct lkl_thread_start *start = arg;
    void (*fn)(void *) = start->fn;
    void *fn_arg = start->arg;
    unsigned long stack_size = start->stack_size;

    free(start);
    pthread_once(&lkl_netd_stack_key_once, lkl_netd_stack_key_init);
    if (lkl_netd_stack_key_ok)
        (void) pthread_setspecific(lkl_netd_stack_key,
                                   (void *) (uintptr_t) stack_size);
    fn(fn_arg);
    host_stats_thread_sub(RTMUX_LKL_NETD_STACK_SIZE);
    return NULL;
}

static lkl_thread_t lkl_netd_thread_create(void (*fn)(void *), void *arg)
{
    pthread_attr_t attr;
    pthread_t tid;
    struct lkl_thread_start *start = malloc(sizeof(*start));

    if (!start)
        return 0;
    memset(start, 0, sizeof(*start));
    start->fn = fn;
    start->arg = arg;
    start->stack_size = RTMUX_LKL_NETD_STACK_SIZE;
    if (pthread_attr_init(&attr) != EOK) {
        free(start);
        return 0;
    }
    (void) pthread_attr_setstacksize(&attr, RTMUX_LKL_NETD_STACK_SIZE);
    host_stats_thread_add(RTMUX_LKL_NETD_STACK_SIZE);
    if (pthread_create(&tid, &attr, lkl_netd_thread_trampoline, start) != EOK) {
        host_stats_thread_sub(RTMUX_LKL_NETD_STACK_SIZE);
        free(start);
        return 0;
    }
    return (lkl_thread_t) (unsigned int) tid;
}

static void lkl_netd_thread_detach(void)
{
    (void) pthread_detach(pthread_self());
}

static void lkl_netd_thread_exit(void)
{
    host_stats_thread_sub(RTMUX_LKL_NETD_STACK_SIZE);
    pthread_exit(NULL);
}

static int lkl_netd_thread_join(lkl_thread_t tid)
{
    int ret;

    lkl_host_trace(RTMUX_TRACE_USER_LKL_HOST_ENTER,
                   RTMUX_TRACE_LKL_HOST_THREAD_JOIN, (uintptr_t) tid);
    ret = thread_join_r((int) tid, NULL) == EOK ? 0 : -1;
    lkl_host_trace(RTMUX_TRACE_USER_LKL_HOST_EXIT,
                   RTMUX_TRACE_LKL_HOST_THREAD_JOIN, (uintptr_t) ret);
    return ret;
}

static lkl_thread_t lkl_netd_thread_self(void)
{
    return lkl_netd_native_thread_handle();
}

static int lkl_netd_thread_equal(lkl_thread_t a, lkl_thread_t b)
{
    return a == b;
}

static void *lkl_netd_thread_stack(unsigned long *size)
{
    struct _thread_local_storage *tls;

    __asm__ volatile("mv %0, tp" : "=r"(tls));

    if (size)
        pthread_once(&lkl_netd_stack_key_once, lkl_netd_stack_key_init);
    if (size && lkl_netd_stack_key_ok)
        *size =
            (unsigned long) (uintptr_t) pthread_getspecific(lkl_netd_stack_key);
    if (size && *size == 0)
        *size = RTMUX_LKL_DEFAULT_STACK_SIZE;
    return tls ? tls->__stackaddr : NULL;
}

static struct lkl_tls_key *lkl_netd_tls_alloc(void (*destructor)(void *))
{
    struct lkl_tls_key *key = calloc(1, sizeof(*key));

    if (!key)
        return NULL;
    if (pthread_key_create(&key->key, destructor) != EOK) {
        free(key);
        return NULL;
    }
    return key;
}

static void lkl_netd_tls_free(struct lkl_tls_key *key)
{
    if (!key)
        return;
    (void) pthread_key_delete(key->key);
    free(key);
}

static int lkl_netd_tls_set(struct lkl_tls_key *key, void *data)
{
    return pthread_setspecific(key->key, data);
}

static void *lkl_netd_tls_get(struct lkl_tls_key *key)
{
    return pthread_getspecific(key->key);
}

static void *lkl_netd_page_alloc(unsigned long size)
{
    void *addr;

    lkl_host_trace(RTMUX_TRACE_USER_LKL_HOST_ENTER,
                   RTMUX_TRACE_LKL_HOST_PAGE_ALLOC, (uintptr_t) size);

    /* LKL boot memory can be tens of MiB. Keep it out of the small libc heap
     * and use the RTMux VM path for a page-aligned anonymous map.
     */
    addr = mmap(NULL, (size_t) size, PROT_READ | PROT_WRITE,
                MAP_ANON | MAP_PRIVATE, NOFD, 0);
    if (addr == MAP_FAILED) {
        lkl_host_trace(RTMUX_TRACE_USER_LKL_HOST_EXIT,
                       RTMUX_TRACE_LKL_HOST_PAGE_ALLOC, 0);
        return NULL;
    }
    host_stats_page_add(size);

    /* MAP_ANON pages are zeroed on fault-in by VM
     * (servers/roottask/mem/mm-reference.c); an explicit memset here defeats
     * demand paging and spikes boot RSS.
     */
    lkl_host_trace(RTMUX_TRACE_USER_LKL_HOST_EXIT,
                   RTMUX_TRACE_LKL_HOST_PAGE_ALLOC, (uintptr_t) addr);
    return addr;
}

static void lkl_netd_page_free(void *addr, unsigned long size)
{
    if (addr && addr != MAP_FAILED) {
        lkl_host_trace(RTMUX_TRACE_USER_LKL_HOST_ENTER,
                       RTMUX_TRACE_LKL_HOST_PAGE_FREE, (uintptr_t) addr);
        (void) munmap(addr, (size_t) size);
        host_stats_page_sub(size);
        lkl_host_trace(RTMUX_TRACE_USER_LKL_HOST_EXIT,
                       RTMUX_TRACE_LKL_HOST_PAGE_FREE, (uintptr_t) size);
    }
}

static unsigned long long lkl_netd_time(void)
{
    uint64_t nsec;

    if (__clock_time(CLOCK_MONOTONIC, NULL, &nsec) != 0)
        return 0;
    return nsec;
}

/* One worker thread per timer waits on a condvar until the armed deadline, then
 * fires the callback off the caller's stack. Re-arming just updates the
 * deadline and signals; the worker recomputes its wait. This replaces the old
 * inline nanosleep(), which froze the calling LKL thread for the full delta and
 * serialized every timer-driven path (TCP retransmit, ARP).
 */
static void *lkl_netd_timer_worker(void *arg)
{
    struct lkl_timer *timer = arg;

    pthread_mutex_lock(&timer->lock);
    for (;;) {
        uint64_t deadline;
        uint64_t now = 0;
        uint64_t gen;
        int expired;

        while (!timer->armed && !timer->quit)
            pthread_cond_wait(&timer->cond, &timer->lock);
        if (timer->quit)
            break;
        gen = timer->generation;
        deadline = (uint64_t) timer->deadline.tv_sec * 1000000000ULL +
                   (uint64_t) timer->deadline.tv_nsec;
        expired =
            __clock_time(CLOCK_MONOTONIC, NULL, &now) == 0 && now >= deadline;
        if ((expired ||
             pthread_cond_timedwait(&timer->cond, &timer->lock,
                                    &timer->deadline) == ETIMEDOUT) &&
            timer->generation == gen && timer->armed && !timer->quit) {
            void (*fn)(void) = timer->fn;
            timer->armed = false;
            pthread_mutex_unlock(&timer->lock);
            fn();
            pthread_mutex_lock(&timer->lock);
        }
    }
    pthread_mutex_unlock(&timer->lock);
    if (timer->self_free) {
        pthread_cond_destroy(&timer->cond);
        pthread_mutex_destroy(&timer->lock);
        free(timer);
    }
    return NULL;
}

static void *lkl_netd_timer_alloc(void (*fn)(void))
{
    pthread_condattr_t condattr;
    struct lkl_timer *timer = calloc(1, sizeof(*timer));

    if (!timer)
        return NULL;
    timer->fn = fn;
    if (pthread_mutex_init(&timer->lock, NULL) != EOK) {
        free(timer);
        return NULL;
    }
    if (pthread_condattr_init(&condattr) != EOK) {
        (void) pthread_mutex_destroy(&timer->lock);
        free(timer);
        return NULL;
    }
    (void) pthread_condattr_setclock(&condattr, CLOCK_MONOTONIC);
    if (pthread_cond_init(&timer->cond, &condattr) != EOK) {
        (void) pthread_condattr_destroy(&condattr);
        (void) pthread_mutex_destroy(&timer->lock);
        free(timer);
        return NULL;
    }
    (void) pthread_condattr_destroy(&condattr);
    return timer;
}

static int lkl_netd_timer_set_oneshot(void *opaque, unsigned long delta)
{
    struct lkl_timer *timer = opaque;
    unsigned long long ns;
    uint64_t now;

    lkl_host_trace(RTMUX_TRACE_USER_LKL_HOST_ENTER,
                   RTMUX_TRACE_LKL_HOST_TIMER_SET_ONESHOT, (uintptr_t) delta);
    if (!timer || !timer->fn)
        goto fail;
    if (__clock_time(CLOCK_MONOTONIC, NULL, &now) != 0)
        goto fail;

    /* Saturate instead of wrapping: a wrapped deadline arms the timer in the
     * past and fires immediately.
     */
    if ((unsigned long long) delta > UINT64_MAX - now)
        ns = UINT64_MAX;
    else
        ns = (unsigned long long) now + delta;
    pthread_mutex_lock(&timer->lock);
    if (!timer->thread_started) {
        if (pthread_create(&timer->thread, NULL, lkl_netd_timer_worker,
                           timer) != EOK) {
            pthread_mutex_unlock(&timer->lock);
            goto fail;
        }
        timer->thread_started = true;
    }
    timer->deadline.tv_sec = (time_t) (ns / 1000000000ULL);
    timer->deadline.tv_nsec = (long) (ns % 1000000000ULL);
    timer->armed = true;
    timer->generation++;
    pthread_cond_signal(&timer->cond);
    pthread_mutex_unlock(&timer->lock);
    lkl_host_trace(RTMUX_TRACE_USER_LKL_HOST_EXIT,
                   RTMUX_TRACE_LKL_HOST_TIMER_SET_ONESHOT, 0);
    return 0;

fail:
    lkl_host_trace(RTMUX_TRACE_USER_LKL_HOST_EXIT,
                   RTMUX_TRACE_LKL_HOST_TIMER_SET_ONESHOT, (uintptr_t) -EINVAL);
    return -EINVAL;
}

static void lkl_netd_timer_free(void *opaque)
{
    struct lkl_timer *timer = opaque;

    if (!timer)
        return;
    pthread_mutex_lock(&timer->lock);
    if (!timer->thread_started) {
        pthread_mutex_unlock(&timer->lock);
        pthread_cond_destroy(&timer->cond);
        pthread_mutex_destroy(&timer->lock);
        free(timer);
        return;
    }
    timer->quit = true;
    timer->self_free = true;
    pthread_cond_broadcast(&timer->cond);
    (void) pthread_detach(timer->thread);
    pthread_mutex_unlock(&timer->lock);
}

static void *lkl_netd_ioremap(long addr, int size)
{
    extern void *lkl_ioremap(long, int);

    return lkl_ioremap(addr, size);
}

static int lkl_netd_iomem_access(const volatile void *addr,
                                 void *val,
                                 int size,
                                 int write)
{
    extern int lkl_iomem_access(const volatile void *, void *, int, int);

    return lkl_iomem_access(addr, val, size, write);
}

_Static_assert(sizeof(((struct lkl_jmp_buf *) 0)->buf) >= sizeof(jmp_buf),
               "lkl_jmp_buf too small to hold jmp_buf");
_Static_assert(alignof(struct lkl_jmp_buf) >= alignof(jmp_buf),
               "lkl_jmp_buf alignment too small for jmp_buf");

static void lkl_netd_jmp_buf_set(struct lkl_jmp_buf *jmpb, void (*fn)(void))
{
    if (setjmp(*(jmp_buf *) jmpb->buf) == 0)
        fn();
}

static void lkl_netd_jmp_buf_longjmp(struct lkl_jmp_buf *jmpb, int val)
{
    longjmp(*(jmp_buf *) jmpb->buf, val == 0 ? 1 : val);
}

static void *lkl_netd_mmap(void *addr, unsigned long size, enum lkl_prot prot)
{
    int host_prot = PROT_NONE;
    int flags = MAP_ANON | MAP_PRIVATE;
    void *p;

    lkl_host_trace(RTMUX_TRACE_USER_LKL_HOST_ENTER, RTMUX_TRACE_LKL_HOST_MMAP,
                   (uintptr_t) size);
    if (prot & LKL_PROT_READ)
        host_prot |= PROT_READ;
    if (prot & LKL_PROT_WRITE)
        host_prot |= PROT_WRITE;
    if (prot & LKL_PROT_EXEC)
        host_prot |= PROT_EXEC;
    if (addr)
        flags |= MAP_FIXED;
    p = mmap(addr, (size_t) size, host_prot, flags, NOFD, 0);
    if (p == MAP_FAILED) {
        lkl_host_trace(RTMUX_TRACE_USER_LKL_HOST_EXIT,
                       RTMUX_TRACE_LKL_HOST_MMAP, 0);
        return NULL;
    }
    if (addr && p != addr) {
        munmap(p, (size_t) size);
        lkl_host_trace(RTMUX_TRACE_USER_LKL_HOST_EXIT,
                       RTMUX_TRACE_LKL_HOST_MMAP, 0);
        return NULL;
    }
    lkl_host_trace(RTMUX_TRACE_USER_LKL_HOST_EXIT, RTMUX_TRACE_LKL_HOST_MMAP,
                   (uintptr_t) p);
    return p;
}

static int lkl_netd_munmap(void *addr, unsigned long size)
{
    int ret;

    lkl_host_trace(RTMUX_TRACE_USER_LKL_HOST_ENTER, RTMUX_TRACE_LKL_HOST_MUNMAP,
                   (uintptr_t) addr);
    ret = munmap(addr, (size_t) size);
    lkl_host_trace(RTMUX_TRACE_USER_LKL_HOST_EXIT, RTMUX_TRACE_LKL_HOST_MUNMAP,
                   (uintptr_t) ret);
    return ret;
}

/* Completions the device posted but we haven't consumed. A value above q->num
 * means the device advanced used->idx past the ring: corruption. Callers issue
 * virtio_mb() before reading, so the barrier is not repeated here.
 */
static uint16_t virtnet_used_pending(const struct rtmux_virtio_queue *q)
{
    return (uint16_t) (q->used->idx - q->last_used);
}

/* Stage one filled descriptor without notifying the device. The barrier between
 * the avail-ring write and the idx bump is the whole point: the device must
 * never observe a fresh avail->idx pointing at a stale descriptor. Both queues
 * go through here so that ordering lives in exactly one place. Virtio 1.2
 * section 3.1 forbids buffer-available notifications before DRIVER_OK, so init
 * stages with this and kicks once afterwards.
 */
static void virtnet_stage_locked(struct rtmux_virtio_queue *q,
                                 unsigned id,
                                 uint64_t paddr,
                                 uint32_t len,
                                 uint16_t flags)
{
    q->desc[id].addr = paddr;
    q->desc[id].len = len;
    q->desc[id].flags = flags;
    q->desc[id].next = 0;
    q->avail->ring[q->avail->idx % q->num] = (uint16_t) id;
    virtio_mb();
    q->avail->idx++;
}

/* Stage and notify: steady-state path, only valid once DRIVER_OK is set. */
static void virtnet_publish_locked(struct rtmux_virtio_mmio *vio,
                                   struct rtmux_virtio_queue *q,
                                   unsigned id,
                                   uint64_t paddr,
                                   uint32_t len,
                                   uint16_t flags)
{
    virtnet_stage_locked(q, id, paddr, len, flags);
    virtio_queue_kick(vio, q);
}

static void virtnet_rx_stage_locked(struct rtmux_lkl_netdev *rtnd, unsigned id)
{
    virtnet_stage_locked(rtnd->rxq, id, rtnd->rx_pool.buf[id].paddr,
                         VIRTNET_BUF_SIZE, VRING_DESC_F_WRITE);
}

static void virtnet_rx_post_locked(struct rtmux_lkl_netdev *rtnd, unsigned id)
{
    virtnet_publish_locked(&rtnd->vio, rtnd->rxq, id,
                           rtnd->rx_pool.buf[id].paddr, VIRTNET_BUF_SIZE,
                           VRING_DESC_F_WRITE);
}

static int virtnet_copy_to_iov(struct iovec *iov,
                               int cnt,
                               const void *buf,
                               size_t len)
{
    const uint8_t *src = buf;
    size_t done = 0;

    if (cnt < 0 || (cnt > 0 && !iov))
        return -EINVAL;
    for (int i = 0; i < cnt && done < len; i++) {
        size_t n = iov[i].iov_len;
        if (n > len - done)
            n = len - done;
        memcpy(iov[i].iov_base, src + done, n);
        done += n;
    }
    return (int) done;
}

static int virtnet_copy_from_iov(void *buf,
                                 size_t buflen,
                                 struct iovec *iov,
                                 int cnt)
{
    uint8_t *dst = buf;
    size_t done = 0;

    if (cnt < 0 || (cnt > 0 && !iov))
        return -EINVAL;
    for (int i = 0; i < cnt; i++) {
        if (iov[i].iov_len > buflen - done)
            return -EMSGSIZE;
        memcpy(dst + done, iov[i].iov_base, iov[i].iov_len);
        done += iov[i].iov_len;
    }
    return (int) done;
}

static int virtnet_discover(struct rtmux_lkl_netdev *rtnd)
{
    return virtio_mmio_discover(&rtnd->vio, VIRTIO_ID_NET, &rtnd->version);
}

static int virtnet_add_dma_range(struct rtmux_virtio_dma_range *ranges,
                                 unsigned *count,
                                 uint64_t paddr,
                                 size_t size)
{
    if (*count >= VIRTNET_IOMMU_RANGE_MAX)
        return EOVERFLOW;
    ranges[*count] = (struct rtmux_virtio_dma_range){
        .paddr = paddr,
        .size = size,
    };
    (*count)++;
    return EOK;
}

static int virtnet_require_iommu_domain(struct rtmux_lkl_netdev *rtnd)
{
    struct rtmux_virtio_dma_range ranges[VIRTNET_IOMMU_RANGE_MAX];
    unsigned count = 0;
    int rc;

    if (rtnd->version == 1) {
        rc = virtnet_add_dma_range(ranges, &count, rtnd->rxq->mem_pa,
                                   rtnd->rxq->mem_size);
        if (rc != EOK)
            return rc;
        rc = virtnet_add_dma_range(ranges, &count, rtnd->txq->mem_pa,
                                   rtnd->txq->mem_size);
        if (rc != EOK)
            return rc;
    } else {
        uint64_t queue_pa[] = {
            rtnd->rxq->desc_pa, rtnd->rxq->avail_pa, rtnd->rxq->used_pa,
            rtnd->txq->desc_pa, rtnd->txq->avail_pa, rtnd->txq->used_pa,
        };

        for (unsigned i = 0; i < sizeof(queue_pa) / sizeof(queue_pa[0]); i++) {
            rc = virtnet_add_dma_range(ranges, &count, queue_pa[i], 4096);
            if (rc != EOK)
                return rc;
        }
    }
    for (unsigned i = 0; i < rtnd->rx_pool.count; i++) {
        rc = virtnet_add_dma_range(ranges, &count, rtnd->rx_pool.buf[i].paddr,
                                   rtnd->rx_pool.size);
        if (rc != EOK)
            return rc;
    }
    for (unsigned i = 0; i < rtnd->tx_pool.count; i++) {
        rc = virtnet_add_dma_range(ranges, &count, rtnd->tx_pool.buf[i].paddr,
                                   rtnd->tx_pool.size);
        if (rc != EOK)
            return rc;
    }
    return virtio_mmio_require_iommu_domain(&rtnd->vio, ranges, count);
}

void rtmux_lkl_netdev_set_delegated_mmio(bool enabled)
{
    virtio_mmio_set_delegated(enabled);
}

void rtmux_lkl_netdev_crash_after_tx(bool enabled)
{
    __atomic_store_n(&lkl_netdev_crash_after_tx, enabled, __ATOMIC_RELEASE);
}

static void virtnet_irq_notify(void *arg)
{
    struct rtmux_lkl_netdev *rtnd = arg;

    pthread_mutex_lock(&rtnd->lock);
    pthread_cond_broadcast(&rtnd->event);
    pthread_mutex_unlock(&rtnd->lock);
}

static int virtnet_init(struct rtmux_lkl_netdev *rtnd)
{
    uint32_t dev_features_low;

    if (virtnet_discover(rtnd) != 0)
        return -ENODEV;
    rtnd->hdr_size =
        rtnd->version == 2 ? VIRTNET_MODERN_HDR_SIZE : VIRTNET_LEGACY_HDR_SIZE;

    virtio_mmio_wr32(&rtnd->vio, VIRTIO_MMIO_STATUS, 0);
    virtio_mmio_wr32(&rtnd->vio, VIRTIO_MMIO_STATUS, VIRTIO_STATUS_ACKNOWLEDGE);
    virtio_mmio_wr32(&rtnd->vio, VIRTIO_MMIO_STATUS,
                     VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER);

    if (rtnd->version == 2) {
        virtio_mmio_wr32(&rtnd->vio, VIRTIO_MMIO_DEVICE_FEATURES_SEL, 1);
        if ((virtio_mmio_rd32(&rtnd->vio, VIRTIO_MMIO_DEVICE_FEATURES) &
             (1U << (VIRTIO_F_VERSION_1 - 32))) == 0)
            return -ENODEV;
        virtio_mmio_wr32(&rtnd->vio, VIRTIO_MMIO_DEVICE_FEATURES_SEL, 0);
    }
    dev_features_low =
        virtio_mmio_rd32(&rtnd->vio, VIRTIO_MMIO_DEVICE_FEATURES);
    (void) dev_features_low;

    if (rtnd->version == 1)
        virtio_mmio_wr32(&rtnd->vio, VIRTIO_MMIO_GUEST_PAGE_SIZE, 4096);
    else
        virtio_mmio_wr32(&rtnd->vio, VIRTIO_MMIO_DRIVER_FEATURES_SEL, 0);
    virtio_mmio_wr32(&rtnd->vio, VIRTIO_MMIO_DRIVER_FEATURES, 0);
    if (rtnd->version == 2) {
        virtio_mmio_wr32(&rtnd->vio, VIRTIO_MMIO_DRIVER_FEATURES_SEL, 1);
        virtio_mmio_wr32(&rtnd->vio, VIRTIO_MMIO_DRIVER_FEATURES,
                         1U << (VIRTIO_F_VERSION_1 - 32));
        virtio_mmio_wr32(&rtnd->vio, VIRTIO_MMIO_STATUS,
                         VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER |
                             VIRTIO_STATUS_FEATURES_OK);
        if ((virtio_mmio_rd32(&rtnd->vio, VIRTIO_MMIO_STATUS) &
             VIRTIO_STATUS_FEATURES_OK) == 0)
            return -ENODEV;
    }

    rtnd->rxq = virtio_queue_alloc(&rtnd->vio, rtnd->version, VIRTNET_RXQ,
                                   VIRTNET_Q_SIZE);
    rtnd->txq = virtio_queue_alloc(&rtnd->vio, rtnd->version, VIRTNET_TXQ,
                                   VIRTNET_Q_SIZE);
    if (!rtnd->rxq || !rtnd->txq)
        return -ENOMEM;

    if (virtio_dma_pool_alloc(&rtnd->rx_pool, rtnd->rxq->num,
                              VIRTNET_BUF_SIZE) != EOK)
        return -ENOMEM;
    if (virtio_dma_pool_alloc(&rtnd->tx_pool, rtnd->txq->num,
                              VIRTNET_BUF_SIZE) != EOK)
        return -ENOMEM;
    int iommu_rc = virtnet_require_iommu_domain(rtnd);

    if (iommu_rc != EOK)
        return -iommu_rc;
    /* Only now may the device see queue addresses or QUEUE_READY/PFN. */
    virtio_queue_publish(&rtnd->vio, rtnd->version, rtnd->rxq);
    virtio_queue_publish(&rtnd->vio, rtnd->version, rtnd->txq);
    for (unsigned i = 0; i < rtnd->rxq->num; i++)
        virtnet_rx_stage_locked(rtnd, i); /* no kick before DRIVER_OK */
    for (unsigned i = 0; i < rtnd->txq->num; i++)
        rtnd->tx_free[i] = true;

    if (virtio_irq_start(&rtnd->vio, virtnet_irq_notify, rtnd) != 0)
        return -ENODEV;

    virtio_fence_w_o(); /* staged descriptors and avail->idx before DRIVER_OK */
    if (rtnd->version == 1)
        virtio_mmio_wr32(&rtnd->vio, VIRTIO_MMIO_STATUS,
                         VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER |
                             VIRTIO_STATUS_DRIVER_OK);
    else
        virtio_mmio_wr32(&rtnd->vio, VIRTIO_MMIO_STATUS,
                         VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER |
                             VIRTIO_STATUS_FEATURES_OK |
                             VIRTIO_STATUS_DRIVER_OK);
    virtio_fence_o_o(); /* DRIVER_OK write before the first QUEUE_NOTIFY */
    /* First and only RX notification of init: after DRIVER_OK. */
    virtio_queue_kick(&rtnd->vio, rtnd->rxq);
    return 0;
}

static int virtnet_rx_ready_locked(struct rtmux_lkl_netdev *rtnd)
{
    virtio_mb();
    return rtnd->rxq && virtnet_used_pending(rtnd->rxq) != 0;
}

static void virtnet_fail_device_locked(struct rtmux_lkl_netdev *rtnd)
{
    if (rtnd->vio.base)
        virtio_mmio_wr32(&rtnd->vio, VIRTIO_MMIO_STATUS, 0);
    rtnd->hup = true;
    pthread_cond_broadcast(&rtnd->event);
}

static int virtnet_tx_reclaim_locked(struct rtmux_lkl_netdev *rtnd)
{
    struct rtmux_virtio_queue *q = rtnd->txq;

    virtio_mb();
    if (q && virtnet_used_pending(q) > q->num) {
        virtnet_fail_device_locked(rtnd);
        return -EIO;
    }
    while (q && q->used->idx != q->last_used) {
        struct vring_used_elem elem = q->used->ring[q->last_used % q->num];

        if (elem.id >= q->num || elem.len > q->desc[elem.id].len ||
            rtnd->tx_free[elem.id] || rtnd->tx_inflight == 0) {
            virtnet_fail_device_locked(rtnd);
            return -EIO;
        }
        rtnd->tx_free[elem.id] = true;
        rtnd->tx_inflight--;
        q->last_used++;
    }
    if (rtnd->tx_inflight < q->num)
        pthread_cond_broadcast(&rtnd->event);
    return 0;
}

static int virtnet_tx_alloc_locked(struct rtmux_lkl_netdev *rtnd)
{
    struct rtmux_virtio_queue *q = rtnd->txq;

    for (unsigned i = 0; i < q->num; i++) {
        if (rtnd->tx_free[i]) {
            rtnd->tx_free[i] = false;
            rtnd->tx_inflight++;
            return (int) i;
        }
    }
    return -EAGAIN;
}

static int rtmux_netdev_tx(struct lkl_netdev *nd, struct iovec *iov, int cnt)
{
    struct rtmux_lkl_netdev *rtnd = (struct rtmux_lkl_netdev *) nd;
    struct rtmux_virtio_queue *q = rtnd->txq;
    int id;
    int n;

    if (!q || rtnd->hup)
        return -ENETDOWN;
    pthread_mutex_lock(&rtnd->lock);
    if (virtnet_tx_reclaim_locked(rtnd) != 0) {
        pthread_mutex_unlock(&rtnd->lock);
        return -EIO;
    }
    if ((id = virtnet_tx_alloc_locked(rtnd)) < 0) {
        uint64_t end = lkl_netd_time() + VIRTNET_TX_STALL_NS;
        struct timespec deadline = {
            .tv_sec = (time_t) (end / 1000000000ULL),
            .tv_nsec = (long) (end % 1000000000ULL),
        };

        while ((id = virtnet_tx_alloc_locked(rtnd)) < 0 && !rtnd->hup) {
            int rc =
                pthread_cond_timedwait(&rtnd->event, &rtnd->lock, &deadline);
            if (virtnet_tx_reclaim_locked(rtnd) != 0) {
                pthread_mutex_unlock(&rtnd->lock);
                return -EIO;
            }

            /* Reclaim above frees the slot the loop head re-allocs. If the
             * stall window elapsed with nothing freed, the device is wedged.
             */
            if (rc == ETIMEDOUT && rtnd->tx_inflight >= q->num)
                virtnet_fail_device_locked(rtnd);
        }
    }
    if (rtnd->hup || id < 0) {
        pthread_mutex_unlock(&rtnd->lock);
        return -ENETDOWN;
    }

    memset(rtnd->tx_pool.buf[id].addr, 0, rtnd->hdr_size);
    n = virtnet_copy_from_iov(
        (uint8_t *) rtnd->tx_pool.buf[id].addr + rtnd->hdr_size,
        VIRTNET_BUF_SIZE - rtnd->hdr_size, iov, cnt);
    if (n < 0) {
        rtnd->tx_free[id] = true;
        if (rtnd->tx_inflight > 0)
            rtnd->tx_inflight--;
        pthread_cond_broadcast(&rtnd->event);
        pthread_mutex_unlock(&rtnd->lock);
        return n;
    }
    rtmux_lkl_host_stats_net_bounce_tx((size_t) n);
    virtnet_publish_locked(&rtnd->vio, q, (unsigned) id,
                           rtnd->tx_pool.buf[id].paddr,
                           (uint32_t) n + rtnd->hdr_size, 0);
    pthread_mutex_unlock(&rtnd->lock);
    if (__atomic_exchange_n(&lkl_netdev_crash_after_tx, false,
                            __ATOMIC_ACQ_REL)) {
        volatile int *p = (volatile int *) 0;

        fprintf(stderr, "lkl: virtio-net crash after bounced tx\n");
        *p = 1;
    }
    return n;
}

static size_t virtnet_iov_capacity(const struct iovec *iov, int cnt)
{
    size_t cap = 0;

    for (int i = 0; iov && i < cnt; i++)
        cap += iov[i].iov_len;
    return cap;
}

static int rtmux_netdev_rx(struct lkl_netdev *nd, struct iovec *iov, int cnt)
{
    struct rtmux_lkl_netdev *rtnd = (struct rtmux_lkl_netdev *) nd;
    struct rtmux_virtio_queue *q = rtnd->rxq;
    struct vring_used_elem elem;
    size_t frame;
    int n;

    if (!q)
        return -ENETDOWN;
    pthread_mutex_lock(&rtnd->lock);
    if (rtnd->hup) {
        pthread_mutex_unlock(&rtnd->lock);
        return -ENETDOWN;
    }
    if (!virtnet_rx_ready_locked(rtnd)) {
        pthread_mutex_unlock(&rtnd->lock);
        return -EAGAIN;
    }
    if (virtnet_used_pending(q) > q->num) {
        virtnet_fail_device_locked(rtnd);
        pthread_mutex_unlock(&rtnd->lock);
        return -EIO;
    }
    elem = q->used->ring[q->last_used % q->num];
    q->last_used++;

    /* A used id outside the ring means the buffer that owns this descriptor is
     * unknown: it cannot be reposted, so fail the device like TX reclaim does
     * rather than leak the descriptor and limp on a corrupt ring.
     */
    if (elem.id >= q->num) {
        virtnet_fail_device_locked(rtnd);
        pthread_mutex_unlock(&rtnd->lock);
        return -EIO;
    }
    if (elem.len < rtnd->hdr_size || elem.len > VIRTNET_BUF_SIZE) {
        virtnet_rx_post_locked(rtnd, elem.id);
        pthread_mutex_unlock(&rtnd->lock);
        return -EIO;
    }
    frame = elem.len - rtnd->hdr_size;

    /* Drop rather than hand LKL a silently truncated Ethernet frame if the
     * caller iov cannot hold the whole packet.
     */
    if (frame > virtnet_iov_capacity(iov, cnt)) {
        virtnet_rx_post_locked(rtnd, elem.id);
        pthread_mutex_unlock(&rtnd->lock);
        return -EIO;
    }
    n = virtnet_copy_to_iov(
        iov, cnt, (uint8_t *) rtnd->rx_pool.buf[elem.id].addr + rtnd->hdr_size,
        frame);
    if (n > 0)
        rtmux_lkl_host_stats_net_bounce_rx((size_t) n);
    virtnet_rx_post_locked(rtnd, elem.id);
    pthread_mutex_unlock(&rtnd->lock);
    return n;
}

static int rtmux_netdev_poll(struct lkl_netdev *nd)
{
    struct rtmux_lkl_netdev *rtnd = (struct rtmux_lkl_netdev *) nd;

    pthread_mutex_lock(&rtnd->lock);
    for (;;) {
        if (rtnd->hup) {
            pthread_mutex_unlock(&rtnd->lock);
            return LKL_DEV_NET_POLL_HUP;
        }
        if (virtnet_rx_ready_locked(rtnd)) {
            pthread_mutex_unlock(&rtnd->lock);
            return LKL_DEV_NET_POLL_RX;
        }
        pthread_cond_wait(&rtnd->event, &rtnd->lock);
    }
}

static void rtmux_netdev_poll_hup(struct lkl_netdev *nd)
{
    struct rtmux_lkl_netdev *rtnd = (struct rtmux_lkl_netdev *) nd;

    pthread_mutex_lock(&rtnd->lock);
    rtnd->hup = true;
    pthread_cond_signal(&rtnd->event);
    pthread_mutex_unlock(&rtnd->lock);
}

static void rtmux_netdev_free(struct lkl_netdev *nd)
{
    struct rtmux_lkl_netdev *rtnd = (struct rtmux_lkl_netdev *) nd;

    pthread_mutex_lock(&rtnd->lock);
    rtnd->hup = true;
    pthread_cond_broadcast(&rtnd->event);
    pthread_mutex_unlock(&rtnd->lock);
    virtio_irq_stop(&rtnd->vio);
    if (rtnd->vio.base)
        virtio_mmio_wr32(&rtnd->vio, VIRTIO_MMIO_STATUS, 0);
    virtio_queue_free(rtnd->rxq);
    virtio_queue_free(rtnd->txq);
    virtio_dma_pool_free(&rtnd->rx_pool);
    virtio_dma_pool_free(&rtnd->tx_pool);
    virtio_mmio_unmap(&rtnd->vio);
    (void) pthread_cond_destroy(&rtnd->event);
    (void) pthread_mutex_destroy(&rtnd->lock);
    free(rtnd);
}

static struct lkl_dev_net_ops rtmux_netdev_ops = {
    .tx = rtmux_netdev_tx,
    .rx = rtmux_netdev_rx,
    .poll = rtmux_netdev_poll,
    .poll_hup = rtmux_netdev_poll_hup,
    .free = rtmux_netdev_free,
};

struct lkl_netdev *rtmux_lkl_netdev_create(void)
{
    struct rtmux_lkl_netdev *rtnd = calloc(1, sizeof(*rtnd));
    pthread_condattr_t condattr;
    int ret;

    if (!rtnd)
        return NULL;
    rtnd->nd.ops = &rtmux_netdev_ops;
    rtnd->nd.has_vnet_hdr = 0;
    rtnd->nd.mac[0] = 0x02;
    rtnd->nd.mac[1] = 0x52;
    rtnd->nd.mac[2] = 0x54;
    rtnd->nd.mac[3] = 0x4d;
    rtnd->nd.mac[4] = 0x55;
    rtnd->nd.mac[5] = 0x58;
    if (pthread_mutex_init(&rtnd->lock, NULL) != EOK) {
        free(rtnd);
        return NULL;
    }

    /* event waits use CLOCK_MONOTONIC deadlines (lkl_netd_time), so the condvar
     * must measure against the same clock or the TX stall timeout is wrong.
     */
    if (pthread_condattr_init(&condattr) != EOK) {
        (void) pthread_mutex_destroy(&rtnd->lock);
        free(rtnd);
        return NULL;
    }
    (void) pthread_condattr_setclock(&condattr, CLOCK_MONOTONIC);
    if (pthread_cond_init(&rtnd->event, &condattr) != EOK) {
        (void) pthread_condattr_destroy(&condattr);
        (void) pthread_mutex_destroy(&rtnd->lock);
        free(rtnd);
        return NULL;
    }
    (void) pthread_condattr_destroy(&condattr);
    ret = virtnet_init(rtnd);
    if (ret != 0) {
        fprintf(stderr, "lkl: virtio-net init: %s\n", strerror(-ret));
        rtmux_netdev_free(&rtnd->nd);
        return NULL;
    }
    return &rtnd->nd;
}

void rtmux_lkl_netdev_destroy(struct lkl_netdev *nd)
{
    if (nd)
        rtmux_netdev_free(nd);
}

extern char lkl_virtio_devs[];
#define RTMUX_LKL_VIRTIO_DEVS lkl_virtio_devs

long __isoc23_strtol(const char *nptr, char **endptr, int base)
{
    return strtol(nptr, endptr, base);
}

unsigned long __isoc23_strtoul(const char *nptr, char **endptr, int base)
{
    return strtoul(nptr, endptr, base);
}

int inet_pton(int af, const char *src, void *dst)
{
    unsigned octet = 0;
    unsigned dots = 0;
    unsigned digits = 0;
    unsigned char *out = dst;

    if (af != LKL_AF_INET)
        return 0;
    memset(out, 0, 4);
    for (const char *p = src; *p != '\0'; p++) {
        if (*p >= '0' && *p <= '9') {
            /* reject leading zero ("01") and >3-digit octets */
            if ((digits == 1U && octet == 0U) || digits >= 3U)
                return 0;
            octet = octet * 10U + (unsigned) (*p - '0');
            if (octet > 255U)
                return 0;
            digits++;
            continue;
        }
        /* a dot needs at least one digit before it, and at most 3 dots */
        if (*p != '.' || digits == 0U || dots >= 3U)
            return 0;
        out[dots++] = (unsigned char) octet;
        octet = 0;
        digits = 0;
    }
    if (dots != 3U || digits == 0U)
        return 0;
    out[dots] = (unsigned char) octet;
    return 1;
}

int usleep(useconds_t usec)
{
    struct timespec ts;

    ts.tv_sec = (time_t) (usec / 1000000U);
    ts.tv_nsec = (long) (usec % 1000000U) * 1000L;
    return nanosleep(&ts, NULL);
}

unsigned int __bswapsi2(unsigned int v)
{
    return ((v & 0x000000ffU) << 24) | ((v & 0x0000ff00U) << 8) |
           ((v & 0x00ff0000U) >> 8) | ((v & 0xff000000U) >> 24);
}

static int lkl_netd_blk_get_capacity(struct lkl_disk disk,
                                     unsigned long long *res)
{
    struct stat st;

    if (!res || disk.fd < 0)
        return -EINVAL;
    if (fstat(disk.fd, &st) != 0)
        return -errno;
    if (st.st_size < 0)
        return -EINVAL;
    *res = (unsigned long long) st.st_size;
    return 0;
}

static int lkl_netd_blk_request(struct lkl_disk disk, struct lkl_blk_req *req)
{
    off_t off;
    int flags;

    if (disk.fd < 0 || !req)
        return LKL_DEV_BLK_STATUS_IOERR;
    flags = fcntl(disk.fd, F_GETFL);
    if (flags < 0)
        return LKL_DEV_BLK_STATUS_IOERR;
    if (req->type == LKL_DEV_BLK_TYPE_FLUSH ||
        req->type == LKL_DEV_BLK_TYPE_FLUSH_OUT) {
        if ((flags & O_ACCMODE) == O_RDONLY)
            return LKL_DEV_BLK_STATUS_OK;
        if (fsync(disk.fd) == 0 || errno == ENOSYS || errno == ENOTSUP ||
            errno == EOPNOTSUPP)
            return LKL_DEV_BLK_STATUS_OK;
        return LKL_DEV_BLK_STATUS_IOERR;
    }
    if (req->type != LKL_DEV_BLK_TYPE_READ &&
        req->type != LKL_DEV_BLK_TYPE_WRITE)
        return LKL_DEV_BLK_STATUS_UNSUP;

    /* req->count is a signed int; a negative count would skip the loop and
     * falsely report success, and a non-empty vector needs a buffer.
     */
    if (req->count < 0 || (req->count > 0 && !req->buf))
        return LKL_DEV_BLK_STATUS_IOERR;

    /* off_t is a signed 64-bit long under LP64; reject sectors whose byte
     * offset would overflow it before computing off.
     */
    if (req->sector > (unsigned long long) __INT64_MAX__ / 512ULL)
        return LKL_DEV_BLK_STATUS_IOERR;
    off = (off_t) (req->sector * 512ULL);
    for (int i = 0; i < req->count; i++) {
        char *p = req->buf[i].iov_base;
        size_t left = req->buf[i].iov_len;

        while (left > 0) {
            size_t chunk = left;
            ssize_t n;

            /* Reject before dispatch: off + left must not overflow off_t, so
             * off + n can't either.
             */
            if (left > (size_t) ((off_t) __INT64_MAX__ - off))
                return LKL_DEV_BLK_STATUS_IOERR;

            /* The iomsg nbytes field is int32_t; cap each transfer so a huge
             * single iov can't truncate the advertised count. The loop already
             * handles partial I/O.
             */
            if (chunk > (size_t) __INT32_MAX__)
                chunk = (size_t) __INT32_MAX__;

            /* Positional I/O: lseek+read races the shared fd offset across
             * concurrent block requests.
             */
            if (req->type == LKL_DEV_BLK_TYPE_READ)
                n = pread(disk.fd, p, chunk, off);
            else
                n = pwrite(disk.fd, p, chunk, off);
            if (n <= 0)
                return LKL_DEV_BLK_STATUS_IOERR;
            if (req->type == LKL_DEV_BLK_TYPE_READ)
                rtmux_lkl_host_stats_lkl_block_read((size_t) n);
            else
                rtmux_lkl_host_stats_lkl_block_write((size_t) n);
            if (__atomic_exchange_n(&lkl_block_crash_after_request, false,
                                    __ATOMIC_ACQ_REL)) {
                volatile int *crash = (volatile int *) 0;

                fprintf(stderr, "lkl: block crash after host copy\n");
                *crash = 1;
            }
            p += n;
            off += n;
            left -= (size_t) n;
        }
    }
    return LKL_DEV_BLK_STATUS_OK;
}

struct lkl_dev_blk_ops lkl_dev_blk_ops = {
    .get_capacity = lkl_netd_blk_get_capacity,
    .request = lkl_netd_blk_request,
};

struct lkl_netdev *lkl_netdev_tap_create(const char *ifname, int offload)
{
    (void) ifname;
    (void) offload;
    return NULL;
}

struct lkl_netdev *lkl_netdev_raw_create(const char *ifname)
{
    (void) ifname;
    return NULL;
}

struct lkl_netdev *lkl_netdev_macvtap_create(const char *path, int offload)
{
    (void) path;
    (void) offload;
    return NULL;
}

struct lkl_netdev *lkl_netdev_pipe_create(const char *ifname, int offload)
{
    (void) ifname;
    (void) offload;
    return NULL;
}

struct lkl_host_operations lkl_host_ops = {
    .virtio_devices = RTMUX_LKL_VIRTIO_DEVS,
    .print = lkl_netd_print,
    .panic = lkl_netd_panic,
    .sem_alloc = lkl_netd_sem_alloc,
    .sem_free = lkl_netd_sem_free,
    .sem_up = lkl_netd_sem_up,
    .sem_down = lkl_netd_sem_down,
    .mutex_alloc = lkl_netd_mutex_alloc,
    .mutex_free = lkl_netd_mutex_free,
    .mutex_lock = lkl_netd_mutex_lock,
    .mutex_unlock = lkl_netd_mutex_unlock,
    .thread_create = lkl_netd_thread_create,
    .thread_detach = lkl_netd_thread_detach,
    .thread_exit = lkl_netd_thread_exit,
    .thread_join = lkl_netd_thread_join,
    .thread_self = lkl_netd_thread_self,
    .thread_equal = lkl_netd_thread_equal,
    .thread_stack = lkl_netd_thread_stack,
    .tls_alloc = lkl_netd_tls_alloc,
    .tls_free = lkl_netd_tls_free,
    .tls_set = lkl_netd_tls_set,
    .tls_get = lkl_netd_tls_get,
    .mem_alloc = malloc,
    .mem_free = free,
    .page_alloc = lkl_netd_page_alloc,
    .page_free = lkl_netd_page_free,
    .time = lkl_netd_time,
    .timer_alloc = lkl_netd_timer_alloc,
    .timer_set_oneshot = lkl_netd_timer_set_oneshot,
    .timer_free = lkl_netd_timer_free,
    .ioremap = lkl_netd_ioremap,
    .iomem_access = lkl_netd_iomem_access,
    .jmp_buf_set = lkl_netd_jmp_buf_set,
    .jmp_buf_longjmp = lkl_netd_jmp_buf_longjmp,
    .memcpy = memcpy,
    .memset = memset,
    .memmove = memmove,
    .mmap = lkl_netd_mmap,
    .munmap = lkl_netd_munmap,
};
