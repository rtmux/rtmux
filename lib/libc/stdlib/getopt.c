/*-
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Copyright (c) 1987, 1993, 1994
 * The Regents of the University of California. All rights reserved.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int opterr = 1;
int optind = 1;
int optopt;
char *optarg;

int getopt(int nargc, char *const nargv[], const char *ostr)
{
    static char *place = "";
    const char *oli;

    if (!*place) {
        if (optind >= nargc || *(place = nargv[optind]) != '-') {
            place = "";
            return -1;
        }
        if (place[1] && *++place == '-') {
            ++optind;
            place = "";
            return -1;
        }
    }

    if ((optopt = (int) *place++) == (int) ':' ||
        !(oli = strchr(ostr, optopt))) {
        if (optopt == (int) '-')
            return -1;
        if (!*place)
            ++optind;
        if (opterr && *ostr != ':')
            fprintf(stderr, "%s: illegal option -- %c\n", nargv[0], optopt);
        return '?';
    }

    if (*++oli != ':') {
        optarg = NULL;
        if (!*place)
            ++optind;
    } else {
        if (*place)
            optarg = place;
        else if (nargc <= ++optind) {
            place = "";
            if (*ostr == ':')
                return ':';
            if (opterr)
                fprintf(stderr, "%s: option requires an argument -- %c\n",
                        nargv[0], optopt);
            return '?';
        } else
            optarg = nargv[optind];
        place = "";
        ++optind;
    }
    return optopt;
}
