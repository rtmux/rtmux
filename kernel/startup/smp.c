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
 * \brief Initialize SMP structure in the system page.
 */
void init_smp(void)
{
    unsigned num_cpus;

    num_cpus = board_smp_num_cpu();
    if (num_cpus > kopt.max_cpus)
        num_cpus = kopt.max_cpus;
    lsp.syspage.p->num_cpu = (uint16_t) num_cpus;

    if (num_cpus > 1)
        set_syspage_section(&lsp.smp, sizeof(*lsp.smp.p));
}
