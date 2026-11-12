/*
 * Static bootstrap capability manifest.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <private/priv-calls.h>
#ifndef RTMUX_BOOTSTRAP_MANIFEST_TEST
#include <sys/cpio.h> /* cpio_find_file: target-only; the host test injects text */
#endif
#include "bootstrap-manifest.h"

#ifndef EOK
#define EOK 0
#endif

/* Per-image expected hashes are not compiled in: they are generated at build
 * time from the shipped binaries and loaded at boot from boot-manifest.hashes
 * (see docs/boot-manifest-hash-generation.md). The table below is policy only
 * (which path gets which caps); the hashes below live in this runtime table,
 * filled by bootstrap_cap_manifest_load_hashes. Fixed capacity, no dynamic
 * allocation (fault-recovery gate): MANIFEST_IMG_HASH_MAX is an architectural
 * bound >= the exec_path row count, MANIFEST_IMG_PATH_MAX bounds a boot path.
 */
#define MANIFEST_IMG_HASH_MAX 48
#define MANIFEST_IMG_PATH_MAX 64

static struct manifest_img_hash {
    char path[MANIFEST_IMG_PATH_MAX];
    uint64_t hash;
} manifest_img_hash[MANIFEST_IMG_HASH_MAX];
static size_t manifest_img_hash_n;
static int manifest_img_hash_loaded;

static const bootstrap_cap_grant_t bootstrap_cap_manifest[] = {
    {
        .server = "roottask",
        .priv_caps = RTMUX_PRIV_CAP_ALL,
        .flags = BOOTSTRAP_GRANT_BOOTSTRAP,
    },
    {
        .server = "coresrv",
        .priv_caps = RTMUX_PRIV_CAP_PROCESS | RTMUX_PRIV_CAP_VM |
                     RTMUX_PRIV_CAP_PTE | RTMUX_PRIV_CAP_PA |
                     RTMUX_PRIV_CAP_IPC | RTMUX_PRIV_CAP_NOTIFICATION |
                     RTMUX_PRIV_CAP_THREAD | RTMUX_PRIV_CAP_CRED,
        .flags = BOOTSTRAP_GRANT_STEADY_STATE,
    },
    {
        .server = "namesrv",
        .priv_caps = RTMUX_PRIV_CAP_IPC,
        .flags = BOOTSTRAP_GRANT_STEADY_STATE,
    },
    {
        .server = "init",
        .exec_path = "/sbin/init",
        .priv_caps = 0,
        .flags = BOOTSTRAP_GRANT_STEADY_STATE,
    },
    {
        .server = "logger",
        .exec_path = "/sbin/logger",
        .priv_caps = 0,
        .flags = BOOTSTRAP_GRANT_STEADY_STATE,
    },
    {
        .server = "virtio-mmio",
        .exec_path = "/sbin/virtio-mmio",
        .priv_caps = 0,
        .flags = BOOTSTRAP_GRANT_STEADY_STATE,
    },
    {
        .server = "romfs",
        .exec_path = "/bin/romfs",
        .priv_caps = 0,
        .flags = BOOTSTRAP_GRANT_STEADY_STATE,
    },
    {
        .server = "pipe",
        .exec_path = "/bin/pipe",
        .priv_caps = 0,
        .flags = BOOTSTRAP_GRANT_STEADY_STATE,
    },
    {
        .server = "waitfor",
        .exec_path = "/bin/waitfor",
        .priv_caps = 0,
        .flags = BOOTSTRAP_GRANT_STEADY_STATE,
    },
    {
        .server = "getty",
        .exec_path = "/sbin/getty",
        .priv_caps = 0,
        .flags = BOOTSTRAP_GRANT_STEADY_STATE,
    },
    /* Device servers that own a hardware IRQ line: they attach an IST and take
     * the IO privilege, so they need RTMUX_PRIV_CAP_IRQ once the gates move off
     * the root-uid predicate. Keyed by the absolute resolved path in the
     * read-only boot image, so a same-named imposter does not match.
     */
    {
        .server = "dev-nvme",
        .exec_path = "/bin/dev-nvme",
        .priv_caps = RTMUX_PRIV_CAP_IRQ,
        .flags = BOOTSTRAP_GRANT_STEADY_STATE,
    },
    {
        .server = "dev-serial-pl011",
        .exec_path = "/bin/dev-serial-pl011",
        .priv_caps = RTMUX_PRIV_CAP_IRQ,
        .flags = BOOTSTRAP_GRANT_STEADY_STATE,
    },
    {
        .server = "dev-serial-8250",
        .exec_path = "/bin/dev-serial-8250",
        .priv_caps = RTMUX_PRIV_CAP_IRQ,
        .flags = BOOTSTRAP_GRANT_STEADY_STATE,
    },
    {
        .server = "dev-serial-sifive",
        .exec_path = "/bin/dev-serial-sifive",
        .priv_caps = RTMUX_PRIV_CAP_IRQ,
        .flags = BOOTSTRAP_GRANT_STEADY_STATE,
    },
    {
        .server = "pci-server",
        .exec_path = "/sbin/pci-server",
        .priv_caps = RTMUX_PRIV_CAP_IRQ,
        .flags = BOOTSTRAP_GRANT_STEADY_STATE,
    },
    {
        .server = "lkl-fsd",
        .exec_path = "/sbin/lkl-fsd",
        .priv_caps = RTMUX_PRIV_CAP_IRQ,
        .flags = BOOTSTRAP_GRANT_STEADY_STATE,
    },
    {
        .server = "lkl-netd",
        .exec_path = "/sbin/lkl-netd",
        .priv_caps = RTMUX_PRIV_CAP_IRQ,
        .flags = BOOTSTRAP_GRANT_STEADY_STATE,
    },
    /* Debug fault injection: the only holder of RTMUX_PRIV_CAP_INJECT. The
     * binary exists only in a CONFIG_DEBUG_INJECT image, and the grant needs a
     * boot-image hash for it, so in any other image this entry is inert (no
     * file, no hash, fails closed) and the kernel call is unbound anyway.
     */
    {
        .server = "inject",
        .exec_path = "/bin/inject",
        .priv_caps = RTMUX_PRIV_CAP_INJECT,
        .flags = BOOTSTRAP_GRANT_STEADY_STATE,
    },
    /* The tracelogger fixture drains the kernel trace ring, gated on
     * RTMUX_PRIV_CAP_TRACE once trace control moves off the root-uid predicate.
     */
    {
        .server = "tracelogger",
        .exec_path = "/bin/tracelogger",
        .priv_caps = RTMUX_PRIV_CAP_TRACE,
        .flags = BOOTSTRAP_GRANT_STEADY_STATE,
    },
    /* Realtime-latency fixtures that take the IO privilege and attach a real
     * IRQ (irq-latency also allocates the trace ring). Granted the same
     * authorities as a device server so they keep working once the gates move
     * off the root-uid predicate.
     */
    {
        .server = "irq-latency",
        .exec_path = "/bin/irq-latency",
        .priv_caps = RTMUX_PRIV_CAP_IRQ | RTMUX_PRIV_CAP_TRACE,
        .flags = BOOTSTRAP_GRANT_STEADY_STATE,
    },
    {
        .server = "irq-notify",
        .exec_path = "/bin/irq-notify",
        .priv_caps = RTMUX_PRIV_CAP_IRQ,
        .flags = BOOTSTRAP_GRANT_STEADY_STATE,
    },
    {
        .server = "gpio-latency",
        .exec_path = "/bin/gpio-latency",
        .priv_caps = RTMUX_PRIV_CAP_IRQ,
        .flags = BOOTSTRAP_GRANT_STEADY_STATE,
    },
    {
        .server = "preempt-latency",
        .exec_path = "/bin/preempt-latency",
        .priv_caps = RTMUX_PRIV_CAP_IRQ,
        .flags = BOOTSTRAP_GRANT_STEADY_STATE,
    },
};

static int manifest_fail(const char **diag, const char *why, int error)
{
    if (diag)
        *diag = why;
    return error;
}

/* Parse "<abs-path> <hex-hash>\n" lines into the runtime hash table.
 *
 * Returns EOK, or EINVAL on malformed input / overflow; on any error the table
 * is left not-loaded so every path-hash grant fails closed.
 */
static int manifest_parse_hashes(const char *text, size_t len)
{
    size_t i = 0;

    manifest_img_hash_n = 0;
    manifest_img_hash_loaded = 0;
    while (i < len) {
        while (i < len && (text[i] == '\n' || text[i] == '\r'))
            i++;
        if (i >= len)
            break;

        size_t ps = i;
        while (i < len && text[i] != ' ' && text[i] != '\n')
            i++;
        size_t plen = i - ps;
        if (i >= len || text[i] != ' ' || plen == 0 ||
            plen >= MANIFEST_IMG_PATH_MAX)
            return EINVAL;
        i++; /* skip the separator */

        uint64_t h = 0;
        size_t nhex = 0;
        while (i < len && text[i] != '\n' && text[i] != '\r') {
            char c = text[i];
            unsigned d;

            if (c >= '0' && c <= '9')
                d = (unsigned) (c - '0');
            else if (c >= 'a' && c <= 'f')
                d = (unsigned) (c - 'a' + 10);
            else if (c >= 'A' && c <= 'F')
                d = (unsigned) (c - 'A' + 10);
            else
                return EINVAL;
            h = (h << 4) | d;
            nhex++;
            i++;
        }
        if (nhex == 0 || nhex > 16)
            return EINVAL;
        if (manifest_img_hash_n >= MANIFEST_IMG_HASH_MAX)
            return EINVAL;
        memcpy(manifest_img_hash[manifest_img_hash_n].path, text + ps, plen);
        manifest_img_hash[manifest_img_hash_n].path[plen] = '\0';
        manifest_img_hash[manifest_img_hash_n].hash = h;
        manifest_img_hash_n++;
    }
    manifest_img_hash_loaded = 1;
    return EOK;
}

/* Look up the expected image hash for a boot path.
 *
 * Returns 1 and sets *out when found in the loaded table, 0 otherwise (table
 * not loaded, or path not listed).
 */
static int manifest_lookup_hash(const char *exec_path, uint64_t *out)
{
    if (!manifest_img_hash_loaded)
        return 0;
    for (size_t i = 0; i < manifest_img_hash_n; i++) {
        if (!strcmp(manifest_img_hash[i].path, exec_path)) {
            *out = manifest_img_hash[i].hash;
            return 1;
        }
    }
    return 0;
}

#ifndef RTMUX_BOOTSTRAP_MANIFEST_TEST
int bootstrap_cap_manifest_load_hashes(const void *cpio_base, size_t cpio_size)
{
    cpio_file_info_t info;

    if (!cpio_base || cpio_size == 0)
        return EINVAL;
    if (!cpio_find_file((const uint8_t *) cpio_base, cpio_size,
                        "boot-manifest.hashes", &info))
        return ENOENT;
    return manifest_parse_hashes((const char *) info.data, info.filesize);
}
#endif

static int manifest_has_server(const bootstrap_cap_grant_t *grants,
                               size_t ngrants,
                               const char *server)
{
    for (size_t i = 0; i < ngrants; i++) {
        if (grants[i].server && !strcmp(grants[i].server, server))
            return 1;
    }
    return 0;
}

static int manifest_validate_list(const bootstrap_cap_grant_t *grants,
                                  size_t ngrants,
                                  const char **diag)
{
    if (diag)
        *diag = NULL;
    if (!grants || ngrants == 0)
        return manifest_fail(diag, "empty manifest", EINVAL);

    for (size_t i = 0; i < ngrants; i++) {
        if (!grants[i].server || grants[i].server[0] == '\0')
            return manifest_fail(diag, "empty server", EINVAL);
        if (grants[i].priv_caps == 0 && !grants[i].exec_path)
            return manifest_fail(diag, "empty grant", EINVAL);
        if (grants[i].priv_caps & ~RTMUX_PRIV_CAP_ALL)
            return manifest_fail(diag, "unknown capability", EPERM);
        if (grants[i].flags & ~BOOTSTRAP_GRANT_KNOWN_FLAGS)
            return manifest_fail(diag, "unknown grant flag", EPERM);

        for (size_t j = i + 1; j < ngrants; j++) {
            if (grants[j].server && !strcmp(grants[i].server, grants[j].server))
                return manifest_fail(diag, "duplicate server", EEXIST);
            if (grants[i].exec_path && grants[j].exec_path &&
                !strcmp(grants[i].exec_path, grants[j].exec_path))
                return manifest_fail(diag, "duplicate exec_path", EEXIST);
        }
    }
    if (!manifest_has_server(grants, ngrants, "roottask"))
        return manifest_fail(diag, "missing roottask", ENOENT);
    if (!manifest_has_server(grants, ngrants, "coresrv"))
        return manifest_fail(diag, "missing coresrv", ENOENT);
    if (!manifest_has_server(grants, ngrants, "namesrv"))
        return manifest_fail(diag, "missing namesrv", ENOENT);
    return EOK;
}

int bootstrap_cap_manifest_validate(const char **diag)
{
    size_t ngrants =
        sizeof(bootstrap_cap_manifest) / sizeof(bootstrap_cap_manifest[0]);

    return manifest_validate_list(bootstrap_cap_manifest, ngrants, diag);
}

/* Return the priv_caps the manifest grants an executable by its absolute
 * resolved path, or 0 when the path is not listed (the default: no privilege).
 * The spawn path passes the path the loader actually opened, so the grant binds
 * to a trusted read-only image, and computes a child's final priv_caps at its
 * start commit.
 */
int bootstrap_cap_manifest_grant_for(const char *exec_path,
                                     uint64_t image_hash,
                                     uint64_t *priv_caps)
{
    size_t ngrants =
        sizeof(bootstrap_cap_manifest) / sizeof(bootstrap_cap_manifest[0]);

    if (priv_caps)
        *priv_caps = 0;
    if (!exec_path)
        return EOK;
    for (size_t i = 0; i < ngrants; i++) {
        if (bootstrap_cap_manifest[i].exec_path &&
            !strcmp(bootstrap_cap_manifest[i].exec_path, exec_path)) {
            uint64_t expected;

            /* The expected hash is loaded at boot from boot-manifest.hashes. A
             * listed path with no loaded hash, or a mismatch, fails closed.
             */
            if (!manifest_lookup_hash(exec_path, &expected) ||
                expected != image_hash)
                return ESTALE;
            if (priv_caps)
                *priv_caps = bootstrap_cap_manifest[i].priv_caps;
            return EOK;
        }
    }
    return EOK;
}

int bootstrap_cap_manifest_listed(const char *exec_path)
{
    size_t ngrants =
        sizeof(bootstrap_cap_manifest) / sizeof(bootstrap_cap_manifest[0]);

    for (size_t i = 0; exec_path && i < ngrants; i++) {
        if (bootstrap_cap_manifest[i].exec_path &&
            !strcmp(bootstrap_cap_manifest[i].exec_path, exec_path))
            return 1;
    }
    return 0;
}

#ifdef RTMUX_BOOTSTRAP_MANIFEST_TEST
void bootstrap_cap_manifest_test_data(const bootstrap_cap_grant_t **grants,
                                      size_t *ngrants)
{
    *grants = bootstrap_cap_manifest;
    *ngrants =
        sizeof(bootstrap_cap_manifest) / sizeof(bootstrap_cap_manifest[0]);
}

int bootstrap_cap_manifest_validate_test(const bootstrap_cap_grant_t *grants,
                                         size_t ngrants,
                                         const char **diag)
{
    return manifest_validate_list(grants, ngrants, diag);
}

int bootstrap_cap_manifest_parse_hashes_test(const char *text, size_t len)
{
    return manifest_parse_hashes(text, len);
}
#endif
