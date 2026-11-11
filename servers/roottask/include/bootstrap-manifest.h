/*
 * Static bootstrap capability manifest.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef BOOTSTRAP_MANIFEST_H
#define BOOTSTRAP_MANIFEST_H

#include <stddef.h>
#include <stdint.h>

typedef struct bootstrap_cap_grant {
    const char *server;

    /* Trusted match key: the absolute resolved path of the server executable in
     * the read-only boot image. The grant lookup keys on this, not on the
     * basename, so a same-named imposter loaded from a writable filesystem does
     * not match. NULL for boot-image entries that are never spawned through the
     * loader (roottask, coresrv, namesrv).
     */
    const char *exec_path;
    uint64_t priv_caps;
    uint32_t flags;
} bootstrap_cap_grant_t;

#define BOOTSTRAP_GRANT_BOOTSTRAP 0x00000001U
#define BOOTSTRAP_GRANT_STEADY_STATE 0x00000002U
#define BOOTSTRAP_GRANT_KNOWN_FLAGS \
    (BOOTSTRAP_GRANT_BOOTSTRAP | BOOTSTRAP_GRANT_STEADY_STATE)

int bootstrap_cap_manifest_validate(const char **diag);

/* Load the per-image expected hashes from the generated boot-manifest.hashes at
 * the root of the boot cpio (base/size from cpiofs). Must be called once, after
 * cpiofs_init and before any boot server is spawned.
 *
 * Returns EOK, or an errno on a missing/malformed file; a failure is fatal
 * (fail-closed: without the table, every path-hash grant is denied).
 */
int bootstrap_cap_manifest_load_hashes(const void *cpio_base, size_t cpio_size);

/* priv_caps the manifest grants an executable by its absolute resolved path, or
 * 0 when the path is not listed. The spawn path passes the path the loader
 * actually opened (not the caller-supplied name), so the grant binds to a
 * trusted read-only image rather than a basename.
 */
int bootstrap_cap_manifest_grant_for(const char *exec_path,
                                     uint64_t image_hash,
                                     uint64_t *priv_caps);

/* Nonzero when the manifest lists exec_path, whatever caps it grants. */
int bootstrap_cap_manifest_listed(const char *exec_path);

#endif
