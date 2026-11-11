/*
 * Guest-side regression tests for RTMux LKL host operations.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include <lkl.h>
#include <lkl_host.h>
#include <lkl/asm-generic/errno.h>

#include "../../servers/lkl/domain.h"
#include "../../servers/lkl/virtio.h"

struct lkl_sem;
struct lkl_mutex;
struct lkl_tls_key;

struct lkl_host_selftest_thread {
    struct lkl_sem *ready;
    struct lkl_tls_key *key;
    volatile bool ok;
};

struct lkl_host_selftest_sem_waiter {
    struct lkl_sem *sem;
    struct lkl_sem *ready;
    volatile bool entered;
    volatile bool woke;
};

static volatile bool lkl_host_selftest_timer_fired;
static volatile bool lkl_host_selftest_jmp_seen;
static struct lkl_jmp_buf *lkl_host_selftest_jmp;

static void lkl_host_selftest_thread_fn(void *arg)
{
    struct lkl_host_selftest_thread *state = arg;
    unsigned long stack_size = 0;
    int marker;

    state->ok = false;
    if (lkl_host_ops.tls_set(state->key, &marker) != EOK)
        goto out;
    if (lkl_host_ops.tls_get(state->key) != &marker)
        goto out;
    if (!lkl_host_ops.thread_stack(&stack_size) ||
        stack_size != RTMUX_LKL_NETD_STACK_SIZE)
        goto out;
    state->ok = true;
out:
    lkl_host_ops.sem_up(state->ready);
}

static void lkl_host_selftest_sem_waiter_fn(void *arg)
{
    struct lkl_host_selftest_sem_waiter *state = arg;

    state->entered = true;
    lkl_host_ops.sem_up(state->ready);
    lkl_host_ops.sem_down(state->sem);
    state->woke = true;
}

static int lkl_host_selftest_sem_blocking(void)
{
    struct lkl_host_selftest_sem_waiter state;
    lkl_thread_t tid;

    memset(&state, 0, sizeof(state));
    state.sem = lkl_host_ops.sem_alloc(0);
    state.ready = lkl_host_ops.sem_alloc(0);
    if (!state.sem || !state.ready)
        goto fail;
    tid = lkl_host_ops.thread_create(lkl_host_selftest_sem_waiter_fn, &state);
    if (tid == 0)
        goto fail;
    lkl_host_ops.sem_down(state.ready);
    if (!state.entered || state.woke)
        goto fail_join;
    lkl_host_ops.sem_up(state.sem);
    if (lkl_host_ops.thread_join(tid) != 0 || !state.woke)
        goto fail;
    lkl_host_ops.sem_free(state.ready);
    lkl_host_ops.sem_free(state.sem);
    return 0;

fail_join:
    lkl_host_ops.sem_up(state.sem);
    (void) lkl_host_ops.thread_join(tid);
fail:
    if (state.ready)
        lkl_host_ops.sem_free(state.ready);
    lkl_host_ops.sem_free(state.sem);
    return -1;
}

static void lkl_host_selftest_timer_fn(void)
{
    lkl_host_selftest_timer_fired = true;
}

static void lkl_host_selftest_jmp_fn(void)
{
    lkl_host_selftest_jmp_seen = true;
    lkl_host_ops.jmp_buf_longjmp(lkl_host_selftest_jmp, 1);
}

static int lkl_host_selftest_threads(void)
{
    struct lkl_host_selftest_thread state;
    lkl_thread_t tid;

    memset(&state, 0, sizeof(state));
    state.ready = lkl_host_ops.sem_alloc(0);
    state.key = lkl_host_ops.tls_alloc(NULL);
    if (!state.ready || !state.key)
        goto fail;
    tid = lkl_host_ops.thread_create(lkl_host_selftest_thread_fn, &state);
    if (tid == 0)
        goto fail;
    lkl_host_ops.sem_down(state.ready);
    if (lkl_host_ops.thread_join(tid) != 0 || !state.ok)
        goto fail;
    lkl_host_ops.tls_free(state.key);
    lkl_host_ops.sem_free(state.ready);
    return 0;

fail:
    if (state.key)
        lkl_host_ops.tls_free(state.key);
    if (state.ready)
        lkl_host_ops.sem_free(state.ready);
    return -1;
}

static int lkl_host_selftest_timer(void)
{
    struct timespec nap = {.tv_sec = 0, .tv_nsec = 10000000L};
    void *timer;

    lkl_host_selftest_timer_fired = false;
    timer = lkl_host_ops.timer_alloc(lkl_host_selftest_timer_fn);
    if (!timer)
        return -1;
    if (lkl_host_ops.timer_set_oneshot(timer, 1000000UL) != 0) {
        lkl_host_ops.timer_free(timer);
        return -1;
    }
    for (int i = 0; i < 200 && !lkl_host_selftest_timer_fired; i++)
        nanosleep(&nap, NULL);
    lkl_host_ops.timer_free(timer);
    return lkl_host_selftest_timer_fired ? 0 : -1;
}

static int lkl_host_selftest_memory(void)
{
    void *page;
    void *map;

    page = lkl_host_ops.page_alloc(4096);
    if (!page)
        return -1;
    memset(page, 0xa5, 4096);
    lkl_host_ops.page_free(page, 4096);

    map = lkl_host_ops.mmap(NULL, 4096, LKL_PROT_READ | LKL_PROT_WRITE);
    if (!map)
        return -1;
    memset(map, 0x5a, 4096);
    return lkl_host_ops.munmap(map, 4096) == 0 ? 0 : -1;
}

static int lkl_host_selftest_dma_limits(void)
{
    struct rtmux_virtio_dma_pool pool;
    int error;

    error = virtio_dma_pool_alloc(&pool, 1, VIRTIO_DMA_ALLOC_MAX + 1U);
    if (error != EINVAL) {
        fprintf(stderr, "lkl-host: oversize DMA alloc returned %d\n", error);
        if (error == EOK)
            virtio_dma_pool_free(&pool);
        return -1;
    }

    error = virtio_dma_pool_alloc(&pool, VIRTIO_QUEUE_MAX_SIZE,
                                  VIRTIO_DMA_POOL_MAX /
                                      VIRTIO_QUEUE_MAX_SIZE + 1U);
    if (error != EINVAL) {
        fprintf(stderr, "lkl-host: oversize DMA pool returned %d\n", error);
        if (error == EOK)
            virtio_dma_pool_free(&pool);
        return -1;
    }
    return 0;
}

static int lkl_host_selftest_jump(void)
{
    struct lkl_jmp_buf jmpb;

    memset(&jmpb, 0, sizeof(jmpb));
    lkl_host_selftest_jmp = &jmpb;
    lkl_host_selftest_jmp_seen = false;
    lkl_host_ops.jmp_buf_set(&jmpb, lkl_host_selftest_jmp_fn);
    lkl_host_selftest_jmp = NULL;
    return lkl_host_selftest_jmp_seen ? 0 : -1;
}

/* lkl_errno() must map a negated Linux errno to the RTMux POSIX value, not pass
 * the number through. Cover the identity range, fs, socket/net, timeout,
 * unsupported, and an unmapped code that has to fail closed as EIO.
 */
static int lkl_host_selftest_errno(void)
{
    static const struct {
        long lkl_ret;
        int want;
    } cases[] = {
        {0, EOK},
        {-LKL_ENOENT, ENOENT},                 /* 1..34 identity */
        {-LKL_ENOSYS, ENOSYS},                 /* divergent generic */
        {-LKL_ENAMETOOLONG, ENAMETOOLONG},     /* fs */
        {-LKL_ELOOP, ELOOP},                   /* fs */
        {-LKL_ECONNREFUSED, ECONNREFUSED},     /* net */
        {-LKL_ETIMEDOUT, ETIMEDOUT},           /* timeout */
        {-LKL_EOPNOTSUPP, EOPNOTSUPP},         /* unsupported op */
        {-LKL_EHWPOISON, EIO},                 /* unmapped -> fail closed */
    };
    size_t i;

    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        int got = lkl_errno(cases[i].lkl_ret);
        if (got != cases[i].want) {
            fprintf(stderr, "lkl-host: errno map %ld -> %d, want %d\n",
                    cases[i].lkl_ret, got, cases[i].want);
            return -1;
        }
    }
    return 0;
}

int rtmux_lkl_host_selftest(void)
{
    struct lkl_mutex *mutex;
    struct lkl_sem *sem;
    unsigned long stack_size = 0;

    fputs("lkl-host: selftest stack\n", stderr);
    if (!lkl_host_ops.thread_stack(&stack_size) ||
        stack_size != RTMUX_LKL_DEFAULT_STACK_SIZE)
        goto fail;
    fputs("lkl-host: selftest sem\n", stderr);
    sem = lkl_host_ops.sem_alloc(0);
    if (!sem)
        goto fail;
    lkl_host_ops.sem_up(sem);
    lkl_host_ops.sem_down(sem);
    lkl_host_ops.sem_free(sem);
    if (lkl_host_selftest_sem_blocking() != 0)
        goto fail;

    fputs("lkl-host: selftest mutex\n", stderr);
    mutex = lkl_host_ops.mutex_alloc(1);
    if (!mutex)
        goto fail;
    lkl_host_ops.mutex_lock(mutex);
    lkl_host_ops.mutex_lock(mutex);
    lkl_host_ops.mutex_unlock(mutex);
    lkl_host_ops.mutex_unlock(mutex);
    lkl_host_ops.mutex_free(mutex);

    fputs("lkl-host: selftest threads\n", stderr);
    if (lkl_host_selftest_threads() != 0)
        goto fail;
    fputs("lkl-host: selftest timer\n", stderr);
    if (lkl_host_selftest_timer() != 0)
        goto fail;
    fputs("lkl-host: selftest memory\n", stderr);
    if (lkl_host_selftest_memory() != 0)
        goto fail;
    fputs("lkl-host: selftest dma limits\n", stderr);
    if (lkl_host_selftest_dma_limits() != 0)
        goto fail;
    fputs("lkl-host: selftest jump\n", stderr);
    if (lkl_host_selftest_jump() != 0)
        goto fail;
    fputs("lkl-host: selftest errno\n", stderr);
    if (lkl_host_selftest_errno() != 0)
        goto fail;

    fputs("lkl-host: selftest OK\n", stderr);
    return 0;

fail:
    fputs("lkl-host: selftest FAIL\n", stderr);
    return -1;
}
