/*
 * Host-side tests for rtmux_cap_table.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdatomic.h>
#include <cap-table.h>

#define RIGHTS_READ 0x1U
#define RIGHTS_WRITE 0x2U
#define RIGHTS_ADMIN 0x80000000U

static rtmux_cap_slot_t slots[4];
static rtmux_cap_table_t table;

struct pin_worker_arg {
    rtmux_cap_t cap;
    atomic_ulong pins;
};

static void test_generation_and_stale(void)
{
    int a, b;
    void *obj;
    rtmux_cap_t cap_a, cap_b;

    rtmux_cap_table_init(&table, slots, sizeof(slots) / sizeof(slots[0]));
    assert(rtmux_cap_alloc(&table, &a, RIGHTS_READ, &cap_a) == EOK);
    assert(rtmux_cap_lookup_pin(&table, cap_a, RIGHTS_READ, &obj) == EOK);
    assert(obj == &a);
    rtmux_cap_unpin(&table, cap_a);
    /* No pins outstanding: revoke returns the object directly. */
    assert(rtmux_cap_revoke(&table, cap_a, &obj) == EOK);
    assert(obj == &a);
    assert(rtmux_cap_lookup_pin(&table, cap_a, RIGHTS_READ, &obj) == ESTALE);

    assert(rtmux_cap_alloc(&table, &b, RIGHTS_READ, &cap_b) == EOK);
    assert(rtmux_cap_slot(cap_a) == rtmux_cap_slot(cap_b));
    assert(rtmux_cap_generation(cap_b) == rtmux_cap_generation(cap_a) + 1);
    assert(rtmux_cap_lookup_pin(&table, cap_a, RIGHTS_READ, &obj) == ESTALE);
}

static void test_wrap_retires_slot(void)
{
    int a, b;
    rtmux_cap_t cap;

    rtmux_cap_table_init(&table, slots, 2);
    slots[1].generation = RTMUX_CAP_GEN_MAX;
    slots[1].state = RTMUX_CAP_DEAD;
    assert(rtmux_cap_alloc(&table, &a, RIGHTS_READ, &cap) == ENOSPC);
    assert(slots[1].retired == 1);
    assert(rtmux_cap_alloc(&table, &b, RIGHTS_READ, &cap) == ENOSPC);
}

static void test_bounds_and_rights(void)
{
    int a;
    void *obj;
    rtmux_cap_t cap;
    rtmux_cap_t malformed;

    rtmux_cap_table_init(&table, slots, sizeof(slots) / sizeof(slots[0]));
    assert(rtmux_cap_alloc(&table, &a, RIGHTS_READ, &cap) == EOK);
    assert(rtmux_cap_lookup_pin(&table, 0, RIGHTS_READ, &obj) == ESTALE);
    assert(rtmux_cap_lookup_pin(&table, rtmux_cap_make(0, rtmux_cap_slot(cap)),
                                RIGHTS_READ, &obj) == ESTALE);
    assert(rtmux_cap_lookup_pin(&table, rtmux_cap_make(1, 99), RIGHTS_READ,
                                &obj) == ESTALE);
    malformed = cap | RTMUX_CAP_SLOT_MASK;
    assert(rtmux_cap_lookup_pin(&table, malformed, RIGHTS_READ, &obj) ==
           ESTALE);
    assert(rtmux_cap_lookup_pin(&table, cap, RIGHTS_WRITE, &obj) == ESTALE);
    assert(rtmux_cap_lookup_pin(&table, cap, RIGHTS_ADMIN, &obj) == ESTALE);
    /* Requesting no rights must be rejected, not silently granted. */
    assert(rtmux_cap_lookup_pin(&table, cap, 0, &obj) == EINVAL);
    assert(rtmux_cap_lookup_pin(&table, cap, RIGHTS_READ, &obj) == EOK);
    assert(!rtmux_cap_unpin(&table, cap));
}

static void test_invalid_inputs_and_pin_overflow(void)
{
    int a;
    void *obj;
    rtmux_cap_t cap;

    assert(rtmux_cap_table_init(NULL, slots,
                                sizeof(slots) / sizeof(slots[0])) == EINVAL);
    assert(rtmux_cap_table_init(&table, slots, (size_t) UINT32_MAX + 1) ==
           EINVAL);
    rtmux_cap_table_init(&table, slots, sizeof(slots) / sizeof(slots[0]));
    assert(rtmux_cap_alloc(NULL, &a, RIGHTS_READ, &cap) == EINVAL);
    assert(rtmux_cap_alloc(&table, NULL, RIGHTS_READ, &cap) == EINVAL);
    assert(rtmux_cap_alloc(&table, &a, 0, &cap) == EINVAL);
    assert(rtmux_cap_alloc(&table, &a, RIGHTS_READ, NULL) == EINVAL);
    assert(rtmux_cap_alloc(&table, &a, RIGHTS_READ, &cap) == EOK);
    assert(rtmux_cap_delegate(&table, cap, 0, &cap) == EINVAL);
    assert(rtmux_cap_delegate(&table, cap, RIGHTS_READ, NULL) == EINVAL);

    slots[rtmux_cap_slot(cap)].pin_count = UINT32_MAX;
    assert(rtmux_cap_lookup_pin(&table, cap, RIGHTS_READ, &obj) == EOVERFLOW);
    slots[rtmux_cap_slot(cap)].pin_count = 0;
    assert(rtmux_cap_revoke(&table, cap, &obj) == EOK);
    assert(obj == &a);
}

/* Revoking a pinned cap must keep the object alive until the last unpin, which
 * then hands the object back exactly once for destruction.
 */
static void test_revoke_while_pinned(void)
{
    int a;
    void *obj;
    rtmux_cap_t cap;

    rtmux_cap_table_init(&table, slots, sizeof(slots) / sizeof(slots[0]));
    assert(rtmux_cap_alloc(&table, &a, RIGHTS_READ, &cap) == EOK);
    assert(rtmux_cap_lookup_pin(&table, cap, RIGHTS_READ, &obj) == EOK);
    assert(obj == &a);
    /* Pin outstanding: revoke defers the hand-back (dead==NULL) to final unpin.
     */
    obj = (void *) -1;
    assert(rtmux_cap_revoke(&table, cap, &obj) == EOK);
    assert(!obj);
    /* New lookups blocked immediately, but the held pin still sees the object.
     */
    assert(rtmux_cap_lookup_pin(&table, cap, RIGHTS_READ, &obj) == ESTALE);
    assert(rtmux_cap_unpin(&table, cap) == &a);
}

static void test_delegation_attenuates_rights(void)
{
    int a;
    void *obj;
    rtmux_cap_t cap;
    rtmux_cap_t read_only;

    rtmux_cap_table_init(&table, slots, sizeof(slots) / sizeof(slots[0]));
    assert(rtmux_cap_alloc(&table, &a, RIGHTS_READ | RIGHTS_WRITE, &cap) ==
           EOK);
    assert(rtmux_cap_delegate(&table, cap, RIGHTS_READ, &read_only) == EOK);
    assert(read_only != cap);
    assert(rtmux_cap_lookup_pin(&table, read_only, RIGHTS_READ, &obj) == EOK);
    assert(obj == &a);
    assert(!rtmux_cap_unpin(&table, read_only));
    assert(rtmux_cap_lookup_pin(&table, read_only, RIGHTS_WRITE, &obj) ==
           ESTALE);
    assert(rtmux_cap_delegate(&table, read_only, RIGHTS_WRITE, &cap) == ESTALE);
    assert(rtmux_cap_revoke(&table, cap, NULL) == EOK);
    assert(rtmux_cap_lookup_pin(&table, read_only, RIGHTS_READ, &obj) ==
           ESTALE);
    assert(rtmux_cap_lookup_pin(&table, cap, RIGHTS_WRITE, &obj) == ESTALE);
}

static void test_delegated_revoke_waits_for_pins(void)
{
    int a;
    void *obj;
    rtmux_cap_t cap;
    rtmux_cap_t read_only;

    rtmux_cap_table_init(&table, slots, sizeof(slots) / sizeof(slots[0]));
    assert(rtmux_cap_alloc(&table, &a, RIGHTS_READ | RIGHTS_WRITE, &cap) ==
           EOK);
    assert(rtmux_cap_delegate(&table, cap, RIGHTS_READ, &read_only) == EOK);
    assert(rtmux_cap_lookup_pin(&table, read_only, RIGHTS_READ, &obj) == EOK);
    assert(obj == &a);
    obj = (void *) -1;
    assert(rtmux_cap_revoke(&table, cap, &obj) == EOK);
    assert(!obj);
    assert(rtmux_cap_lookup_pin(&table, cap, RIGHTS_READ, &obj) == ESTALE);
    assert(rtmux_cap_lookup_pin(&table, read_only, RIGHTS_READ, &obj) ==
           ESTALE);
    assert(rtmux_cap_unpin(&table, read_only) == &a);
}

/* Revocation retires a group slot by slot: unpinned members die at once and
 * their slots come straight back for reuse, only a pinned member lingers in
 * REVOKING until its last pin drops.
 */
static void test_revoke_frees_unpinned_group_slots(void)
{
    int a, b;
    void *obj;
    rtmux_cap_t cap;
    rtmux_cap_t read_only;
    rtmux_cap_t reused;

    rtmux_cap_table_init(&table, slots, sizeof(slots) / sizeof(slots[0]));
    assert(rtmux_cap_alloc(&table, &a, RIGHTS_READ | RIGHTS_WRITE, &cap) ==
           EOK);
    assert(rtmux_cap_delegate(&table, cap, RIGHTS_READ, &read_only) == EOK);
    assert(rtmux_cap_lookup_pin(&table, read_only, RIGHTS_READ, &obj) == EOK);
    assert(rtmux_cap_revoke(&table, cap, NULL) == EOK);

    /* The root slot carried no pin, so the next alloc lands right back on it.
     */
    assert(rtmux_cap_alloc(&table, &b, RIGHTS_READ, &reused) == EOK);
    assert(rtmux_cap_slot(reused) == rtmux_cap_slot(cap));
    assert(rtmux_cap_generation(reused) == rtmux_cap_generation(cap) + 1);

    /* Reuse mints a fresh group, so the old group's final unpin still hands the
     * original object back exactly once and leaves the new cap alone.
     */
    assert(rtmux_cap_unpin(&table, read_only) == &a);
    assert(rtmux_cap_lookup_pin(&table, reused, RIGHTS_READ, &obj) == EOK);
    assert(obj == &b);
    assert(!rtmux_cap_unpin(&table, reused));
}

/* Two independent alloc()s of the same object pointer are separate revocation
 * groups: revoking one must not disturb the other, and each returns the object
 * for destruction on its own revoke.
 */
static void test_independent_roots_revoke_separately(void)
{
    int a;
    void *obj;
    rtmux_cap_t cap1;
    rtmux_cap_t cap2;

    rtmux_cap_table_init(&table, slots, sizeof(slots) / sizeof(slots[0]));
    assert(rtmux_cap_alloc(&table, &a, RIGHTS_READ, &cap1) == EOK);
    assert(rtmux_cap_alloc(&table, &a, RIGHTS_READ, &cap2) == EOK);
    assert(cap1 != cap2);

    obj = NULL;
    assert(rtmux_cap_revoke(&table, cap1, &obj) == EOK);
    assert(obj == &a);
    /* cap2 is untouched by cap1's revoke. */
    assert(rtmux_cap_lookup_pin(&table, cap2, RIGHTS_READ, &obj) == EOK);
    assert(obj == &a);
    assert(!rtmux_cap_unpin(&table, cap2));

    obj = NULL;
    assert(rtmux_cap_revoke(&table, cap2, &obj) == EOK);
    assert(obj == &a);
    assert(rtmux_cap_lookup_pin(&table, cap2, RIGHTS_READ, &obj) == ESTALE);
}

static void test_group_id_crosses_32bit_wrap(void)
{
    int a, b;
    void *obj;
    rtmux_cap_t cap1;
    rtmux_cap_t cap2;

    rtmux_cap_table_init(&table, slots, sizeof(slots) / sizeof(slots[0]));
    assert(rtmux_cap_alloc(&table, &a, RIGHTS_READ, &cap1) == EOK);
    table.next_group = UINT32_MAX;
    assert(rtmux_cap_alloc(&table, &b, RIGHTS_READ, &cap2) == EOK);
    assert(slots[rtmux_cap_slot(cap1)].group !=
           slots[rtmux_cap_slot(cap2)].group);

    assert(rtmux_cap_revoke(&table, cap2, &obj) == EOK);
    assert(obj == &b);
    assert(rtmux_cap_lookup_pin(&table, cap1, RIGHTS_READ, &obj) == EOK);
    assert(obj == &a);
    assert(!rtmux_cap_unpin(&table, cap1));
}

static void test_group_id_skips_live_after_64bit_wrap(void)
{
    int a, b;
    void *obj;
    rtmux_cap_t cap1;
    rtmux_cap_t cap2;

    rtmux_cap_table_init(&table, slots, sizeof(slots) / sizeof(slots[0]));
    assert(rtmux_cap_alloc(&table, &a, RIGHTS_READ, &cap1) == EOK);
    table.next_group = UINT64_MAX;
    assert(rtmux_cap_alloc(&table, &b, RIGHTS_READ, &cap2) == EOK);
    assert(slots[rtmux_cap_slot(cap1)].group == 1);
    assert(slots[rtmux_cap_slot(cap2)].group != 1);

    assert(rtmux_cap_revoke(&table, cap2, &obj) == EOK);
    assert(obj == &b);
    assert(rtmux_cap_lookup_pin(&table, cap1, RIGHTS_READ, &obj) == EOK);
    assert(obj == &a);
    assert(!rtmux_cap_unpin(&table, cap1));
}

static void *pin_worker(void *arg)
{
    struct pin_worker_arg *worker = arg;
    rtmux_cap_t cap = worker->cap;
    void *obj;

    for (;;) {
        int rc = rtmux_cap_lookup_pin(&table, cap, RIGHTS_READ, &obj);
        if (rc == ESTALE)
            return NULL;
        assert(rc == EOK);
        assert(obj);
        (void) atomic_fetch_add_explicit(&worker->pins, 1,
                                         memory_order_relaxed);
        rtmux_cap_unpin(&table, cap);
    }
}

static void wait_worker_pins(struct pin_worker_arg *args, int nworkers)
{
    int tries;

    for (tries = 0; tries < 1000000; tries++) {
        int i;
        int ready = 1;

        for (i = 0; i < nworkers; i++) {
            if (atomic_load_explicit(&args[i].pins, memory_order_relaxed) ==
                0) {
                ready = 0;
                break;
            }
        }
        if (ready)
            return;
        sched_yield();
    }
    assert(0 && "pin workers did not start");
}

static void test_lookup_pin_revoke_contention(void)
{
    int a;
    rtmux_cap_t cap;
    pthread_t th;
    void *obj;

    rtmux_cap_table_init(&table, slots, sizeof(slots) / sizeof(slots[0]));
    assert(rtmux_cap_alloc(&table, &a, RIGHTS_READ, &cap) == EOK);
    {
        struct pin_worker_arg arg;

        arg.cap = cap;
        atomic_init(&arg.pins, 0);
        assert(pthread_create(&th, NULL, pin_worker, &arg) == 0);
        wait_worker_pins(&arg, 1);
        assert(rtmux_cap_lookup_pin(&table, cap, RIGHTS_READ, &obj) == EOK);
        assert(rtmux_cap_revoke(&table, cap, NULL) == EOK);
        rtmux_cap_unpin(&table, cap);
        assert(pthread_join(th, NULL) == 0);
    }
    assert(rtmux_cap_lookup_pin(&table, cap, RIGHTS_READ, &obj) == ESTALE);
}

static void assert_no_live_slots(const rtmux_cap_slot_t *slots, size_t nslots)
{
    size_t i;

    for (i = 1; i < nslots; i++) {
        assert(slots[i].pin_count == 0);
        assert(slots[i].state == RTMUX_CAP_FREE ||
               slots[i].state == RTMUX_CAP_DEAD);
    }
}

static void test_revoke_contention_stress(void)
{
    enum { NSTRESS_SLOTS = 16, NWORKERS = 4, NCYCLES = 64 };
    static rtmux_cap_slot_t stress_slots[NSTRESS_SLOTS];
    struct pin_worker_arg args[NWORKERS];
    pthread_t threads[NWORKERS];
    rtmux_cap_t caps[NWORKERS];
    rtmux_cap_transfer_t xfers[2];
    int objects[NCYCLES];
    int cycle;
    int i;

    rtmux_cap_table_init(&table, stress_slots, NSTRESS_SLOTS);
    for (cycle = 0; cycle < NCYCLES; cycle++) {
        void *obj = NULL;

        assert(rtmux_cap_alloc(&table, &objects[cycle],
                               RIGHTS_READ | RIGHTS_WRITE, &caps[0]) == EOK);
        assert(rtmux_cap_delegate(&table, caps[0], RIGHTS_READ, &caps[1]) ==
               EOK);
        xfers[0].cap = caps[0];
        xfers[0].rights = RIGHTS_READ;
        xfers[0].flags = 0;
        xfers[1] = xfers[0];
        assert(rtmux_cap_transfer(&table, xfers, &caps[2], 2) == EOK);

        assert(rtmux_cap_lookup_pin(&table, caps[0], RIGHTS_READ, &obj) == EOK);
        assert(obj == &objects[cycle]);
        for (i = 0; i < NWORKERS; i++) {
            args[i].cap = caps[i];
            atomic_store_explicit(&args[i].pins, 0, memory_order_relaxed);
            assert(pthread_create(&threads[i], NULL, pin_worker, &args[i]) ==
                   0);
        }
        wait_worker_pins(args, NWORKERS);

        obj = (void *) -1;
        assert(rtmux_cap_revoke(&table, caps[0], &obj) == EOK);
        assert(!obj);
        for (i = 0; i < NWORKERS; i++)
            assert(pthread_join(threads[i], NULL) == 0);
        for (i = 0; i < NWORKERS; i++) {
            assert(rtmux_cap_lookup_pin(&table, caps[i], RIGHTS_READ, &obj) ==
                   ESTALE);
        }
        assert(rtmux_cap_unpin(&table, caps[0]) == &objects[cycle]);
        assert_no_live_slots(stress_slots, NSTRESS_SLOTS);
    }
}

static void test_bounded_transfer_vector(void)
{
    int a;
    void *obj;
    rtmux_cap_t cap;
    rtmux_cap_t out[RTMUX_CAP_TRANSFER_MAX];
    rtmux_cap_transfer_t xfers[RTMUX_CAP_TRANSFER_MAX + 1];

    rtmux_cap_table_init(&table, slots, sizeof(slots) / sizeof(slots[0]));
    assert(rtmux_cap_alloc(&table, &a, RIGHTS_READ | RIGHTS_WRITE, &cap) ==
           EOK);

    xfers[0].cap = cap;
    xfers[0].rights = RIGHTS_READ;
    xfers[0].flags = 0;
    assert(rtmux_cap_transfer(&table, xfers, out, 1) == EOK);
    assert(rtmux_cap_lookup_pin(&table, out[0], RIGHTS_READ, &obj) == EOK);
    assert(obj == &a);
    assert(!rtmux_cap_unpin(&table, out[0]));
    assert(rtmux_cap_lookup_pin(&table, out[0], RIGHTS_WRITE, &obj) == ESTALE);
    assert(rtmux_cap_revoke(&table, cap, NULL) == EOK);
    assert(rtmux_cap_lookup_pin(&table, out[0], RIGHTS_READ, &obj) == ESTALE);

    assert(rtmux_cap_transfer(&table, xfers, out, 0) == EOK);
    assert(rtmux_cap_transfer(&table, NULL, out, 1) == EINVAL);
    assert(rtmux_cap_transfer(&table, xfers, NULL, 1) == EINVAL);
    assert(rtmux_cap_transfer(&table, xfers, out, RTMUX_CAP_TRANSFER_MAX + 1) ==
           E2BIG);
}

static void test_transfer_rejects_flags_without_partial_alloc(void)
{
    int a;
    void *obj;
    rtmux_cap_t cap;
    rtmux_cap_t out[2];
    rtmux_cap_transfer_t xfers[2];

    rtmux_cap_table_init(&table, slots, sizeof(slots) / sizeof(slots[0]));
    assert(rtmux_cap_alloc(&table, &a, RIGHTS_READ | RIGHTS_WRITE, &cap) ==
           EOK);

    xfers[0].cap = cap;
    xfers[0].rights = RIGHTS_READ;
    xfers[0].flags = 0;
    xfers[1].cap = cap;
    xfers[1].rights = RIGHTS_READ;
    xfers[1].flags = 1;
    assert(rtmux_cap_transfer(&table, xfers, out, 2) == EINVAL);
    assert(rtmux_cap_lookup_pin(&table, cap, RIGHTS_READ, &obj) == EOK);
    assert(obj == &a);
    assert(!rtmux_cap_unpin(&table, cap));

    xfers[1].flags = 0;
    assert(rtmux_cap_transfer(&table, xfers, out, 2) == EOK);
    assert(rtmux_cap_lookup_pin(&table, out[0], RIGHTS_READ, &obj) == EOK);
    assert(obj == &a);
    assert(!rtmux_cap_unpin(&table, out[0]));
    assert(rtmux_cap_lookup_pin(&table, out[1], RIGHTS_READ, &obj) == EOK);
    assert(obj == &a);
    assert(!rtmux_cap_unpin(&table, out[1]));
}

static void test_transfer_enospc_is_all_or_nothing(void)
{
    int a;
    void *obj;
    rtmux_cap_t cap;
    rtmux_cap_t out[3];
    rtmux_cap_transfer_t xfers[3];

    rtmux_cap_table_init(&table, slots, sizeof(slots) / sizeof(slots[0]));
    assert(rtmux_cap_alloc(&table, &a, RIGHTS_READ | RIGHTS_WRITE, &cap) ==
           EOK);

    xfers[0].cap = cap;
    xfers[0].rights = RIGHTS_READ;
    xfers[0].flags = 0;
    xfers[1] = xfers[0];
    xfers[2] = xfers[0];
    assert(rtmux_cap_transfer(&table, xfers, out, 3) == ENOSPC);
    assert(rtmux_cap_transfer(&table, xfers, out, 2) == EOK);
    assert(rtmux_cap_lookup_pin(&table, out[0], RIGHTS_READ, &obj) == EOK);
    assert(obj == &a);
    assert(!rtmux_cap_unpin(&table, out[0]));
    assert(rtmux_cap_lookup_pin(&table, out[1], RIGHTS_READ, &obj) == EOK);
    assert(obj == &a);
    assert(!rtmux_cap_unpin(&table, out[1]));
}

static void test_transfer_enospc_does_not_retire_slots(void)
{
    rtmux_cap_slot_t src_slots[4];
    rtmux_cap_slot_t dst_slots[3];
    rtmux_cap_table_t src;
    rtmux_cap_table_t dst;
    rtmux_cap_stats_t stats;
    int endpoint;
    rtmux_cap_t cap;
    rtmux_cap_t out[2] = {0, 0};
    rtmux_cap_transfer_t xfers[2];

    rtmux_cap_table_init(&src, src_slots,
                         sizeof(src_slots) / sizeof(src_slots[0]));
    rtmux_cap_table_init(&dst, dst_slots,
                         sizeof(dst_slots) / sizeof(dst_slots[0]));
    dst_slots[1].generation = RTMUX_CAP_GEN_MAX;
    dst_slots[1].state = RTMUX_CAP_DEAD;
    assert(rtmux_cap_alloc(&src, &endpoint, RIGHTS_READ, &cap) == EOK);

    xfers[0].cap = cap;
    xfers[0].rights = RIGHTS_READ;
    xfers[0].flags = 0;
    xfers[1] = xfers[0];
    assert(rtmux_cap_transfer_to(&src, &dst, xfers, out, 2) == ENOSPC);
    assert(dst_slots[1].retired == 0);
    assert(dst_slots[2].state == RTMUX_CAP_FREE);
    assert(rtmux_cap_stats(&dst, &stats) == EOK);
    assert(stats.retired_slots == 0);
}

static void test_cross_table_transfer_and_object_revoke(void)
{
    rtmux_cap_slot_t src_slots[4];
    rtmux_cap_slot_t dst_slots[4];
    rtmux_cap_table_t src;
    rtmux_cap_table_t dst;
    int endpoint;
    void *obj;
    rtmux_cap_t cap;
    rtmux_cap_t out;
    rtmux_cap_transfer_t xfer;
    rtmux_cap_stats_t stats;

    rtmux_cap_table_init(&src, src_slots,
                         sizeof(src_slots) / sizeof(src_slots[0]));
    rtmux_cap_table_init(&dst, dst_slots,
                         sizeof(dst_slots) / sizeof(dst_slots[0]));
    assert(rtmux_cap_alloc(&src, &endpoint, RIGHTS_READ | RIGHTS_WRITE, &cap) ==
           EOK);

    xfer.cap = cap;
    xfer.rights = RIGHTS_READ;
    xfer.flags = 0;
    assert(rtmux_cap_transfer_to(&src, &dst, &xfer, &out, 1) == EOK);
    assert(rtmux_cap_lookup_pin(&dst, out, RIGHTS_READ, &obj) == EOK);
    assert(obj == &endpoint);
    assert(!rtmux_cap_unpin(&dst, out));
    assert(rtmux_cap_lookup_pin(&dst, out, RIGHTS_WRITE, &obj) == ESTALE);

    assert(rtmux_cap_revoke_object(&dst, &endpoint) == EOK);
    assert(rtmux_cap_lookup_pin(&dst, out, RIGHTS_READ, &obj) == ESTALE);

    assert(rtmux_cap_stats(&src, &stats) == EOK);
    assert(stats.transfer_count == 1);
    assert(rtmux_cap_stats(&dst, &stats) == EOK);
    assert(stats.transfer_count == 1);
    assert(stats.revoke_count == 1);
}

/* A multi-cap cross-table transfer lands each cap in its own destination slot
 * under its own freshly minted revocation group, so revoking one leaves the
 * others alone. Exercises the mint-then-commit split and the single-walk slot
 * cursor in rtmux_cap_transfer_to.
 */
static void test_cross_table_multi_transfer_isolates_groups(void)
{
    rtmux_cap_slot_t src_slots[8];
    rtmux_cap_slot_t dst_slots[8];
    rtmux_cap_table_t src;
    rtmux_cap_table_t dst;
    int a, b, c;
    int *objects[3];
    void *obj;
    rtmux_cap_t caps[3];
    rtmux_cap_t out[3];
    rtmux_cap_transfer_t xfers[3];
    size_t i;

    rtmux_cap_table_init(&src, src_slots,
                         sizeof(src_slots) / sizeof(src_slots[0]));
    rtmux_cap_table_init(&dst, dst_slots,
                         sizeof(dst_slots) / sizeof(dst_slots[0]));
    objects[0] = &a;
    objects[1] = &b;
    objects[2] = &c;
    for (i = 0; i < 3; i++) {
        assert(rtmux_cap_alloc(&src, objects[i], RIGHTS_READ, &caps[i]) == EOK);
        xfers[i].cap = caps[i];
        xfers[i].rights = RIGHTS_READ;
        xfers[i].flags = 0;
    }
    assert(rtmux_cap_transfer_to(&src, &dst, xfers, out, 3) == EOK);

    for (i = 0; i < 3; i++) {
        size_t j;

        assert(rtmux_cap_lookup_pin(&dst, out[i], RIGHTS_READ, &obj) == EOK);
        assert(obj == objects[i]);
        assert(!rtmux_cap_unpin(&dst, out[i]));
        for (j = 0; j < i; j++) {
            assert(rtmux_cap_slot(out[i]) != rtmux_cap_slot(out[j]));
            assert(dst_slots[rtmux_cap_slot(out[i])].group !=
                   dst_slots[rtmux_cap_slot(out[j])].group);
        }
    }

    /* Revoking the middle transfer must not disturb its neighbours. */
    obj = NULL;
    assert(rtmux_cap_revoke(&dst, out[1], &obj) == EOK);
    assert(obj == &b);
    assert(rtmux_cap_lookup_pin(&dst, out[1], RIGHTS_READ, &obj) == ESTALE);
    assert(rtmux_cap_lookup_pin(&dst, out[0], RIGHTS_READ, &obj) == EOK);
    assert(obj == &a);
    assert(!rtmux_cap_unpin(&dst, out[0]));
    assert(rtmux_cap_lookup_pin(&dst, out[2], RIGHTS_READ, &obj) == EOK);
    assert(obj == &c);
    assert(!rtmux_cap_unpin(&dst, out[2]));
}

/* A generation-saturated slot is taken out of circulation by whichever
 * operation walks it first. Transfer used to skip such a slot without retiring
 * it, so retired_slots depended on whether an alloc happened to pass by.
 */
static void test_transfer_retires_saturated_slot(void)
{
    rtmux_cap_slot_t src_slots[4];
    rtmux_cap_slot_t dst_slots[4];
    rtmux_cap_table_t src;
    rtmux_cap_table_t dst;
    rtmux_cap_stats_t stats;
    int endpoint;
    void *obj;
    rtmux_cap_t cap;
    rtmux_cap_t out;
    rtmux_cap_transfer_t xfer;

    rtmux_cap_table_init(&src, src_slots,
                         sizeof(src_slots) / sizeof(src_slots[0]));
    rtmux_cap_table_init(&dst, dst_slots,
                         sizeof(dst_slots) / sizeof(dst_slots[0]));
    dst_slots[1].generation = RTMUX_CAP_GEN_MAX;
    dst_slots[1].state = RTMUX_CAP_DEAD;

    assert(rtmux_cap_alloc(&src, &endpoint, RIGHTS_READ, &cap) == EOK);
    xfer.cap = cap;
    xfer.rights = RIGHTS_READ;
    xfer.flags = 0;
    assert(rtmux_cap_transfer_to(&src, &dst, &xfer, &out, 1) == EOK);

    /* Skipped the saturated slot, flagged it, and landed on the next one. */
    assert(dst_slots[1].retired == 1);
    assert(rtmux_cap_slot(out) == 2);
    assert(rtmux_cap_stats(&dst, &stats) == EOK);
    assert(stats.retired_slots == 1);

    assert(rtmux_cap_lookup_pin(&dst, out, RIGHTS_READ, &obj) == EOK);
    assert(obj == &endpoint);
    assert(!rtmux_cap_unpin(&dst, out));
}

static void test_abort_transfer_is_slot_local(void)
{
    rtmux_cap_slot_t src_slots[4];
    rtmux_cap_slot_t dst_slots[5];
    rtmux_cap_table_t src;
    rtmux_cap_table_t dst;
    int endpoint;
    void *obj;
    rtmux_cap_t cap;
    rtmux_cap_t first;
    rtmux_cap_t second;
    rtmux_cap_transfer_t xfer;

    rtmux_cap_table_init(&src, src_slots,
                         sizeof(src_slots) / sizeof(src_slots[0]));
    rtmux_cap_table_init(&dst, dst_slots,
                         sizeof(dst_slots) / sizeof(dst_slots[0]));
    assert(rtmux_cap_alloc(&src, &endpoint, RIGHTS_READ | RIGHTS_WRITE, &cap) ==
           EOK);

    xfer.cap = cap;
    xfer.rights = RIGHTS_READ;
    xfer.flags = 0;
    assert(rtmux_cap_transfer_to(&src, &dst, &xfer, &first, 1) == EOK);
    assert(rtmux_cap_transfer_to(&src, &dst, &xfer, &second, 1) == EOK);

    assert(rtmux_cap_abort(&dst, second) == EOK);
    assert(rtmux_cap_lookup_pin(&dst, first, RIGHTS_READ, &obj) == EOK);
    assert(obj == &endpoint);
    assert(!rtmux_cap_unpin(&dst, first));
    assert(rtmux_cap_lookup_pin(&dst, second, RIGHTS_READ, &obj) == ESTALE);
}

static void test_stats_accounting(void)
{
    rtmux_cap_slot_t stats_slots[6];
    int a;
    void *obj;
    rtmux_cap_t cap;
    rtmux_cap_t delegated;
    rtmux_cap_t out[2];
    rtmux_cap_transfer_t xfers[2];
    rtmux_cap_stats_t stats;

    rtmux_cap_table_init(&table, stats_slots,
                         sizeof(stats_slots) / sizeof(stats_slots[0]));
    assert(rtmux_cap_stats(&table, &stats) == EOK);
    assert(stats.alloc_count == 0);
    assert(stats.delegate_count == 0);
    assert(stats.transfer_count == 0);
    assert(stats.revoke_count == 0);
    assert(stats.revoke_stale_count == 0);
    assert(stats.lookup_denied_count == 0);
    assert(stats.retired_slots == 0);
    assert(stats.last_denied == 0);

    assert(rtmux_cap_alloc(&table, &a, RIGHTS_READ | RIGHTS_WRITE, &cap) ==
           EOK);
    assert(rtmux_cap_lookup_pin(&table, cap, RIGHTS_ADMIN, &obj) == ESTALE);
    assert(rtmux_cap_lookup_pin(&table, cap, 0, &obj) == EINVAL);
    assert(rtmux_cap_delegate(&table, cap, RIGHTS_READ, &delegated) == EOK);

    xfers[0].cap = cap;
    xfers[0].rights = RIGHTS_READ;
    xfers[0].flags = 0;
    xfers[1] = xfers[0];
    assert(rtmux_cap_transfer(&table, xfers, out, 2) == EOK);
    assert(rtmux_cap_revoke(&table, cap, NULL) == EOK);
    assert(rtmux_cap_revoke(&table, cap, NULL) == ESTALE);

    assert(rtmux_cap_stats(&table, &stats) == EOK);
    assert(stats.alloc_count == 1);
    assert(stats.delegate_count == 1);
    assert(stats.transfer_count == 2);
    assert(stats.revoke_count == 1);
    assert(stats.revoke_stale_count == 1);
    assert(stats.lookup_denied_count == 2);
    assert(stats.retired_slots == 0);
    assert(stats.last_denied == ESTALE);
}

static void test_stats_retired_slots(void)
{
    int a;
    rtmux_cap_t cap;
    rtmux_cap_stats_t stats;

    rtmux_cap_table_init(&table, slots, 2);
    slots[1].generation = RTMUX_CAP_GEN_MAX;
    slots[1].state = RTMUX_CAP_DEAD;
    assert(rtmux_cap_alloc(&table, &a, RIGHTS_READ, &cap) == ENOSPC);
    assert(rtmux_cap_stats(&table, &stats) == EOK);
    assert(stats.retired_slots == 1);
}

/* rtmux_cap_has_object backs the global-channel connect gate: it matches a live
 * cap by object pointer (not table-local id) with a rights subset, and stops
 * matching once the object is revoked, which is what makes a reused soul safe.
 */
static void test_has_object(void)
{
    int a = 0, b = 0;
    rtmux_cap_t cap_a;

    rtmux_cap_table_init(&table, slots, sizeof(slots) / sizeof(slots[0]));
    assert(rtmux_cap_alloc(&table, &a, RIGHTS_READ | RIGHTS_WRITE, &cap_a) ==
           EOK);

    assert(rtmux_cap_has_object(&table, &a, RIGHTS_READ));
    assert(rtmux_cap_has_object(&table, &a, RIGHTS_READ | RIGHTS_WRITE));
    assert(!rtmux_cap_has_object(&table, &a, RIGHTS_ADMIN));
    assert(!rtmux_cap_has_object(&table, &b, RIGHTS_READ));
    assert(!rtmux_cap_has_object(&table, &a, 0));
    assert(!rtmux_cap_has_object(NULL, &a, RIGHTS_READ));
    assert(!rtmux_cap_has_object(&table, NULL, RIGHTS_READ));

    assert(rtmux_cap_revoke_object(&table, &a) == EOK);
    assert(!rtmux_cap_has_object(&table, &a, RIGHTS_READ));
}

static void test_typed_object_discrimination(void)
{
    int obj = 0;
    rtmux_cap_t ep, proc, deleg, bad;
    void *out;

    rtmux_cap_table_init(&table, slots, sizeof(slots) / sizeof(slots[0]));

    /* An endpoint cap and a process cap may name the same object with the same
     * context and rights; the type tag keeps a lookup from confusing them.
     */
    assert(rtmux_cap_alloc(&table, &obj, RIGHTS_READ, &ep) == EOK);
    assert(rtmux_cap_alloc_typed(&table, &obj, 0, RIGHTS_READ,
                                 RTMUX_CAP_TYPE_PROCESS, &proc) == EOK);
    assert(ep != proc);

    /* Endpoint lookup accepts the endpoint cap and rejects the process cap. */
    assert(rtmux_cap_lookup_pin(&table, ep, RIGHTS_READ, &out) == EOK &&
           out == &obj);
    rtmux_cap_unpin(&table, ep);
    assert(rtmux_cap_lookup_pin(&table, proc, RIGHTS_READ, &out) == ESTALE);

    /* Typed lookup accepts the matching type and rejects the others. */
    assert(rtmux_cap_lookup_pin_typed(&table, proc, RIGHTS_READ,
                                      RTMUX_CAP_TYPE_PROCESS, &out,
                                      NULL) == EOK &&
           out == &obj);
    rtmux_cap_unpin(&table, proc);
    assert(rtmux_cap_lookup_pin_typed(&table, proc, RIGHTS_READ,
                                      RTMUX_CAP_TYPE_THREAD, &out,
                                      NULL) == ESTALE);
    assert(rtmux_cap_lookup_pin_typed(&table, ep, RIGHTS_READ,
                                      RTMUX_CAP_TYPE_PROCESS, &out,
                                      NULL) == ESTALE);

    /* An out-of-range type is rejected at alloc and at lookup. */
    assert(rtmux_cap_alloc_typed(&table, &obj, 0, RIGHTS_READ,
                                 (uint8_t) (RTMUX_CAP_TYPE_THREAD + 1),
                                 &bad) == EINVAL);
    assert(rtmux_cap_lookup_pin_typed(&table, proc, RIGHTS_READ,
                                      (uint8_t) (RTMUX_CAP_TYPE_THREAD + 1),
                                      &out, NULL) == EINVAL);

    /* Delegation preserves the type: the child is still a process cap. */
    assert(rtmux_cap_delegate(&table, proc, RIGHTS_READ, &deleg) == EOK);
    assert(rtmux_cap_lookup_pin(&table, deleg, RIGHTS_READ, &out) == ESTALE);
    assert(rtmux_cap_lookup_pin_typed(&table, deleg, RIGHTS_READ,
                                      RTMUX_CAP_TYPE_PROCESS, &out,
                                      NULL) == EOK);
    rtmux_cap_unpin(&table, deleg);

    /* Cross-table transfer preserves the type too. */
    {
        rtmux_cap_slot_t dslots[3];
        rtmux_cap_table_t dtable;
        rtmux_cap_transfer_t xfer = {
            .cap = proc, .rights = RIGHTS_READ, .flags = 0};
        rtmux_cap_t moved;

        rtmux_cap_table_init(&dtable, dslots, 3);
        assert(rtmux_cap_transfer_to(&table, &dtable, &xfer, &moved, 1) == EOK);
        assert(rtmux_cap_lookup_pin(&dtable, moved, RIGHTS_READ, &out) ==
               ESTALE);
        assert(rtmux_cap_lookup_pin_typed(&dtable, moved, RIGHTS_READ,
                                          RTMUX_CAP_TYPE_PROCESS, &out,
                                          NULL) == EOK);
        rtmux_cap_unpin(&dtable, moved);
    }

    /* Object-death revoke is type-agnostic: revoking by the object pointer
     * kills the process cap as well as the endpoint cap. Process soul-free
     * relies on this to revoke a process cap before the soul is recycled.
     */
    assert(rtmux_cap_revoke_object(&table, &obj) == EOK);
    assert(rtmux_cap_lookup_pin_typed(&table, proc, RIGHTS_READ,
                                      RTMUX_CAP_TYPE_PROCESS, &out,
                                      NULL) == ESTALE);
    assert(rtmux_cap_lookup_pin(&table, ep, RIGHTS_READ, &out) == ESTALE);
}

int main(void)
{
    test_generation_and_stale();
    test_wrap_retires_slot();
    test_bounds_and_rights();
    test_invalid_inputs_and_pin_overflow();
    test_revoke_while_pinned();
    test_delegation_attenuates_rights();
    test_delegated_revoke_waits_for_pins();
    test_revoke_frees_unpinned_group_slots();
    test_independent_roots_revoke_separately();
    test_group_id_crosses_32bit_wrap();
    test_group_id_skips_live_after_64bit_wrap();
    test_lookup_pin_revoke_contention();
    test_revoke_contention_stress();
    test_bounded_transfer_vector();
    test_transfer_rejects_flags_without_partial_alloc();
    test_transfer_enospc_is_all_or_nothing();
    test_transfer_enospc_does_not_retire_slots();
    test_cross_table_transfer_and_object_revoke();
    test_cross_table_multi_transfer_isolates_groups();
    test_transfer_retires_saturated_slot();
    test_abort_transfer_is_slot_local();
    test_stats_accounting();
    test_stats_retired_slots();
    test_has_object();
    test_typed_object_discrimination();
    puts("cap-table-test: ok");
    return 0;
}
