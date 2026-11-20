/*-
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Copyright (c) 1987, 1993, 1994
 * The Regents of the University of California. All rights reserved.
 *
 * The short-option scan keeps the structure of the BSD getopt; the restart
 * rules, getopt_long, and getopt_long_only are RTMux additions.
 */

/*
 * getopt, getopt_long, and getopt_long_only.
 *
 * getopt follows POSIX.1-2024: parsing stops at the first operand, at a bare
 * "-" (which is an operand), or after an exact "--". Any other argument that
 * starts with "--" is a group of option letters like any other, so with
 * optstring "a" the argument "--foo" reports '-' as an unknown option.
 *
 * getopt_long adds long options: "--name", "--name=value", "--name value" for a
 * required argument, and any unambiguous prefix of a name. Like GNU getopt, it
 * permutes argv, so options after an operand are seen: the operands it steps
 * over are moved behind the options, in their order, and optind ends at the
 * first operand. Permutation is off when POSIXLY_CORRECT is in the environment
 * or optstring starts with '+' (parsing then stops at the first operand, as in
 * getopt), and does not apply with a leading '-', where each operand is
 * returned as it comes. getopt itself never permutes. getopt_long_only also
 * takes long names after a single '-', falling back to option letters when no
 * name matches.
 *
 * Extensions: optind = 0 (GNU) or optreset = 1 (BSD) restarts the parser,
 * clearing the position inside a group such as "-ab". Setting optind to 1 is
 * not a restart signal, since optind stays 1 while the letters of a group in
 * argv[1] are returned; but when argv[optind] is no longer the argument the
 * parser was inside, the position is dropped, so pointing optind at a fresh
 * argv also works. In optstring, a leading '+' is accepted and ignored, a
 * leading '-' returns each operand as option 1 with optarg set to it, and "x::"
 * gives x an optional argument, which must be attached ("-xval").
 */

#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int opterr = 1;
int optind = 1;
int optopt;
int optreset;
char *optarg;

/* Offset into argv[optind] while the letters of a group are being returned, or
 * 0 between arguments; group is the argument the offset belongs to.
 */
static size_t pos;
static const char *group;

/* While getopt_long permutes: the operands it has stepped over are
 * argv[first_non..last_non), and argv[last_non..optind) are the options read
 * since. Both are empty, and equal to optind, between parses.
 */
static int first_non;
static int last_non;

static void clear_group(void)
{
    pos = 0;
    group = NULL;
}

/* Finish the current argument and move to the next. */
static void next_arg(void)
{
    optind++;
    clear_group();
}

static void reverse(char **v, int lo, int hi)
{
    while (lo < --hi) {
        char *t = v[lo];

        v[lo++] = v[hi];
        v[hi] = t;
    }
}

/* Move the skipped operands behind the options read since, keeping the order of
 * each: argv[first_non..optind) becomes options, then operands.
 */
static void settle(char *const argv[])
{
    if (first_non == last_non || last_non == optind)
        return;
    reverse((char **) argv, first_non, last_non);
    reverse((char **) argv, last_non, optind);
    reverse((char **) argv, first_non, optind);
    first_non += optind - last_non;
    last_non = optind;
}

/* End of options: leave optind at the first operand skipped, if any. */
static int finish(char *const argv[])
{
    settle(argv);
    if (first_non != last_non)
        optind = first_non;
    first_non = last_non = optind;
    return -1;
}

/* The optstring entry for option letter c, or NULL; ':' is never an option. */
static const char *short_spec(const char *optstring, int c)
{
    return c != ':' ? strchr(optstring, c) : NULL;
}

/* Find the long option named by name[0..len), exactly or as the prefix of
 * exactly one entry (or of several that all behave the same). An exact match
 * anywhere in the table wins.
 *
 * Returns its index, -1 for none (including an empty name, as in "--=value"),
 * or -2 for an ambiguous prefix.
 */
static int find_long(const struct option *longopts,
                     const char *name,
                     size_t len)
{
    int match = -1;

    if (len == 0)
        return -1;
    for (int i = 0; longopts[i].name; i++) {
        const struct option *o = &longopts[i];

        if (strncmp(o->name, name, len) != 0)
            continue;
        if (o->name[len] == '\0')
            return i;
        if (match == -1) {
            match = i;
        } else if (match >= 0) {
            const struct option *m = &longopts[match];

            if (m->has_arg != o->has_arg || m->flag != o->flag ||
                m->val != o->val)
                match = -2;
        }
    }
    return match;
}

/* Parse argv[optind] as the long option longopts[idx] (or an error for idx -1
 * or -2, see find_long) whose name is name[0..len), followed by '=' and a value
 * or by the end of the argument.
 */
static int parse_long(int argc,
                      char *const argv[],
                      int quiet,
                      const struct option *longopts,
                      int *longindex,
                      const char *name,
                      size_t len,
                      int idx)
{
    const char *arg = argv[optind];
    const char *eq = name[len] == '=' ? name + len : NULL;

    next_arg();
    if (idx < 0) {
        if (opterr && !quiet)
            fprintf(stderr, "%s: %s option '%.*s'\n", argv[0],
                    idx == -2 ? "ambiguous" : "unrecognized",
                    (int) (name + len - arg), arg);
        optopt = 0;
        return '?';
    }

    const struct option *o = &longopts[idx];

    /* On an error, optopt is the option's value when it has one. */
    optopt = o->flag ? 0 : o->val;
    if (eq) {
        if (o->has_arg == no_argument) {
            if (opterr && !quiet)
                fprintf(stderr, "%s: option '%.*s' doesn't allow an argument\n",
                        argv[0], (int) (eq - arg), arg);
            return '?';
        }
        optarg = (char *) eq + 1;
    } else if (o->has_arg == required_argument) {
        if (optind >= argc) {
            if (opterr && !quiet)
                fprintf(stderr, "%s: option '%s' requires an argument\n",
                        argv[0], arg);
            return quiet ? ':' : '?';
        }
        optarg = argv[optind++];
    } else {
        optarg = NULL;
    }
    if (longindex)
        *longindex = idx;
    if (o->flag) {
        *o->flag = o->val;
        return 0;
    }
    return o->val;
}

static int parse(int argc,
                 char *const argv[],
                 const char *optstring,
                 const struct option *longopts,
                 int *longindex,
                 int long_only)
{
    int operands_as_1, plus, posix = -1;
    const char *arg;

    if (optind == 0 || optreset) {
        optreset = 0;
        if (optind == 0)
            optind = 1;
        clear_group();
        first_non = last_non = optind;
    }
    /* A caller that moved optind behind the operands drops the block. */
    if (first_non > last_non || last_non > optind)
        first_non = last_non = optind;
    plus = *optstring == '+';
    operands_as_1 = *optstring == '-';
    if (plus || operands_as_1)
        optstring++;

    /* A leading ':' (after any '+' or '-') reports a missing argument as ':'
     * and silences the messages.
     */
    int quiet = *optstring == ':';

again:
    if (optind < 1 || optind >= argc || !(arg = argv[optind]))
        return finish(argv);
    if (arg != group)
        pos = 0; /* between arguments, or the caller moved optind elsewhere */
    if (pos == 0) {
        if (arg[0] != '-' || arg[1] == '\0') {
            if (operands_as_1) {
                optarg = argv[optind++];
                return 1;
            }
            if (!longopts || plus)
                return finish(argv);
            if (posix < 0)
                posix = getenv("POSIXLY_CORRECT") != NULL;
            if (posix)
                return finish(argv);
            /* Step over the operand; the options after it come first. */
            settle(argv);
            if (first_non == last_non)
                first_non = optind;
            last_non = ++optind;
            goto again;
        }
        if (arg[1] == '-' && arg[2] == '\0') {
            optind++;
            return finish(argv);
        }
        if (longopts && (arg[1] == '-' || long_only)) {
            /* For getopt_long_only, a single letter that is a short option
             * stays one, and a longer name falls back to option letters when it
             * names no long option and its first letter is a short option.
             */
            const char *name = arg[1] == '-' ? arg + 2 : arg + 1;
            int is_short = arg[1] != '-' && short_spec(optstring, arg[1]);

            if (!is_short || arg[2] != '\0') {
                size_t len = strcspn(name, "=");
                int idx = find_long(longopts, name, len);

                if (idx != -1 || !is_short)
                    return parse_long(argc, argv, quiet, longopts, longindex,
                                      name, len, idx);
            }
        }
        pos = 1;
        group = arg;
    }

    int c = (unsigned char) arg[pos++];
    const char *spec = short_spec(optstring, c);
    int last = arg[pos] == '\0';

    optopt = c;
    if (!spec || spec[1] != ':') {
        if (last)
            next_arg();
        if (spec) {
            optarg = NULL;
            return c;
        }
        if (opterr && !quiet)
            fprintf(stderr, "%s: illegal option -- %c\n", argv[0], c);
        return '?';
    }
    /* An attached argument; an optional one ("x::") can only be attached. */
    optarg = last ? NULL : (char *) arg + pos;
    next_arg();
    if (optarg || spec[2] == ':')
        return c;
    if (optind >= argc) {
        if (quiet)
            return ':';
        if (opterr)
            fprintf(stderr, "%s: option requires an argument -- %c\n", argv[0],
                    c);
        return '?';
    }
    optarg = argv[optind++];
    return c;
}

int getopt(int argc, char *const argv[], const char *optstring)
{
    return parse(argc, argv, optstring, NULL, NULL, 0);
}

int getopt_long(int argc,
                char *const argv[],
                const char *optstring,
                const struct option *longopts,
                int *longindex)
{
    return parse(argc, argv, optstring, longopts, longindex, 0);
}

int getopt_long_only(int argc,
                     char *const argv[],
                     const char *optstring,
                     const struct option *longopts,
                     int *longindex)
{
    return parse(argc, argv, optstring, longopts, longindex, 1);
}
