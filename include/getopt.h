/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* getopt.h — getopt_long and getopt_long_only (GNU and BSD extensions). */

#ifndef _GETOPT_H_INCLUDED
#define _GETOPT_H_INCLUDED

#include <unistd.h>

#define no_argument 0
#define required_argument 1
#define optional_argument 2

__BEGIN_DECLS

/* Set nonzero to restart getopt (BSD extension; optind = 0 works too). Kept out
 * of <unistd.h> so it does not enter the POSIX namespace.
 */
extern int optreset;

struct option {
    const char *name; /* long name, without the leading dashes */
    int has_arg;      /* no_argument, required_argument, or optional_argument */
    int *flag;        /* if not NULL, set *flag = val and return 0 */
    int val;          /* returned, or stored through flag */
};

extern int getopt_long(int __argc,
                       char *const __argv[],
                       const char *__optstring,
                       const struct option *__longopts,
                       int *__longindex);
extern int getopt_long_only(int __argc,
                            char *const __argv[],
                            const char *__optstring,
                            const struct option *__longopts,
                            int *__longindex);

__END_DECLS
#endif
