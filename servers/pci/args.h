/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Command line of the PCI server, parsed apart from the server so a host test
 * can run it.
 */

#ifndef PCI_ARGS_H
#define PCI_ARGS_H

/* The settings the options change. The caller fills in the defaults first. */
struct pci_args {
    int scan_buses;
    int class_override;
    int enable_crt_msi;
    int map_irqs;
    int use_msi;
    int max_msi;
    int max_msi_set; /* -n was given */
    int program_display_device;
    int verbose;
    int dont_touch;
};

enum pci_args_status {
    PCI_ARGS_OK,
    PCI_ARGS_BAD_CLASS,   /* -c is not 1, 2 or 3 */
    PCI_ARGS_BAD_MSI,     /* -n is not a power of two up to 32 */
    PCI_ARGS_RAW_MAPPING, /* -P is unsupported */
    PCI_ARGS_UNKNOWN,     /* unknown option or missing argument */
};

/* Parse argv into *a. On success *first_operand is the index of the first
 * operand: "--" ends the options, so what follows it is an operand whatever it
 * looks like. On PCI_ARGS_UNKNOWN, *bad names the offending argument.
 */
enum pci_args_status pci_parse_args(int argc,
                                    char **argv,
                                    struct pci_args *a,
                                    int *first_operand,
                                    const char **bad);

#endif /* PCI_ARGS_H */
