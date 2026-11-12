/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * RISC-V as_init_mem — initialize virtual memory.
 *
 * Phase 0 (kernel init): set up physical allocator, page size list,
 *   user boundary address, and xfer slots.
 * Phase 1 (roottask init): background processing startup.
 *
 * This path only initializes Sv39 state; platform-specific memory typing lives
 * outside the address-space bootstrap.
 */

#include "as.h"

/* Initialize RISC-V virtual memory.
 *
 * Parameters:
 *   phase : 0 = set up physical allocator, page sizes, and user boundary;
 *           non-zero = start background processing.
 */
void as_init_mem(int phase)
{
    if (phase != 0) {
        pa_start_background();
        return;
    }

    /* Sv39 supports 4K base pages and 2M megapages. Set the page size list
     * (largest first, zero-terminated).
     */
    pgszlist[0] = 2 * 1024 * 1024; /* 2M megapage */
    pgszlist[1] = __PAGESIZE;      /* 4K base page */

    user_boundary_addr = CPU_USER_VADDR_END;

    pgtbl_init();

    /* 1 color — RISC-V has no cache coloring issues */
    pa_init(1);
}
