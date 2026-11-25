#!/bin/sh
# RTMux is freely redistributable under the Apache License. See the file
# "LICENSE" for information on usage and redistribution of this file.
#
# SPDX-License-Identifier: Apache-2.0
#
# A process's flags (process_t.flags) are written from paths that hold no common
# lock: the exit marks TERMING and DESTROYALL under vec_slock, while the end of
# a load, the debugger and the wait paths write other bits without it. A plain
# read-modify-write ("|=", "&=", "^=") can write back the word without a bit
# another hart just set, which once undid the exit's marks and let a thread
# start in a process whose address space was going away. Those writes go
# through PROC_FLG_SET and PROC_FLG_CLR (kernel/include/core/macros.h). This
# gate fails on a compound assignment to the flags of a process pointer: prp,
# a name the same file declares as a process pointer, or any expression ending
# in ->process. A plain "=" is allowed, since it is how a process not yet
# visible to anyone gets its first flags. A self-test feeds it planted writes
# first, so the pattern cannot silently stop matching.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$ROOT"

# Print FILE with its comments and string and character literals blanked out,
# one output line per input line, so line numbers stay those of FILE.
mask() {
    awk '{
        out = ""
        n = length($0)
        for (i = 1; i <= n; i++) {
            c = substr($0, i, 1)
            d = substr($0, i + 1, 1)
            if (inblock) {
                if (c == "*" && d == "/") { inblock = 0; i++ }
            } else if (quote != "") {
                if (c == "\\") i++
                else if (c == quote) quote = ""
            } else if (c == "/" && d == "*") { inblock = 1; i++ }
            else if (c == "/" && d == "/") break
            else if (c == "\"" || c == "'\''") quote = c
            else { out = out c; continue }
            out = out " "
        }
        quote = ""
        print out
    }' "$1"
}

# The names a masked file on stdin declares as process pointers, one per line:
# "process_t *a", "struct process_entry *b", and the "*c" declarators that
# follow one in the same declaration. This is no type resolution: a pointer
# reached through a typedef of its own, a cast or an initialized declarator
# before it is not seen, and a name that is a process pointer in one function
# and something else in another is reported in both, which errs toward a false
# alarm rather than a missed write.
ID='[A-Za-z_][A-Za-z0-9_]*'
proc_names() {
    grep -oE "(process_t|struct[[:space:]]+process_entry)[[:space:]]*(const[[:space:]]*)?\*[[:space:]]*$ID([[:space:]]*,[[:space:]]*\*[[:space:]]*$ID)*" |
        sed 's/^[^*]*\*//' | tr ',*' '\n\n' | tr -d ' \t' | grep -E "^$ID\$" |
        sort -u || true
}

# Print every write in the files given, as file:line:text, matching only code:
# a write inside a comment or a string literal is not one. The process pointers
# are prp, the names proc_names finds in the file, and anything->process.
scan() {
    for f in "$@"; do
        names=$(mask "$f" | proc_names | tr '\n' '|')
        mask "$f" | grep -nE "(\\b(${names}prp)|->process)->flags[[:space:]]*(\\||&|\\^)=" |
            sed "s|^|$f:|" || true
    done
}

# Self-test: every planted write must be reported, and the planted others not.
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
mkdir -p "$tmp/kernel/ext"
printf 'void f(process_t *prp) { prp->flags |= 1; }\n' >"$tmp/kernel/ext/planted.c"
printf 'void g(thread_t *act) { act->process->flags &= ~2; }\n' \
    >>"$tmp/kernel/ext/planted.c"
printf 'void h(thread_t *t) { t->process->flags ^= 4; }\n' \
    >>"$tmp/kernel/ext/planted.c"
printf 'void i(process_t *prp) { prp->flags = 8; }\n' >>"$tmp/kernel/ext/planted.c"
printf 'void j(thread_t *thp) { thp->flags |= 1; }\n' >>"$tmp/kernel/ext/planted.c"
printf '/*\n * prp->flags |= X in a comment\n */\n' >>"$tmp/kernel/ext/planted.c"
printf '    x = 0; // prp->flags |= X after code\n' >>"$tmp/kernel/ext/planted.c"
printf '    /* prp->flags |= X */ x = 0;\n' >>"$tmp/kernel/ext/planted.c"
printf '    /* a block comment whose next line\n' >>"$tmp/kernel/ext/planted.c"
printf '       prp->flags |= X has no star */\n' >>"$tmp/kernel/ext/planted.c"
printf '    puts("prp->flags |= X");\n' >>"$tmp/kernel/ext/planted.c"
printf '    prp->flags |= 2; /* a real write before a comment */\n' \
    >>"$tmp/kernel/ext/planted.c"
printf 'void k(process_t *parent, process_t *child) { child->flags |= 1; }\n' \
    >>"$tmp/kernel/ext/planted.c"
printf '    process_t *a, *sib; sib->flags ^= 1;\n' >>"$tmp/kernel/ext/planted.c"
printf '    struct process_entry *pp = a; pp->flags &= ~1;\n' \
    >>"$tmp/kernel/ext/planted.c"
printf '    /* process_t *thp names no process pointer */\n' \
    >>"$tmp/kernel/ext/planted.c"
printf '    thp->flags |= 2;\n' >>"$tmp/kernel/ext/planted.c"
planted=$(cd "$tmp" && scan kernel/ext/planted.c | cut -d: -f2 | tr '\n' ' ')
want="1 2 3 14 15 16 17 "
if [ "$planted" != "$want" ]; then
    echo "check-proc-flags: self-test matched lines [$planted], want [$want]" >&2
    exit 1
fi

bad=$(find kernel arch -name '*.[ch]' | sort | while read -r f; do scan "$f"; done)
if [ -n "$bad" ]; then
    echo "check-proc-flags: plain read-modify-write of a process's flags" >&2
    echo "(use PROC_FLG_SET or PROC_FLG_CLR from kernel/include/core/macros.h):" >&2
    printf '%s\n' "$bad" >&2
    exit 1
fi
echo "check-proc-flags: ok"
