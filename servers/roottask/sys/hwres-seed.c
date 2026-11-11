/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <sys/rtmux-core.h>
#include <sys/syspage.h>
#include <hw/sysinfo.h>
#include <hwres.h>

/* Roottask-local implementation of __hwi_find_string(). The kernel startup
 * version uses lsp.strings.p which is startup-only. Here we go through the
 * syspage pointer directly.
 */
char *__hwi_find_string(unsigned off)
{
    /* Defensive: a desynced HWINFO/ASINFO walk can hand us a wild name offset.
     * Validate against the strings-section size so a malformed syspage cannot
     * fault roottask, and log the bad offset so the root cause (which tag
     * desynced) can be tracked down.
     */
    unsigned ssize = _syspage_ptr->strings.entry_size;
    if (off >= ssize) {
        root_log(
            "hwres: __hwi_find_string off=%#x out of range "
            "(strings size=%#x)\n",
            off, ssize);
        return "";
    }
    return SYSPAGE_ENTRY(strings)->data + off;
}

/* Seed one resource range into the database.
 *
 * Parameters:
 *   alloc : the resource range to add
 * Return value: 0 on success, -1 on failure.
 */
static int add_range(hwres_range_t *alloc)
{
    if (hwres_proc_interface(alloc, 1, HWRES_REQ_CREATE) != EOK) {
        return -1;
    }

    return 0;
}

/* Add the interrupts from the IRQ list */
static void add_irq(void)
{
    struct intrinfo_entry *intrinfo;
    int num;

    num = _syspage_ptr->intrinfo.entry_size / sizeof(*intrinfo);
    intrinfo = SYSPAGE_ENTRY(intrinfo);

    while (num-- > 0) {
        hwres_range_t alloc = {
            .start = intrinfo->vector_base,
            .end = intrinfo->vector_base + intrinfo->num_vectors - 1,
            .flags = HWRES_IRQ,
        };
        if (add_range(&alloc) == -1) {
            root_log("hwres: add_range failed (irq %d)\n", num);
        }
        intrinfo++;
    }
}

/* Return the type of this ASINFO offset item (based on as) */
static int get_as_type(int addrspace)
{
    struct asinfo_entry *asinfo;
    char *name;
    int type;

    type = -1;
    while (addrspace != HWI_NULL_OFF) {
        asinfo = SYSPAGE_ENTRY(asinfo);
        asinfo = (struct asinfo_entry *) ((char *) asinfo + addrspace);

        name = __hwi_find_string(asinfo->name);
        if (!strcmp(name, "io")) {
            type = HWRES_IO_PORT;
            break;
        } else if (!strcmp(name, "memory")) {
            type = HWRES_MEMORY;
            break;
        } else if (!strcmp(name, "pcimemory")) {
            type = HWRES_PCI_MEMORY;
            break;
        }
        addrspace = asinfo->owner;
    }
    return type;
}

/* Add the IO and MEMORY from the ASINFO section */
static void add_io_and_memory(void)
{
    struct asinfo_entry *asinfo;
    int num;

    num = _syspage_ptr->asinfo.entry_size / sizeof(*asinfo);
    asinfo = SYSPAGE_ENTRY(asinfo);

    while (num-- > 0) {
        char *name;
        name = __hwi_find_string(asinfo->name);

        /* Here we check to see if the item is IO or MEMORY memory. Note that we
         * need to check for io that lives in both worlds.
         */
        if (name && !strcmp(name, "io")) {
            hwres_range_t alloc = {
                .start = asinfo->start,
                .end = asinfo->end,
                .flags = HWRES_IO_PORT,
            };
            /* We only add io that is not memory io */
            switch (get_as_type(asinfo->owner)) {
            case HWRES_MEMORY:
                break;
            case HWRES_IO_PORT:
            default:
                if (add_range(&alloc) == -1) {
                    root_log("hwres: add_range failed (io %d)\n", num);
                }
                break;
            }
        } else if (name && !strcmp(name, "memory")) {
            hwres_range_t alloc = {
                .start = asinfo->start,
                .end = asinfo->end,
                .flags = HWRES_MEMORY,
            };
            if (add_range(&alloc) == -1) {
                root_log("hwres: add_range failed (mem %d)\n", num);
            }
        } else if (name && !strcmp(name, "pcimemory")) {
            hwres_range_t alloc = {
                .start = asinfo->start,
                .end = asinfo->end,
                .flags = HWRES_PCI_MEMORY,
            };
            if (add_range(&alloc) == -1) {
                root_log("hwres: add_range failed (pcimem %d)\n", num);
            }
        }
        asinfo++;
    }
}


/* Mark hardware-reserved ranges (RSVP) from the HWINFO list.
 *
 * Walks the syspage HWINFO tags and reserves each "location" and "irq" tag's
 * range so it cannot be handed out as a free resource. Parameters:
 */
static void reserve_ranges(void)
{
    hwi_tag *tag;
    void *next;
    char *name;
    int type;

    /* Walk the HWINFO list first */
    char *hwi_base = (char *) SYSPAGE_ENTRY(hwinfo);
    char *hwi_end = hwi_base + _syspage_ptr->hwinfo.entry_size;

    tag = (hwi_tag *) hwi_base;
    while ((char *) tag + sizeof(tag->prefix) <= hwi_end &&
           tag->prefix.size != 0) {
        next = (hwi_tag *) ((uint32_t *) tag + tag->prefix.size);

        /* Bound the stride to the HWINFO section. A tag whose size walks us out
         * of the section (or backwards) means the walk has desynced — log the
         * offset/size/name so the offending tag can be identified, and stop
         * rather than read wild memory.
         */
        if ((char *) next <= (char *) tag || (char *) next > hwi_end) {
            root_log(
                "hwres: HWINFO walk desync at off=%#lx "
                "(size=%u name=%#x) — stopping\n",
                (unsigned long) ((char *) tag - hwi_base), tag->prefix.size,
                tag->prefix.name);
            break;
        }
        name = __hwi_find_string(tag->prefix.name);

        /* Really I need to look at the AS of this entry to determine if this is
         * io memory or regular memory.
         */
        if (name && !strcmp(name, HWI_TAG_NAME_location)) {
            /* location.addrspace is the HWI_ADDRSPACE_* KIND (0=io, 1=memory)
             * set by hwitag_add_location() — NOT an asinfo byte offset. Map it
             * straight to the hwres type. Do NOT pass it to get_as_type(): that
             * walks the asinfo section by byte offset, so a kind of 1 would
             * deref a misaligned phantom entry at asinfo_base+1 (the FU740
             * "__hwi_find_string out of range" desync).
             */
            switch (tag->location.addrspace) {
            case HWI_ADDRSPACE_IO:
                type = HWRES_IO_PORT;
                break;
            case HWI_ADDRSPACE_MEMORY:
                type = HWRES_MEMORY;
                break;
            default:
                tag = next;
                continue;
            }
            hwres_range_t alloc = {
                .start = tag->location.base,
                .end = tag->location.base + tag->location.len - 1,
                .flags = (unsigned) type | HWRES_FLAG_RSVP,
            };
            if (add_range(&alloc) == -1) {
                root_log("hwres: add_range failed (rsvp %x)\n", alloc.flags);
            }
        } else if (name && !strcmp(name, HWI_TAG_NAME_irq)) {
            hwres_range_t alloc = {
                .start = tag->irq.vector,
                .end = tag->irq.vector,
                .flags = HWRES_IRQ | HWRES_FLAG_RSVP,
            };
            if (add_range(&alloc) == -1) {
                root_log("hwres: add_range failed (rsvp irq %x)\n",
                         alloc.flags);
            }
        }
        tag = next;
    }
}
void hwres_seed()
{
    /* *** Mark these areas as reserved */
    reserve_ranges();

    /* *** Make the rest of these items as being free IRQ -- Taken from the
     * intrinfo section (vector_base, num_vectors)
     */
    add_irq();

    /* IO_PORTS & MEMORY -- Taken from the ASINFO section */
    add_io_and_memory();

    /* DMA -- Not yet available on the system page */
}
