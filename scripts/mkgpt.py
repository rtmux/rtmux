#!/usr/bin/env python3
# mkgpt.py — write a minimal valid GPT to an image file.
#
# Usage: mkgpt.py [--rtmux N] <image> [partition_mib ...]
#
# Creates:
#   LBA 0:       Protective MBR (one 0xEE partition)
#   LBA 1:       Primary GPT header
#   LBAs 2..33:  Primary partition entry array (128 × 128 bytes)
# Backup header/entries are NOT written — dev-nvme (and most readers)
# only use the primary copy.
#
# Each partition is marked with the Linux-filesystem-data type GUID,
# with name "rtmux-test-pN".  --rtmux N (1-based partition index) marks
# partition N with the RTMux partition type GUID instead and names it
# "rtmux" — the canonical layout matches what mkromfs expects on
# /dev/nvme0n1pN on real hardware.
#
# The image file is NOT resized; caller ensures it's big enough.

import os
import struct
import sys
import uuid
import zlib

LBA_SIZE      = 512
ENTRIES_LBA   = 2
N_ENTRIES     = 128
ENTRY_SIZE    = 128
ENTRIES_BYTES = N_ENTRIES * ENTRY_SIZE           # 16 KiB
ENTRIES_LBAS  = ENTRIES_BYTES // LBA_SIZE        # 32

# Linux filesystem data
TYPE_GUID     = uuid.UUID("0fc63daf-8483-4772-8e79-3d69d8477de4")
# RTMux partition type (custom, encodes RTM + EBCDIC YURIMZ + 1979 etc.)
RTMUX_TYPE_GUID = uuid.UUID("52544d11-322e-4017-bae8-e4d9c9d4e979")

def utf16le(s: str, n_chars: int) -> bytes:
    out = s.encode("utf-16-le")
    out = out[: 2 * n_chars]
    return out + b"\x00" * (2 * n_chars - len(out))

def crc32(buf: bytes) -> int:
    return zlib.crc32(buf) & 0xFFFFFFFF

def write_pmbr(img_size_lbas: int) -> bytes:
    p = bytearray(LBA_SIZE)
    # Partition entry 0: bootable=0, chs_start, type=0xEE, chs_end, lba_start=1,
    # size = img - 1 (clamped to 0xFFFFFFFF)
    size = min(img_size_lbas - 1, 0xFFFFFFFF)
    p[446 + 4] = 0xEE
    struct.pack_into("<I", p, 446 + 8, 1)
    struct.pack_into("<I", p, 446 + 12, size)
    p[510] = 0x55
    p[511] = 0xAA
    return bytes(p)

def build_entries(parts):
    buf = bytearray(ENTRIES_BYTES)
    for i, (start, end, name, type_guid) in enumerate(parts):
        off = i * ENTRY_SIZE
        struct.pack_into("<16s", buf, off, type_guid.bytes_le)
        struct.pack_into("<16s", buf, off + 16, uuid.uuid4().bytes_le)
        struct.pack_into("<Q", buf, off + 32, start)
        struct.pack_into("<Q", buf, off + 40, end)
        struct.pack_into("<Q", buf, off + 48, 0)
        buf[off + 56 : off + 56 + 72] = utf16le(name, 36)
    return bytes(buf)

def build_header(disk_lbas, disk_guid, entries_crc):
    hdr = bytearray(92)
    struct.pack_into("<8s", hdr, 0, b"EFI PART")
    struct.pack_into("<I", hdr, 8, 0x00010000)          # revision 1.0
    struct.pack_into("<I", hdr, 12, 92)                 # header_size
    # crc32 @16 — zero for now, fill in later
    struct.pack_into("<I", hdr, 20, 0)                  # reserved
    struct.pack_into("<Q", hdr, 24, 1)                  # current_lba
    struct.pack_into("<Q", hdr, 32, disk_lbas - 1)      # backup_lba
    struct.pack_into("<Q", hdr, 40, 34)                 # first_usable_lba
    struct.pack_into("<Q", hdr, 48, disk_lbas - 34)     # last_usable_lba
    struct.pack_into("<16s", hdr, 56, disk_guid.bytes_le)
    struct.pack_into("<Q", hdr, 72, ENTRIES_LBA)
    struct.pack_into("<I", hdr, 80, N_ENTRIES)
    struct.pack_into("<I", hdr, 84, ENTRY_SIZE)
    struct.pack_into("<I", hdr, 88, entries_crc)
    struct.pack_into("<I", hdr, 16, crc32(bytes(hdr))) # header CRC32
    # pad to LBA
    return bytes(hdr) + bytes(LBA_SIZE - len(hdr))

def main():
    # Parse --rtmux N (1-based partition index) before the positional args.
    args = sys.argv[1:]
    rtmux_idx = None
    i = 0
    while i < len(args):
        if args[i] == "--rtmux" and i + 1 < len(args):
            rtmux_idx = int(args[i + 1])
            del args[i:i + 2]
            continue
        i += 1

    if len(args) < 2:
        print("usage: mkgpt.py [--rtmux N] <image> <part1_mib> [part2_mib ...]",
              file=sys.stderr)
        sys.exit(1)
    img_path = args[0]
    part_mibs = [int(x) for x in args[1:]]

    size_bytes = os.path.getsize(img_path)
    disk_lbas = size_bytes // LBA_SIZE
    if disk_lbas < 34 + 1:
        print("image too small", file=sys.stderr); sys.exit(1)

    # Layout: first_usable = 34, last_usable = disk-34.  Place partitions
    # back-to-back starting at 34.
    parts = []
    cursor = 34
    for i, mib in enumerate(part_mibs):
        nlba = (mib * 1024 * 1024) // LBA_SIZE
        end = cursor + nlba - 1
        if end > disk_lbas - 34:
            print(f"partition {i+1} exceeds disk capacity", file=sys.stderr)
            sys.exit(1)
        if rtmux_idx is not None and (i + 1) == rtmux_idx:
            parts.append((cursor, end, "rtmux", RTMUX_TYPE_GUID))
        else:
            parts.append((cursor, end, f"rtmux-test-p{i+1}", TYPE_GUID))
        cursor = end + 1

    entries = build_entries(parts)
    entries_crc = crc32(entries)
    disk_guid = uuid.uuid4()
    header_lba = build_header(disk_lbas, disk_guid, entries_crc)
    pmbr = write_pmbr(disk_lbas)

    with open(img_path, "r+b") as f:
        f.seek(0);                   f.write(pmbr)
        f.seek(LBA_SIZE);            f.write(header_lba)
        f.seek(ENTRIES_LBA * LBA_SIZE); f.write(entries)

    print(f"mkgpt: {img_path} — {len(parts)} partition(s), disk GUID "
          f"{disk_guid}")
    for i, (start, end, name, type_guid) in enumerate(parts, 1):
        nlba = end - start + 1
        kind = "rtmux" if type_guid == RTMUX_TYPE_GUID else "linux-data"
        print(f"  p{i}: LBA {start}..{end} ({nlba * LBA_SIZE // (1024*1024)} MiB) "
              f"name=\"{name}\" [{kind}]")

if __name__ == "__main__":
    main()
