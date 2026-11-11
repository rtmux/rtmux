/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <startup.h>
#include <startup-api.h>

/**
 * Return the number of bits this machine/platform supports.
 */
unsigned arch_max_paddr_bits(void)
{
    return 39;
}
