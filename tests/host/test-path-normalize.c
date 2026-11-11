/*
 * Host-side tests for path_normalize().
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include "path-normalize.h"

struct testcase {
    const char *input;
    const char *want;
    int want_dir;
};

static const struct testcase cases[] = {
    {"/", "/", 1},
    {"///", "/", 1},
    {"/dev//ser1", "/dev/ser1", 0},
    {"/dev/./ser1", "/dev/ser1", 0},
    {"/dev/../proc/1", "/proc/1", 0},
    {"/../../proc", "/proc", 0},
    {"dev//ser1", "dev/ser1", 0},
    {"dev/../proc", "proc", 0},
    {"./dev/.", "dev", 1},
    {".", ".", 1},
    {"", ".", 1},
    {"dev/", "dev", 1},
    {"file/.", "file", 1},
    {"foo/..", ".", 1},
    /* Relative paths must preserve leading ".." traversals — otherwise
     * "../proc" silently becomes "proc" and changes meaning.
     */
    {"../proc", "../proc", 0},
    {"../..", "../..", 1},
    {"../foo/..", "..", 1},
    {"foo/../../bar", "../bar", 0},
};

static int check_case(const struct testcase *tc)
{
    char out[64];
    int is_dir = 0;

    if (path_normalize(tc->input, out, sizeof(out), &is_dir) != 0) {
        fprintf(stderr, "%s: normalize failed: %s\n", tc->input,
                strerror(errno));
        return 1;
    }
    if (strcmp(out, tc->want) != 0 || is_dir != tc->want_dir) {
        fprintf(stderr, "%s: got '%s' dir=%d, want '%s' dir=%d\n", tc->input,
                out, is_dir, tc->want, tc->want_dir);
        return 1;
    }
    return 0;
}

static int check_enametoolong(void)
{
    char out[4];

    if (path_normalize("/abcd", out, sizeof(out), NULL) != -1 ||
        errno != ENAMETOOLONG) {
        fprintf(stderr, "expected ENAMETOOLONG for small output buffer\n");
        return 1;
    }
    return 0;
}

int main(void)
{
    int failed = 0;

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
        failed += check_case(&cases[i]);
    failed += check_enametoolong();
    if (failed)
        return 1;
    puts("path-normalize-test: ok");
    return 0;
}
