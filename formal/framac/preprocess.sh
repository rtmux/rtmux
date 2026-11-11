#!/bin/sh
# Preprocess kernel sources into formal/framac/pp/*.i for Frama-C.
#
# The MCP server cannot pass -no-frama-c-stdlib or -nostdinc, so loading .c
# files through it mixes Frama-C's libc into a freestanding tree that has its
# own headers.  Preprocessing here with the real cross compiler and the real
# build flags removes the question: Frama-C never runs cpp, and the .i keeps
# #line markers so every location still points at the original .c.
#
# Usage: formal/framac/preprocess.sh [file.c ...]   (default: all of kernel/)
set -e
TOP=$(cd "$(dirname "$0")/../.." && pwd)
SHIM=$TOP/formal/framac/shim
OUT=$TOP/formal/framac/pp
CC=${CROSS_COMPILE:-riscv-none-elf-}gcc
GCC_INC=$($CC -print-file-name=include)

mkdir -p "$OUT"
[ $# -gt 0 ] && set -- "$@" || set -- $(find "$TOP/kernel" -name '*.c' | sort)

for src; do
    rel=${src#"$TOP"/}
    rel=${rel#./}
    dst=$OUT/$(echo "${rel%.c}" | tr / -).i
    $CC -E -std=gnu23 -nostdinc -I"$SHIM" -isystem "$GCC_INC" \
        -I"$TOP/internal/include" -I"$TOP/lib/common" -I"$TOP/kernel" \
        -I"$TOP/arch/riscv/include" -I"$TOP/include" -I"$TOP/kernel/include" \
        -I"$TOP/servers/roottask/mem" -I"$TOP/servers/roottask/mem/riscv" \
        -I"$TOP/servers/roottask/include" \
        -DKERNEL_INTERNAL -D_GCC_NULLPTR_T -include "$SHIM/prelude.h" \
        -o "$dst" "$src"
    echo "$dst"
done
