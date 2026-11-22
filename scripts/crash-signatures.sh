# SPDX-License-Identifier: Apache-2.0
#
# Log signatures that end a guest run as a failure, shared by the QEMU and the
# FU740 harnesses (POSIX sh, sourced). scripts/check-crash-signatures.sh checks
# them.
#
# Each signature is text only a crash or a fatal setup error prints, never
# errno text: a test may print any strerror() string (syscall_testing prints
# them all), and a run must not end on one.
#   RTMUX_CRASH_RE  the kernel's crash dump (every trap and crash() report is
#                   inside it) and its FATAL lines, roottask's RT:CRASH, and an
#                   LKL kernel's panic.
#   RTMUX_FATAL_RE  the part of RTMUX_CRASH_RE printed outside a crash dump
#                   (for a mode that crashes on purpose and checks its dump).
#   RTMUX_SETUP_RE  init's rescue banner and the dynamic loader's unresolved
#                   symbol.
#   RTMUX_BAD_RE    both.

RTMUX_FATAL_RE='FATAL[: ]|Kernel panic|panic:|FREED-ACTIVE|__kernel_restore bad ptr'
RTMUX_CRASH_RE="=== CRASH DUMP ===|RT:CRASH|lkl-host: panic|$RTMUX_FATAL_RE"
RTMUX_SETUP_RE='/usr unavailable|Undefined symbol'
RTMUX_BAD_RE="$RTMUX_CRASH_RE|$RTMUX_SETUP_RE"

# crash_signature_report LOG RE: print the first and the last lines of LOG
# that match RE, with their line numbers, so a run ended on a signature says
# which line it was. Fails, printing nothing, when no line matches.
crash_signature_report()
{
    crash_sig_lines="$(grep -anE -- "$2" "$1")" || return 1
    crash_sig_first="${crash_sig_lines%%
*}"
    echo "  first signature, log line ${crash_sig_first%%:*}: ${crash_sig_first#*:}"
    printf '%s\n' "$crash_sig_lines" | tail -n 20 | sed 's/^/    /'
}
