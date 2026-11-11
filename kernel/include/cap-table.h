/*
 * RTMux capability handle table.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef _RTMUX_CAP_TABLE_H
#define _RTMUX_CAP_TABLE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>

#ifndef EOK
#define EOK 0
#endif

#define RTMUX_CAP_GEN_SHIFT 32
#define RTMUX_CAP_SLOT_MASK 0xffffffffULL
#define RTMUX_CAP_GEN_MAX UINT32_MAX
#define RTMUX_CAP_TRANSFER_MAX 4U

typedef uint64_t rtmux_cap_t;

enum rtmux_cap_state {
    RTMUX_CAP_FREE = 0,
    RTMUX_CAP_LIVE,
    RTMUX_CAP_REVOKING,
    RTMUX_CAP_DEAD,
};

/* Object class a capability names. ENDPOINT is 0 so a zero-initialized or
 * recycled slot keeps today's endpoint meaning, and every current caller stays
 * ENDPOINT. PROCESS and THREAD exist for the typed per-object direction; they
 * become live only once a table holds more than one class, at which point a
 * type-discriminating lookup keeps a process cap from being resolved through
 * the endpoint path when both name the same object pointer.
 */
enum rtmux_cap_obj_type {
    RTMUX_CAP_TYPE_ENDPOINT = 0,
    RTMUX_CAP_TYPE_PROCESS,
    RTMUX_CAP_TYPE_THREAD,
};

typedef struct rtmux_cap_slot {
    uint64_t group; /* revocation group: all slots sharing it die together */
    uint32_t generation;
    uint32_t pin_count;
    uint32_t rights;
    uint8_t state;
    bool retired;
    uint8_t obj_type; /* enum rtmux_cap_obj_type */
    uint8_t pad[1];
    void *object;
    uint64_t context;
} rtmux_cap_slot_t;

/* The type tag reuses padding: the slot must not grow (LP64). */
_Static_assert(sizeof(rtmux_cap_slot_t) == 40, "cap slot must not grow");

typedef struct rtmux_cap_table {
    rtmux_cap_slot_t *slots;
    uint32_t nslots;
    bool group_wrapped; /* next_group has wrapped: ids may alias a live group */
    uint64_t next_group; /* monotonic group-id source; 0 means unassigned */
    /* Per-table test-and-set spinlock, deliberately outside the kspinlock_t
     * LOCK framework so this table stays self-contained and embeddable. It is
     * therefore NOT covered by CONFIG_LOCK_DEBUG cycle detection. Invariant
     * that keeps it deadlock-free: a cap_lock() is always a LEAF acquire — no
     * kspinlock_t or other cap_lock may be taken while one is held (transfer's
     * cap_lock_pair takes two cap_locks, but always in pointer order, which is
     * the only nesting permitted). Callers take their kspinlock_t (e.g.
     * process_vector_slock) FIRST, then the cap_lock. Cap ops run in thread
     * context only; cap_lock does not mask interrupts.
     */
    atomic_uint lock;
    uint64_t alloc_count;
    uint64_t delegate_count;
    uint64_t transfer_count;
    uint64_t revoke_count;
    uint64_t revoke_stale_count;
    uint64_t lookup_denied_count;
    int last_denied;
} rtmux_cap_table_t;

typedef struct rtmux_cap_transfer {
    rtmux_cap_t cap;
    uint32_t rights;
    uint32_t flags;
} rtmux_cap_transfer_t;

typedef struct rtmux_cap_stats {
    uint64_t alloc_count;
    uint64_t delegate_count;
    uint64_t transfer_count;
    uint64_t revoke_count;
    uint64_t revoke_stale_count;
    uint64_t lookup_denied_count;
    uint64_t retired_slots;
    int last_denied;
} rtmux_cap_stats_t;

/* Deliberately uncontracted, unlike the two decoders below. Nothing in the
 * proof set calls it, and an ACSL contract on a function that is not proved is
 * an axiom every goal above it would rest on. A shift-or postcondition here
 * also does not discharge: the provers handle the arithmetic form and not the
 * bitwise one. Contract it when a proof needs it, and prove it in the same
 * change.
 */
static inline rtmux_cap_t rtmux_cap_make(uint32_t generation, uint32_t slot)
{
    return ((uint64_t) generation << RTMUX_CAP_GEN_SHIFT) | slot;
}

/* The two decoders carry ACSL contracts for Frama-C/WP. Without them WP treats
 * each call as assigning \everything and loses the caller's own preconditions
 * across it, so the contracts in kernel/cap/table.c cannot be discharged. See
 * formal/framac/.
 */
/*@ assigns \nothing;
    ensures \result == (uint32_t) (cap & RTMUX_CAP_SLOT_MASK);
 */
static inline uint32_t rtmux_cap_slot(rtmux_cap_t cap)
{
    return (uint32_t) (cap & RTMUX_CAP_SLOT_MASK);
}

/*@ assigns \nothing;
    ensures \result == (uint32_t) (cap >> RTMUX_CAP_GEN_SHIFT);
 */
static inline uint32_t rtmux_cap_generation(rtmux_cap_t cap)
{
    return (uint32_t) (cap >> RTMUX_CAP_GEN_SHIFT);
}

int rtmux_cap_table_init(rtmux_cap_table_t *table,
                         rtmux_cap_slot_t *slots,
                         size_t nslots);
int rtmux_cap_alloc(rtmux_cap_table_t *table,
                    void *object,
                    uint32_t rights,
                    rtmux_cap_t *out);
int rtmux_cap_alloc_context(rtmux_cap_table_t *table,
                            void *object,
                            uint64_t context,
                            uint32_t rights,
                            rtmux_cap_t *out);

/* Allocate a capability of an explicit object class. The plain alloc entry
 * points above are ENDPOINT; this one is how the typed per-object direction
 * mints a PROCESS or THREAD cap. rtmux_cap_lookup_pin(_context) accepts only
 * ENDPOINT, so a non-endpoint cap must be resolved through
 * rtmux_cap_lookup_pin_typed with the matching type.
 */
int rtmux_cap_alloc_typed(rtmux_cap_table_t *table,
                          void *object,
                          uint64_t context,
                          uint32_t rights,
                          uint8_t obj_type,
                          rtmux_cap_t *out);
int rtmux_cap_get_or_alloc_context(rtmux_cap_table_t *table,
                                   void *object,
                                   uint64_t context,
                                   uint32_t rights,
                                   rtmux_cap_t *out);
int rtmux_cap_lookup_pin(rtmux_cap_table_t *table,
                         rtmux_cap_t cap,
                         uint32_t rights,
                         void **object);
int rtmux_cap_lookup_pin_context(rtmux_cap_table_t *table,
                                 rtmux_cap_t cap,
                                 uint32_t rights,
                                 void **object,
                                 uint64_t *context);

/* Pin a capability only if its object class matches obj_type. The plain lookup
 * entry points above demand ENDPOINT; this one resolves a PROCESS or THREAD cap
 * and rejects a type mismatch with ESTALE before the rights check.
 */
int rtmux_cap_lookup_pin_typed(rtmux_cap_table_t *table,
                               rtmux_cap_t cap,
                               uint32_t rights,
                               uint8_t obj_type,
                               void **object,
                               uint64_t *context);

/* Drops one pin.
 *
 * Returns the underlying object iff this was the final pin on a revoked slot
 * (caller then owns its destruction); NULL otherwise.
 */
void *rtmux_cap_unpin(rtmux_cap_table_t *table, rtmux_cap_t cap);

/* Revokes the cap. Blocks new lookups immediately; if no pins are outstanding
 * the object is returned via *dead (may be NULL if the caller doesn't need it),
 * otherwise the final rtmux_cap_unpin() returns it. The object thus comes back
 * for destruction exactly once.
 */
int rtmux_cap_revoke(rtmux_cap_table_t *table, rtmux_cap_t cap, void **dead);
int rtmux_cap_abort(rtmux_cap_table_t *table, rtmux_cap_t cap);
int rtmux_cap_delegate(rtmux_cap_table_t *table,
                       rtmux_cap_t cap,
                       uint32_t rights,
                       rtmux_cap_t *out);
int rtmux_cap_transfer(rtmux_cap_table_t *table,
                       const rtmux_cap_transfer_t *xfers,
                       rtmux_cap_t *out,
                       size_t nxfers);
int rtmux_cap_transfer_to(rtmux_cap_table_t *src_table,
                          rtmux_cap_table_t *dst_table,
                          const rtmux_cap_transfer_t *xfers,
                          rtmux_cap_t *out,
                          size_t nxfers);

/* Bounded scan for a live capability naming "object" that carries at least the
 * requested rights. Unlike rtmux_cap_lookup_pin this matches on the object
 * pointer, not the (table-local) cap id, so it works across process tables: an
 * IPC-transferred cap names the same object under a new receiver-local id. Does
 * not pin. Connect-time authority check only, off the message hot path.
 */
bool rtmux_cap_has_object(rtmux_cap_table_t *table,
                          const void *object,
                          uint32_t rights);

/* Same bounded scan, but also requires the cap's object class to be obj_type.
 * Use this when the object pointer alone is not a safe discriminator, i.e. a
 * security-sensitive gate that must not admit a cap of another class that
 * happens to name the same address.
 */
bool rtmux_cap_has_object_typed(rtmux_cap_table_t *table,
                                const void *object,
                                uint32_t rights,
                                uint8_t obj_type);
int rtmux_cap_revoke_object(rtmux_cap_table_t *table, const void *object);
int rtmux_cap_revoke_object_context(rtmux_cap_table_t *table,
                                    const void *object,
                                    uint64_t context);
int rtmux_cap_stats(rtmux_cap_table_t *table, rtmux_cap_stats_t *out);

#endif
