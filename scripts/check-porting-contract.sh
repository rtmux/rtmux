#!/bin/sh
# Every interface in the omission table of docs/porting.md must stay absent from
# libc: a probe that calls it must fail to link against libc.a, and libc.so must
# not define it. Adding a symbol named in the table fails this gate.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
CC=${CC:-riscv-none-elf-gcc}
LD=${LD:-riscv-none-elf-ld}
NM=${NM:-riscv-none-elf-nm}
PORTING_DOC=${PORTING_DOC:-}
LIBC_A=${LIBC_A:-$ROOT/build/lib/libc/libc.a}
LIBC_SO=${LIBC_SO:-$ROOT/build/lib/libc/libc.so}
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

[ -f "$LIBC_A" ] && [ -f "$LIBC_SO" ] || {
    echo "check-porting-contract: build libc first (make)" >&2
    exit 1
}

names=$(awk -F'|' '
    /^## Omitted interfaces/ { on = 1; next }
    on && /^## / { on = 0 }
    on && $2 ~ /`[A-Za-z_][A-Za-z_0-9]*`/ {
        gsub(/[ `]/, "", $2); print $2
    }' "${PORTING_DOC:-$ROOT/docs/porting.md}")
[ -n "$names" ] || {
    echo "check-porting-contract: no omission rows in the porting table" >&2
    exit 1
}

fail=0
count=0
case "${ARCH:-riscv}" in
    aarch64) arch_cflags='-mcpu=cortex-a53'; arch_ldflags='' ;;
    *) arch_cflags='-march=rv64g -mabi=lp64d'; arch_ldflags='-melf64lriscv' ;;
esac
for name in $names; do
    count=$((count + 1))
    printf 'extern int %s();\nint main(void) { return %s(); }\n' "$name" "$name" \
        > "$TMP/p.c"
    $CC $arch_cflags -ffreestanding -nostdinc \
        -Wno-implicit-function-declaration -c -o "$TMP/p.o" "$TMP/p.c"
    if $LD $arch_ldflags -nostdlib -e main -o "$TMP/p.out" "$TMP/p.o" "$LIBC_A" \
        >"$TMP/link.log" 2>&1; then
        echo "check-porting-contract: $name links against libc.a" >&2
        fail=1
    elif ! grep -Eq "undefined reference to .*${name}'|undefined symbol: ${name}([[:space:]]|$)" "$TMP/link.log"; then
        echo "check-porting-contract: $name failed to link for another reason:" >&2
        cat "$TMP/link.log" >&2
        fail=1
    fi
    if "$NM" -D --defined-only "$LIBC_SO" | awk '{print $3}' | grep -qx "$name"; then
        echo "check-porting-contract: libc.so defines $name" >&2
        fail=1
    fi
done
[ "$fail" -eq 0 ] || exit 1
echo "check-porting-contract: ok ($count omissions)"
