#!/usr/bin/env python3
"""Reject FP/SIMD use outside the AArch64 exception save/restore assembly."""
import re
import subprocess
import sys

def violations(disassembly):
    function = ''
    for line in disassembly.splitlines():
        symbol = re.match(r'^[0-9a-f]+ <([^>]+)>:', line)
        if symbol:
            function = symbol[1]
        if re.search(r'\.(?:word|byte|short|long|quad)\b', line):
            continue  # literal pools are data, even when their bytes spell b0
        insn = re.match(r'^\s*[0-9a-f]+:\s+([a-z][a-z0-9.]*)\s+(.*)', line)
        if not insn:
            continue
        if re.search(r'\b(?:[bhsdqv][0-9]+|fpcr|fpsr)\b', insn[2]):
            if function not in {'aarch64_vectors', 'aarch64_restore'}:
                yield f'{function}: {line.strip()}'

def main():
    if sys.argv[1:] == ['--self-test']:
        assert not list(violations('0000 <aarch64_vectors>:\n 0: stp q0, q1, [sp]'))
        for insn in ('fadd d0, d1, d2', 'mrs x0, fpcr', 'movi v0.16b, #0',
                     'str q1, [sp]', 'fmov s0, #1.0'):
            assert list(violations('0000 <kernel_c>:\n 0: ' + insn)), insn
        print('aarch64-kernel-nofp: detector self-test PASS')
        return 0
    output = subprocess.check_output([sys.argv[1], '-d', '--no-show-raw-insn',
                                      sys.argv[2]], text=True)
    bad = list(violations(output))
    if bad:
        print('\n'.join(bad[:30]), file=sys.stderr)
        return 1
    if '<aarch64_vectors>:' not in output or '<aarch64_restore>:' not in output:
        raise RuntimeError('Expected AArch64 kernel context symbols are missing')
    print('aarch64-kernel-nofp: PASS')
    return 0

if __name__ == '__main__':
    sys.exit(main())
