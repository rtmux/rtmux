#!/bin/sh
# RTMux is freely redistributable under the Apache License. See the file
# "LICENSE" for information on usage and redistribution of this file.
#
# SPDX-License-Identifier: Apache-2.0
#
# SRR deadlock-freedom gate (Q43; docs/ipc-deadlock-freedom.md).
#
#   1. scripts/check-ipc-send-ranks.py: every blocking send call site in the tree
#      carries an `ipc-send:` annotation, and the edges from the annotations plus
#      docs/ipc-send-ranks.tsv all go to a strictly higher rank; the longest
#      wait-for chain is within the PI walk bound.
#   2. tests/spin/ipc-deadlock.pml over the graph generated from them: no invalid
#      end state (deadlock) for every non-empty subset of the clients with sinks
#      omitted, and for every client single and pair with sinks modeled.
#   3. Mutations that must fail: the checked-in cyclic, lower-rank-site and
#      unannotated-site fixtures, a rank-inverted table edge, and scratch trees
#      with an unannotated, a lower-rank, a roottask-core, and a mislabeled site.
#
# Needs Spin, a C compiler and python3 (the reference host has spin 6.5.2).
set -eu

. "$(dirname "$0")/gate-cert-lib.sh"
cert_begin

if ! command -v spin >/dev/null 2>&1; then
    echo "ipc-deadlock: spin not found (apt-get install spin)" >&2
    exit 1
fi
CC=${CC:-cc}
if ! command -v "$CC" >/dev/null 2>&1; then
    echo "ipc-deadlock: C compiler '$CC' not found" >&2
    exit 1
fi

ROOT=$(cd "$(dirname "$0")/.." && pwd)
RANKS="$ROOT/scripts/check-ipc-send-ranks.py"
TABLE="$ROOT/docs/ipc-send-ranks.tsv"
FIX="$ROOT/tests/ipc-deadlock"
CYCLIC="$FIX/cyclic.tsv"
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

fail() {
    echo "ipc-deadlock: $*" >&2
    exit 1
}

# 1. The rank table and the tree scan (records its own assertion).
python3 "$RANKS" || fail "send-rank table rejected"

# 3a. Mutations of the rank check.
# expect_reject <reason> <args...>: the checker must refuse, for that reason.
expect_reject() {
    reason=$1
    shift
    if err=$(python3 "$RANKS" --expect-fail "$@" 2>&1 >/dev/null); then
        case $err in
        *"$reason"*) return 0 ;;
        esac
        fail "rejected for the wrong reason (wanted '$reason'): $err"
    fi
    fail "mutation was accepted ($*)"
}

expect_reject "equal or lower rank" --table-only --table "$CYCLIC"
cert_assert "cyclic fixture rejected by the rank check"
sed 's/^\(pci	[^	]*	\)5	/\11	/' "$TABLE" > "$work/inverted.tsv"
if cmp -s "$TABLE" "$work/inverted.tsv"; then
    fail "rank-inversion mutation did not apply"
fi
expect_reject "equal or lower rank" --table "$work/inverted.tsv"
cert_assert "rank-inverted edge rejected by the rank check"
expect_reject "equal or lower rank" --table "$FIX/lower-rank-site/table.tsv" \
    --root "$FIX/lower-rank-site"
cert_assert "checked-in fixture with an annotated call site to a lower rank rejected, table unedited"
expect_reject "no ipc-send annotation" --table "$FIX/unannotated-site/table.tsv" \
    --root "$FIX/unannotated-site"
cert_assert "checked-in fixture with an unannotated call site rejected"

# Scratch trees against the real table: a new call site must be rejected
# without any table edit.
mkdir -p "$work/tree/servers/rogue"
for d in $(grep -v '^#' "$TABLE" | cut -f2 | tr ',' ' '); do
    mkdir -p "$work/tree/$d"
done
site() { # <file> <annotation or empty>
    printf '%s\nint f(int fd) { return msg_send(fd, 0, 0, 0, 0); }\n' "$2" > "$work/tree/$1"
}
site servers/rogue/x.c ""
expect_reject "no ipc-send annotation" --root "$work/tree"
rm "$work/tree/servers/rogue/x.c"
site userland/romfs/x.c "/* ipc-send: romfs -> utils */"
expect_reject "equal or lower rank" --root "$work/tree"
cert_assert "new call site to a lower rank rejected against the unedited table"
rm "$work/tree/userland/romfs/x.c"
site servers/roottask/x.c "/* ipc-send: roottask-core -> pipe */"
expect_reject "equal or lower rank" --root "$work/tree"
cert_assert "a send labeled roottask-core (the top rank) rejected: roottask-core sends nothing"
rm "$work/tree/servers/roottask/x.c"
site servers/pci/x.c "/* ipc-send: romfs -> roottask-core */"
expect_reject "does not cover this file" --root "$work/tree"
cert_assert "annotation naming a node that does not own the file rejected"
rm "$work/tree/servers/pci/x.c"

# open() gets per-site cover only (Q71c): no file-wide blanket, no member calls,
# and the scan is a whole-file lexer (comments/strings blanked, calls may span
# lines).  Each case writes one file and names the exact file:line: reason, or
# "ok" for a file that must be accepted.
u="$work/tree/userland/utils/x.c"
UR="userland/utils/x.c"
NOTE_OK='/* ipc-send: utils -> romfs */'
NA="blocking send \`open(\` has no ipc-send annotation"
# case_ <label> <ok | file:line: reason>; the fixture source is read from stdin
case_() {
    cat > "$u"
    if [ "$2" = ok ]; then
        python3 "$RANKS" --root "$work/tree" >/dev/null 2>"$work/err" ||
            fail "$1: rejected: $(cat "$work/err")"
    else
        expect_reject "$2" --root "$work/tree"
    fi
    cert_assert "open() scan: $1"
}

case_ "ipc-send-file is rejected as retired" "$UR:1: ipc-send-file is retired" <<FIX_END
/* ipc-send-file: utils -> roottask-core */
int f(const char *p) { return open(p, 0); }
FIX_END
case_ "ipc-send-file does not cover an open()" "$UR:2: $NA" <<FIX_END
/* ipc-send-file: utils -> roottask-core */
int f(const char *p) { return open(p, 0); }
FIX_END
case_ "one annotation does not cover two calls on a line" "$UR:2: $NA" <<FIX_END
$NOTE_OK
int f(const char *p) { return open(p, 0) + open(p, 1); }
FIX_END
case_ "two annotations cover two calls on one line" ok <<FIX_END
$NOTE_OK
$NOTE_OK
int f(const char *p) { return open(p, 0) + open(p, 1); }
FIX_END
case_ "a later open() needs its own annotation" "$UR:4: $NA" <<FIX_END
$NOTE_OK
int f(const char *p) { return open(p, 0); }
int g(void) { return 0; }
int h(const char *p) { return open(p, 0); }
FIX_END
case_ "annotation more than 4 lines above does not reach" "$UR:6: $NA" <<FIX_END
$NOTE_OK
int a;
int b;
int c;
int d;
int f(const char *p) { return open(p, 0); }
FIX_END
case_ "annotated open() accepted" ok <<FIX_END
$NOTE_OK
int f(const char *p) { return open(p, 0); }
FIX_END
case_ "a stale annotation covering no call is rejected" "$UR:1: ipc-send annotation covers no call" <<FIX_END
$NOTE_OK
int f(void) { return 0; }
FIX_END
case_ "a space after a comma in the target list is parsed" "$UR:1: utils (rank" <<FIX_END
/* ipc-send: utils -> romfs, utils */
int f(const char *p) { return open(p, 0); }
FIX_END
case_ "annotation to an equal rank rejected" "$UR:1: utils (rank" <<FIX_END
/* ipc-send: utils -> utils */
int f(const char *p) { return open(p, 0); }
FIX_END
case_ "call after a leading * is scanned" "$UR:2: $NA" <<FIX_END
int f(int *q, const char *p) {
    *q = open(p, 0);
    return 0;
}
FIX_END
case_ "call on the line after a leading * is scanned" "$UR:3: $NA" <<FIX_END
int f(const char *p, int *q) {
    *q =
    open(p, 0);
    return 0;
}
FIX_END
case_ "call after a block comment on the same line" "$UR:1: $NA" <<FIX_END
/* c */ int f(const char *p) { return open(p, 0); }
FIX_END
case_ "open split from its paren across lines" "$UR:2: $NA" <<FIX_END
int f(const char *p) { return
    open
    (p, 0); }
FIX_END
case_ "annotated split open accepted" ok <<FIX_END
$NOTE_OK
int f(const char *p) { return open
    (p, 0); }
FIX_END
case_ "// annotation covers a call" ok <<FIX_END
// ipc-send: utils -> romfs
int f(const char *p) { return open(p, 0); }
FIX_END
case_ "open( in comments is not a call" ok <<FIX_END
/* open(p, 0) is described here
 * open(q, 1) too */
// open(r, 2)
int f(void) { return 0; }
FIX_END
case_ "open( in a string is not a call" ok <<FIX_END
const char *s = "open(p, 0)";
FIX_END
case_ "annotation inside a string covers nothing" "$UR:2: $NA" <<FIX_END
const char *s = "ipc-send: utils -> romfs";
int f(const char *p) { return open(p, 0); }
FIX_END
case_ "annotation inside a string on the call line covers nothing" "$UR:1: $NA" <<FIX_END
int f(const char *p) { const char *s = "/* ipc-send: utils -> romfs */"; return open(p, 0); }
FIX_END
case_ "escaped quote in a string does not end it early" "$UR:1: $NA" <<FIX_END
int f(const char *p) { const char *s = "a\"b"; return open(p, 0); }
FIX_END
case_ "quote char literal does not open a string" "$UR:1: $NA" <<FIX_END
int f(const char *p) { char c = '"'; return open(p, 0); }
FIX_END
case_ "escaped quote char literal does not open a string" "$UR:1: $NA" <<FIX_END
int f(const char *p) { char c = '\''; return open(p, 0); }
FIX_END
case_ "comment opener inside a string does not start a comment" "$UR:1: $NA" <<FIX_END
int f(const char *p) { const char *s = "/*"; return open(p, 0); }
FIX_END
case_ "// inside a string does not hide the call" "$UR:1: $NA" <<FIX_END
int f(const char *p) { const char *s = "//"; return open(p, 0); }
FIX_END
case_ "backslash-newline continues a // comment" ok <<FIX_END
// open(p, 0) \\
open(q, 1)
int f(void) { return 0; }
FIX_END
case_ "unterminated block comment rejected" "$UR:1: unterminated /* comment" <<FIX_END
/* never closed
int f(void) { return 0; }
FIX_END
case_ "unterminated string literal rejected" "$UR:1: unterminated string literal" <<FIX_END
const char *s = "abc;
int f(const char *p) { return open(p, 0); }
FIX_END
case_ "unterminated char literal rejected" "$UR:1: unterminated char literal" <<FIX_END
char c = 'a;
int f(const char *p) { return open(p, 0); }
FIX_END
case_ "C23 digit separator rejected" "$UR:1: C23 digit separator" <<FIX_END
int n = 1'000;
int f(const char *p) { return open(p, 0); }
FIX_END
case_ "hex digit separator rejected" "$UR:1: C23 digit separator" <<FIX_END
int n = 0xFF'FF;
FIX_END
case_ "encoding-prefixed char literals accepted" ok <<FIX_END
int a = L'x', b = u'y', c = U'z', d = u8'w';
FIX_END
REF="used other than as a direct call"
case_ "parenthesized call (open)(...) rejected" "$UR:1: \`open\` $REF" <<FIX_END
int f(const char *p) { return (open)(p, 0); }
FIX_END
case_ "address of open taken (function pointer) rejected" "$UR:1: \`open\` $REF" <<FIX_END
int (*fp)(const char *, int, ...) = open;
FIX_END
case_ "open passed as an argument rejected" "$UR:2: \`open\` $REF" <<FIX_END
int call(int (*)(const char *, int, ...));
int f(void) { return call(open); }
FIX_END
case_ "a macro body naming open rejected" "$UR:1: \`open\` on a preprocessor line" <<FIX_END
#define OPEN(p) open(p, 0)
FIX_END
case_ "a macro that renames open rejected" "$UR:1: \`open\` on a preprocessor line" <<FIX_END
#define sys_open open
FIX_END
case_ "a macro named like a send rejected even when annotated" \
    "$UR:2: \`open\` on a preprocessor line" <<FIX_END
$NOTE_OK
#define open(p, f) my_open(p, f)
FIX_END
case_ "#include names are not directives that hide a send" ok <<FIX_END
#include <open.h>
FIX_END
case_ "a call spelled across a line splice is one token" "$UR:1: $NA" <<FIX_END
int f(const char *p) { return op\\
en(p, 0); }
FIX_END
case_ "an annotated splice-spelled call is accepted" ok <<FIX_END
$NOTE_OK
int f(const char *p) { return op\\
en(p, 0); }
FIX_END
case_ "a splice does not shift later line numbers" "$UR:4: $NA" <<FIX_END
int a = 1 + \\
    2;
int f(const char *p) {
    return open(p, 0); }
FIX_END
case_ "a field declarator named open is not a send" ok <<FIX_END
struct msg { io_open_t open; int dup; };
FIX_END
for w in fopen freopen creat openat opendir open64 creat64; do
    case_ "libc wrapper $w needs an annotation" "$UR:1: blocking send \`$w(\` has no" <<FIX_END
int f(const char *p) { return $w(p, 0) != 0; }
FIX_END
done
case_ "annotated fopen accepted" ok <<FIX_END
$NOTE_OK
int f(const char *p) { return fopen(p, "r") != 0; }
FIX_END
case_ "msg_send_notification is not a blocking send" ok <<FIX_END
int f(int c) { return msg_send_notification(c, 0, 0, 0); }
FIX_END
case_ "a new msg_send variant is not skipped" "$UR:1: blocking send \`msg_send_future(\`" <<FIX_END
int f(int c) { return msg_send_future(c, 0, 0, 0, 0); }
FIX_END
case_ "member calls (->, .) are not open()" ok <<FIX_END
int f(struct ops *o) { return o->open(o, 0) + o . open(o, 1); }
FIX_END
case_ "member call split across lines is not open()" ok <<FIX_END
int f(struct ops *o) { return o->
    open(o, 0) + o
    .open(o, 1); }
FIX_END
case_ "member call does not hide a real open() on the line" "$UR:1: $NA" <<FIX_END
int f(struct ops *o, const char *p) { return o->open(o, 0) + open(p, 0); }
FIX_END
case_ "msg_send still needs its own annotation" \
    "$UR:1: blocking send \`msg_send(\` has no ipc-send annotation" <<FIX_END
int f(int fd) { return msg_send(fd, 0, 0, 0, 0); }
FIX_END
rm "$u"

# The target of open("literal") is derived from the ioserv_attach table: an
# annotation that omits a derived target is rejected, one that names extra
# (conservative) targets is rank-checked only.
cat > "$work/tree/servers/lkl-netd/attach.c" <<'FIX_END'
int init(void) { return ioserv_attach(dpp, 0, "/net", 0, 0, 0, 0, 0); }
FIX_END
DERIVED="utils -> roottask-core,lkl-netd"
case_ "literal /net path with the derived targets accepted" ok <<FIX_END
/* ipc-send: $DERIVED */
int f(void) { return open("/net/dns", 0); }
FIX_END
case_ "literal /net path omitting lkl-netd rejected" \
    "$UR:2: \`open(\"/net/dns\")\` reaches lkl-netd" <<FIX_END
/* ipc-send: utils -> roottask-core */
int f(void) { return open("/net/dns", 0); }
FIX_END
case_ "literal /net path omitting the resolver rejected" \
    "$UR:2: \`open(\"/net/dns\")\` reaches roottask-core" <<FIX_END
/* ipc-send: utils -> lkl-netd */
int f(void) { return open("/net/dns", 0); }
FIX_END
case_ "a concatenated literal path is derived too" \
    "$UR:2: \`open(\"/net/dns\")\` reaches lkl-netd" <<FIX_END
/* ipc-send: utils -> roottask-core */
int f(void) { return open("/net" "/dns", 0); }
FIX_END
case_ "openat derives from its second argument" \
    "$UR:2: \`openat(\"/net/dns\")\` reaches lkl-netd" <<FIX_END
/* ipc-send: utils -> roottask-core */
int f(int d) { return openat(d, "/net/dns", 0); }
FIX_END
case_ "extra conservative targets are accepted" ok <<FIX_END
/* ipc-send: utils -> roottask-core,lkl-netd,romfs */
int f(void) { return open("/net/dns", 0); }
FIX_END
case_ "a sibling prefix (/network) is not under /net" ok <<FIX_END
/* ipc-send: utils -> roottask-core */
int f(void) { return open("/network/x", 0); }
FIX_END
case_ "a path variable stays asserted" ok <<FIX_END
/* ipc-send: utils -> roottask-core */
int f(const char *p) { return open(p, 0); }
FIX_END
rm "$work/tree/servers/lkl-netd/attach.c"

# 2. Spin over a generated graph for one set of clients (bit i of the mask is
# node i); sets PAN_ERRORS to pan's error count.
run_set() { # <dir> <mask>
    cp "$ROOT/tests/spin/ipc-deadlock.pml" "$1/m.pml"
    if ! (cd "$1" && spin -a -DCLIENT_MASK="$2" m.pml) >"$1/spin.log" 2>&1; then
        cat "$1/spin.log" >&2
        fail "Spin generation failed for client mask $2"
    fi
    if ! (cd "$1" && "$CC" -O2 -DCOLLAPSE -o pan pan.c) >"$1/cc.log" 2>&1; then
        cat "$1/cc.log" >&2
        fail "pan compilation failed for client mask $2"
    fi
    if ! out=$(cd "$1" && ./pan -m200000 2>&1); then
        printf '%s\n' "$out" >&2
        fail "pan failed for client mask $2"
    fi
    # A truncated search proves nothing.
    case $out in
    *"search depth too small"* | *"search not completed"* | *"out of memory"*)
        fail "pan search incomplete for client mask $2" ;;
    esac
    PAN_ERRORS=$(printf '%s\n' "$out" | sed -n 's/.*errors: \([0-9][0-9]*\).*/\1/p' | head -n 1)
    [ -n "$PAN_ERRORS" ] || fail "pan produced no error count for client mask $2"
}

# Every mask given must search to completion with no error.
expect_clean() { # <dir> <label> <mask>...
    dir=$1
    label=$2
    shift 2
    for m in "$@"; do
        run_set "$dir" "$m"
        [ "$PAN_ERRORS" = 0 ] || fail "$label: deadlock, client mask $m (errors: $PAN_ERRORS)"
    done
}

# Sinks omitted: any number of clients at once (every non-empty subset).
mkdir "$work/nosink"
GATE_ASSERTIONS= python3 "$RANKS" --omit-sinks --emit-promela "$work/nosink/edges.inc" >/dev/null
clients=$(sed -n 's/^\/\* clients: \(.*\) \*\/$/\1/p' "$work/nosink/edges.inc")
[ -n "$clients" ] || fail "no clients found in the generated graph"
nc=$(echo $clients | wc -w)
[ "$nc" -le 8 ] || fail "too many clients ($nc) to enumerate every subset"
masks=
sub=1
while [ "$sub" -lt $((1 << nc)) ]; do
    mask=0
    i=0
    for c in $clients; do
        [ $(((sub >> i) & 1)) = 1 ] && mask=$((mask | (1 << c)))
        i=$((i + 1))
    done
    masks="$masks $mask"
    sub=$((sub + 1))
done
# shellcheck disable=SC2086
expect_clean "$work/nosink" "sinks omitted" $masks
cert_assert "ipc-deadlock.pml over the shipped graph, sinks omitted: no invalid end state for all $((sub - 1)) non-empty subsets of $nc clients, searches complete"

# Sinks modeled (they receive and reply): every single client and every pair.
mkdir "$work/shipped"
GATE_ASSERTIONS= python3 "$RANKS" --emit-promela "$work/shipped/edges.inc" >/dev/null
clients=$(sed -n 's/^\/\* clients: \(.*\) \*\/$/\1/p' "$work/shipped/edges.inc")
masks=
for a in $clients; do
    for b in $clients; do
        [ "$a" -le "$b" ] || continue
        masks="$masks $(((1 << a) | (1 << b)))"
    done
done
# shellcheck disable=SC2086
expect_clean "$work/shipped" "sinks modeled" $masks
cert_assert "ipc-deadlock.pml over the full shipped graph (sinks modeled): no invalid end state for every single client and pair, searches complete"

# 3b. The planted cyclic graph must deadlock.
mkdir "$work/cyclic"
python3 "$RANKS" --emit-only --table "$CYCLIC" --emit-promela "$work/cyclic/edges.inc"
run_set "$work/cyclic" 1
if [ "$PAN_ERRORS" = 0 ]; then
    fail "planted cyclic graph did not deadlock; the model is vacuous"
fi
cert_assert "ipc-deadlock.pml over the cyclic fixture: invalid end state (errors: $PAN_ERRORS)"

cert_result "shipped graph rank-ordered and deadlock-free; cyclic, rank-inverted and unlisted-sender mutations all rejected"
echo "ipc-deadlock: ok"
