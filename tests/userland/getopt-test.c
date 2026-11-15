/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * getopt-test.c — table tests for getopt, getopt_long, and getopt_long_only,
 * including the optind = 0 and optreset restarts.
 *
 * Pure C over the standard headers, so tests/host/test-libc-algorithms.c
 * builds it against the RTMux getopt.c as well.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <getopt.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "test.h"

extern int optreset; /* BSD extension; host headers may not declare it */

#define MAXARGS 8

enum which { SHORT, LONG, LONG_ONLY };

static int flag_verbose;

static const struct option longopts[] = {
    {"xray", required_argument, NULL, 'x'},
    {"xenon", no_argument, NULL, 'n'},
    {"verbose", no_argument, &flag_verbose, 1},
    {"value", optional_argument, NULL, 'v'},
    {"all", no_argument, NULL, 'A'},
    {"minus", no_argument, NULL, -1},
    {"color", no_argument, NULL, 'C'},
    {"colour", no_argument, NULL, 'C'},
    {NULL, 0, NULL, 0},
};

/* Run a whole parse and render it: each result as its character ('0' for a
 * flag store, '1' for an operand), '?' and ':' followed by optopt (or '0'), an optarg in
 * brackets, a long index as #n, and finally "|" plus the stopping optind and
 * "v" if the --verbose flag was stored.
 */
static void run(enum which which, const char *optstring, const char *const *args, char *out,
                size_t outsz)
{
    char *argv[MAXARGS + 1];
    int argc = 0;
    size_t len = 0;

    argv[argc++] = "prog";
    for (; *args && argc < MAXARGS; args++)
        argv[argc++] = (char *) *args;
    argv[argc] = NULL;
    out[0] = '\0';
    for (int guard = 0; guard < 32; guard++) {
        int idx = -1;
        int c;

        optarg = NULL;
        if (which == SHORT)
            c = getopt(argc, argv, optstring);
        else if (which == LONG)
            c = getopt_long(argc, argv, optstring, longopts, &idx);
        else
            c = getopt_long_only(argc, argv, optstring, longopts, &idx);
        if (c == -1) {
            snprintf(out + len, outsz - len, "|%d%s", optind, flag_verbose ? "v" : "");
            return;
        }
        if (c == '?' || c == ':')
            len += (size_t) snprintf(out + len, outsz - len, "%c%c", c,
                                     optopt ? optopt : '0');
        else
            len += (size_t) snprintf(out + len, outsz - len, "%c", c > 1 ? c : '0' + c);
        if (len >= outsz)
            return;
        if (optarg)
            len += (size_t) snprintf(out + len, outsz - len, "[%s]", optarg);
        if (len >= outsz)
            return;
        if (idx >= 0)
            len += (size_t) snprintf(out + len, outsz - len, "#%d", idx);
        if (len >= outsz)
            return;
    }
}

struct getopt_case {
    const char *name;
    enum which which;
    const char *optstring;
    const char *args[MAXARGS];
    const char *want;
};

static const struct getopt_case cases[] = {
    {"getopt -a --foo gives a, then ?", SHORT, "a", {"-a", "--foo"}, "a?-?f?o?o|3"},
    {"getopt -ab gives a then b", SHORT, "ab", {"-ab", "x"}, "ab|2"},
    {"getopt a: takes --foo as the argument", SHORT, "a:", {"-a", "--foo"}, "a[--foo]|3"},
    {"getopt attached argument", SHORT, "a:b", {"-bafile"}, "ba[file]|2"},
    {"getopt -- ends options", SHORT, "ab", {"-a", "--", "-b"}, "a|3"},
    {"getopt bare - is an operand", SHORT, "a", {"-a", "-", "-a"}, "a|2"},
    {"getopt stops at the first operand", SHORT, "a", {"x", "-a"}, "|1"},
    {"getopt missing argument is ?", SHORT, "b:", {"-b"}, "?b|2"},
    {"getopt missing argument is : with leading :", SHORT, ":b:", {"-b"}, ":b|2"},
    {"getopt missing argument at the end of a group", SHORT, "ab:", {"-ab"}, "a?b|2"},
    {"getopt unknown letter", SHORT, "a", {"-z", "-a"}, "?za|3"},
    {"getopt ':' is never an option", SHORT, "a:", {"-:"}, "?:|2"},
    {"getopt optional argument", SHORT, "o::", {"-oval", "-o", "x"}, "o[val]o|3"},
    {"getopt leading - returns operands as 1", SHORT, "-a", {"x", "-a", "y"},
     "1[x]a1[y]|4"},
    {"getopt_long --x=1", LONG, "", {"--xray=1"}, "x[1]#0|2"},
    {"getopt_long --x 1", LONG, "", {"--xray", "1"}, "x[1]#0|3"},
    {"getopt_long unambiguous prefix", LONG, "", {"--xr", "1", "--xe"}, "x[1]#0n#1|4"},
    {"getopt_long ambiguous prefix is ?", LONG, "", {"--x", "--all"}, "?0A#4|3"},
    {"getopt_long unknown name is ?", LONG, "", {"--nope"}, "?0|2"},
    {"getopt_long empty name --=v is ?", LONG, "", {"--=1"}, "?0|2"},
    {"getopt_long argument to a no_argument option", LONG, "", {"--xenon=3"}, "?n|2"},
    {"getopt_long missing argument", LONG, "", {"--xray"}, "?x|2"},
    {"getopt_long missing argument is : with leading :", LONG, ":", {"--xray"}, ":x|2"},
    {"getopt_long flag store returns 0", LONG, "", {"--verbose"}, "0#2|2v"},
    {"getopt_long optional argument", LONG, "", {"--value", "--value=3"}, "v#3v[3]#3|3"},
    {"getopt_long mixes short options", LONG, "ab:", {"-a", "--all", "-bz"},
     "aA#4b[z]|4"},
    {"getopt_long -- ends options", LONG, "", {"--", "--all"}, "|2"},
    {"getopt_long_only -name", LONG_ONLY, "a", {"-xray=2", "-all"}, "x[2]#0A#4|3"},
    {"getopt_long_only single short letter", LONG_ONLY, "a", {"-a"}, "a|2"},
    {"getopt_long_only falls back to letters", LONG_ONLY, "ab", {"-ab"}, "ab|2"},
    {"getopt_long_only option whose val is -1", LONG_ONLY, "m", {"-minus", "-m"}, "|2"},
    {"getopt_long prefix of aliases is not ambiguous", LONG, "", {"--col"}, "C#6|2"},
};

static void test_getopt_table(void)
{
    char out[128];
    int save_opterr = opterr;

    opterr = 0;
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        const struct getopt_case *t = &cases[i];

        flag_verbose = 0;
        optind = 0;
        run(t->which, t->optstring, t->args, out, sizeof out);
        if (strcmp(out, t->want) == 0) {
            test_ok(t->name);
        } else {
            char detail[200];

            snprintf(detail, sizeof detail, "got \"%s\", want \"%s\"", out, t->want);
            test_fail(t->name, detail);
        }
    }
    opterr = save_opterr;
}

static char *first[] = {"prog", "-abc", NULL};
static char *second[] = {"prog", "-xy", NULL};

static int g(char **av)
{
    return getopt(2, av, "abcxy");
}

/* A restart in the middle of a group must not carry the old position into the
 * next parse, whichever way it is requested.
 */
static void test_getopt_restart(void)
{
    int ok;

    opterr = 0;
    optind = 0;
    ok = g(first) == 'a' && optind == 1;
    optind = 0;
    ok = ok && g(second) == 'x' && g(second) == 'y' && g(second) == -1 && optind == 2;
    check(ok, "getopt optind=0 restarts after a partly consumed group",
          "old group position leaked into the second parse");

    optind = 0;
    ok = g(first) == 'a' && g(first) == 'b';
    optind = 1;
    optreset = 1;
    ok = ok && g(first) == 'a' && g(first) == 'b' && g(first) == 'c' && g(first) == -1;
    check(ok && optreset == 0, "getopt optreset restarts the same argv",
          "parse did not restart from the first letter");

    optind = 0;
    ok = g(first) == 'a';
    optind = 1; /* plain optind = 1 on a different argv: no carried position */
    ok = ok && g(second) == 'x';
    check(ok, "getopt optind=1 on a new argv starts at its first letter",
          "position from the previous argv was reused");

    ok = 1;
    for (int round = 0; round < 3; round++) {
        optind = 0;
        ok = ok && g(first) == 'a' && g(first) == 'b' && g(first) == 'c' && g(first) == -1 &&
             optind == 2;
    }
    check(ok, "getopt repeated parses after optind=0", "a later round differed");

    /* The toolbox pattern: a finished parse, then optind = 1 for the next. */
    ok = 1;
    for (int round = 0; round < 2; round++) {
        optind = 1;
        ok = ok && g(first) == 'a' && g(first) == 'b' && g(first) == 'c' && g(first) == -1;
    }
    check(ok, "getopt optind=1 after a finished parse", "second parse differed");
    optind = 1;
    opterr = 1;
}

void test_getopt(void)
{
    test_getopt_table();
    test_getopt_restart();
}
