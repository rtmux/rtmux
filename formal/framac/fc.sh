#!/bin/sh
# Frama-C front end for the RTMux kernel.  Usage: formal/framac/fc.sh <args...>
#
# Mirrors kernel/Makefile's include path and -DKERNEL_INTERNAL.  Deviations
# from the real build, each of which bounds what a proof here means:
#   -machdep gcc_rv64     machdep_gcc_rv64.yaml next to this script, installed
#                         into Frama-C's share dir on first use.  Frama-C ships
#                         no RISC-V machdep, and the nearest one (gcc_x86_64)
#                         models char as signed while RISC-V is unsigned; that
#                         is not a caveat but a different program, since
#                         <sys/compiler-lp64.h> branches on __CHAR_UNSIGNED__
#                         and picks a different CHAR_MIN.
#   -std c23              gcc defaults to gnu23 and the tree relies on it.
#   -D_GCC_NULLPTR_T      hides nullptr_t, which Frama-C cannot type.
#   -include prelude.h    erases C11 atomics and defines errno as a macro;
#                         see shim/prelude.h.
set -e
TOP=$(cd "$(dirname "$0")/../.." && pwd)
SHIM=$TOP/formal/framac/shim
CC=${CROSS_COMPILE:-riscv-none-elf-}gcc
GCC_INC=$($CC -print-file-name=include)

# Frama-C loads machdeps only from its own share directory, so the rv64 model
# has to be placed there.  Idempotent, and it announces itself rather than
# silently writing into an opam switch.
MACHDEP_SRC=$TOP/formal/framac/machdep_gcc_rv64.yaml
MACHDEP_DST=$(frama-c -print-share-path)/machdeps/machdep_gcc_rv64.yaml
if ! cmp -s "$MACHDEP_SRC" "$MACHDEP_DST" 2>/dev/null; then
    if cp "$MACHDEP_SRC" "$MACHDEP_DST" 2>/dev/null; then
        echo "fc.sh: installed rv64 machdep into $MACHDEP_DST" >&2
    else
        echo "fc.sh: cannot write $MACHDEP_DST; falling back to gcc_x86_64," \
             "which models char as SIGNED and RISC-V does not" >&2
        MACHDEP=gcc_x86_64
    fi
fi

exec frama-c -std c23 -machdep "${MACHDEP:-gcc_rv64}" -no-frama-c-stdlib \
    -cpp-extra-args="-nostdinc -I$SHIM -isystem $GCC_INC \
-I$TOP/internal/include -I$TOP/lib/common -I$TOP/kernel \
-I$TOP/arch/riscv/include -I$TOP/include -I$TOP/kernel/include \
-I$TOP/servers/roottask/mem -I$TOP/servers/roottask/mem/riscv \
-I$TOP/servers/roottask/include \
-DKERNEL_INTERNAL -D_GCC_NULLPTR_T \
-D__riscv -D__riscv_xlen=64 -include $SHIM/prelude.h" \
    "$@"
