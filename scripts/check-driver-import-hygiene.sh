#!/bin/sh
set -eu

# Fail the gate if a search root is missing rather than letting `|| true`
# silently turn a typo/rename into a green check.
for root in include kernel lib userland servers; do
    test -d "$root" || { echo "driver hygiene: missing search root: $root" >&2; exit 1; }
done

# grep returns 1 on no-match (clean); `|| true` keeps that from tripping `set -e`.
# grep -H forces filename prefixes so the path-based exclusions below work even
# when find -exec hands grep a single file.

# Fail the gate with a header plus the offending matches when any were found.
fail_if_matched()
{
    matched=$1
    test -n "$matched" || return 0
    echo "driver hygiene: $2" >&2
    echo "$matched" >&2
    exit 1
}

bad=$(find include -type f -name '*.h' \
    -exec grep -HnE '^#[[:space:]]*define[[:space:]]+(BIT|ARRAY_SIZE|container_of)[[:space:]]*[[:space:](]' {} + || true)
fail_if_matched "$bad" "common helper macro leaked into public include/"

bad=$(find kernel lib userland servers include -type f \
    \( -name '*.c' -o -name '*.h' -o -name Makefile \) \
    -exec grep -HnE '(^|[^a-zA-Z0-9_])-Wno-|pragma[[:space:]]+(GCC|clang)[[:space:]]+diagnostic' {} + || true)
fail_if_matched "$bad" "warning suppression outside imported subtree"

bad=$(find kernel lib userland servers include -type f \
    \( -name '*.c' -o -name '*.h' \) \
    -exec grep -HnE '^#[[:space:]]*define[[:space:]]+[A-Z0-9_]*(MMIO|_REG_|REG_|CSR)[A-Z0-9_]*[[:space:]]+\(?[[:space:]]*0[xX][0-9A-Fa-f]+' {} + |
    grep -vE '^(arch/|userland/dev-|servers/pci/|servers/lkl/|include/pci/)' || true)
fail_if_matched "$bad" "raw register/MMIO define outside owning driver or arch shim"
