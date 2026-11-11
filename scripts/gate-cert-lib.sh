# RTMux is freely redistributable under the Apache License. See the file
# "LICENSE" for information on usage and redistribution of this file.
#
# SPDX-License-Identifier: Apache-2.0
#
# Sourced by the formal gate scripts.  When $GATE_ASSERTIONS names a file (the
# Makefile sets it), each gate appends the facts it observed in tool output;
# scripts/gate_cert.py write turns them into the gate certificate.  Without the
# variable these are no-ops, so a gate still runs standalone.

cert_begin() {
    if [ -n "${GATE_ASSERTIONS:-}" ]; then
        : > "$GATE_ASSERTIONS"
    fi
}

cert_assert() {
    if [ -n "${GATE_ASSERTIONS:-}" ]; then
        printf '%s\n' "$*" >> "$GATE_ASSERTIONS"
    fi
}

cert_result() {
    cert_assert "result: $*"
}
