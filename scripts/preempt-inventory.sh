#!/bin/sh
#
# preempt-inventory.sh - preemption-checkpoint inventory gate.
#
# Fully preemptive RTMux needs an auditable rule: no unbounded interrupt-enabled
# kernel loop without a checkpoint, and no long interrupt-disabled lock window
# without a measured owner. This gate enforces that every long or unbounded loop
# in the scheduling-critical kernel subsystems is accounted for in a committed
# inventory, so a new unclassified loop cannot slip in unreviewed.
#
# Scope: every kernel directory that holds C files, plus kernel/include and
# arch/riscv, is scanned (DIRS below): C sources and headers (static inline helpers
# and macros) with the loop rules here, and assembly (.S) for backward branches, that
# is a branch or jump to a label defined earlier in the file. The inventory carries a
# "# scope:" line that must equal DIRS so the stated and scanned coverage cannot
# differ. A backward branch to a numeric label inside an inline-asm string is a loop too.
# Loops in files outside DIRS (userland, the libraries, tools) are NOT scanned. Loops
# written as a backward C goto, recursion, and assembly loops built from jal/call are not
# recognised.
#
# A loop is treated as trivially bounded and skipped only when it is a counting
# loop: a for loop with an integer induction step AND a relational condition, a
# for loop that walks a NULL-terminated kernel list, or a while whose condition
# is a pure integer countdown (a decrement, no disjunction). Every other loop (a
# condition-only for, a spin, a pointer-walk while, a queue drain, a do-while)
# is a candidate long path and must appear in scripts/preempt-inventory.txt,
# classified as one of the classes below. The reason column must carry the
# listed fields; the check rejects an entry that omits them, that says a lock
# context is unaudited, or that carries TODO.
#
#   bounded        real finite runtime bound that kernel code enforces
#                  (reason has "bound:" or "cap:"). A hard cap that ends in
#                  crash() counts; a loop that merely terminates for sane
#                  inputs does not
#   checkpointed   yields via NEED_PREEMPT/KSYS_PREEMPT/KEXT_PREEMPT so a
#                  higher-priority thread preempts it (reason has
#                  "checkpoint: <macro>")
#   irqoff         runs interrupts-off under a named spinlock; reason has
#                  "lock: <name>" and states what limits the walk. It is a
#                  window, not a constant bound
#   peer_dependent terminates only if another hart or thread makes progress (a
#                  RTMUX_ITF_WAIT_PENDING handshake spin, a lock-free CAS retry,
#                  a cross-hart quiescence wait). It has no hard iteration cap,
#                  so it is NOT bounded. The reason has "waits-for:" (the peer
#                  event) and "holds:" (the locks and RCU sections the spinner
#                  holds, or "none"); a spin that cannot name them is rejected
#   terminal       never ends by design: a hart parked for good (reason has
#                  "terminal:" and "holds:"); it waits on no peer and has no
#                  bound, so it is neither peer_dependent nor bounded
#   input_dependent finite work whose length a user controls (an iovec part
#                  count, a per-user thread list) with no kernel cap and no
#                  checkpoint. The reason has "input:" and "review: Q<n>"; it
#                  is an open latency gap that every run reports
#
# A loop whose while/for header spans several lines is keyed on the joined
# header, so a RTMUX_ITF_WAIT_PENDING test on a continuation line still forces
# peer_dependent. Spins hidden behind a helper or a different flag are not
# detected and need manual review.
#
# The inventory is keyed on the file, the enclosing function, and the
# normalized loop text, matched as a multiset, so it survives line-number drift
# and reformatting while still telling apart identical loop text in different
# functions. Editing a loop's text, or adding a new candidate loop, makes it
# appear as unclassified until the inventory is regenerated
# (make preempt-inventory-update) and the new entry is reviewed. A removed loop
# shows up as a stale inventory entry.
#
# Modes:
#   (default)  check the tree against the inventory; non-zero on any drift
#   dump       print "file<TAB>line<TAB>func<TAB>normtext" for every candidate
#              loop; used to (re)generate the inventory

set -eu

DIRS="kernel/ipc kernel/sync kernel/xfer kernel/thread kernel/ext kernel/sched kernel/time kernel/core kernel/irq kernel/mm kernel/object kernel/cap kernel/fault kernel/security kernel/trace kernel/debug kernel/startup kernel/init kernel/printk kernel/module kernel/lib kernel/include arch/riscv"

cd "$(dirname "$0")/.."

# A kernel directory holding C, header or assembly files must be scanned: a new
# directory cannot
# bypass the gate by being absent from DIRS (the inventory's scope line only
# proves the script and the inventory agree, not that DIRS covers the tree).
SOURCES=$(find kernel arch/riscv \( -name '*.c' -o -name '*.h' -o -name '*.S' \) 2>/dev/null | sort)
for d in $(printf '%s\n' "$SOURCES" | sed -nE 's|^(kernel/[^/]+)/.*|\1|p' | sort -u); do
  case " $DIRS " in
  *" $d "*) ;;
  *) echo "  FAIL    preemption-inventory: $d holds source files but is not in DIRS"; exit 1 ;;
  esac
done

INV="scripts/preempt-inventory.txt"
mode="${1:-check}"

detected=$(mktemp)
report=$(mktemp)
trap 'rm -f "$detected" "$report"' EXIT

# Assembly: a backward branch (to a label defined earlier, or Nb) is a loop. The
# enclosing function is the last non-local label, so the key survives line drift.
# Branch and jump mnemonics that make a loop when they go backward; both awk
# programs below match against this one list.
BRANCHES='j|jr|tail|b|beq|bne|blt|bge|bltu|bgeu|beqz|bnez|bgtz|blez|bltz|bgez|bgt|ble|bgtu|bleu'

ASM_AWK='
function trim(s) { sub(/^[ \t]+/, "", s); sub(/[ \t]+$/, "", s); return s }
# Drop block and line comments (a block comment may span lines) so a trailing
# "/* retry */" is not mistaken for the branch target.
function uncomment(s,   out, i) {
    out = ""
    while (s != "") {
        if (inc) {
            i = index(s, "*/")
            if (i == 0)
                return out
            s = substr(s, i + 2)
            inc = 0
        } else {
            i = match(s, /\/\*|\/\/|#/)
            if (i == 0)
                return out s
            out = out substr(s, 1, i - 1)
            if (substr(s, i, 2) == "/*") {
                s = substr(s, i + 2)
                inc = 1
            } else
                return out
        }
    }
    return out
}
{ text[NR] = $0; total = NR }
END {
    func_name = "?"
    inc = 0
    for (n = 1; n <= total; n++) {
        line = uncomment(text[n])
        # gas takes ";" as a statement separator on RISC-V
        ns = split(line, stmts, ";")
        for (q = 1; q <= ns; q++) {
            s = trim(stmts[q])
            while (match(s, /^[A-Za-z_.$0-9][A-Za-z_.$0-9]*:/)) {
                lab = substr(s, 1, RLENGTH - 1)
                labels[lab] = n
                if (lab !~ /^[0-9]+$/ && lab !~ /^\.L/)
                    func_name = lab
                s = trim(substr(s, RLENGTH + 1))
            }
            if (s !~ "^(" BRANCHES ")[ \t]")
                continue
            k = split(s, ops, /[ \t,]+/)
            tgt = ops[k]
            if (tgt ~ /^[0-9]+f$/)
                continue
            if (tgt ~ /^[0-9]+b$/ || ((tgt in labels) && labels[tgt] <= n)) {
                gsub(/[ \t]+/, " ", s)
                printf "%s\t%d\t%s\t%s\n", fname, n, func_name, s
            }
        }
    }
}'

printf '%s\n' "$SOURCES" | while IFS= read -r f; do
    case "$f" in
    *.S)
        awk -v fname="$f" -v BRANCHES="$BRANCHES" "$ASM_AWK" "$f"
        continue
        ;;
    esac
    awk -v fname="$f" -v BRANCHES="$BRANCHES" '
    function trim(s) { sub(/^[ \t]+/, "", s); sub(/[ \t]+$/, "", s); return s }
    function semis(s) { return gsub(/;/, ";", s) }
    # Drop inline comments before keyword detection so a "/* end for (...) */"
    # trailer on a non-loop line (a closing brace, say) is not misread as a loop.
    function strip(s) {
        sub(/\/\/.*/, "", s)
        gsub(/\/\*.*\*\//, " ", s)
        sub(/\/\*.*/, "", s)
        return s
    }
    # Canonical loop text: comment-free, whitespace-collapsed. Part of the
    # inventory key, so it is stable across reindentation and line moves.
    function norm(s) {
        s = strip(s)
        gsub(/[ \t]+/, " ", s)
        return trim(s)
    }
    # A for header can span up to three physical lines; join continuations so
    # the step clause is complete before it is inspected.
    function forhdr(n,   s, k) {
        s = lines[n]
        for (k = n + 1; k <= n + 2 && k <= total; k++)
            s = s " " lines[k]
        return s
    }
    function parens(s,   o, c) {
        o = gsub(/\(/, "(", s); c = gsub(/\)/, ")", s)
        return o - c
    }
    # A while/for header whose parentheses do not close on its first line is a
    # multiline header: join up to four continuation lines so the whole
    # condition is part of the key and is visible to the class rules (a
    # RTMUX_ITF_WAIT_PENDING test on the second line must not hide a spin).
    function fullhdr(n,   s, k, t, inc) {
        s = strip(lines[n])
        inc = 0
        for (k = n + 1; parens(s) > 0 && k <= n + 6 && k <= total; k++) {
            t = lines[k]
            if (inc) {              # inside a block comment between header lines
                if (t !~ /\*\//)
                    continue
                sub(/^.*\*\//, "", t)
                inc = 0
            }
            if (t ~ /\/\*/ && t !~ /\*\//) {
                sub(/\/\*.*/, "", t)
                inc = 1
            }
            s = s " " strip(t)
        }
        return s
    }
    { lines[NR] = $0; total = NR }
    END {
        curfunc = "?"
        for (n = 1; n <= total; n++) {
            l = lines[n]
            # Track the enclosing function so the key can tell apart identical
            # loop text (four "for (;;) {" live in different functions). K&R
            # style: a function definition starts in column 0 with an identifier
            # and an open paren, and nothing inside a body does. This only needs
            # to be a deterministic discriminator, not the true name, since dump
            # and check compute it the same way.
            if (l ~ /^[A-Za-z_][A-Za-z_0-9]*[ \t]*\([A-Za-z_][A-Za-z_0-9]*\)[ \t]*\(/) {
                d = l; sub(/^[^(]*\(/, "", d); sub(/\).*/, "", d)
                curfunc = d
            } else if (l ~ /^[A-Za-z_]/ && l ~ /\(/ && l !~ /^#/ && l !~ /;[ \t]*$/) {
                d = l; sub(/\(.*/, "", d)
                fnf = split(d, dd, /[ \t\*]+/)
                if (dd[fnf] ~ /^[A-Za-z_][A-Za-z_0-9]*$/)
                    curfunc = dd[fnf]
            }
            t = trim(l)
            if (t ~ /^\/\//)      # whole-line // comment
                continue
            if (t ~ /^\*/)        # block comment interior
                continue
            sl = strip(l)
            if (trim(sl) == "")   # line held only a block comment and space
                continue
            is_for   = (sl ~ /(^|[^_A-Za-z])for[ \t]*\(/)
            is_while = (sl ~ /(^|[^_A-Za-z])while[ \t]*\(/)
            # A "do {" that ends in a backslash is a do-while(0) macro wrapper,
            # a single-pass statement grouping rather than a loop; any real loop
            # inside the macro sits on its own while/for line and is caught there.
            is_do    = ((sl ~ /(^|[^_A-Za-z])do[ \t]*\{/ || sl ~ /(^|[^_A-Za-z])do[ \t]*$/) &&
                        l !~ /\\[ \t]*$/)
            # A backward branch to a numeric label inside an inline-asm string
            # (a lock spin, an lr/sc retry or a park loop) is a loop too.
            is_asm   = (sl ~ /"/ &&
                        sl ~ ("(^|[^A-Za-z_0-9])(" BRANCHES ")[ \t]+([^\"]*[ \t,])?[0-9]+b([^A-Za-z0-9_]|$)"))
            if (!is_for && !is_while && !is_do && !is_asm)
                continue
            # A do-while continuation "while (...)" at the end of a do block is
            # the same loop already seen at the do; skip a while that is really
            # the tail of a do-while (heuristic: line starts with }).
            if (is_while && trim(l) ~ /^\}/ && sl !~ /WAIT_PENDING/)
                continue
            # A while whose condition is a pure integer countdown, while (len--)
            # or while (parts-- > 0 && ...), is bounded by the decrement. Require
            # a decrement and forbid a disjunction, so while (ready || retries--)
            # is not mistaken for a countdown. Only inspect the parenthesized
            # condition so a pointer walk in the body is not consulted.
            if (is_while && match(sl, /while[ \t]*\([^)]*\)/)) {
                wc = substr(sl, RSTART, RLENGTH)
                if (wc ~ /--/ && wc !~ /->/ && wc !~ /\|\|/)
                    continue
            }
            if (is_for) {
                # A counting for loop is trivially bounded when its step
                # advances an integer induction variable and its condition is a
                # real relational bound. Inspect only the step and condition
                # clauses so a bound that derefs a pointer, like
                # i < prp->threads.nentries, is not mistaken for a pointer walk,
                # and so for (;; i++) with no condition is not skipped.
                hdr = sl
                if (semis(sl) < 2)
                    hdr = strip(forhdr(n))
                nf = split(hdr, parts, ";")
                if (nf >= 3) {
                    step = parts[3]
                    cond = parts[2]
                    # Integer counting loop: step advances an induction variable,
                    # does not walk a chain, and the condition really bounds it.
                    cmp = cond
                    gsub(/->/, "", cmp) # a member access is not a comparison
                    if (step ~ /(\+\+|--|\+=|-=)/ && step !~ /->/ &&
                        step !~ /next|prev|sibling/ && step !~ /= *\*/ &&
                        cmp ~ /(<|>|!=)/)
                        continue
                    # NULL-terminated chain walk: the step reassigns the cursor
                    # to a linked field and the condition is just the cursor, so
                    # the walk is bounded by a kernel-owned list length. Only the
                    # unbounded shapes (for (;;), condition-only, spin) remain.
                    if (step ~ /=.*(->|next|prev|sibling|link)/ && cond ~ /[A-Za-z]/ &&
                        cond !~ /(&&|\|\||<|>|==|!=)/)
                        continue
                }
            }
            printf "%s\t%d\t%s\t%s\n", fname, n, curfunc, norm(fullhdr(n))
        }
    }
    ' "$f"
done > "$detected"

if [ "$mode" = "dump" ]; then
    cat "$detected"
    exit 0
fi

# Regenerate the inventory: keep the class and reason of every loop whose key
# still exists, drop entries for removed loops, and emit UNCLASSIFIED (an
# invalid class, so the gate still fails) for any newly appeared loop so the
# author must classify it. Writes the new inventory to stdout.
if [ "$mode" = "update" ]; then
    grep '^#' "$INV" 2>/dev/null || true
    awk -F'\t' -v inv="$INV" '
        BEGIN {
            while ((getline line < inv) > 0) {
                if (line ~ /^#/ || line == "")
                    continue
                n = split(line, a, "\t")
                key = a[2] SUBSEP a[3] SUBSEP a[4]
                c[key]++
                store[key SUBSEP c[key]] = a[1] "\t" (n >= 5 ? a[5] : "")
            }
        }
        {
            key = $1 SUBSEP $3 SUBSEP $4
            use[key]++
            s = store[key SUBSEP use[key]]
            if (s == "") { cls = "UNCLASSIFIED"; rsn = "TODO classify this loop" }
            else { split(s, ss, "\t"); cls = ss[1]; rsn = ss[2] }
            printf "%s\t%s\t%s\t%s\t%s\n", cls, $1, $3, $4, rsn
        }
    ' "$detected" | LC_ALL=C sort -s -t"$(printf '\t')" -k2,2 -k3,3 -k4,4
    exit 0
fi

if [ ! -f "$INV" ]; then
    echo "  FAIL    preemption-inventory: $INV missing (run make preempt-inventory-update)"
    exit 1
fi

# The scanned directories are the stated coverage: the inventory must name
# exactly the set this script scans, so the two cannot drift apart silently.
inv_scope=$(sed -n 's/^# scope: //p' "$INV")
if [ "$inv_scope" != "$DIRS" ]; then
    echo "  FAIL    preemption-inventory: scan scope differs from the stated scope"
    echo "    script:    $DIRS"
    echo "    inventory: ${inv_scope:-(no '# scope:' line)}"
    exit 1
fi

# Multiset compare on (file, function, normtext): an over-quota key is a new
# unclassified loop; an under-quota inventory entry is stale (loop removed or
# its text changed).
awk -F'\t' -v inv="$INV" '
    function badclass(why) {
        print "BADCLASS\t" a[2] "\t" a[3] "\t" a[1] "\t" a[4] "\t" why
    }
    BEGIN {
        valid["bounded"] = 1; valid["checkpointed"] = 1; valid["irqoff"] = 1
        valid["peer_dependent"] = 1; valid["input_dependent"] = 1
        valid["terminal"] = 1
        while ((getline line < inv) > 0) {
            if (line ~ /^#/ || line == "")
                continue
            # class <TAB> file <TAB> func <TAB> normtext <TAB> reason; the
            # reason is not part of the key, but the class rules below require
            # it to carry specific facts.
            nf = split(line, a, "\t")
            if (nf < 5 || !(a[1] in valid) || a[5] ~ /^[[:space:]]*$/ ||
                a[5] ~ /TODO|unaudited|not audited|to be audited|not enumerated/) {
                print "BADENTRY\t" line
                continue
            }
            # A handshake spin on the wait bit of another thread depends on that
            # peer (the header may be multiline; the key holds all of it), so it
            # may not be filed under a class that implies a bound.
            if ((a[4] ~ /ITF_WAIT_PENDING/ || (a[4] ~ /compare_exchange|cmpxchg/ && a[5] !~ /cap: [0-9]/)) &&
                a[1] != "peer_dependent")
                badclass("WAIT_PENDING/CAS spin must be peer_dependent")
            if (a[1] == "peer_dependent" &&
                (a[5] !~ /waits-for:/ || a[5] !~ /holds:/))
                badclass("peer_dependent needs waits-for: and holds:")
            if (a[1] == "terminal" &&
                (a[5] !~ /terminal:/ || a[5] !~ /holds:/))
                badclass("terminal needs terminal: and holds:")
            if (a[1] == "irqoff" &&
                a[5] !~ /lock: [A-Za-z_(][A-Za-z_0-9>.()*&-]*(lock|slock|LOCK)([^A-Za-z_0-9]|$)/)
                badclass("irqoff needs a named lock: <name>")
            if (a[1] == "checkpointed" && a[5] !~ /checkpoint:/)
                badclass("checkpointed needs checkpoint: <macro>")
            if (a[1] == "bounded" && a[5] !~ /(cap|bound):/)
                badclass("bounded needs bound: or cap:")
            if (a[1] == "input_dependent") {
                if (a[5] !~ /input:/ || a[5] !~ /review: Q[0-9]+/)
                    badclass("input_dependent needs input: and review: Q<n>")
                print "REVIEW\t" a[2] "\t" a[3] "\t" a[4]
            }
            invc[a[2] SUBSEP a[3] SUBSEP a[4]]++
        }
    }
    {
        key = $1 SUBSEP $3 SUBSEP $4
        detc[key]++
        if (detc[key] > invc[key])
            print "NEW\t" $1 "\t" $2 "\t" $4
    }
    END {
        for (k in invc) {
            if (detc[k] < invc[k]) {
                split(k, kk, SUBSEP)
                print "STALE\t" kk[1] "\t" (invc[k] - detc[k]) "\t" kk[3]
            }
        }
    }
' "$detected" > "$report"

bad=$(grep -c '^BADENTRY' "$report" || true)
badclass=$(grep -c '^BADCLASS' "$report" || true)
new=$(grep -c '^NEW' "$report" || true)
stale=$(grep -c '^STALE' "$report" || true)

if [ "$bad" -ne 0 ]; then
    echo "  FAIL    preemption-inventory: malformed inventory line(s):"
    grep '^BADENTRY' "$report" | sed 's/^BADENTRY\t/    /'
fi
if [ "$badclass" -ne 0 ]; then
    echo "  FAIL    preemption-inventory: misclassified or under-justified loop(s):"
    grep '^BADCLASS' "$report" | awk -F'\t' '{printf "    %s %s: [%s] %s: %s\n", $2, $3, $4, $6, $5}'
    echo "  (see the class rules in the header of scripts/preempt-inventory.sh)"
fi
if [ "$new" -ne 0 ]; then
    echo "  FAIL    preemption-inventory: unclassified long loop(s):"
    grep '^NEW' "$report" | awk -F'\t' '{printf "    %s:%s: %s\n", $2, $3, $4}'
    echo "  ($new new; classify in $INV, then make preempt-inventory-update)"
fi
if [ "$stale" -ne 0 ]; then
    echo "  FAIL    preemption-inventory: stale inventory entr(ies) (loop removed or changed):"
    grep '^STALE' "$report" | awk -F'\t' '{printf "    %s (x%s): %s\n", $2, $3, $4}'
    echo "  (regenerate with make preempt-inventory-update)"
fi

if [ "$bad" -ne 0 ] || [ "$badclass" -ne 0 ] || [ "$new" -ne 0 ] || [ "$stale" -ne 0 ]; then
    exit 1
fi

nreview=$(grep -c '^REVIEW' "$report" || true)
if [ "$nreview" -ne 0 ]; then
    echo "  REVIEW  preemption-inventory: $nreview input-dependent loop(s) remain open latency gaps:"
    grep '^REVIEW' "$report" | awk -F'\t' '{printf "    %s %s\n", $2, $3}' | sort | uniq -c
fi
echo "  PASS    preemption-inventory: all long loops classified (scope: $DIRS)"
