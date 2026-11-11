/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * \brief Minimalist getopt() implementation.
 *
 * \license Apache 2.0 (http://www.apache.org/licenses/LICENSE-2.0)
 */

char *optarg;
int optind = 0;

int getopt(int argc, char **argv, char *opts)
{
    static char *cp;
    static int last_optind;
    int opt;

    if (optind == 0) {
        optind = 1;
        last_optind = 0;
    }
    if (optind >= argc) {
        return -1;
    }
    if (last_optind != optind) {
        cp = argv[optind];
        last_optind = optind;
        if (*cp++ != '-' || *cp == 0) {
            return -1;
        }
    }
    if (*cp == '-') {
        ++optind;
        return -1;
    }
    for (; *opts; ++opts) {
        if (*opts == ':')
            continue;
        if (*cp == *opts)
            break;
    }
    optarg = cp;
    ++optind;
    opt = *opts++;
    if (opt) {
        if (*++cp) {
            if (*opts == ':') {
                optarg = cp;
            } else {
                --optind;
            }
        } else {
            if (*opts == ':') {
                if (optind < argc) {
                    optarg = argv[optind];
                    optind++;
                } else {
                    opt = '?';
                }
            }
        }
    } else {
        opt = '?';
    }
    return opt;
}

#include <sys/platform.h>
