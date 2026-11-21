/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <getopt.h>
#include <stdlib.h>

#include "args.h"

/* getopt_long wants a table even when there are no long options. */
static const struct option no_long_options[] = {{NULL, 0, NULL, 0}};

enum pci_args_status pci_parse_args(int argc,
                                    char **argv,
                                    struct pci_args *a,
                                    int *first_operand,
                                    const char **bad)
{
    static char optbuf[3];
    int c;

    /* getopt_long permutes, so options may follow an operand; "--" ends them,
     * and whatever follows it is an operand. A POSIXLY_CORRECT environment
     * turns permutation off: the first operand then ends the options.
     */
    optind = 0; /* restart the parser, as a test may parse more than once */
    while ((c = getopt_long(argc, argv, "b:c:DmMn:pP:vx", no_long_options,
                            NULL)) != -1) {
        switch (c) {
        case 'b':
            a->scan_buses = atoi(optarg);
            break;
        case 'c':
            a->class_override = atoi(optarg);

            /* A mask of the two override bits, so only 1, 2 and 3; a value such
             * as 5 or -1 would otherwise pass on its low bits alone.
             */
            if (a->class_override < 1 || a->class_override > 3)
                return PCI_ARGS_BAD_CLASS;
            break;
        case 'D':
            a->enable_crt_msi = 1;
            break;
        case 'm':
            a->map_irqs ^= 1; /* Flip the flag */
            break;
        case 'M':
            a->use_msi = 0;
            break;
        case 'n':
            a->max_msi = atoi(optarg);
            a->max_msi_set = 1;
            switch (a->max_msi) {
            case 1:
            case 2:
            case 4:
            case 8:
            case 16:
            case 32:
                break;
            default:
                return PCI_ARGS_BAD_MSI;
            }
            break;
        case 'p':
            a->program_display_device = 1;
            break;
        case 'P':
            return PCI_ARGS_RAW_MAPPING;
        case 'v':
            a->verbose++;
            break;
        case 'x':
            a->dont_touch = 1;
            break;
        default:
            /* A long name leaves optopt 0: report argv[optind - 1]. */
            if (optopt) {
                optbuf[0] = '-';
                optbuf[1] = (char) optopt;
                *bad = optbuf;
            } else {
                *bad = argv[optind - 1];
            }
            return PCI_ARGS_UNKNOWN;
        }
    }
    *first_operand = optind;
    return PCI_ARGS_OK;
}
