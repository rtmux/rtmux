/* SPDX-License-Identifier: Apache-2.0 */
#include <assert.h>
#include <errno.h>
#include <spawn.h>
#include <stdlib.h>
#include <sys/stat.h>

static unsigned probes;
void __env_lock(void) {}
void __env_unlock(void) {}

static int failing_stat(const char *path, struct stat *st)
{
    (void) path;
    (void) st;
    probes++;
    errno = ENOMEM;
    return -1;
}

/* Include after the host declarations so libc's inline stat ABI cannot
 * bypass the fault injection. */
#define stat(path, st) failing_stat(path, st)
#define posix_spawnp test_spawnp
#include "../../lib/libc/spawn/posix-spawnp.c"

int main(void)
{
    char *argv[] = {"program", NULL};
    pid_t pid = -1;
    assert(setenv("PATH", "/first:/second", 1) == 0);
    assert(posix_spawnp(&pid, "program", NULL, NULL, argv, NULL) == ENOMEM);
    assert(probes == 1 && pid == -1);
    return 0;
}
