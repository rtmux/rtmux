/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Host test of the PCI server's command line (servers/pci/args.c) with the
 * libc getopt_long: options before and after an operand, and "--", after which
 * every argument is an operand even when it looks like an option.
 */

#include <stdio.h>
#include <string.h>

#include "args.h"

static int failures;
static int checks;

#define CHECK(cond, ...)                                      \
    do {                                                      \
        checks++;                                             \
        if (!(cond)) {                                        \
            failures++;                                       \
            fprintf(stderr, "pci-args: FAIL line %d: ", __LINE__); \
            fprintf(stderr, __VA_ARGS__);                     \
            fputc('\n', stderr);                              \
        }                                                     \
    } while (0)

static struct pci_args defaults(void)
{
    return (struct pci_args){.scan_buses = 255, .map_irqs = 1, .use_msi = 1,
                             .max_msi = 1};
}

#define RUN(av, firstp, badp, a) \
    pci_parse_args((int) (sizeof(av) / sizeof((av)[0])) - 1, (av), (a), \
                   (firstp), (badp))

int main(void)
{
    struct pci_args a;
    const char *bad = NULL;
    int first = -1;

    /* No arguments. */
    {
        char *av[] = {"pci", NULL};

        a = defaults();
        CHECK(RUN(av, &first, &bad, &a) == PCI_ARGS_OK && first == 1 &&
                  a.use_msi && !a.dont_touch,
              "no arguments");
    }

    /* Options, in either order with an operand. */
    {
        char *av[] = {"pci", "-x", "-v", "-v", "-n", "4", NULL};

        a = defaults();
        CHECK(RUN(av, &first, &bad, &a) == PCI_ARGS_OK && first == 6 &&
                  a.dont_touch && a.verbose == 2 && a.max_msi == 4,
              "options set their fields");
    }
    {
        char *av[] = {"pci", "extra", "-x", NULL};

        a = defaults();
        CHECK(RUN(av, &first, &bad, &a) == PCI_ARGS_OK && first == 2 &&
                  a.dont_touch && !strcmp(av[first], "extra"),
              "an option after an operand is still an option");
    }

    /* "--" ends the options: what follows is an operand, never an option. */
    {
        char *av[] = {"pci", "-x", "--", "-M", "-n", "3", NULL};

        a = defaults();
        CHECK(RUN(av, &first, &bad, &a) == PCI_ARGS_OK && first == 3 &&
                  a.dont_touch && a.use_msi && a.max_msi == 1,
              "-M and -n after -- change nothing (first %d)", first);
        CHECK(!strcmp(av[first], "-M") && !strcmp(av[first + 1], "-n") &&
                  !strcmp(av[first + 2], "3"),
              "the arguments after -- are the operands, in order");
    }
    {
        char *av[] = {"pci", "--", "-P", NULL};

        a = defaults();
        CHECK(RUN(av, &first, &bad, &a) == PCI_ARGS_OK && first == 2 &&
                  !strcmp(av[first], "-P"),
              "-P after -- is not the unsupported raw mapping option");
    }
    {
        char *av[] = {"pci", "--", NULL};

        a = defaults();
        CHECK(RUN(av, &first, &bad, &a) == PCI_ARGS_OK && first == 2,
              "a lone -- leaves no operands");
    }

    /* Errors are reported, and one before -- still counts. */
    {
        char *av[] = {"pci", "-c", "4", NULL};

        a = defaults();
        CHECK(RUN(av, &first, &bad, &a) == PCI_ARGS_BAD_CLASS,
              "-c 4 is refused");
    }
    {
        char *av[] = {"pci", "-c", "5", NULL};

        a = defaults();
        CHECK(RUN(av, &first, &bad, &a) == PCI_ARGS_BAD_CLASS,
              "-c 5 is refused");
    }
    {
        char *av[] = {"pci", "-n", "3", NULL};

        a = defaults();
        CHECK(RUN(av, &first, &bad, &a) == PCI_ARGS_BAD_MSI,
              "-n 3 is refused");
    }
    {
        char *av[] = {"pci", "-P", "x", NULL};

        a = defaults();
        CHECK(RUN(av, &first, &bad, &a) == PCI_ARGS_RAW_MAPPING,
              "-P is refused");
    }
    {
        char *av[] = {"pci", "-q", NULL};

        a = defaults();
        CHECK(RUN(av, &first, &bad, &a) == PCI_ARGS_UNKNOWN && bad &&
                  !strcmp(bad, "-q"),
              "an unknown option is named");
    }
    {
        char *av[] = {"pci", "-q", "--", "-x", NULL};

        a = defaults();
        CHECK(RUN(av, &first, &bad, &a) == PCI_ARGS_UNKNOWN,
              "an error before -- still counts");
    }

    printf("pci-args: %s (%d checks, %d failures)\n",
           failures ? "FAIL" : "PASS", checks, failures);
    return failures ? 1 : 0;
}
