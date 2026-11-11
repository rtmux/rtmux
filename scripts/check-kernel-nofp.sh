#!/bin/sh
# User FP registers are switched lazily on the assumption that the kernel never
# executes an FP instruction: f0-f31 and fcsr keep the user thread's values
# across a trap. This gate fails if the kernel image contains an FP instruction
# anywhere except the two context routines in arch/riscv/fpcontext.S.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
OBJDUMP=${OBJDUMP:-riscv-none-elf-objdump}
KERNEL=${KERNEL:-$ROOT/build/arch/riscv/rtmux-kernel}

[ -f "$KERNEL" ] || { echo "check-kernel-nofp: build the kernel first (make)" >&2; exit 1; }

bad=$($OBJDUMP -d "$KERNEL" | awk '
    /^[0-9a-f]+ <[^>]+>:$/ { fn = $2; gsub(/[<>:]/, "", fn); next }
    $3 ~ /^csr[rw][sc]?i?$/ && $0 ~ /(fcsr|frm|fflags)/ {
        if (fn != "cpu_fpu_save" && fn != "cpu_fpu_restore")
            print fn ": " $3 " " $4
    }
    $3 ~ /^(fld|fsd|flw|fsw|fmv\.|fadd|fsub|fmul|fdiv|fsqrt|fmin|fmax|fcvt|feq|flt|fle|fclass|fsgnj|fmadd|fmsub|fnmadd|fnmsub|frcsr|fscsr|frrm|fsrm|frflags|fsflags)/ {
        if (fn != "cpu_fpu_save" && fn != "cpu_fpu_restore")
            print fn ": " $3
    }' | sort | uniq -c | head)
if [ -n "$bad" ]; then
    echo "check-kernel-nofp: FP instructions outside cpu_fpu_save/restore:" >&2
    echo "$bad" >&2
    exit 1
fi
echo "check-kernel-nofp: ok"
