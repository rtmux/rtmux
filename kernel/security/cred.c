/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <core/globals.h>

void rdecl cred_dirty(process_t *prp)
{
    if (++prp->seq == 0) {
        vector_t *vec;
        int i;

        /* Don't allow the prp sequence number to be zero.. */
        ++prp->seq;
        for (vec = &prp->fdcons;;) {
            for (i = 0; i < vec->nentries; ++i) {
                connect_t *cop;

                if ((cop = VECP2(cop, vec, i))) {
                    if (cop->type == TYPE_CONNECTION && cop->process == prp &&
                        (cop->flags & COF_NETCON) == 0) {
                        /* Seq connection to zero indicate DIRTY credentials */
                        cop->un.lcl.seq = 0;
                    }
                }
            }
            if (vec == &prp->chancons) {
                break;
            }
            vec = &prp->chancons;
        }
    }
}

int rdecl cred_set(credential_t **pcrp, struct _cred_info *cip)
{
    credential_t *crp = *pcrp;
    unsigned size;
    struct _cred_info info;

    size = 0;
    if (cip) {
        /* Check to see if the number of groups passed is more than we were
         * compiled for.
         */
        if (cip->ngroups > NUM_ELTS(cip->grouplist)) {
            /* Only remember the number of groups set a compile time. */
            info = *cip;
            info.ngroups = NUM_ELTS(cip->grouplist);
            cip = &info;
        }
        size = (unsigned) (offsetof(struct _cred_info, grouplist) +
                           (cip->ngroups * sizeof(cip->grouplist[0])));
    }

    if (crp) {
#ifndef NDEBUG
        if (crp->links == 0)
            crash();
#endif

        /* If there was no change ignore the request. */
        if (cip && memcmp(&crp->info, cip, size) == 0) {
            return EOK;
        }
        /* Decrement the link count and free if not used any more. */
        if (--crp->links == 0) {
            LINK1_REM(credential_list, crp, credential_t);
            object_free(0, &credential_souls, crp);
        }
    }
    /* No credential data means unlink the entry from the process. */
    if (!cip) {
        *pcrp = NULL;
        return EOK;
    }

    /* We need a new credential entry. Try and find an existing match and link
     * to it first.
     */
    for (crp = credential_list; crp; crp = crp->next) {
        if (crp->info.ruid == cip->ruid) {
            if (memcmp(&crp->info, cip, size) == 0) {
                ++crp->links;
                *pcrp = crp;

                return EOK;
            }
        }
    }

    /* No match so we need to create a new entry. */
    if (!(crp = object_alloc(0, &credential_souls))) {
        return EAGAIN;
    }

    crp->links = 1;
    memcpy(&crp->info, cip, size);
    crp->next = credential_list;
    credential_list = crp;
    *pcrp = crp;

    return EOK;
}
