/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* Minor operations.
 *
 * Device numbers use 10 minor bits and 6 major bits, encoded by the internal
 * hwres_dev_* helpers.
 */
#include <sys/types.h>
#include <hwres.h>

static rsrc_devno_t g_devno_table[MAX_NUM_MAJORS];

/* Attach or detach a minor device number under a major device entry.
 *
 * On attach, locates (or creates) the named major entry and allocates either
 * the requested minor or, when *minor_request is negative, the first free
 * minor. On detach, clears the minor's bit and frees the major's name once no
 * minors remain. Parameters:
 *   name          : major device name (used on attach)
 *   major         : in/out — major index; set to the allocated index
 *                   on attach
 *   minor_request : in/out — requested minor (<0 to auto-allocate);
 *                   set to the chosen minor on attach
 *   flags         : HWRES_REQ_CLAIM or HWRES_REQ_RELEASE
 * Return value: EOK on success, or an errno (EINVAL, ENOMEM).
 */
int _rsrc_minor(const char *name, int *major, int *minor_request, int flags)
{
    rsrc_devno_t *head = NULL;
    uint16_t index;  /* At most MAX_NUM_MINORS (1024) / 8 */
    uint16_t eindex; /* Never more than MAX_NUM_MAJORS (64) */
    uint8_t bit;     /* At most 8 right now */
    uint8_t mask;    /* A mask for 8 bits */

    /*** Previously encapsulated in a different function ***/

    /* major, name are assumed to be valid if they are being searched for */
    for (index = 0, eindex = MAX_NUM_MAJORS; index < MAX_NUM_MAJORS; index++) {
        if (!g_devno_table[index].name) {
            if (index < eindex)
                eindex = index;
        } else if ((!(flags & HWRES_REQ_CLAIM) && index == *major) ||
                   ((flags & HWRES_REQ_CLAIM) &&
                    !strcmp(g_devno_table[index].name, name))) {
            eindex = index;
            head = &g_devno_table[index];
        }
    }

    if (!head) {
        /* No major device being created, or no space left */
        if (!(flags & HWRES_REQ_CLAIM) || eindex == MAX_NUM_MAJORS) {
            return EINVAL;
        }

        /* Pre-allocate with 8 entries if nothing there already */
        g_devno_table[eindex].name = strdup(name);
        if (!g_devno_table[eindex].minors) {
            g_devno_table[eindex].minors =
                _scalloc(sizeof(struct _devno_minors));
            g_devno_table[eindex].minors->count = 1;
        }

        head = &g_devno_table[eindex];
    }

    /****/

    if (flags & HWRES_REQ_CLAIM) {
        if (*minor_request < 0) { /* Locate a minor entry */
            for (bit = 0, index = 0; index < head->minors->count; index++) {
                if ((mask = head->minors->bits[index]) != 0xFF) {
                    for (bit = 0; mask & (0x1 << bit); bit++) {
                        ;
                    }
                    break;
                }
            }
            *minor_request = (index * 8) + bit;
        }
    }

    if (*minor_request >= MAX_NUM_MINORS) {
        return EINVAL;
    }

    index = (uint16_t) (*minor_request / 8);
    bit = (uint8_t) (*minor_request % 8);
    mask = 0x1 << bit;

    if (flags & HWRES_REQ_CLAIM) {
        if (index >= head->minors->count) {
            struct _devno_minors *tmp = head->minors;

            if (!(head->minors = _srealloc(
                      head->minors,
                      (head->minors->count - 1) + sizeof(*head->minors),
                      index + sizeof(*head->minors)))) {
                head->minors = tmp;
                return ENOMEM;
            }

            memset(&head->minors->bits[head->minors->count], 0,
                   (size_t) (index + 1 - head->minors->count));
            head->minors->count = (uint8_t) (index + 1);
        } else if (head->minors->bits[index] & mask) {
            return EINVAL;
        }

        head->minors->bits[index] |= mask;
        *major = eindex;
    } else {
        head->minors->bits[index] &= ~mask;

        for (index = 0; index < head->minors->count; index++) {
            if (head->minors->bits[index]) {
                return EOK;
            }
        }

        /* Only free the name, leave the bits allocated */
        free(head->name);
        head->name = NULL;
    }

    return EOK;
}
