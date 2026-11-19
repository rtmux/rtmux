#!/usr/bin/env python3
# RTMux is freely redistributable under the Apache License. See the file
# "LICENSE" for information on usage and redistribution of this file.
#
# SPDX-License-Identifier: Apache-2.0
"""Inspect and modify the CV18xx/SG200x fip.bin used by the Milk-V Duo.

The layout follows pack_param1(), pack_param2() and pack_uboot() in
https://github.com/sophgo/fiptool. All "cksum" fields there are CRC-16/XMODEM
followed by the bytes FE CA.

  fip.py parse <fip.bin> [--extract DIR]
  fip.py replace-opensbi <fip.bin> <fw_dynamic.bin> <out>
  fip.py patch-env <fip.bin> <var> <value> <out>

replace-opensbi swaps the MONITOR section and moves LOADER_2ND behind it.
patch-env rewrites one variable of the U-Boot default environment in place,
space-padded to its old length, and recompresses LOADER_2ND.
"""
import argparse
import binascii
import lzma
import os
import struct
import sys

PARAM1_MAGIC = b'CVBL01\n\0'
PARAM2_MAGIC = b'CVLD02\n\0'
LOADER_MAGIC = b'B3MA'
PARAM2_FIELDS = 17
PARAM2_RESERVED = 4016


def cksum(data):
    return binascii.crc_hqx(data, 0).to_bytes(2, 'little') + b'\xFE\xCA'


def u32(b, off):
    return struct.unpack_from('<I', b, off)[0]


def pad512(data):
    return data + b'\0' * (-len(data) % 512)


def lzma_compress(data):
    z = lzma.LZMACompressor(lzma.FORMAT_ALONE, preset=lzma.PRESET_EXTREME)
    return z.compress(data) + z.flush()


class Fip:
    def __init__(self, path):
        self.b = bytearray(open(path, 'rb').read())
        b = self.b
        if b[0:8] != PARAM1_MAGIC:
            sys.exit(f'{path}: bad Param1 magic {bytes(b[0:8])!r}')
        # Param1 DATA: NAND_INFO(128) NOR_INFO(36) FIP_FLAG(8) CHIP_CONF_SIZE,
        # BLCP_IMG_CKSUM/SIZE/RUNADDR, BLCP_PARAM_LOADADDR/SIZE, BL2_IMG_CKSUM,
        # BL2_IMG_SIZE, BLD_IMG_SIZE, PARAM2_LOADADDR, REVERSED1, then
        # CHIP_CONF, BL_EK(32), ROOT_PK(512), BL_PK(512).
        o = 16 + 128 + 36 + 8
        self.chip_conf_size = u32(b, o)
        o += 4 + 4 * 5
        self.bl2_cksum = bytes(b[o:o + 4])
        self.bl2_size = u32(b, o + 4)
        self.p2 = u32(b, o + 12)
        self.p1_data_len = (o + 20 - 16) + self.chip_conf_size + 32 + 512 + 512
        self.bl2_off = 16 + self.p1_data_len + 4 * 512
        if b[self.p2:self.p2 + 8] != PARAM2_MAGIC:
            sys.exit(f'{path}: bad Param2 magic')
        self.f = list(struct.unpack_from(f'<{PARAM2_FIELDS}I', b, self.p2 + 12))
        self.ldr = self.f[14]
        if b[self.ldr + 4:self.ldr + 8] != LOADER_MAGIC:
            sys.exit(f'{path}: bad LOADER_2ND magic')
        self.ldr_size = u32(b, self.ldr + 12)

    def write_param2(self):
        d = self.p2 + 12
        struct.pack_into(f'<{PARAM2_FIELDS}I', self.b, d, *self.f)
        self.b[self.p2 + 8:self.p2 + 12] = cksum(
            bytes(self.b[d:d + PARAM2_FIELDS * 4 + PARAM2_RESERVED]))

    def section(self, idx):
        ck, la, sz = self.f[idx], self.f[idx + 1], self.f[idx + 2]
        return ck, la, sz, bytes(self.b[la:la + sz])

    def uboot_lzma(self):
        return bytes(self.b[self.ldr + 32:self.ldr + self.ldr_size])


def check(name, stored, data):
    ok = stored == cksum(data)
    print(f'  {name:<12} cksum {"OK" if ok else "MISMATCH"}')
    return ok


def cmd_parse(args):
    fip = Fip(args.fip)
    b = fip.b
    print(f'{args.fip}: {len(b)} bytes')
    good = check('PARAM1', bytes(b[12:16]), bytes(b[16:16 + fip.p1_data_len]))
    fsbl = bytes(b[fip.bl2_off:fip.bl2_off + fip.bl2_size])
    print(f'  FSBL       @0x{fip.bl2_off:x} size 0x{fip.bl2_size:x}')
    good &= check('FSBL', fip.bl2_cksum, fsbl)
    good &= check('PARAM2', bytes(b[fip.p2 + 8:fip.p2 + 12]),
                  bytes(b[fip.p2 + 12:fip.p2 + 12 + PARAM2_FIELDS * 4 + PARAM2_RESERVED]))
    parts = {'fsbl.bin': fsbl}
    for name, idx, out in (('DDR_PARAM', 1, 'ddr_param.bin'),
                           ('RTOS', 5, 'rtos.bin'),
                           ('MONITOR', 9, 'opensbi.bin')):
        ck, la, sz, blob = fip.section(idx)
        print(f'  {name:<10} @0x{la:x} size 0x{sz:x}')
        if sz:
            good &= check(name, ck.to_bytes(4, 'little'), blob)
        parts[out] = blob
    L = fip.ldr
    print(f'  LOADER_2ND @0x{L:x} size 0x{fip.ldr_size:x}')
    good &= check('LOADER_2ND', bytes(b[L + 8:L + 12]), bytes(b[L + 12:L + fip.ldr_size]))
    lz = fip.uboot_lzma()
    parts['uboot.lzma'] = lz
    try:
        parts['uboot.bin'] = lzma.LZMADecompressor(lzma.FORMAT_ALONE).decompress(lz)
        print(f'  U-Boot     {len(parts["uboot.bin"])} bytes uncompressed')
    except lzma.LZMAError as e:
        good = False
        print(f'  U-Boot     LZMA error: {e}')
    print('all checksums OK' if good else 'CHECKSUM MISMATCH')
    if args.extract:
        os.makedirs(args.extract, exist_ok=True)
        for n, blob in parts.items():
            open(os.path.join(args.extract, n), 'wb').write(blob)
    return 0 if good else 1


def cmd_replace_opensbi(args):
    fip = Fip(args.fip)
    opensbi = pad512(open(args.opensbi, 'rb').read())
    mon_la, mon_sz = fip.f[10], fip.f[11]
    if mon_la + mon_sz != fip.ldr or fip.ldr + fip.ldr_size != len(fip.b):
        sys.exit('expected MONITOR followed by LOADER_2ND at the end of the file')
    loader = bytes(fip.b[fip.ldr:])
    fip.f[9] = u32(cksum(opensbi), 0)
    fip.f[11] = len(opensbi)
    fip.f[14] = mon_la + len(opensbi)
    fip.write_param2()
    out = bytes(fip.b[:mon_la]) + opensbi + loader
    open(args.out, 'wb').write(out)
    print(f'MONITOR 0x{mon_sz:x} -> 0x{len(opensbi):x} bytes; {args.out}: {len(out)} bytes')
    return 0


def cmd_patch_env(args):
    fip = Fip(args.fip)
    if fip.ldr + fip.ldr_size != len(fip.b):
        sys.exit('expected LOADER_2ND at the end of the file')
    uboot = bytearray(lzma.LZMADecompressor(lzma.FORMAT_ALONE).decompress(fip.uboot_lzma()))
    key = b'\0' + args.var.encode() + b'='
    if uboot.count(key) != 1:
        sys.exit(f'{args.var}= found {uboot.count(key)} times in U-Boot')
    start = uboot.index(key) + len(key)
    end = uboot.index(b'\0', start)
    new = args.value.encode()
    if len(new) > end - start:
        sys.exit(f'new value is {len(new)} bytes, only {end - start} available')
    uboot[start:end] = new.ljust(end - start, b' ')
    lz = lzma_compress(bytes(uboot))
    lz += b'\0' * ((-len(lz) - 0x20) % 512)
    L = fip.ldr
    data = struct.pack('<I', len(lz) + 0x20) + bytes(fip.b[L + 16:L + 24]) + b'\0' * 8 + lz
    out = bytes(fip.b[:L]) + struct.pack('<I', 0) + LOADER_MAGIC + cksum(data) + data
    open(args.out, 'wb').write(out)
    print(f'{args.var}: {len(new)} of {end - start} bytes used; {args.out}: {len(out)} bytes')
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = ap.add_subparsers(dest='cmd', required=True)
    p = sub.add_parser('parse')
    p.add_argument('fip')
    p.add_argument('--extract')
    p.set_defaults(fn=cmd_parse)
    p = sub.add_parser('replace-opensbi')
    p.add_argument('fip')
    p.add_argument('opensbi')
    p.add_argument('out')
    p.set_defaults(fn=cmd_replace_opensbi)
    p = sub.add_parser('patch-env')
    p.add_argument('fip')
    p.add_argument('var')
    p.add_argument('value')
    p.add_argument('out')
    p.set_defaults(fn=cmd_patch_env)
    args = ap.parse_args()
    return args.fn(args)


if __name__ == '__main__':
    sys.exit(main())
