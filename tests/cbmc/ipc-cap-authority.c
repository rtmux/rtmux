/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * CBMC harness for the section 13 refinement rung: the capability check/use
 * TOCTOU guard on the IPC fast path.
 *
 * inline_lookup_connect (kernel/ipc/fastmsg.c:349-358) resolves an endpoint
 * capability to a connect_t like this: pin the capability, which yields the
 * object the capability authorizes; re-read the fd/chancon vector slot the cap
 * names under vec_slock; and if that slot no longer holds the authorized object
 * (the slot was reused after a revoke), return NULL instead of the wrong
 * object:
 *
 *     if (rtmux_cap_lookup_pin_context(..., &object, ...) != EOK) return NULL;
 *     kspinlock_lock(&prp->vec_slock);
 *     cop = vector_lookup2(vec, cap_context);
 *     if (cop != object) cop = NULL;      // the TOCTOU recheck
 *     kspinlock_unlock(&prp->vec_slock);
 *
 * Security property: any connect_t the fast path acts on is exactly the object
 * the caller's capability authorized; a slot reused by a concurrent revoke can
 * never be used through a stale capability. The recheck is what enforces it.
 *
 * Clean build: cbmc tests/cbmc/ipc-cap-authority.c -> VERIFICATION SUCCESSFUL
 * Planted bug: cbmc -DPLANT_BUG ... -> VERIFICATION FAILED (-DPLANT_BUG drops
 * the cop != object recheck, modeling a check/use reorder; the fast path then
 * uses whatever the reused slot holds, an unauthorized object, which the
 * assertion catches.)
 */

int nondet_int(void);

#define AUTHORIZED 1 /* the object this capability authorizes */
#define REUSED 2     /* a different object the slot was reused for */
#define EMPTY (-1)   /* cleared slot / NULL */

int main(void)
{
    /* The vector slot may still hold the authorized object, or have been reused
     * for a different object, or been cleared by a concurrent revoke.
     */
    int slot = nondet_int();
    __CPROVER_assume(slot == AUTHORIZED || slot == REUSED || slot == EMPTY);

    int object = AUTHORIZED; /* rtmux_cap_lookup_pin_context -> authorized obj */
    int cop = slot;          /* vector_lookup2(vec, cap_context) */

#ifndef PLANT_BUG
    if (cop != object) /* the TOCTOU recheck: reject a reused/cleared slot */
        cop = EMPTY;
#endif

    /* If the fast path proceeds with cop (non-NULL), it must be exactly the
     * capability-authorized object.
     */
    if (cop != EMPTY)
        __CPROVER_assert(cop == AUTHORIZED,
                         "only the capability-authorized object is used");
    return 0;
}
