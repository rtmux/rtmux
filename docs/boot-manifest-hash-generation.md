# Design spec: build-time generation of boot-manifest image hashes

Status: IMPLEMENTED (Option B). `scripts/gen-boot-manifest-hashes.py`
writes `boot-manifest.hashes` at the boot-cpio root during the `initramfs` build;
`bootstrap_cap_manifest_load_hashes` (bootstrap-manifest.c) loads it via
`cpio_find_file` at roottask boot, fail-closed, and `grant_for` keys the
provenance check on that table. The manifest is now policy-only. Validated: both
configs boot, `make check-boot-manifest` verifies the generated file against the
shipped binaries, host tests inject synthetic hashes, and a tampered-hash boot
fails closed to a rescue shell. A `make indent` reformat no longer needs any
manual hash edit.

## Problem

`servers/roottask/sys/bootstrap-manifest.c` pins a per-server FNV `image_hash`
for every boot executable, plus a `BOOTSTRAP_CAP_MANIFEST_HASH` fingerprint of
the whole table. The loader (`proc-loader.c`) fails closed with `ESTALE` when a
spawned boot binary's hash does not match, which drops boot to a rescue shell.

These hashes are hand-maintained, and the boot binaries embed source-line-derived
bytes, so **any** edit to a boot server — including whitespace/comment
reformatting from `make indent` — changes its bytes and invalidates its pinned
hash. Every such edit then requires a manual four-point regeneration:

1. the server's `.image_hash` in `bootstrap-manifest.c`,
2. the `BOOTSTRAP_CAP_MANIFEST_HASH` self-hash,
3. the matching row and fingerprint in `docs/boot-manifest.md`,
4. the hardcoded copy in `tests/host/test-bootstrap-manifest.c`.

A single forgotten step silently bricks boot. Recent history shows this is not
hypothetical: one editing session regenerated hashes for nine distinct binaries
across four separate rounds. The manifest *policy* (which server gets which
`priv_caps`) is stable; only the *hashes* churn.

## Goal

Make the image hashes a build artifact derived from the actually-shipped
binaries, so no human edits a hash and a mismatch is impossible by construction.
Keep the hand-authored policy table (server, `exec_path`, `priv_caps`, `flags`)
as the source of truth — that is real policy and does not churn.

## Constraints from the current build

- The shipped binaries are **stripped** during the `initramfs` install step, so a
  correct hash must be computed over `build/initramfs/<exec_path>`, not the
  pre-strip object.
- `roottask` embeds the manifest and is built *after* the other servers
  (`Makefile:157`), but the stripped binaries do not exist until the `initramfs`
  target runs (`Makefile:193`), which itself depends on `roottask`. So hashes of
  the final binaries are not available at the time `roottask` is compiled — this
  is the core ordering problem.
- The boot cpio (initramfs) is already the root of trust: it is kernel-loaded,
  read-only, and served by `cpiofs`, a roottask-internal path service. The
  provenance manifest exists to reject a *mounted imposter* filesystem, not the
  boot cpio itself.

## Option A — compile-time header, roottask built last

Emit a generated header (e.g. `internal/include/generated/bootstrap-image-hashes.h`)
of `#define`d hashes and the computed fingerprint; `bootstrap-manifest.c`
references them by symbol instead of literals.

Build phases become:

1. build + strip + install every non-roottask boot binary into `build/initramfs/`,
2. run a generator that FNV-hashes each `exec_path` and writes the header,
3. build + install `roottask` (now compiled against the header),
4. pack `initramfs.cpio`.

- Pros: hashes remain compiled-in constants (strongest tamper story); no runtime
  parsing.
- Cons: requires splitting the build so `roottask` installs *after* the other
  servers are stripped, with a hash-gen step in between — a non-trivial Makefile
  reordering. `roottask`'s own bytes must not be self-referential (it is not
  hash-checked, so this holds).

## Option B — generated hash file in the initramfs (recommended)

Keep the policy table compiled into `roottask`, but move the hashes out of it.
At build time, after all binaries are installed and stripped, a generator writes
a small read-only file into the image, e.g. `build/initramfs/etc/boot-manifest.hashes`,
mapping `exec_path -> fnv64`. At boot, `roottask` reads it through `cpiofs`
(its own trusted service) before spawning the gated servers and uses those
hashes in the provenance check.

Build phases stay almost as today:

1. build + install + strip all binaries (unchanged),
2. generate `etc/boot-manifest.hashes` from `build/initramfs/*` (new step, before cpio pack),
3. pack `initramfs.cpio` (unchanged).

- Pros: no build reordering and no `roottask` rebuild; eliminates every manual
  hash edit; the file is generated from the same binaries the loader will check,
  so it cannot drift.
- Cons: adds a small parser + fail-closed loader to `roottask` (missing/corrupt
  file → refuse all path-hash grants). One-time code cost.
- Trust: the hash file lives in the same read-only boot cpio as the binaries and
  is served only by `cpiofs` (roottask-internal), so an imposter mounted
  filesystem cannot supply it — the same trust boundary the compiled-in constants
  rely on. No weakening versus today.

Recommendation: **Option B.** It removes the ordering hazard entirely and is the
smaller, lower-risk change; the added roottask parser is bounded and fail-closed.

## What stays hand-authored

- The policy rows: `server`, `exec_path`, `priv_caps`, `flags`. These are the
  security decision and must remain reviewed source, not generated.
- The generator only ever fills in `image_hash` values; it never invents policy.

## Gate and docs changes

- `scripts/check-boot-manifest.py` stops comparing against hardcoded literals and
  instead verifies the generated hashes match `build/initramfs/*` (a tautology
  that mainly catches a stale generator) and that every policy `exec_path` has a
  hash. The doc-token and host-fixture checks for literal hashes are dropped.
- `docs/boot-manifest.md` stops listing per-row hash literals (or lists them from
  the generated source), so it no longer needs manual edits.
- `tests/host/test-bootstrap-manifest.c` stops hardcoding real image hashes; it
  keeps testing the grant *logic* (path keying, unlisted-path denial,
  rights mapping) with synthetic fixtures, which is what it should test anyway.

## Migration steps

1. Add the generator script and wire the new build step (Option B: after install,
   before cpio pack).
2. Have `roottask` load and fail-closed-verify the hash file at startup; route
   the provenance check through it.
3. Convert `bootstrap-manifest.c` rows to policy-only (drop `image_hash` literals
   and the self-hash define).
4. Update the gate, doc, and host test as above.
5. Delete this fragility note's TODO once green on `make check-boot-manifest`,
   both default and `CONFIG_LKL_SERVERS=y`.

## Residual risk / open questions

- Reproducibility still matters for *offline* verification (an auditor rebuilding
  and comparing): the binaries must build deterministically for a given toolchain.
  That is orthogonal to this change but worth stating in the evidence kit.
- If a future requirement needs the hashes provable from compiled code alone
  (no data file), revisit Option A.
