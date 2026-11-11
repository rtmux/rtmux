/*
 * RTMux capability handle table.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <string.h>
#include <cap-table.h>

/*@ assigns \nothing; */
static inline void cap_cpu_relax(void)
{
    atomic_signal_fence(memory_order_acq_rel);
}

/* "make verify" discharges the contracts below against this file, so they
 * describe the shipping body rather than a copy of it.
 *
 * The marker line below is what the gate reads to decide what to prove here. It
 * names functions rather than the file because a contract creates proof
 * obligations at every call site: the public rtmux_cap_* wrappers do not carry
 * contracts yet, so their calls into cap_lock and cap_lookup_pin_typed cannot
 * be discharged. Give a wrapper the same table/slots preconditions and it can
 * join the list. framac-verify:
 * cap_cpu_relax,cap_lock,cap_unlock,cap_last_denied,cap_lookup_pin_typed,rtmux_cap_slot,rtmux_cap_generation,rtmux_cap_lookup_pin,rtmux_cap_lookup_pin_context,rtmux_cap_lookup_pin_typed
 * framac-scan: kernel/cap/table.c kernel/include/cap-table.h framac-min-goals:
 * 135
 */
/*@ requires \valid(lock);
    assigns *lock;
 */
static void cap_lock(atomic_uint *lock)
{
    /*@ loop assigns *lock; */
    while (atomic_exchange_explicit(lock, 1, memory_order_acquire) != 0) {
        /*@ loop assigns \nothing; */
        while (atomic_load_explicit(lock, memory_order_relaxed) != 0)
            cap_cpu_relax();
    }
}

/*@ requires \valid(lock);
    assigns *lock;
 */
static void cap_unlock(atomic_uint *lock)
{
    atomic_store_explicit(lock, 0, memory_order_release);
}

static void cap_lock_pair(rtmux_cap_table_t *a, rtmux_cap_table_t *b)
{
    if (a == b) {
        cap_lock(&a->lock);
    } else if ((uintptr_t) a < (uintptr_t) b) {
        cap_lock(&a->lock);
        cap_lock(&b->lock);
    } else {
        cap_lock(&b->lock);
        cap_lock(&a->lock);
    }
}

static void cap_unlock_pair(rtmux_cap_table_t *a, rtmux_cap_table_t *b)
{
    if (a == b) {
        cap_unlock(&a->lock);
    } else if ((uintptr_t) a < (uintptr_t) b) {
        cap_unlock(&b->lock);
        cap_unlock(&a->lock);
    } else {
        cap_unlock(&a->lock);
        cap_unlock(&b->lock);
    }
}

/*@ requires \valid(table);
    assigns table->last_denied;
 */
static void cap_last_denied(rtmux_cap_table_t *table, int error)
{
    table->last_denied = error;
}

/* A slot still bound to its object: either freely usable or pending the final
 * unpin of a revocation. Both group scans below key off this.
 */
static inline bool cap_slot_active(const rtmux_cap_slot_t *slot)
{
    return slot->state == RTMUX_CAP_LIVE || slot->state == RTMUX_CAP_REVOKING;
}

/* Unbind a slot from its object. The generation stands, so every handle that
 * named it goes stale rather than becoming someone else's cap.
 */
static inline void cap_slot_kill(rtmux_cap_slot_t *slot)
{
    slot->object = NULL;
    slot->context = 0;
    slot->rights = 0;
    slot->state = RTMUX_CAP_DEAD;
}

/* Index of the first reusable slot at or after "from", or 0 when the table has
 * no room left (slot 0 is reserved, so it doubles as the failure value).
 *
 * The one place a saturated slot is taken out of circulation for alloc and
 * delegate, so those operations agree on what "retired" means.
 */
static uint32_t cap_slot_take(rtmux_cap_table_t *table, uint32_t from)
{
    uint32_t i;

    for (i = from; i < table->nslots; i++) {
        rtmux_cap_slot_t *slot = &table->slots[i];

        if (slot->retired ||
            (slot->state != RTMUX_CAP_FREE && slot->state != RTMUX_CAP_DEAD))
            continue;

        /* Saturated: it can never mint a fresh handle again, so stop rescanning
         * it on every future allocation.
         */
        if (slot->generation == RTMUX_CAP_GEN_MAX) {
            slot->retired = true;
            continue;
        }
        return i;
    }
    return 0;
}

/* Transfer reservation must be side-effect free until the whole batch fits. */
static uint32_t cap_slot_find(rtmux_cap_table_t *table, uint32_t from)
{
    uint32_t i;

    for (i = from; i < table->nslots; i++) {
        rtmux_cap_slot_t *slot = &table->slots[i];

        if (!slot->retired &&
            (slot->state == RTMUX_CAP_FREE || slot->state == RTMUX_CAP_DEAD) &&
            slot->generation != RTMUX_CAP_GEN_MAX)
            return i;
    }
    return 0;
}

/* Bind a slot taken by cap_slot_take to an object and return the handle naming
 * it. The generation bump cannot wrap into the reserved 0 generation, because
 * cap_slot_take never hands back a saturated slot.
 */
static rtmux_cap_t cap_slot_fill(rtmux_cap_slot_t *slot,
                                 uint32_t index,
                                 uint64_t group,
                                 void *object,
                                 uint64_t context,
                                 uint32_t rights,
                                 uint8_t obj_type)
{
    slot->generation++;
    slot->group = group;
    slot->object = object;
    slot->context = context;
    slot->rights = rights;
    slot->obj_type = obj_type;
    slot->pin_count = 0;
    slot->state = RTMUX_CAP_LIVE;
    return rtmux_cap_make(slot->generation, index);
}

int rtmux_cap_table_init(rtmux_cap_table_t *table,
                         rtmux_cap_slot_t *slots,
                         size_t nslots)
{
    if (!table || nslots > UINT32_MAX)
        return EINVAL;
    table->slots = slots;
    table->nslots = (uint32_t) nslots;
    table->next_group = 0;
    table->group_wrapped = false;
    atomic_store_explicit(&table->lock, 0, memory_order_relaxed);
    table->alloc_count = 0;
    table->delegate_count = 0;
    table->transfer_count = 0;
    table->revoke_count = 0;
    table->revoke_stale_count = 0;
    table->lookup_denied_count = 0;
    table->last_denied = 0;
    if (slots)
        memset(slots, 0, table->nslots * sizeof slots[0]);
    return EOK;
}

/* Group identity, not object identity, defines a revocation domain. Two
 * independent rtmux_cap_alloc() calls for the same object pointer get distinct
 * groups, so revoking one never touches the other. Delegation copies the root's
 * group, so an attenuated handle dies with its root.
 *
 * A bounded scan, not a free list. At most nslots-1 groups can be live.
 */
static bool cap_group_live(rtmux_cap_table_t *table, uint64_t group)
{
    uint32_t i;

    for (i = 1; i < table->nslots; i++) {
        rtmux_cap_slot_t *slot = &table->slots[i];

        if (slot->group == group && cap_slot_active(slot))
            return true;
    }
    return false;
}

/* Ids are minted monotonically, so a freshly incremented id has never been
 * handed out and cannot alias a live group — that is the O(1) path taken by
 * every real execution. Only once the 64-bit counter has wrapped can a
 * candidate collide, and only then does the bounded rescan earn its keep.
 */
static int cap_next_group(rtmux_cap_table_t *table, uint64_t *group)
{
    uint32_t i;

    for (i = 0; i < table->nslots; i++) {
        if (++table->next_group == 0) {
            table->next_group++;
            table->group_wrapped = true;
        }
        if (!table->group_wrapped ||
            !cap_group_live(table, table->next_group)) {
            *group = table->next_group;
            return EOK;
        }
    }
    return ENOSPC;
}

/* Retire a revocation group. Slots with no outstanding pin die on the spot and
 * become reusable; a pinned slot sits in REVOKING, blocking new lookups while
 * its holder keeps a valid object, until the final rtmux_cap_unpin() retires it
 * too.
 *
 * Returns true when nothing in the group was pinned, i.e. the caller now owns
 * the object and must destroy it.
 */
static bool cap_group_retire(rtmux_cap_table_t *table, uint64_t group)
{
    uint32_t i;
    bool unpinned = true;

    for (i = 1; i < table->nslots; i++) {
        rtmux_cap_slot_t *slot = &table->slots[i];

        if (slot->group != group || !cap_slot_active(slot))
            continue;
        if (slot->pin_count != 0) {
            slot->state = RTMUX_CAP_REVOKING;
            unpinned = false;
        } else {
            cap_slot_kill(slot);
        }
    }
    return unpinned;
}

static int cap_alloc_new_locked(rtmux_cap_table_t *table,
                                void *object,
                                uint64_t context,
                                uint32_t rights,
                                uint8_t obj_type,
                                rtmux_cap_t *out)
{
    uint32_t i = cap_slot_take(table, 1);
    uint64_t group;

    if (!i || cap_next_group(table, &group) != EOK) {
        cap_last_denied(table, ENOSPC);
        return ENOSPC;
    }

    *out = cap_slot_fill(&table->slots[i], i, group, object, context, rights,
                         obj_type);
    table->alloc_count++;
    return EOK;
}

int rtmux_cap_alloc(rtmux_cap_table_t *table,
                    void *object,
                    uint32_t rights,
                    rtmux_cap_t *out)
{
    return rtmux_cap_alloc_context(table, object, 0, rights, out);
}

int rtmux_cap_alloc_context(rtmux_cap_table_t *table,
                            void *object,
                            uint64_t context,
                            uint32_t rights,
                            rtmux_cap_t *out)
{
    int ret;

    if (!table || !table->slots || !out || !object || rights == 0)
        return EINVAL;

    cap_lock(&table->lock);
    ret = cap_alloc_new_locked(table, object, context, rights,
                               RTMUX_CAP_TYPE_ENDPOINT, out);
    cap_unlock(&table->lock);
    return ret;
}

int rtmux_cap_alloc_typed(rtmux_cap_table_t *table,
                          void *object,
                          uint64_t context,
                          uint32_t rights,
                          uint8_t obj_type,
                          rtmux_cap_t *out)
{
    int ret;

    if (!table || !table->slots || !out || !object || rights == 0)
        return EINVAL;
    if (obj_type > RTMUX_CAP_TYPE_THREAD)
        return EINVAL;

    cap_lock(&table->lock);
    ret = cap_alloc_new_locked(table, object, context, rights, obj_type, out);
    cap_unlock(&table->lock);
    return ret;
}

int rtmux_cap_get_or_alloc_context(rtmux_cap_table_t *table,
                                   void *object,
                                   uint64_t context,
                                   uint32_t rights,
                                   rtmux_cap_t *out)
{
    uint32_t i;
    int ret;

    if (!table || !table->slots || !out || !object || rights == 0)
        return EINVAL;

    cap_lock(&table->lock);
    for (i = 1; i < table->nslots; i++) {
        rtmux_cap_slot_t *slot = &table->slots[i];

        if (slot->state == RTMUX_CAP_LIVE && slot->object == object &&
            slot->context == context && slot->rights == rights &&
            slot->obj_type == RTMUX_CAP_TYPE_ENDPOINT) {
            *out = rtmux_cap_make(slot->generation, i);
            cap_unlock(&table->lock);
            return EOK;
        }
    }
    ret = cap_alloc_new_locked(table, object, context, rights,
                               RTMUX_CAP_TYPE_ENDPOINT, out);
    cap_unlock(&table->lock);
    return ret;
}

/* What a successful lookup guarantees, in the order the body establishes it.
 * The separation requirement is not decorative: it is what lets WP carry the
 * pre-state slot facts across cap_lock's write to table->lock. Every kernel
 * caller satisfies it, because process_entry holds the table and the slot array
 * as sibling members with disjoint byte ranges.
 */
/*@ requires table == \null || \valid(table);
    requires table == \null || table->slots == \null ||
             \valid(table->slots + (0 .. table->nslots - 1));
    requires table == \null || table->slots == \null ||
             \separated(table, table->slots + (0 .. table->nslots - 1));
    requires object == \null || \valid(object);
    requires context == \null || \valid(context);

    ensures \result == EOK ==> (cap & RTMUX_CAP_SLOT_MASK) != 0;
    ensures \result == EOK ==>
            (cap & RTMUX_CAP_SLOT_MASK) < \at(table->nslots, Pre);
    ensures \result == EOK ==>
            \at(table->slots[cap & RTMUX_CAP_SLOT_MASK].generation, Pre) ==
            (uint32_t) (cap >> RTMUX_CAP_GEN_SHIFT);
    ensures \result == EOK ==>
            \at(table->slots[cap & RTMUX_CAP_SLOT_MASK].state, Pre) ==
            RTMUX_CAP_LIVE;
    ensures \result == EOK ==>
            \at(table->slots[cap & RTMUX_CAP_SLOT_MASK].obj_type, Pre) ==
            obj_type;
    ensures \result == EOK ==>
            (rights &
             (uint32_t) ~\at(table->slots[cap & RTMUX_CAP_SLOT_MASK].rights,
                             Pre)) == 0;
    ensures \result == EOK ==>
            *object == \at(table->slots[cap & RTMUX_CAP_SLOT_MASK].object, Pre);
    ensures \result != EOK && object != \null ==> *object == \old(*object);

    assigns *object \from table->slots[cap & RTMUX_CAP_SLOT_MASK].object;
    assigns *context \from table->slots[cap & RTMUX_CAP_SLOT_MASK].context;
    assigns table->lock, table->last_denied, table->lookup_denied_count,
            table->slots[cap & RTMUX_CAP_SLOT_MASK].pin_count;
 */
static int cap_lookup_pin_typed(rtmux_cap_table_t *table,
                                rtmux_cap_t cap,
                                uint32_t rights,
                                uint8_t obj_type,
                                void **object,
                                uint64_t *context)
{
    uint32_t index = rtmux_cap_slot(cap);
    rtmux_cap_slot_t *slot;
    int error;

    if (!table || !table->slots || !object)
        return ESTALE;
    if (index == 0 || index >= table->nslots || rtmux_cap_generation(cap) == 0)
        return ESTALE;

    slot = &table->slots[index];
    cap_lock(&table->lock);

    /* A lookup with no rights requested passes the (rights & ~slot->rights)
     * mask trivially and would hand back the object unguarded. Reject it. The
     * object class must match too, before the rights check: a cap of a
     * different class over the same object pointer is not this object.
     */
    if (rights == 0) {
        error = EINVAL;
    } else if (slot->state != RTMUX_CAP_LIVE ||
               slot->generation != rtmux_cap_generation(cap) ||
               slot->obj_type != obj_type || (rights & ~slot->rights) != 0) {
        error = ESTALE;
    } else if (slot->pin_count == UINT32_MAX) {
        /* Live and valid, but the pin count is saturated — the handle isn't
         * stale, the caller just can't take another reference right now.
         */
        error = EOVERFLOW;
    } else {
        slot->pin_count++;
        *object = slot->object;
        if (context)
            *context = slot->context;
        cap_unlock(&table->lock);
        return EOK;
    }
    table->lookup_denied_count++;
    cap_last_denied(table, error);
    cap_unlock(&table->lock);
    return error;
}

/* One layer up from cap_lookup_pin_typed, and the layer is the whole point:
 * these carry the same table and slot-array preconditions because a contract
 * creates an obligation at every call site, so a wrapper cannot stay silent
 * once its callee speaks. The postconditions are deliberately weaker than the
 * callee's: a caller of the public API needs to know that success resolved a
 * live, in-range slot of the class it asked for, not how the slot was found.
 */
/*@ requires table == \null || \valid(table);
    requires table == \null || table->slots == \null ||
             \valid(table->slots + (0 .. table->nslots - 1));
    requires table == \null || table->slots == \null ||
             \separated(table, table->slots + (0 .. table->nslots - 1));
    requires object == \null || \valid(object);

    ensures \result == EOK ==>
            (cap & RTMUX_CAP_SLOT_MASK) != 0 &&
            (cap & RTMUX_CAP_SLOT_MASK) < \at(table->nslots, Pre);
    ensures \result == EOK ==>
            \at(table->slots[cap & RTMUX_CAP_SLOT_MASK].state, Pre) ==
            RTMUX_CAP_LIVE;
    ensures \result == EOK ==>
            \at(table->slots[cap & RTMUX_CAP_SLOT_MASK].obj_type, Pre) ==
            RTMUX_CAP_TYPE_ENDPOINT;
    ensures \result != EOK && object != \null ==> *object == \old(*object);

    assigns *object \from table->slots[cap & RTMUX_CAP_SLOT_MASK].object;
    assigns table->lock, table->last_denied, table->lookup_denied_count,
            table->slots[cap & RTMUX_CAP_SLOT_MASK].pin_count;
 */
int rtmux_cap_lookup_pin(rtmux_cap_table_t *table,
                         rtmux_cap_t cap,
                         uint32_t rights,
                         void **object)
{
    return cap_lookup_pin_typed(table, cap, rights, RTMUX_CAP_TYPE_ENDPOINT,
                                object, NULL);
}

/* One layer up from cap_lookup_pin_typed, and the layer is the whole point:
 * these carry the same table and slot-array preconditions because a contract
 * creates an obligation at every call site, so a wrapper cannot stay silent
 * once its callee speaks. The postconditions are deliberately weaker than the
 * callee's: a caller of the public API needs to know that success resolved a
 * live, in-range slot of the class it asked for, not how the slot was found.
 */
/*@ requires table == \null || \valid(table);
    requires table == \null || table->slots == \null ||
             \valid(table->slots + (0 .. table->nslots - 1));
    requires table == \null || table->slots == \null ||
             \separated(table, table->slots + (0 .. table->nslots - 1));
    requires object == \null || \valid(object);
    requires context == \null || \valid(context);
    ensures \result == EOK ==>
            (cap & RTMUX_CAP_SLOT_MASK) != 0 &&
            (cap & RTMUX_CAP_SLOT_MASK) < \at(table->nslots, Pre);
    ensures \result == EOK ==>
            \at(table->slots[cap & RTMUX_CAP_SLOT_MASK].state, Pre) ==
            RTMUX_CAP_LIVE;
    ensures \result == EOK ==>
            \at(table->slots[cap & RTMUX_CAP_SLOT_MASK].obj_type, Pre) ==
            RTMUX_CAP_TYPE_ENDPOINT;
    ensures \result != EOK && object != \null ==> *object == \old(*object);

    assigns *object \from table->slots[cap & RTMUX_CAP_SLOT_MASK].object;
    assigns *context \from table->slots[cap & RTMUX_CAP_SLOT_MASK].context;
    assigns table->lock, table->last_denied, table->lookup_denied_count,
            table->slots[cap & RTMUX_CAP_SLOT_MASK].pin_count;
 */
int rtmux_cap_lookup_pin_context(rtmux_cap_table_t *table,
                                 rtmux_cap_t cap,
                                 uint32_t rights,
                                 void **object,
                                 uint64_t *context)
{
    return cap_lookup_pin_typed(table, cap, rights, RTMUX_CAP_TYPE_ENDPOINT,
                                object, context);
}

/* The typed entry point, so the class comes from the caller rather than being
 * fixed at ENDPOINT. The first postcondition is the one that matters: an
 * out-of-range class is rejected outright, before any table access.
 */
/*@ requires table == \null || \valid(table);
    requires table == \null || table->slots == \null ||
             \valid(table->slots + (0 .. table->nslots - 1));
    requires table == \null || table->slots == \null ||
             \separated(table, table->slots + (0 .. table->nslots - 1));
    requires object == \null || \valid(object);
    requires context == \null || \valid(context);

    ensures \old(obj_type) > RTMUX_CAP_TYPE_THREAD ==> \result == EINVAL;
    ensures \result == EOK ==>
            (cap & RTMUX_CAP_SLOT_MASK) != 0 &&
            (cap & RTMUX_CAP_SLOT_MASK) < \at(table->nslots, Pre);
    ensures \result == EOK ==>
            \at(table->slots[cap & RTMUX_CAP_SLOT_MASK].state, Pre) ==
            RTMUX_CAP_LIVE;
    ensures \result == EOK ==>
            \at(table->slots[cap & RTMUX_CAP_SLOT_MASK].obj_type, Pre) ==
            \old(obj_type);
    ensures \result != EOK && object != \null ==> *object == \old(*object);

    assigns *object \from table->slots[cap & RTMUX_CAP_SLOT_MASK].object;
    assigns *context \from table->slots[cap & RTMUX_CAP_SLOT_MASK].context;
    assigns table->lock, table->last_denied, table->lookup_denied_count,
            table->slots[cap & RTMUX_CAP_SLOT_MASK].pin_count;
 */
int rtmux_cap_lookup_pin_typed(rtmux_cap_table_t *table,
                               rtmux_cap_t cap,
                               uint32_t rights,
                               uint8_t obj_type,
                               void **object,
                               uint64_t *context)
{
    if (obj_type > RTMUX_CAP_TYPE_THREAD)
        return EINVAL;
    return cap_lookup_pin_typed(table, cap, rights, obj_type, object, context);
}

void *rtmux_cap_unpin(rtmux_cap_table_t *table, rtmux_cap_t cap)
{
    uint32_t index = rtmux_cap_slot(cap);
    rtmux_cap_slot_t *slot;
    void *dead = NULL;

    if (!table || !table->slots || index == 0 || index >= table->nslots)
        return NULL;

    slot = &table->slots[index];
    cap_lock(&table->lock);
    if (slot->generation == rtmux_cap_generation(cap) && slot->pin_count > 0) {
        void *object = slot->object;
        uint64_t group = slot->group;

        /* Only the pin that drops this slot to zero can finish the revocation:
         * while it still holds pins, every other slot in the group is already
         * DEAD or REVOKING, so the group scan has nothing to do.
         */
        if (--slot->pin_count == 0 && object &&
            slot->state == RTMUX_CAP_REVOKING && cap_group_retire(table, group))
            dead = object;
    }
    cap_unlock(&table->lock);
    return dead;
}

int rtmux_cap_revoke(rtmux_cap_table_t *table, rtmux_cap_t cap, void **dead)
{
    uint32_t index = rtmux_cap_slot(cap);
    rtmux_cap_slot_t *slot;
    void *object;

    /* The object is handed back exactly once for destruction: here if no pins
     * are outstanding, otherwise from the final rtmux_cap_unpin(). Either way
     * dead is non-NULL exactly once across the cap's revoke + unpins.
     */
    if (dead)
        *dead = NULL;
    if (!table || !table->slots || index == 0 || index >= table->nslots ||
        rtmux_cap_generation(cap) == 0)
        return ESTALE;

    slot = &table->slots[index];
    cap_lock(&table->lock);
    if (slot->state != RTMUX_CAP_LIVE ||
        slot->generation != rtmux_cap_generation(cap)) {
        table->revoke_stale_count++;
        cap_last_denied(table, ESTALE);
        cap_unlock(&table->lock);
        return ESTALE;
    }

    /* Block new lookups immediately, but keep object/rights live on any pinned
     * slot until its last pin drops (see rtmux_cap_unpin) so pinned holders
     * never touch a NULL'd or freed object.
     */
    object = slot->object;
    if (cap_group_retire(table, slot->group) && dead)
        *dead = object;
    table->revoke_count++;
    cap_unlock(&table->lock);
    return EOK;
}

int rtmux_cap_abort(rtmux_cap_table_t *table, rtmux_cap_t cap)
{
    uint32_t index = rtmux_cap_slot(cap);
    rtmux_cap_slot_t *slot;

    if (!table || !table->slots || index == 0 || index >= table->nslots ||
        rtmux_cap_generation(cap) == 0)
        return ESTALE;

    cap_lock(&table->lock);
    slot = &table->slots[index];
    if (slot->state != RTMUX_CAP_LIVE ||
        slot->generation != rtmux_cap_generation(cap)) {
        table->revoke_stale_count++;
        cap_last_denied(table, ESTALE);
        cap_unlock(&table->lock);
        return ESTALE;
    }

    if (slot->pin_count != 0) {
        cap_last_denied(table, EBUSY);
        cap_unlock(&table->lock);
        return EBUSY;
    }
    cap_slot_kill(slot);
    table->revoke_count++;
    cap_unlock(&table->lock);
    return EOK;
}

int rtmux_cap_delegate(rtmux_cap_table_t *table,
                       rtmux_cap_t cap,
                       uint32_t rights,
                       rtmux_cap_t *out)
{
    uint32_t index = rtmux_cap_slot(cap);
    rtmux_cap_slot_t *slot;
    uint32_t i;

    if (!table || !table->slots || !out || rights == 0)
        return EINVAL;
    if (index == 0 || index >= table->nslots || rtmux_cap_generation(cap) == 0)
        return ESTALE;

    cap_lock(&table->lock);
    slot = &table->slots[index];
    if (slot->state != RTMUX_CAP_LIVE ||
        slot->generation != rtmux_cap_generation(cap) ||
        (rights & ~slot->rights) != 0) {
        cap_last_denied(table, ESTALE);
        cap_unlock(&table->lock);
        return ESTALE;
    }

    i = cap_slot_take(table, 1);
    if (!i) {
        cap_last_denied(table, ENOSPC);
        cap_unlock(&table->lock);
        return ENOSPC;
    }

    /* The delegated handle joins its root's revocation group, so revoking the
     * root kills the attenuated copy with it.
     */
    *out = cap_slot_fill(&table->slots[i], i, slot->group, slot->object,
                         slot->context, rights, slot->obj_type);
    table->delegate_count++;
    cap_unlock(&table->lock);
    return EOK;
}

int rtmux_cap_transfer(rtmux_cap_table_t *table,
                       const rtmux_cap_transfer_t *xfers,
                       rtmux_cap_t *out,
                       size_t nxfers)
{
    return rtmux_cap_transfer_to(table, table, xfers, out, nxfers);
}

int rtmux_cap_transfer_to(rtmux_cap_table_t *src_table,
                          rtmux_cap_table_t *dst_table,
                          const rtmux_cap_transfer_t *xfers,
                          rtmux_cap_t *out,
                          size_t nxfers)
{
    uint64_t groups[RTMUX_CAP_TRANSFER_MAX];
    uint32_t dst_index[RTMUX_CAP_TRANSFER_MAX];
    size_t i;
    uint32_t j;
    uint32_t next_free;
    uint64_t saved_next_group;
    bool saved_group_wrapped;

    if (nxfers == 0)
        return EOK;
    if (!src_table || !src_table->slots || !dst_table || !dst_table->slots ||
        !xfers || !out)
        return EINVAL;
    if (nxfers > RTMUX_CAP_TRANSFER_MAX)
        return E2BIG;

    cap_lock_pair(src_table, dst_table);
    saved_next_group = dst_table->next_group;
    saved_group_wrapped = dst_table->group_wrapped;
    for (i = 0; i < nxfers; i++) {
        uint32_t index = rtmux_cap_slot(xfers[i].cap);
        rtmux_cap_slot_t *slot;

        if (xfers[i].flags != 0 || xfers[i].rights == 0) {
            cap_last_denied(src_table, EINVAL);
            cap_unlock_pair(src_table, dst_table);
            return EINVAL;
        }
        if (index == 0 || index >= src_table->nslots ||
            rtmux_cap_generation(xfers[i].cap) == 0) {
            cap_last_denied(src_table, ESTALE);
            cap_unlock_pair(src_table, dst_table);
            return ESTALE;
        }
        slot = &src_table->slots[index];
        if (slot->state != RTMUX_CAP_LIVE ||
            slot->generation != rtmux_cap_generation(xfers[i].cap) ||
            (xfers[i].rights & ~slot->rights) != 0) {
            cap_last_denied(src_table, ESTALE);
            cap_unlock_pair(src_table, dst_table);
            return ESTALE;
        }
    }

    /* Reserve a destination slot and mint a group for every transfer before
     * mutating anything. Both failures live here, so the commit loop below
     * cannot fail and a rejected transfer leaves the destination table and
     * out[] untouched. One walk of the destination table serves all transfers:
     * next_free only ever moves forward, and nothing frees slots while both
     * locks are held.
     *
     * Same-table delegation shares the root's revocation group so an attenuated
     * handle dies with its root. A cross-table transfer must mint a fresh
     * group: group ids are per-table and sequential, so copying the source id
     * collides with an unrelated dst group and lets a dst group-revoke wrongly
     * kill the transferred cap.
     */
    next_free = 1;
    for (i = 0; i < nxfers; i++) {
        dst_index[i] = cap_slot_find(dst_table, next_free);
        if (!dst_index[i]) {
            cap_last_denied(dst_table, ENOSPC);
            dst_table->next_group = saved_next_group;
            dst_table->group_wrapped = saved_group_wrapped;
            cap_unlock_pair(src_table, dst_table);
            return ENOSPC;
        }
        next_free = dst_index[i] + 1;

        if (dst_table == src_table) {
            groups[i] = src_table->slots[rtmux_cap_slot(xfers[i].cap)].group;
        } else if (cap_next_group(dst_table, &groups[i]) != EOK) {
            cap_last_denied(dst_table, ENOSPC);
            dst_table->next_group = saved_next_group;
            dst_table->group_wrapped = saved_group_wrapped;
            cap_unlock_pair(src_table, dst_table);
            return ENOSPC;
        }
    }

    /* The scan above was deliberately read-only. Retire only the saturated
     * slots it passed once every reservation has succeeded.
     */
    for (j = 1; j < next_free; j++) {
        rtmux_cap_slot_t *slot = &dst_table->slots[j];

        if (!slot->retired &&
            (slot->state == RTMUX_CAP_FREE || slot->state == RTMUX_CAP_DEAD) &&
            slot->generation == RTMUX_CAP_GEN_MAX)
            slot->retired = true;
    }
    for (i = 0; i < nxfers; i++) {
        rtmux_cap_slot_t *src = &src_table->slots[rtmux_cap_slot(xfers[i].cap)];

        out[i] = cap_slot_fill(&dst_table->slots[dst_index[i]], dst_index[i],
                               groups[i], src->object, src->context,
                               xfers[i].rights, src->obj_type);
    }
    src_table->transfer_count += nxfers;
    if (dst_table != src_table)
        dst_table->transfer_count += nxfers;
    cap_unlock_pair(src_table, dst_table);
    return EOK;
}

static bool cap_has_object_scan(rtmux_cap_table_t *table,
                                const void *object,
                                uint32_t rights,
                                bool typed,
                                uint8_t obj_type)
{
    uint32_t i;
    bool found = false;

    if (!table || !table->slots || !object || rights == 0)
        return false;

    cap_lock(&table->lock);
    for (i = 1; i < table->nslots; i++) {
        rtmux_cap_slot_t *slot = &table->slots[i];

        if (slot->state == RTMUX_CAP_LIVE && slot->object == object &&
            (!typed || slot->obj_type == obj_type) &&
            (rights & ~slot->rights) == 0) {
            found = true;
            break;
        }
    }
    cap_unlock(&table->lock);
    return found;
}

bool rtmux_cap_has_object(rtmux_cap_table_t *table,
                          const void *object,
                          uint32_t rights)
{
    return cap_has_object_scan(table, object, rights, false, 0);
}

bool rtmux_cap_has_object_typed(rtmux_cap_table_t *table,
                                const void *object,
                                uint32_t rights,
                                uint8_t obj_type)
{
    return cap_has_object_scan(table, object, rights, true, obj_type);
}

int rtmux_cap_revoke_object(rtmux_cap_table_t *table, const void *object)
{
    return rtmux_cap_revoke_object_context(table, object, UINT64_MAX);
}

int rtmux_cap_revoke_object_context(rtmux_cap_table_t *table,
                                    const void *object,
                                    uint64_t context)
{
    uint32_t i;
    uint32_t revoked = 0;

    if (!table || !table->slots || !object)
        return EINVAL;

    cap_lock(&table->lock);
    for (i = 1; i < table->nslots; i++) {
        rtmux_cap_slot_t *slot = &table->slots[i];

        if (slot->object == object && slot->state == RTMUX_CAP_LIVE &&
            (context == UINT64_MAX || slot->context == context)) {
            /* No object handback on this path: the caller named the object, so
             * it already owns it.
             */
            (void) cap_group_retire(table, slot->group);
            revoked++;
        }
    }
    table->revoke_count += revoked;
    cap_unlock(&table->lock);
    return EOK;
}

int rtmux_cap_stats(rtmux_cap_table_t *table, rtmux_cap_stats_t *out)
{
    uint32_t i;

    if (!table || !table->slots || !out)
        return EINVAL;

    cap_lock(&table->lock);
    out->alloc_count = table->alloc_count;
    out->delegate_count = table->delegate_count;
    out->transfer_count = table->transfer_count;
    out->revoke_count = table->revoke_count;
    out->revoke_stale_count = table->revoke_stale_count;
    out->lookup_denied_count = table->lookup_denied_count;
    out->last_denied = table->last_denied;
    out->retired_slots = 0;
    for (i = 1; i < table->nslots; i++) {
        if (table->slots[i].retired)
            out->retired_slots++;
    }
    cap_unlock(&table->lock);
    return EOK;
}
