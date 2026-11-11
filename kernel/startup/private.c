/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * \brief Implementation of init_system_private().
 *
 * \license Apache 2.0 (http://www.apache.org/licenses/LICENSE-2.0)
 */

#include <startup.h>
#include <startup-api.h>

/**
 * \brief Initialize system_private_entry in the syspage.
 */
void init_system_private()
{
    kprintf("%s\n", __func__);

    struct system_private_entry *private =
        set_syspage_section(&lsp.system_private, sizeof(*lsp.system_private.p));

    /* Set page size. Failing to do so here will result in some very unexpected
     * results, such as ROUND() returning zero value and mem routines silently
     * stop working.
     */
private
    ->pagesize = __PAGESIZE;

    /* kcall moved to dedicated trampoline page (cpu-syspage-memory.c) */

    /* Set the hostname if it hasn't been set yet */
    if (find_typed_string(_CS_HOSTNAME) < 0) {
        add_typed_string(_CS_HOSTNAME, "localhost");
    }

    /* Default PATH for confstr(_CS_PATH) */
    if (find_typed_string(_CS_PATH) < 0) {
        add_typed_string(_CS_PATH, "/rd/bin:/rd/sbin:/bin:/sbin");
    }

    /* Record kernel location in the syspage */
    unsigned mem = as_find(AS_NULL_OFF, "cpu_addr_space", NULL);
    as_add((paddr_t) _start, (paddr_t) _end, AS_ATTR_RAM, "kernel", mem);

    /* Record FDT location also */
    as_add((paddr_t) fdt_addr, (paddr_t) fdt_addr + fdt_size, AS_ATTR_ROM,
           "fdt", mem);

    /* Record initramfs location, if necessary */
    if (initramfs_phys_start)
        as_add(initramfs_phys_start, initramfs_phys_end, AS_ATTR_ROM, "ramdisk",
               mem);

    /* If the user wanted to reserve some memory, allocate it now. Get it from
     * the top of available memory so that we we use as little of any one-to-one
     * mapping area as possible.
     */
    if (kopt.reserved_size > 0) {
        reserve_ram((unsigned) kopt.reserved_size, kopt.reserved_align,
                    private->pagesize);
    }

    /* Add system RAM */
    add_sysram();

    /* Allocate the real storage location for the system & cpu pages. We can't
     * call set_syspage_section()/grow_syspage_section() after this.
     */
    alloc_syspage_memory();
}
