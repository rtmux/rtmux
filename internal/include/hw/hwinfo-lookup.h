/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * hwinfo-lookup.h — user-space hwinfo query helpers.
 *
 * Drivers include this header to discover devices from the syspage hwinfo
 * section. The functions walk the hwinfo tag array linearly.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef _HW_HWINFO_LOOKUP_H
#define _HW_HWINFO_LOOKUP_H

#include <hw/sysinfo.h>
#include <sys/syspage.h>
#include <string.h>

/* hwinfo_base — return pointer to the start of the hwinfo section. */
static inline void *hwinfo_base(void)
{
    return (uint8_t *) _syspage_ptr + _syspage_ptr->hwinfo.entry_off;
}

static inline char *hwinfo_strings(void)
{
    return (char *) _syspage_ptr + _syspage_ptr->strings.entry_off;
}

/* hwinfo_next — advance to the next tag (prefix.size is in uint32_t units). */
static inline hwi_tag *hwinfo_next(hwi_tag *tag)
{
    return (hwi_tag *) ((uint32_t *) tag + tag->prefix.size);
}

/* hwinfo_find_device — find a device by name in the hwinfo section.
 *
 * Pass NULL for prev to start from the beginning. Pass a previously returned
 * tag to continue searching (find next device with same name).
 *
 * Usage:
 *     hwi_tag *dev = hwinfo_find_device("ns16550", NULL);
 *     // iterate: dev = hwinfo_find_device("virtio", dev);
 */
static inline hwi_tag *hwinfo_find_device(const char *name, hwi_tag *prev)
{
    uint8_t *base = hwinfo_base();
    char *strings = hwinfo_strings();
    unsigned section_size = _syspage_ptr->hwinfo.entry_size;
    hwi_tag *tag = prev ? hwinfo_next(prev) : (hwi_tag *) base;

    while ((uintptr_t) tag < (uintptr_t) base + section_size) {
        if (tag->prefix.size == 0)
            break;

        const char *tagname = strings + tag->prefix.name;

        if (!strcmp(tagname, HWI_TAG_NAME_device)) {
            const char *devname = strings + tag->item.itemname;
            if (!strcmp(devname, name))
                return tag;
        }

        tag = hwinfo_next(tag);
    }
    return NULL;
}

/* hwinfo_find_bus — find a bus by name in the hwinfo section.
 *
 * Mirrors hwinfo_find_device but matches "Bus" items instead of "Device". Pass
 * NULL for prev to start from the beginning.
 */
static inline hwi_tag *hwinfo_find_bus(const char *name, hwi_tag *prev)
{
    uint8_t *base = hwinfo_base();
    char *strings = hwinfo_strings();
    unsigned section_size = _syspage_ptr->hwinfo.entry_size;
    hwi_tag *tag = prev ? hwinfo_next(prev) : (hwi_tag *) base;

    while ((uintptr_t) tag < (uintptr_t) base + section_size) {
        if (tag->prefix.size == 0)
            break;

        const char *tagname = strings + tag->prefix.name;

        if (!strcmp(tagname, HWI_TAG_NAME_bus)) {
            const char *busname = strings + tag->item.itemname;
            if (!strcmp(busname, name))
                return tag;
        }

        tag = hwinfo_next(tag);
    }
    return NULL;
}

/* hwinfo_find_tag_after — find the first tag of a given type after a device
 * item. Searches forward until the next item (Device/Bus/Group) or end of
 * section.
 *
 * Usage:
 *     hwi_tag *dev = hwinfo_find_device("ns16550");
 *     hwi_tag *loc = hwinfo_find_tag_after(dev, HWI_TAG_NAME_location);
 *     if (loc) phys_addr = loc->location.base;
 */
static inline hwi_tag *hwinfo_find_tag_after(hwi_tag *start,
                                             const char *tag_name)
{
    uint8_t *base = hwinfo_base();
    char *strings = hwinfo_strings();
    unsigned section_size = _syspage_ptr->hwinfo.entry_size;

    hwi_tag *tag = hwinfo_next(start);

    while ((uintptr_t) tag < (uintptr_t) base + section_size) {
        if (tag->prefix.size == 0)
            break;

        const char *name = strings + tag->prefix.name;

        /* Stop at next item boundary */
        if (!strcmp(name, HWI_TAG_NAME_device) ||
            !strcmp(name, HWI_TAG_NAME_bus) ||
            !strcmp(name, HWI_TAG_NAME_group))
            break;

        if (!strcmp(name, tag_name))
            return tag;

        tag = hwinfo_next(tag);
    }
    return NULL;
}

#endif /* _HW_HWINFO_LOOKUP_H */
