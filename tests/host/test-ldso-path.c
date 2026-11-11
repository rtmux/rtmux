/*
 * Host-side tests for ldso path helpers.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <assert.h>
#include <stdarg.h>
#include <string.h>

#include <ldso-path.h>

void _ldso_error(const char *fmt, ...)
{
    (void) fmt;
}

const char *ldso_strerror(int errnum)
{
    return strerror(errnum);
}

static void test_basename(void)
{
    assert(!strcmp(ldso_basename("prog"), "prog"));
    assert(!strcmp(ldso_basename("/bin/prog"), "prog"));
    assert(!strcmp(ldso_basename("/bin/"), ""));
}

static void expect_dirname(const char *path, const char *want)
{
    char buf[256];

    assert(ldso_dirname(path, buf) == 0);
    assert(!strcmp(buf, want));
}

static void test_dirname(void)
{
    expect_dirname(NULL, ".");
    expect_dirname("", ".");
    expect_dirname("prog", ".");
    expect_dirname("/", "/");
    expect_dirname("/bin/prog", "/bin");
    expect_dirname("/bin///prog", "/bin");
    expect_dirname("/bin///", "/");
    expect_dirname("a/b/c", "a/b");
}

int main(void)
{
    test_basename();
    test_dirname();
    return 0;
}
