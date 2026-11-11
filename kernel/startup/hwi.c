/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * \brief Hardware item routines.
 *
 * \license Apache 2.0 (http://www.apache.org/licenses/LICENSE-2.0)
 */

#include <startup.h>
#include <startup-api.h>

static void *hwi_alloc_tag(const char *name, unsigned size, unsigned align);
static void *hwi_alloc_item(const char *name,
                            unsigned size,
                            unsigned align,
                            const char *itemname,
                            unsigned owner);

void hwi_default()
{
    hwi_tag *tag;

    tag = hwi_alloc_item(HWI_TAG_INFO(group), HWI_ITEM_ROOT_HW, HWI_NULL_OFF);

    hwi_alloc_item(HWI_TAG_INFO(bus), HWI_ITEM_BUS_UNKNOWN,
                   (unsigned) hwi_tag2off(tag));
}

char *__hwi_find_string(unsigned off)
{
    char *p = lsp.strings.p->data;
    return p + off;
}

void *__hwi_base()
{
    return lsp.hwinfo.p->data;
}


static unsigned offset = 0;
static unsigned last_item = 0;

static void align_tag(unsigned align)
{
    unsigned grow;

    grow = (unsigned) (ROUND(offset, align) - offset);
    if (grow != 0) {
        hwi_alloc_tag(HWI_TAG_INFO(pad));
    }
}

static void *hwi_alloc_tag(const char *name, unsigned size, unsigned align)
{
    uint8_t *p;
    hwi_tag *tag;
    unsigned name_off;

    align_tag(align);
    name_off = add_string(name);
    p = grow_syspage_section(&lsp.hwinfo, size);
    tag = (hwi_tag *) (p + last_item);
    tag->item.itemsize += (uint16_t) (size / 4);
    tag = (hwi_tag *) (p + offset);
    tag->prefix.size = (uint16_t) (size / 4);
    tag->prefix.name = (uint16_t) name_off;
    offset += size;
    return tag;
}

static void *hwi_alloc_item(const char *name,
                            unsigned size,
                            unsigned align,
                            const char *itemname,
                            unsigned owner)
{
    hwi_tag *tag;
    unsigned saved_in;

    /* Do add_string() up front so that the tag pointer doesn't move due to
     * shifting section sizes.
     */
    saved_in = add_string(itemname);
    align_tag(align);
    last_item = offset;
    tag = hwi_alloc_tag(name, size, align);
    tag->item.itemname = (uint16_t) saved_in;
    tag->item.owner = (uint16_t) owner;
    if (owner != HWI_NULL_OFF) {
        hwi_tag *owner_tag;

        owner_tag = hwi_off2tag(owner);
        owner_tag->item.kids++;
    }
    return tag;
}

/* Convenience functions for adding buses, devices, and tags. */

unsigned hwibus_add(const char *bus_name, unsigned parent_hwi_off)
{
    hwi_tag *tag;

    if (parent_hwi_off == HWI_NULL_OFF)
        return HWI_NULL_OFF;

    tag = hwi_alloc_item(HWI_TAG_INFO(bus), bus_name, parent_hwi_off);
    return tag ? (unsigned) hwi_tag2off(tag) : HWI_NULL_OFF;
}

unsigned hwidev_add(const char *device_name, unsigned pnp, unsigned bus_hwi_off)
{
    hwi_tag *tag;

    if (bus_hwi_off == HWI_NULL_OFF)
        return HWI_NULL_OFF;

    tag = hwi_alloc_item(HWI_TAG_INFO(device), device_name, bus_hwi_off);
    if (tag) {
        tag->device.pnpid = pnp;
        return (unsigned) hwi_tag2off(tag);
    }
    return HWI_NULL_OFF;
}

void hwitag_add_location(unsigned hwi_off,
                         paddr_t base,
                         paddr_t len,
                         unsigned reg_shift,
                         unsigned addr_space)
{
    hwi_tag *tag;
    (void) hwi_off;

    tag = hwi_alloc_tag(HWI_TAG_INFO(location));
    tag->location.base = base;
    tag->location.len = (uint32_t) len;
    tag->location.regshift = (uint16_t) reg_shift;
    tag->location.addrspace = (uint16_t) addr_space;
}

void hwitag_add_irq(unsigned hwi_off, unsigned vector)
{
    hwi_tag *tag;
    (void) hwi_off;

    tag = hwi_alloc_tag(HWI_TAG_INFO(irq));
    tag->irq.vector = vector;
}

/**
 * \brief Attach an "optstr" tag carrying a free-form string.
 *
 * Used to publish FDT compatible strings and other option text that doesn't fit
 * the structured tag schema.
 */
void hwitag_add_optstr(unsigned hwi_off, const char *string)
{
    hwi_tag *tag;
    unsigned str_off;
    (void) hwi_off;

    /* Reserve string first so the tag pointer can't move mid-allocation. */
    str_off = add_string(string);
    tag = hwi_alloc_tag(HWI_TAG_INFO(optstr));
    tag->optstr.string = str_off;
}

/**
 * \brief Attach a PCI address window tag to the last opened item.
 *
 * One tag is expected per entry of the PCIe host bridge "ranges" property.
 *
 * \param hwi_off offset of the owning hwi item (kept for API symmetry)
 * \param pci_addr bus-side base of the window
 * \param cpu_addr CPU-side physical base of the window
 * \param size window length in bytes
 * \param flags HWI_PCI_WINDOW_* bits (IO / MEM / PREFETCH)
 */
void hwitag_add_pci_window(unsigned hwi_off,
                           uint64_t pci_addr,
                           uint64_t cpu_addr,
                           uint64_t size,
                           uint32_t flags)
{
    hwi_tag *tag;
    (void) hwi_off;

    tag = hwi_alloc_tag(HWI_TAG_INFO(pci_window));
    tag->pci_window.flags = flags;
    tag->pci_window.pci_addr = pci_addr;
    tag->pci_window.cpu_addr = cpu_addr;
    tag->pci_window.size = size;
}
