#! /bin/sh
#
# Run by sh-posix-test: the cases where this shell once differed from a
# reference POSIX shell. Every expected value below is what dash makes of the
# same command, and the file runs under dash as it is (SH=dash dash gaps.sh), so
# a check that fails here and passes there is a bug of this shell. Prints
# "  ok NAME" or "  FAIL NAME: ..." per case, then "gaps: PASS" or "gaps: FAIL".
#
# $SH is the shell the cases run as `sh -c`, and the one a case runs as a child;
# it defaults to sh. $GAPS_DIR is the directory for scratch files.
#
# SPDX-License-Identifier: Apache-2.0
#
SH=${SH:-sh}
export SH
bad=
n=0
D=${GAPS_DIR:-/dev/shmem}
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

out=$($SH -c 'echo before; echo "$((1 / 0))"; echo after' 2>/dev/null)
rc=$?
ck arithmetic-error-command "before" "$out"
if test "$rc" -gt 0; then ck arithmetic-error-exits yes yes; else ck arithmetic-error-exits yes no; fi
out=$($SH -c 'unset GLOCAL; GLOCAL=private; printenv GLOCAL || echo unset; export GLOCAL; printenv GLOCAL')
ck shell-local-export "$(printf 'unset\nprivate')" "$out"
out=$($SH -c 'unset GLOCAL; GLOCAL=private; echo "$(echo "$GLOCAL")"; GLOCAL=scoped printenv GLOCAL; printenv GLOCAL || echo hidden')
ck shell-local-scope "$(printf 'private\nscoped\nhidden')" "$out"
out=$($SH -c 'unset GLOCAL; GLOCAL=direct exec printenv GLOCAL')
ck exec-prefix direct "$out"
out=$($SH -c 'true &' 2>&1)
ck quiet-background "" "$out"

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

# --- a quoted glob character is literal in a word that has an unquoted one --------
: > "$P-g*x"
: > "$P-gax"
out=$($SH -c 'set -- "$1-g*"x*; echo "$#:$1"' sh "$P")
ck glob-quoted-star-literal "1:$P-g*x" "$out"
out=$($SH -c 'set -- "$1-g*"*; echo "$#:$1"' sh "$P")
ck glob-quoted-star-then-star "1:$P-g*x" "$out"
out=$($SH -c 'set -- "$1-g"*x; echo "$#"' sh "$P")
ck glob-unquoted-star 2 "$out"
out=$($SH -c 'p="$1-g*"; set -- "$p"*; echo "$#:$1"' sh "$P")
ck glob-quoted-expansion-star "1:$P-g*x" "$out"
out=$($SH -c 'p="$1-g*"; set -- $p; echo "$#"' sh "$P")
ck glob-unquoted-expansion-star 2 "$out"
out=$($SH -c 'set -- "$1-g"\**; echo "$#:$1"' sh "$P")
ck glob-escaped-star "1:$P-g*x" "$out"
out=$($SH -c "set -- \"\$1-g\"'*'*; echo \"\$#:\$1\"" sh "$P")
ck glob-squoted-star "1:$P-g*x" "$out"
out=$($SH -c 'set -- "$1-g*"nomatch*; echo "$#:$1"' sh "$P")
ck glob-quoted-nomatch "1:$P-g*nomatch*" "$out"
out=$($SH -c 'set -- ${nov:-"$1-g*"}x*; echo "$#:$1"' sh "$P")
ck glob-quoted-in-default "1:$P-g*x" "$out"
control=$(printf '\003')
: > "$P-g${control}a"
out=$($SH -c 'set -- "$1-g$2"*; echo "$#:$1"' sh "$P" "$control")
ck glob-control-byte "1:$P-g${control}a" "$out"
: > "$P-bracket-"
: > "$P-bracket]"
: > "$P-bracketb"
out=$($SH -c 'set -- "$1-bracket"[a"-"c]; echo "$#:$1"' sh "$P")
ck glob-bracket-quoted-hyphen "1:$P-bracket-" "$out"
out=$($SH -c 'set -- "$1-bracket"[a"]"c]; echo "$#:$1"' sh "$P")
ck glob-bracket-quoted-close "1:$P-bracket]" "$out"
rmf "$P-g${control}a" "$P-bracket-" "$P-bracket]" "$P-bracketb"
rmf "$P-g*x" "$P-gax"

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

# --- test -s: a file that exists and is not empty ---------------------------------
: > "$P-ts0"
echo x > "$P-ts1"
test -s "$P-ts1"
ck test-s-nonempty 0 $?
test -s "$P-ts0"
ck test-s-empty 1 $?
test -s "$P-ts-none"
ck test-s-missing 1 $?
test ! -s "$P-ts0"
ck test-s-negated 0 $?
rmf "$P-ts0" "$P-ts1"

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
# a spawned child gets the descriptor really closed: what it opens takes that
# number (so cat's write into its own input file fails), and the shell's own
# descriptor is untouched afterwards
echo data > "$P-cl"
out=$($SH -c 'cat '"$P-cl"' >&-; echo "rc=$?"; echo after' 2>/dev/null)
ck close-child-own-open "$(printf 'rc=1\nafter')" "$out"
ck close-child-file-intact "data" "$(cat "$P-cl")"
out=$($SH -c '$SH -c "echo hi" >&-; echo "rc=$?"; echo after' 2>/dev/null)
ck close-child-stdout "$(printf 'rc=1\nafter')" "$out"
out=$($SH -c 'echo x | cat >&-; echo x | cat; echo done' 2>/dev/null)
ck close-child-then-use "$(printf 'x\ndone')" "$out"
out=$($SH -c '$SH -c "echo e >&2" 2>&-; test "$?" = 0 && echo wrote || echo failed' 2>&1)
ck close-child-stderr failed "$out"
# a descriptor that is closed cannot be duplicated, where a stand-in for it
# (an open /dev/null) could
out=$($SH -c '$SH -c "true >&2" 2>&- >/dev/null && echo opened || echo closed')
ck close-child-really-closed closed "$out"
out=$($SH -c 'for i in 1 2; do echo $i; done >&-; echo "rc=$?"; echo after' 2>/dev/null)
ck close-compound-for "$(printf 'rc=1\nafter')" "$out"
out=$($SH -c '{ echo a; } >&-; echo "rc=$?"; echo after' 2>/dev/null)
ck close-compound-brace "$(printf 'rc=1\nafter')" "$out"
out=$($SH -c '{ $SH -c "echo first"; a=$?; $SH -c "echo second"; b=$?; } >&-; echo "$a $b"' 2>/dev/null)
ck close-compound-external "1 1" "$out"
out=$($SH -c '{ { echo recovered; } >"$1"; $SH -c "echo hidden"; rc=$?; } >&-; cat "$1"; echo "$rc"' _ "$P-cl" 2>/dev/null)
ck close-compound-inner-open "$(printf 'recovered\n1')" "$out"

rmf "$P-cl"

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

# --- a case command inside a brace group, and one inside another's arm -------------
out=$($SH -c '{ case x in x) echo a;; esac; }')
ck case-in-brace-group a "$out"
out=$($SH -c '{ case y in x) echo a;; y) echo b;; esac; echo after; }')
ck case-in-brace-group-rest "b after" "$(echo $out)"
out=$($SH -c '{ case a in a) echo one;; esac; case b in b) echo two;; esac; }')
ck case-twice-in-brace-group "one two" "$(echo $out)"
out=$($SH -c 'if true; then case y in y) echo yes;; esac; fi')
ck case-in-if yes "$out"
out=$($SH -c 'for i in 1 2; do case $i in 1) echo one;; *) echo other;; esac; done')
ck case-in-for "one other" "$(echo $out)"
out=$($SH -c 'case a in a) case b in b) echo nested;; esac;; esac')
ck case-nested nested "$out"
out=$($SH -c 'case a in a) case c in b) echo no;; *) echo inner-star;; esac;; z) echo z;; esac')
ck case-nested-then-arm inner-star "$out"
out=$($SH -c 'case a in a) case b in b) echo x;; esac; echo tail;; esac')
ck case-nested-then-command "x tail" "$(echo $out)"
out=$($SH -c 'case a in a) case b in b) echo x;; esac ;; esac; echo post')
ck case-nested-then-list "x post" "$(echo $out)"
out=$($SH -c 'case b in a) echo a;; b) if true; then echo ifarm; fi;; esac')
ck case-arm-with-if ifarm "$out"
out=$($SH -c 'case a in a) echo before; case b in b) echo yes;; esac;; esac')
ck case-nested-after-command "before yes" "$(echo $out)"
out=$($SH -c 'case a in a) if true; then case b in b) echo yes;; esac; fi;; esac')
ck case-nested-after-then yes "$out"
out=$($SH -c 'case a in a) echo ";;" ;; esac')
ck case-arm-quoted-semis ";;" "$out"

# --- a quoted string or a substitution that goes on over several lines -------------
printf '%s\n' 'echo "a' 'b"' > "$P-ml.sh"
out=$($SH "$P-ml.sh")
ck multiline-dquote "a b" "$(echo $out)"
printf '%s\n' "echo 'x" "y'" 'echo done' > "$P-ml.sh"
out=$($SH "$P-ml.sh")
ck multiline-squote "x y done" "$(echo $out)"
printf '%s\n' 'v=$(echo p' 'echo q)' 'echo "$v"' > "$P-ml.sh"
out=$($SH "$P-ml.sh")
ck multiline-cmdsub "p q" "$(echo $out)"
printf '%s\n' 'for i in 1; do' '  echo "l1' '  l2"' 'done' > "$P-ml.sh"
out=$($SH "$P-ml.sh")
ck multiline-in-loop "l1 l2" "$(echo $out)"
printf '%s\n' 'echo $((1+' '2))' > "$P-ml.sh"
out=$($SH "$P-ml.sh")
ck multiline-arith 3 "$out"
printf '%s\n' 'echo "open' > "$P-ml.sh"
out=$($SH "$P-ml.sh" 2>/dev/null; echo "rc=$?")
ck multiline-unterminated "rc=2" "$out"
printf '%s\n' 'echo "a\' 'b"' "echo 'c\\" "d'" > "$P-ml.sh"
out=$($SH "$P-ml.sh")
ck multiline-backslash-dquote "$(printf 'ab\nc\\\nd')" "$out"
rmf "$P-ml.sh"
# at a prompt too: a shell reading commands from a pipe asks for the rest
raw=$(printf '%s\n' 'echo "a' 'b"' 'echo done' | $SH 2>&1)
out=$(echo "$raw" | tr -d '$>#')
ck multiline-prompt "a b done" "$(echo $out)"

# --- exec with a command applies its redirections to that command -------------
out=$($SH -c 'exec echo hi 1>&2' 2>&1 >/dev/null)
ck exec-cmd-redirect hi "$out"

# --- elif ---------------------------------------------------------------------------------
out=$($SH -c 'if false; then echo a; elif true; then echo b; else echo c; fi')
ck elif-one-line b "$out"
out=$($SH -c 'if false; then echo 1; elif false; then echo 2; elif true; then echo 3; else echo 4; fi')
ck elif-chain 3 "$out"
out=$($SH -c 'if false; then echo 1; elif false; then echo 2; else echo 4; fi')
ck elif-else 4 "$out"
out=$($SH -c 'if true; then echo 1; elif true; then echo 2; fi')
ck elif-not-reached 1 "$out"
printf '%s\n' 'if false; then' '  echo a' 'elif false; then' '  if true; then echo no; fi' 'elif true; then' '  echo c' 'else' '  echo d' 'fi' > "$P-el.sh"
out=$($SH "$P-el.sh")
ck elif-multiline c "$out"
rmf "$P-el.sh"

# --- the status of a compound command that ran nothing is 0 ---------------------------------
out=$($SH -c 'false; if false; then echo a; fi; echo $?')
ck if-no-branch-status 0 "$out"
out=$($SH -c 'false; while false; do :; done; echo $?')
ck while-no-pass-status 0 "$out"
out=$($SH -c 'false; for i in; do :; done; echo $?')
ck for-no-items-status 0 "$out"
out=$($SH -c 'false; case x in y) :;; esac; echo $?')
ck case-no-match-status 0 "$out"
$SH -c 'if false; then echo a; fi'
ck if-no-branch-exit 0 "$?"

# --- the special parameters in braces -------------------------------------------------------
out=$($SH -c 'set -- a b c; echo ${#} "${#}" ${?}')
ck brace-special-params "3 3 0" "$out"
out=$($SH -c 'false; echo ${?}; echo ${$}' | tr -d '0-9')
ck brace-status-pid "$(printf '\n')" "$out"
out=$($SH -c 'f() { echo ${#}; }; f x y')
ck brace-count-in-function 2 "$out"

# --- case patterns lose their quotes, and an arm may span lines -----------------------------
out=$($SH -c 'x="a b"; case "$x" in "a b") echo dq;; esac')
ck case-dquote-pattern dq "$out"
out=$($SH -c "case 'a*' in 'a*') echo lit;; esac")
ck case-squote-pattern lit "$out"
out=$($SH -c "case abc in 'a*') echo bad;; *) echo glob;; esac")
ck case-squote-pattern-no-glob glob "$out"
out=$($SH -c 'case a in a) echo A;; esac; case b in b) echo B;; esac')
ck case-twice-one-line "A B" "$(echo $out)"
out=$($SH -c 'if case x in x) true;; esac; then echo matched; fi')
ck case-if-condition matched "$out"
out=$($SH -c 'x=go; while case "$x" in go) true;; *) false;; esac; do echo once; x=stop; done')
ck case-while-condition once "$out"
out=$($SH -c '{ case a in a) case b in b) echo nested;; esac;; esac; }')
ck case-nested-brace nested "$out"
out=$($SH -c 'y="a b"; case "a b" in "$y") echo var;; esac')
ck case-quoted-var-pattern var "$out"
out=$($SH -c 'case ")" in ")") echo paren;; esac')
ck case-quoted-paren-pattern paren "$out"
printf '%s\n' 'case b in' '  a) echo A ;;' '  b|c)' '    echo B1' '    echo "B2 (x)"' '    ;;' '  *) echo other ;;' 'esac' > "$P-ca.sh"
out=$($SH "$P-ca.sh")
ck case-multiline-arm "B1 B2 (x)" "$(echo $out)"
printf '%s\n' 'case z in' '  a)' '    echo "(A)"' '    ;;' '  *)' '    echo star' 'esac' > "$P-ca.sh"
out=$($SH "$P-ca.sh")
ck case-multiline-last-arm star "$out"
printf '%s\n' 'case a in' '  a)' '    case b in' '      b)' '        echo inner' '        ;;' '    esac' '    echo outer' '    ;;' '  *) echo no ;;' 'esac' > "$P-ca.sh"
out=$($SH "$P-ca.sh")
ck case-multiline-nested "inner outer" "$(echo $out)"
printf '%s\n' 'f() {' '  case $1 in' '    a)' '      if [ "$2" = y ]; then' '        echo AY' '      fi' '      ;;' '    *) echo other ;;' '  esac' '}' 'f a y' 'f b' > "$P-ca.sh"
out=$($SH "$P-ca.sh")
ck case-multiline-in-function "AY other" "$(echo $out)"
rmf "$P-ca.sh"

out=$($SH -c 'cat <<"END" | cat
;; esac ${literal}
END
echo done')
ck pipe-quoted-heredoc "$(printf ';; esac ${literal}\ndone')" "$out"
out=$($SH -c 'f() { f() { echo second; }; echo first; }; f; f')
ck redefine-running-function "$(printf 'first\nsecond')" "$out"
out=$($SH -c 'for i in first do last; do echo "$i"; done')
ck for-do-word "$(printf 'first\ndo\nlast')" "$out"

for invalid in 'echo before; fi' 'echo before; ; true' 'if true; then fi'; do
    out=$($SH -c "$invalid" 2>/dev/null; echo "rc=$?")
    ck malformed-compound "rc=2" "$out"
done

echo "  $n gap cases"
if test -z "$bad"; then
    echo "gaps: PASS"
else
    echo "gaps: FAIL: $bad"
    exit 1
fi
