/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * \brief Various debugging routines.
 *
 * \license Apache 2.0 (http://www.apache.org/licenses/LICENSE-2.0)
 */

#include <startup.h>
#include <startup-api.h>

extern void print_syspage_enable(const char *name);
extern void print_syspage_sections(void);

void print_syspage(void)
{
    int opt;

    if (kopt.loglevel > 1) {
        /* We enable things here so that this code doesn't get hauled in unless
         * the user calls print_syspage().
         */
        optind = 0;
        while ((opt = getopt(_argc, _argv, "S:")) != -1) {
            if (opt == 'S') {
                print_syspage_enable(optarg);
            }
        }
        print_syspage_sections();
    }
}
