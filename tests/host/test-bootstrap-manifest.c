/*
 * Bootstrap manifest host tests.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <assert.h>
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <private/priv-calls.h>
#include "bootstrap-manifest.h"

#ifndef EOK
#define EOK 0
#endif

void bootstrap_cap_manifest_test_data(const bootstrap_cap_grant_t **grants,
                                      size_t *ngrants);
int bootstrap_cap_manifest_validate_test(const bootstrap_cap_grant_t *grants,
                                         size_t ngrants,
                                         const char **diag);
int bootstrap_cap_manifest_parse_hashes_test(const char *text, size_t len);

static void expect_grant_for(const char *tag,
                             const char *path,
                             uint64_t image_hash,
                             int expected_rc,
                             uint64_t expected_caps,
                             uint64_t *caps)
{
    *caps = UINT64_MAX;

    int rc = bootstrap_cap_manifest_grant_for(path, image_hash, caps);
    if (rc != expected_rc || *caps != expected_caps) {
        fprintf(stderr,
                "grant %s: rc=%d caps=%#llx expected rc=%d caps=%#llx\n",
                tag, rc, (unsigned long long) *caps, expected_rc,
                (unsigned long long) expected_caps);
        abort();
    }
}

#define EXPECT_GRANT_FOR(tag, path, image_hash, expected_rc, expected_caps)    \
    do {                                                                      \
        uint64_t tag##_caps;                                                  \
        expect_grant_for(#tag, path, image_hash, expected_rc, expected_caps,   \
                         &tag##_caps);                                        \
    } while (0)

int main(void)
{
    const char *diag = NULL;
    const bootstrap_cap_grant_t *bootstrap_cap_manifest;
    size_t bootstrap_cap_manifest_len;
    bootstrap_cap_grant_t missing_namesrv[] = {
        {.server = "roottask",
         .priv_caps = RTMUX_PRIV_CAP_SYSTEM,
         .flags = BOOTSTRAP_GRANT_BOOTSTRAP},
        {.server = "coresrv",
         .priv_caps = RTMUX_PRIV_CAP_PROCESS,
         .flags = BOOTSTRAP_GRANT_STEADY_STATE},
    };
    bootstrap_cap_grant_t bad_flags[] = {
        {.server = "roottask",
         .priv_caps = RTMUX_PRIV_CAP_SYSTEM,
         .flags = BOOTSTRAP_GRANT_BOOTSTRAP},
        {.server = "coresrv",
         .priv_caps = RTMUX_PRIV_CAP_PROCESS,
         .flags = BOOTSTRAP_GRANT_STEADY_STATE},
        {.server = "namesrv",
         .priv_caps = RTMUX_PRIV_CAP_IPC,
         .flags = 0x00000004U},
    };
    bootstrap_cap_grant_t duplicate[] = {
        {.server = "roottask",
         .priv_caps = RTMUX_PRIV_CAP_SYSTEM,
         .flags = BOOTSTRAP_GRANT_BOOTSTRAP},
        {.server = "roottask", .priv_caps = RTMUX_PRIV_CAP_VM, .flags = 0},
    };
    bootstrap_cap_grant_t duplicate_exec[] = {
        {.server = "roottask",
         .priv_caps = RTMUX_PRIV_CAP_SYSTEM,
         .flags = BOOTSTRAP_GRANT_BOOTSTRAP},
        {.server = "coresrv",
         .priv_caps = RTMUX_PRIV_CAP_PROCESS,
         .flags = BOOTSTRAP_GRANT_STEADY_STATE},
        {.server = "namesrv",
         .priv_caps = RTMUX_PRIV_CAP_IPC,
         .flags = BOOTSTRAP_GRANT_STEADY_STATE},
        {.server = "first",
         .exec_path = "/bin/first",
         .priv_caps = 0,
         .flags = BOOTSTRAP_GRANT_STEADY_STATE},
        {.server = "second",
         .exec_path = "/bin/first",
         .priv_caps = 0,
         .flags = BOOTSTRAP_GRANT_STEADY_STATE},
    };
    bootstrap_cap_grant_t unknown[] = {
        {.server = "namesrv", .priv_caps = RTMUX_PRIV_CAP_ALL << 1, .flags = 0},
    };
    bootstrap_cap_grant_t empty[] = {
        {.server = "coresrv", .priv_caps = 0, .flags = 0},
    };
    bootstrap_cap_grant_t changed[] = {
        {.server = "roottask",
         .priv_caps = RTMUX_PRIV_CAP_SYSTEM,
         .flags = BOOTSTRAP_GRANT_BOOTSTRAP},
        {.server = "coresrv",
         .priv_caps = RTMUX_PRIV_CAP_PROCESS,
         .flags = BOOTSTRAP_GRANT_STEADY_STATE},
        {.server = "namesrv",
         .priv_caps = RTMUX_PRIV_CAP_IPC | RTMUX_PRIV_CAP_TRACE,
         .flags = BOOTSTRAP_GRANT_STEADY_STATE},
    };

    bootstrap_cap_manifest_test_data(&bootstrap_cap_manifest,
                                     &bootstrap_cap_manifest_len);

    assert(bootstrap_cap_manifest_validate(&diag) == EOK);
    assert(!diag);
    assert(bootstrap_cap_manifest_len >= 3);

    /* grant_for consults the runtime hash table loaded at boot from
     * boot-manifest.hashes; inject a synthetic one so the grant checks below
     * exercise the match / mismatch / not-loaded paths. */
    static const char test_hashes[] = "/bin/dev-nvme 1\n"
                                       "/sbin/pci-server 2\n"
                                       "/sbin/lkl-netd 3\n"
                                       "/bin/tracelogger 4\n"
                                       "/sbin/logger 5\n";
    assert(bootstrap_cap_manifest_parse_hashes_test(
               test_hashes, sizeof(test_hashes) - 1) == EOK);

    assert(!strcmp(bootstrap_cap_manifest[0].server, "roottask"));
    assert(bootstrap_cap_manifest[0].flags == BOOTSTRAP_GRANT_BOOTSTRAP);
    assert(!strcmp(bootstrap_cap_manifest[1].server, "coresrv"));
    assert((bootstrap_cap_manifest[1].priv_caps & RTMUX_PRIV_CAP_PROCESS) != 0);
    assert((bootstrap_cap_manifest[1].priv_caps & RTMUX_PRIV_CAP_PA) != 0);
    assert((bootstrap_cap_manifest[1].priv_caps & RTMUX_PRIV_CAP_TRACE) == 0);
    assert(!strcmp(bootstrap_cap_manifest[2].server, "namesrv"));
    assert(bootstrap_cap_manifest[2].priv_caps == RTMUX_PRIV_CAP_IPC);

    /* Grant lookup keys on the absolute resolved path: IRQ-owning device
     * servers get RTMUX_PRIV_CAP_IRQ, the tracelogger gets
     * RTMUX_PRIV_CAP_TRACE. A bare basename does not match (the imposter
     * defense), and an unlisted path or NULL gets no privilege.
     */
    EXPECT_GRANT_FOR(nvme_irq, "/bin/dev-nvme", 1, EOK, RTMUX_PRIV_CAP_IRQ);
    EXPECT_GRANT_FOR(nvme_stale, "/bin/dev-nvme", 9, ESTALE, 0);
    EXPECT_GRANT_FOR(nvme_zero_hash, "/bin/dev-nvme", 0, ESTALE, 0);
    EXPECT_GRANT_FOR(pci_irq, "/sbin/pci-server", 2, EOK, RTMUX_PRIV_CAP_IRQ);
    EXPECT_GRANT_FOR(lkl_net_irq, "/sbin/lkl-netd", 3, EOK, RTMUX_PRIV_CAP_IRQ);
    EXPECT_GRANT_FOR(trace, "/bin/tracelogger", 4, EOK, RTMUX_PRIV_CAP_TRACE);
    EXPECT_GRANT_FOR(logger, "/sbin/logger", 5, EOK, 0);
    /* A manifest path with no loaded hash fails closed, not silently granted. */
    EXPECT_GRANT_FOR(no_loaded_hash, "/bin/dev-serial-8250", 7, ESTALE, 0);
    EXPECT_GRANT_FOR(bare_basename, "dev-nvme", 0, EOK, 0);
    EXPECT_GRANT_FOR(wrong_path, "/usr/dev-nvme", 0, EOK, 0);
    EXPECT_GRANT_FOR(shmem_path, "/dev/shmem/dev-nvme", 0, EOK, 0);
    EXPECT_GRANT_FOR(missing_server, "no-such-server", 0, EOK, 0);
    EXPECT_GRANT_FOR(null_path, NULL, 0, EOK, 0);

    assert(bootstrap_cap_manifest_validate_test(changed, 3, &diag) == EOK);

    assert(bootstrap_cap_manifest_validate_test(duplicate, 2, &diag) == EEXIST);
    assert(diag);

    assert(bootstrap_cap_manifest_validate_test(duplicate_exec, 5, &diag) ==
           EEXIST);
    assert(diag);

    assert(bootstrap_cap_manifest_validate_test(unknown, 1, &diag) == EPERM);
    assert(diag);

    assert(bootstrap_cap_manifest_validate_test(empty, 1, &diag) == EINVAL);
    assert(diag);

    assert(bootstrap_cap_manifest_validate_test(bad_flags, 3, &diag) == EPERM);
    assert(diag);

    assert(bootstrap_cap_manifest_validate_test(missing_namesrv, 2, &diag) ==
           ENOENT);
    assert(diag);

    return 0;
}
