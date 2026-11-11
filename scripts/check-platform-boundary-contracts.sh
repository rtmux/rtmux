#!/bin/sh
# Cross-check `platform:` citations against docs/platform-boundary-contracts.md.
#
# usage: check-platform-boundary-contracts.sh [--expect-fail] [--contracts FILE] [FILE ...]
#
# With no FILE, checks docs/rt-budgets.md and docs/formal-claims.md.  A citation
# is a line `platform: <claim-class> PB-ID[, PB-ID...]` or a table cell of the
# same form.  Fails when an id is unknown, a contract row is malformed, or a
# `production` citation names any contract that is not class `production` (a
# gate that has not yet produced hardware evidence does not upgrade a row), or a
# contract lists `make check-fu740-smoke-scripts` (a synthetic-log parser check)
# (named anywhere in the gate, backticked or not, with or without make options)
# as its hardware gate.  With --expect-fail the run
# succeeds only if the check fails with the message named by the one
# `<!-- expect: TEXT -->` line across the contracts and fixture files (negative
# fixtures under tests/platform-boundary/) and every error line contains TEXT,
# so a fixture cannot pass for an unrelated failure.  A `production` contract
# row must also name the hardware and a documented in-repo file in both its
# evidence and gate.  The path must be a regular file (not a directory) that
# exists with no `..` component and no symbolic link on its path, and the
# evidence must not say QEMU/emulated/synthetic.  These are structural
# documentation checks only: nothing here, and no wording, proves a hardware run
# happened.  The root path is quoted so it is never executed, and reaches awk
# through the environment so backslashes in it are not interpreted.  Column
# state does not carry from one input file to the next.  The claim syntax lives in
# scripts/platform-field.ere, shared with rt-budgets-check.py, check-formal-claims.py
# and rt-trace-budget.py.
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$root"

expect_fail=0
contracts=docs/platform-boundary-contracts.md
while [ $# -gt 0 ]; do
    case $1 in
    --expect-fail) expect_fail=1 ;;
    --contracts)
        shift
        contracts=${1:?--contracts needs a file}
        ;;
    --) shift; break ;;
    -*) echo "check-platform-boundary-contracts: unknown option $1" >&2; exit 2 ;;
    *) break ;;
    esac
    shift
done
if [ $# -eq 0 ]; then
    set -- docs/rt-budgets.md docs/formal-claims.md
fi
if [ ! -e "$contracts" ] || { [ -f "$contracts" ] && [ ! -s "$contracts" ]; }; then
    echo "check-platform-boundary-contracts: contracts file is empty or missing: $contracts" >&2
    exit 1
fi
field_ere=$(cat "$root/scripts/platform-field.ere")
# A signal trap that only cleans up would let the script carry on; exit from it
# (EXIT then removes the file) with the conventional 128+signal status.  The
# traps go in before mktemp so no signal can find a file without a handler.
errlog=
trap 'rm -f "$errlog"' EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM
errlog=$(mktemp)

check()
{
    # root and the claim pattern reach awk through the environment: -v would
    # interpret backslash escapes in them
    PB_ROOT=$root PB_FIELD_ERE=$field_ere awk '
    function trim(s) { gsub(/^[ \t]+|[ \t]+$/, "", s); return s }
    function bad(file, n, msg) { printf "%s:%d: %s\n", file, n, msg > "/dev/stderr"; errs++ }
    # a path component `..` leaves the repository; such a path documents nothing
    function escapes(p) { return p ~ /(^|\/)\.\.(\/|$)/ }
    # single-quote s for the shell, so a checkout root with spaces, quotes or
    # metacharacters is data and never a command
    function q(s,    n, a, i, o) {
        n = split(s, a, "\047"); o = a[1]
        for (i = 2; i <= n; i++) o = o "\047\\\047\047" a[i]
        return "\047" o "\047"
    }
    # p names an existing file or directory under the checkout reached without
    # crossing a symbolic link: every component below root must be a real entry,
    # so a link cannot point a documented path outside the tree
    function in_repo(p, want_file,    n, parts, i, cur) {
        if (escapes(p)) return 0
        n = split(p, parts, "/"); cur = root
        for (i = 1; i <= n; i++) {
            if (parts[i] == "" || parts[i] == ".") continue
            cur = cur "/" parts[i]
            if (system("test -e " q(cur) " && test ! -L " q(cur)) != 0) return 0
        }
        # a documented file must be a regular file, not a directory
        if (want_file && system("test -f " q(cur)) != 0) return 0
        return 1
    }
    # a backticked token is a documentary path when it is path-shaped and either
    # has a directory separator or a letter-led extension; a version token such
    # as v0.42 or 1.2.3 is not a file
    function is_doc_path(p) {
        if (p !~ /^[A-Za-z0-9_.-]+(\/[A-Za-z0-9_.\/-]+)?$/) return 0
        if (p ~ /^[vV]?[0-9]+(\.[0-9]+)+$/) return 0
        return p ~ /\// || p ~ /[A-Za-z0-9_-]\.[A-Za-z][A-Za-z0-9]*$/
    }
    # s without its documentary path spans, so a path such as docs/qemu-notes.md
    # is not scanned for emulator words
    function strip_paths(s,    rest, out, p) {
        rest = s; out = ""
        while (match(rest, /`[^`]*`/)) {
            p = substr(rest, RSTART + 1, RLENGTH - 2)
            out = out substr(rest, 1, RSTART - 1) (is_doc_path(p) ? " " : substr(rest, RSTART, RLENGTH))
            rest = substr(rest, RSTART + RLENGTH)
        }
        return out rest
    }
    function gate_ok(g,    rest, p) {
        # every backticked path in a gate must exist inside the repository
        rest = g
        while (match(rest, /`[^`]*`/)) {
            p = substr(rest, RSTART + 1, RLENGTH - 2)
            rest = substr(rest, RSTART + RLENGTH)
            if (p ~ /^[A-Za-z0-9_.-]+\/[A-Za-z0-9_.\/-]+$/ && !in_repo(p))
                return 0
        }
        return 1
    }
    # a backticked repo path in the text that exists (documentation only: this
    # is a shape check, it does not and cannot prove that a hardware run happened)
    function has_path(g,    rest, p) {
        rest = g
        while (match(rest, /`[^`]*`/)) {
            p = substr(rest, RSTART + 1, RLENGTH - 2)
            rest = substr(rest, RSTART + RLENGTH)
            if (is_doc_path(p) && in_repo(p, 1))
                return 1
        }
        return 0
    }
    # every documentary path (a backticked repo path) in g is a regular file in
    # the tree: one valid file does not excuse a missing, linked or directory path
    function all_files(g,    rest, p) {
        rest = g
        while (match(rest, /`[^`]*`/)) {
            p = substr(rest, RSTART + 1, RLENGTH - 2)
            rest = substr(rest, RSTART + RLENGTH)
            if (is_doc_path(p) && !in_repo(p, 1))
                return 0
        }
        return 1
    }
    function claim(file, n, v,    ids, k, i, c) {
        if (v == "n/a") return
        nclaims++
        if (v !~ ("^" field_ere "$")) {
            bad(file, n, "malformed platform field: " v)
            return
        }
        k = split(substr(v, index(v, " ") + 1), ids, /, */)
        c = v; sub(/ .*/, "", c)
        for (i = 1; i <= k; i++) {
            if (!(ids[i] in cls)) {
                bad(file, n, "unknown platform contract " ids[i]); continue
            }
            if (c == "production" && cls[ids[i]] != "production")
                bad(file, n, "production claim cites " cls[ids[i]] " contract " ids[i] \
                    " (a hardware gate does not make a row production)")
        }
    }
    BEGIN {
        root = ENVIRON["PB_ROOT"]; field_ere = ENVIRON["PB_FIELD_ERE"]
        # Emulator words in production evidence.  This is a structural tripwire
        # for an obvious slip, not a test of truth: words cannot prove that a
        # hardware run happened, and prose that denies one is not detected.
        contra = "(^|[^a-z0-9])(qemu|tcg|emulat[a-z]*|simulat[a-z]*|synthetic)([^a-z0-9]|$)"
    }
    FNR == 1 { fileno++; pcol = 0 }
    fileno == 1 {
        if ($0 !~ /^\| PB-/) next
        n = split($0, f, "|")
        if (n != 7) { bad(FILENAME, FNR, "contract row needs 5 columns"); next }
        id = trim(f[2]); c = trim(f[4]); ev = trim(f[5]); g = trim(f[6])
        if (id in cls) bad(FILENAME, FNR, "duplicate contract " id)
        cls[id] = c; ncontracts++
        if (c != "production" && c != "qemu-only" && c != "fixture")
            bad(FILENAME, FNR, id ": class must be production, qemu-only, or fixture")
        if (trim(f[3]) == "" || ev == "" || ev == "none")
            bad(FILENAME, FNR, id ": empty assumption or evidence")
        if (g == "") bad(FILENAME, FNR, id ": empty hardware gate (write none)")
        if (c == "production") {
            lg = tolower(g); le = tolower(strip_paths(ev))
            if (lg ~ /^(none|-|n\/a|tbd|tba|todo|pending|unknown|\?)([^a-z0-9]|$)/)
                bad(FILENAME, FNR, id ": production row needs the hardware run that backs it")
            else if (le ~ contra)
                bad(FILENAME, FNR, id ": production evidence names an emulator, which contradicts a hardware run")
            else if (le ~ /^(tbd|tba|todo|pending|unknown|-|n\/a|\?)([^a-z0-9]|$)/ ||
                     le !~ /(unmatched|fu740|board|hardware|silicon)/ || !has_path(ev))
                bad(FILENAME, FNR, id ": production evidence must name the hardware and a documented file")
            else if (!all_files(ev))
                bad(FILENAME, FNR, id ": production evidence has a documented path that is not a regular file")
            else if (gate_ok(g) && !has_path(g))
                bad(FILENAME, FNR, id ": production gate must name a documented file")
            else if (gate_ok(g) && !all_files(g))
                bad(FILENAME, FNR, id ": production gate has a documented path that is not a regular file")
        }
        if (index(tolower(g), "check-fu740-smoke-scripts"))
            bad(FILENAME, FNR, id ": hardware gate is a synthetic-log parser check, not a hardware run")
        if (!gate_ok(g)) bad(FILENAME, FNR, id ": hardware gate names a missing file")
        next
    }
    tolower($0) ~ /^[ \t]*platform[ \t]*:/ {
        v = $0; sub(/^[ \t]*[Pp][Ll][Aa][Tt][Ff][Oo][Rr][Mm][ \t]*:/, "", v)
        claim(FILENAME, FNR, trim(v)); next
    }
    /^[ \t]*\|/ {
        n = split($0, f, "|")
        # a table with a `platform` column: every cell in it is a claim
        hdr = 0
        for (i = 2; i < n; i++)
            if (tolower(trim(f[i])) == "platform") { hdr = i; break }
        if (hdr) { pcol = hdr; next }
        for (i = 2; i < n; i++) {
            v = trim(f[i])
            if (v ~ /^-+$|^:?-+:?$/) continue
            if (i == pcol || tolower(v) ~ /^(production|qemu-only|qemu only|fixture)[ \t]+pb-/)
                claim(FILENAME, FNR, v)
        }
        next
    }
    { pcol = 0 }
    END {
        if (ncontracts == 0) { print "no PB- contract rows found" > "/dev/stderr"; errs++ }
        if (nclaims == 0) { print "no platform citations found" > "/dev/stderr"; errs++ }
        exit errs ? 1 : 0
    }' "$contracts" "$@"
}

if check "$@" 2>"$errlog"; then rc=0; else rc=1; fi
if [ "$expect_fail" -eq 1 ]; then
    if [ "$rc" -eq 0 ]; then
        echo "check-platform-boundary-contracts: expected a failure, got none" >&2
        exit 1
    fi
    want=$(sed -n 's/^<!-- expect: \(.*\) -->$/\1/p' "$contracts" "$@" | sort -u)
    if [ -z "$want" ] || [ "$(printf '%s\n' "$want" | wc -l)" -ne 1 ]; then
        echo "check-platform-boundary-contracts: need exactly one '<!-- expect: TEXT -->' line" >&2
        exit 1
    fi
    if ! grep -Fq -- "$want" "$errlog"; then
        echo "check-platform-boundary-contracts: failed, but not with '$want':" >&2
        cat "$errlog" >&2
        exit 1
    fi
    if grep -Fv -- "$want" "$errlog" | grep -q .; then
        echo "check-platform-boundary-contracts: failed with errors besides '$want':" >&2
        grep -Fv -- "$want" "$errlog" >&2
        exit 1
    fi
    echo "check-platform-boundary-contracts: ok (rejected as expected)"
    exit 0
fi
if [ "$rc" -ne 0 ]; then
    cat "$errlog" >&2
    exit 1
fi
echo "check-platform-boundary-contracts: ok"
