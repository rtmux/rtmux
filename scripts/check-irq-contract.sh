#!/bin/sh
set -eu

checklist=docs/driver-review-checklist.md

test -f "$checklist" || {
    echo "missing $checklist" >&2
    exit 1
}

bad=$(find userland servers -type f \
    \( -name '*.c' -o -name '*.h' -o -name Makefile \) \
    -exec grep -nE 'softirq_|workqueue_' {} + || true)
if test -n "$bad"; then
    echo "IRQ contract: banned kernel-context follow-up name found" >&2
    echo "$bad" >&2
    exit 1
fi

files=$(find userland servers -type f \( -name '*.c' -o -name '*.h' \) \
    -exec grep -lE 'interrupt_attach(_thread|_event)?\(' {} + || true)
for f in $files; do
    case "$f" in
        servers/pci/pci-msix.c) continue ;; # capability scaffold, no attach user
    esac
    if ! grep -q "\`$f\`" "$checklist"; then
        echo "IRQ contract: $f uses interrupt_attach* but has no checklist row" >&2
        exit 1
    fi
done
