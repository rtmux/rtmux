#!/usr/bin/env python3
"""Generate boot-manifest.hashes from the shipped boot binaries.

Parses the exec_path entries out of the bootstrap capability manifest and writes
one `<exec_path> <fnv64-hex>` line per entry, hashing the actual file under the
initramfs root. roottask loads this at boot and uses it for the image-provenance
check, so no image hash is ever hand-edited (docs/boot-manifest-hash-generation.md).

The FNV-1a parameters match boot_image_hash_fd() in proc-loader.c and
check-boot-manifest.py.
"""
import argparse
import re
import sys
from pathlib import Path

FNV_OFFSET = 1469598103934665603
FNV_PRIME = 1099511628211
MASK64 = (1 << 64) - 1


def fnv1a(data: bytes) -> int:
    h = FNV_OFFSET
    for b in data:
        h ^= b
        h = (h * FNV_PRIME) & MASK64
    return h


def exec_paths(manifest_text: str) -> list[str]:
    body = manifest_text.split("bootstrap_cap_manifest[] = {", 1)[1].split("};", 1)[0]
    paths = []
    for block in re.finditer(r"\{(.*?)\}", body, re.S):
        m = re.search(r'\.exec_path\s*=\s*"([^"]+)"', block.group(1))
        if m:
            paths.append(m.group(1))
    return paths


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--manifest", required=True)
    ap.add_argument("--root", required=True, help="initramfs staging directory")
    ap.add_argument("--out", required=True)
    args = ap.parse_args()

    root = Path(args.root)
    paths = exec_paths(Path(args.manifest).read_text())
    if not paths:
        print("gen-boot-manifest-hashes: no exec_path entries", file=sys.stderr)
        return 1

    # Some manifest rows are config-conditional (e.g. the LKL servers only ship
    # under CONFIG_LKL_SERVERS=y). Hash every listed binary that is actually in
    # this image; a row with no file is simply absent from the table, so if such
    # a server were ever spawned it would fail closed (no hash -> ESTALE).
    lines = []
    for p in sorted(set(paths)):
        f = root / p.lstrip("/")
        if not f.exists():
            print(f"gen-boot-manifest-hashes: skip absent {p}", file=sys.stderr)
            continue
        lines.append(f"{p} {fnv1a(f.read_bytes()):016x}\n")

    Path(args.out).write_text("".join(lines))
    return 0


if __name__ == "__main__":
    sys.exit(main())
