#!/usr/bin/env python3
"""Check the boot capability manifest, its generated hash file, and gate wiring.

Image hashes are no longer hand-edited: they are generated at build time from
the shipped binaries into boot-manifest.hashes and loaded by roottask at boot
(docs/boot-manifest-hash-generation.md). This gate therefore verifies:

  * bootstrap-manifest.c is policy-only (no compiled-in image hashes);
  * the generated boot-manifest.hashes matches the actual binaries and covers
    every manifest exec_path present in the image;
  * the load/verify path and the build step are wired.
"""
import argparse
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MANIFEST = ROOT / "servers/roottask/sys/bootstrap-manifest.c"
DOC = ROOT / "docs/boot-manifest.md"
IMAGES = {
    "default": ROOT / "build/initramfs",
    "lkl-regress": ROOT / "build/lkl-regress/initramfs",
}

FNV_OFFSET = 1469598103934665603
FNV_PRIME = 1099511628211
MASK64 = (1 << 64) - 1


def fnv1a(data: bytes) -> int:
    h = FNV_OFFSET
    for b in data:
        h ^= b
        h = (h * FNV_PRIME) & MASK64
    return h


def manifest_exec_paths(text: str) -> list[str]:
    body = text.split("bootstrap_cap_manifest[] = {", 1)[1].split("};", 1)[0]
    paths = []
    for block in re.finditer(r"\{(.*?)\}", body, re.S):
        m = re.search(r'\.exec_path\s*=\s*"([^"]+)"', block.group(1))
        if m:
            paths.append(m.group(1))
    return paths


def parse_hash_file(text: str) -> dict:
    out = {}
    for line in text.splitlines():
        line = line.strip()
        if not line:
            continue
        parts = line.split()
        if len(parts) != 2:
            raise ValueError(f"malformed line: {line!r}")
        out[parts[0]] = int(parts[1], 16)
    return out


def require(errors, path, text, token):
    if token not in text:
        errors.append(f"{path}: missing {token}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--image-root', type=Path, action='append', default=[],
                        help='Verify an explicit built image instead of the default output trees')
    args = parser.parse_args()
    images = {str(p): p for p in args.image_root} if args.image_root else IMAGES
    manifest_text = MANIFEST.read_text()
    errors = []

    exec_paths = manifest_exec_paths(manifest_text)
    if not exec_paths:
        errors.append("bootstrap-manifest.c: no exec_path entries")

    # The manifest must be policy-only now: no compiled-in hashes.
    for stale in (".image_hash", "BOOTSTRAP_CAP_MANIFEST_HASH", "manifest_hash_list"):
        if stale in manifest_text:
            errors.append(f"bootstrap-manifest.c: stale hash artifact {stale}")

    # Verify each built image's generated hash file against the real binaries.
    for label, root in images.items():
        hashfile = root / "boot-manifest.hashes"
        if not root.exists():
            if args.image_root:
                errors.append(f"{label}: image root does not exist")
            continue
        if not hashfile.exists():
            errors.append(f"{label}: missing boot-manifest.hashes")
            continue
        try:
            entries = parse_hash_file(hashfile.read_text())
        except ValueError as e:
            errors.append(f"{label}: boot-manifest.hashes {e}")
            continue
        # Every listed entry must match its binary.
        for path, want in entries.items():
            binary = root / path.lstrip("/")
            if not binary.exists():
                errors.append(f"{label}: {path} in hash file but not in image")
                continue
            actual = fnv1a(binary.read_bytes())
            if actual != want:
                errors.append(
                    f"{label}: {path} hash 0x{want:016x} != binary 0x{actual:016x}"
                )
        # Every manifest exec_path present in the image must be covered.
        for path in exec_paths:
            if (root / path.lstrip("/")).exists() and path not in entries:
                errors.append(f"{label}: {path} shipped but absent from hash file")

    # Wiring: generator runs at build, roottask loads + fail-closes, loader keys
    # the grant on the opened image, and the docs describe the boundary.
    makefile = (ROOT / "Makefile").read_text()
    manifest_c = manifest_text
    bootimage = (ROOT / "servers/roottask/sys/bootimage-init.c").read_text()
    loader = (ROOT / "servers/roottask/sys/proc-loader.c").read_text()
    doc_text = DOC.read_text()

    require(errors, "Makefile", makefile, "gen-boot-manifest-hashes.py")
    require(errors, "bootstrap-manifest.c", manifest_c,
            "bootstrap_cap_manifest_load_hashes")
    require(errors, "bootstrap-manifest.c", manifest_c, "manifest_lookup_hash")
    require(errors, "bootstrap-manifest.c", manifest_c, "ESTALE")
    require(errors, "bootimage-init.c", bootimage,
            "bootstrap_cap_manifest_load_hashes(cpio_base, cpio_size)")
    require(errors, "bootimage-init.c", bootimage, "crash();")
    require(errors, "proc-loader.c", loader,
            "bootstrap_cap_manifest_grant_for(buffer, exec_hash")

    # Docs describe the mechanism and every service row, without hash literals.
    doc_rows = set()
    for line in doc_text.splitlines():
        if line.startswith("| ") and not line.startswith("| ---"):
            cell = line.strip().strip("|").split("|")[0].strip(" `")
            if cell and cell != "service":
                doc_rows.add(cell)
    manifest_servers = {
        re.search(r'\.server\s*=\s*"([^"]+)"', b.group(1)).group(1)
        for b in re.finditer(
            r"\{(.*?)\}",
            manifest_text.split("bootstrap_cap_manifest[] = {", 1)[1].split("};", 1)[0],
            re.S,
        )
        if ".server" in b.group(1)
    }
    for server in manifest_servers:
        if server not in doc_rows:
            errors.append(f"docs/boot-manifest.md: missing row {server}")
    for token in ("boot-manifest.hashes", "generated"):
        require(errors, "docs/boot-manifest.md", doc_text, token)

    if errors:
        for error in errors:
            print(error)
        return 1
    print("check-boot-manifest: ok")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
