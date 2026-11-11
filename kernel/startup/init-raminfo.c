/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * Initialize raminfo structure in the system page.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <startup.h>
#include <startup-api.h>

static void handle_property_meminfo(const char *node,
                                    const char *prop,
                                    void *data,
                                    const unsigned datalen)
{
    const uint64_t *ranges = data;
    unsigned nranges = datalen / (2 * sizeof *ranges);

    if (strncmp(node, "memory@", 7) || strcmp(prop, "reg"))
        return;

    for (unsigned i = 0; i < nranges; i++) {
        paddr_t base = ENDIAN_RET64(ranges[i * 2]);
        paddr_t size = ENDIAN_RET64(ranges[i * 2 + 1]);

        if (kopt.ram_limit && ram_phys_start) {
            paddr_t limit = ram_phys_start + kopt.ram_limit;

            /* A bogus --limit-ram can make ram_phys_start + ram_limit wrap to a
             * low value, which would then drop every RAM range. Saturate.
             */
            if (limit < ram_phys_start)
                limit = (paddr_t) -1;

            if (base >= limit)
                continue;

            /* base < limit proven above, so limit - base can't overflow; avoid
             * base + size which can wrap on a malformed FDT range.
             */
            if (size > limit - base)
                size = limit - base;
        }

        add_ram(base, size);
        LOGF_D("RAM at %#p, size %#p\n", base, size);
    }
}

void init_raminfo(void)
{
    LOGF_I("%s\n", __func__);

    fdt_handle_node_cb = NULL;
    fdt_handle_property_cb = handle_property_meminfo;
    parse_fdt(NULL);
}
