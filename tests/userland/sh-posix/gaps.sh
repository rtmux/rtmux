#! /bin/sh
#
# Run by sh-posix-test: the cases where this shell once differed from a
# reference POSIX shell. Every expected value below is what dash makes of the
# same command, and the file runs under dash as it is (SH=dash dash gaps.sh), so
# a check that fails here and passes there is a bug of this shell. Prints
# "  ok NAME" or "  FAIL NAME: ..." per case, then "gaps: PASS" or "gaps: FAIL".
#
# $SH is the shell the cases run as `sh -c`; it defaults to sh.
#
# SPDX-License-Identifier: Apache-2.0
#
SH=${SH:-sh}
bad=
n=0
D=/dev/shmem
test -d "$D" && test -w "$D" || D=/tmp
P=$D/gaps$$

rmf() {
    for f in "$@"; do
        rm "$f" 2>/dev/null
    done
    return 0
}

ck() {
    n=$((n + 1))
    if test "$2" = "$3"; then
        echo "  ok $1"
    else
        echo "  FAIL $1: want [$2] got [$3]"
        bad="$bad$1 "
    fi
}

# --- a quote or a substitution that never ends is a syntax error, and nothing
# of the line runs -----------------------------------------------------------
out=$($SH -c 'echo a; echo "b' 2>/dev/null; echo "rc=$?")
ck unterminated-dquote "rc=2" "$out"
out=$($SH -c "echo a; echo 'b" 2>/dev/null; echo "rc=$?")
ck unterminated-squote "rc=2" "$out"
out=$($SH -c 'echo a; echo $(echo b' 2>/dev/null; echo "rc=$?")
ck unterminated-cmdsub "rc=2" "$out"
out=$($SH -c 'echo a; echo ${x' 2>/dev/null; echo "rc=$?")
ck unterminated-brace "rc=2" "$out"
out=$($SH -c 'echo a; echo `echo b' 2>/dev/null; echo "rc=$?")
ck unterminated-backtick "rc=2" "$out"
out=$($SH -c 'echo a # it'"'"'s a comment; echo b' 2>&1; echo "rc=$?")
ck quote-in-comment "$(printf 'a\nrc=0')" "$out"

# --- nested defaults and an empty quoted word -----------------------------------
out=$($SH -c 'unset x y; set -- ${x:-${y:-"a b"}}; echo $#')
ck nested-default-quoted 1 "$out"
out=$($SH -c 'unset x y; set -- ${x:-${y:-a b}}; echo $#')
ck nested-default-split 2 "$out"
out=$($SH -c 'unset x y; set -- "${x:-${y:-"a b"}}"; echo $#')
ck nested-default-dq 1 "$out"
out=$($SH -c 'unset x y; set -- ${x:-${y:-"a b"} c}; echo $#')
ck nested-default-tail 2 "$out"
out=$($SH -c 'unset x; set -- ${x:-""}; echo $#')
ck default-empty-quoted 1 "$out"
out=$($SH -c 'unset x; set -- ${x:-}; echo $#')
ck default-empty 0 "$out"
out=$($SH -c "unset x; set -- \${x:-''}; echo \$#")
ck default-empty-squoted 1 "$out"
out=$($SH -c 'unset x; set -- a${x:-""}b; echo "$#:$1"')
ck default-empty-joined "1:ab" "$out"
out=$($SH -c 'x=1; set -- ${x:+""}; echo $#')
ck alt-empty-quoted 1 "$out"
out=$($SH -c 'unset x; set -- ${x:-"a  b" c}; echo "$#:$1"')
ck default-two-words "2:a  b" "$out"

# --- unquoted $@ and $* split under another IFS --------------------------------
out=$($SH -c 'IFS=:; set -- "a b" "c:d" "" e; for f in $@; do printf "<%s>" "$f"; done')
ck at-ifs "<a b><c><d><e>" "$out"
out=$($SH -c 'IFS=:; set -- "a b" "c:d" "" e; for f in $*; do printf "<%s>" "$f"; done')
ck star-ifs "<a b><c><d><e>" "$out"
out=$($SH -c 'IFS=:; set -- "a b" "c:d"; for f in ${@}; do printf "<%s>" "$f"; done')
ck braced-at-ifs "<a b><c><d>" "$out"
out=$($SH -c 'set -- "" b; for f in x$@y; do printf "<%s>" "$f"; done')
ck at-edge-empty "<x><by>" "$out"
out=$($SH -c 'IFS=:; set -- ":a" "b:"; for f in $@; do printf "<%s>" "$f"; done')
ck at-ifs-delim "<><a><b>" "$out"
out=$($SH -c 'set -- a "" b; for f in $@; do printf "<%s>" "$f"; done')
ck at-empty-dropped "<a><b>" "$out"

# --- pathname expansion of an expansion's result ----------------------------------
: > "$P-a.x"
: > "$P-b.x"
out=$($SH -c 'p="$1-*.x"; set -- $p; echo "$#"' sh "$P")
ck glob-var 2 "$out"
out=$($SH -c 'p="$1-*.x"; set -- "$p"; echo "$#"' sh "$P")
ck glob-var-quoted 1 "$out"
out=$($SH -c 'set -- $(echo "$1-*.x"); echo "$#"' sh "$P")
ck glob-cmdsub 2 "$out"
out=$($SH -c 'set -- ${1}-*.x; echo "$#"' sh "$P")
ck glob-literal-tail 2 "$out"
out=$($SH -c 'p="$1-*.x"; set -- ${p}; echo "$#"' sh "$P")
ck glob-braced 2 "$out"
out=$($SH -c 'set -- "$1"-*.x; echo "$#"' sh "$P")
ck glob-quoted-prefix 2 "$out"
out=$($SH -c 'p="$1-*.zz"; set -- $p; echo "$#:${1#*-}"' sh "$P")
ck glob-nomatch "1:*.zz" "$out"
out=$($SH -c 'p="$1-*.x"; v=$p; echo "${v#*-}"' sh "$P")
ck glob-assign-none "*.x" "$out"
out=$($SH -c 'set -- "$1-*.x" "$1-b.x"; set -- $@; echo "$#"' sh "$P")
ck glob-at 3 "$out"
rmf "$P-a.x" "$P-b.x"

# --- a ) in a comment or a case pattern does not end a $( ... ) -------------------
out=$($SH -c 'echo $(echo hi # x)' 2>/dev/null; echo "rc=$?")
ck paren-in-comment "rc=2" "$out"
out=$($SH -c 'echo $(echo a#b)')
ck hash-inside-word "a#b" "$out"
out=$($SH -c 'echo $(case x in x) echo hi;; esac)')
ck paren-in-case-pattern hi "$out"
out=$($SH -c 'echo $(case y in x) echo no;; y|z) echo yes;; esac)')
ck paren-in-case-second yes "$out"
out=$($SH -c 'echo $(echo case x in)')
ck case-as-argument "case x in" "$out"
out=$($SH -c 'echo $(case x in x) echo b; esac; echo c)')
ck case-last-arm "b c" "$out"
out=$($SH -c 'echo "$(case x in x) echo hi;; esac)"')
ck paren-in-case-dquote hi "$out"

# --- a function that loops over its own arguments ---------------------------------
out=$($SH -c 'g() { for f in "$@"; do echo "[$f]"; done; return 0; }; g a "b c"')
ck function-loop-args "$(printf '[a]\n[b c]')" "$out"

# --- a closed descriptor ------------------------------------------------------------
out=$($SH -c 'echo hi >&-; echo "rc=$?"' 2>/dev/null)
ck close-builtin-status "rc=1" "$out"
out=$($SH -c 'echo hi >&-; echo after' 2>/dev/null)
ck close-restored after "$out"
out=$($SH -c 'echo x | cat >&-; echo "rc=$?"' 2>/dev/null)
ck close-external-status "rc=1" "$out"
out=$($SH -c 'echo x >&-; echo hi >&1' 2>/dev/null; echo "rc=$?")
ck close-not-sticky "$(printf 'hi\nrc=0')" "$out"
out=$($SH -c 'echo hi 2>&-; echo ok' 2>&1)
ck close-stderr-unused "$(printf 'hi\nok')" "$out"

# --- a background compound command, with a command after it -------------------------
rmf "$P-bg"
out=$(F=$P-bg $SH -c 'for i in 1; do sleep 2; echo late > $F; done & test -e $F && echo fg || echo bg; sleep 3')
ck done-amp-background bg "$out"
rmf "$P-bg"
out=$($SH -c 'while false; do :; done & echo x; sleep 1')
ck while-amp-then x "$out"

# --- brace groups -----------------------------------------------------------------------
out=$($SH -c '{ echo a; echo b; } | wc -l')
set -- $out
ck brace-pipe 2 "$1"
out=$($SH -c 'x=1; { x=2; }; echo $x')
ck brace-same-shell 2 "$out"
out=$($SH -c '{ echo a; echo b; } > '"$P-br"'; cat '"$P-br"' | wc -l')
set -- $out
ck brace-redirect 2 "$1"
rmf "$P-br"
out=$($SH -c '{ false; } || echo alt')
ck brace-or alt "$out"
out=$($SH -c '{ true; } && echo ok')
ck brace-and ok "$out"
out=$($SH -c 'echo x; { echo y; }; echo z')
ck brace-between "x y z" "$(echo $out)"
out=$($SH -c '{ { echo n; }; echo m; }')
ck brace-nested "n m" "$(echo $out)"
out=$($SH -c '{ echo a; false; }; echo "rc=$?"')
ck brace-status "a rc=1" "$(echo $out)"
printf '%s\n' '{' '  echo one' '  echo two' '}' 'echo three' > "$P-bs.sh"
out=$($SH "$P-bs.sh")
ck brace-multiline "one two three" "$(echo $out)"
printf '%s\n' 'f() {' '  echo in-f; { echo grouped; }' '}' 'f' > "$P-bs.sh"
out=$($SH "$P-bs.sh")
ck brace-in-function "in-f grouped" "$(echo $out)"
printf '%s\n' 'for i in 1 2; do' '  { echo "g$i"; }' 'done' > "$P-bs.sh"
out=$($SH "$P-bs.sh")
ck brace-in-loop "g1 g2" "$(echo $out)"
rmf "$P-bs.sh"
out=$($SH -c '{ echo }; }')
ck brace-literal-arg "}" "$out"
out=$($SH -c 'unset x; echo ${x:-}}')
ck brace-param-closer "}" "$out"

# --- a case command on one line -------------------------------------------------------------
out=$($SH -c 'case b in a) echo A;; b) echo B;; esac')
ck case-one-line B "$out"
out=$($SH -c 'case a in a) echo A; esac')
ck case-one-line-bare A "$out"
out=$($SH -c 'case z in a) echo A;; *) echo other;; esac | wc -l')
set -- $out
ck case-one-line-pipe 1 "$1"
out=$($SH -c 'case x in a) echo A;; esac; echo after')
ck case-one-line-nomatch after "$out"

echo "  $n gap cases"
if test -z "$bad"; then
    echo "gaps: PASS"
else
    echo "gaps: FAIL: $bad"
    exit 1
fi
