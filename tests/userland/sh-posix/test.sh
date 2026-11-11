#! /bin/sh
#
# POSIX Shell Command Language conformance regression for RTMux /bin/sh.
# Run by the `sh-posix` qemu-regress mode. Each check prints "  ok NAME" or
# records NAME in $res; the final line is "sh-posix: PASS" iff every check
# passed.
#
# Uses only shell features that are already implemented (no functions, command
# groups, or arithmetic yet), so the suite itself never depends on an
# unimplemented feature.
#
# SPDX-License-Identifier: Apache-2.0
#
res=""

echo "sh-posix: BEGIN"

# --- positional parameters --------------------------------------------------
set -- alpha beta gamma
test "$#" = 3 && echo "  ok pcount" || res="${res}pcount "
test "$1" = alpha && echo "  ok p1" || res="${res}p1 "
test "$3" = gamma && echo "  ok p3" || res="${res}p3 "
test "$@" = "alpha beta gamma" && echo "  ok pat" || res="${res}pat "
test "${@}" = "alpha beta gamma" && echo "  ok braced-at" || res="${res}braced-at "
test "${1x:-fallback}" = fallback && echo "  ok digit-name" || res="${res}digit-name "
shift
test "$1" = beta && echo "  ok shift1" || res="${res}shift1 "
test "$#" = 2 && echo "  ok shift-count" || res="${res}shift-count "
shift 2
test "$#" = 0 && echo "  ok shift2" || res="${res}shift2 "

# --- parameter expansion ----------------------------------------------------
unset MISSING
test "${MISSING:-def}" = def && echo "  ok default" || res="${res}default "
HAVE=value
test "${HAVE:-def}" = value && echo "  ok default-set" || res="${res}default-set "
test "${HAVE:+alt}" = alt && echo "  ok alt" || res="${res}alt "
test "${MISSING:+alt}" = "" && echo "  ok alt-unset" || res="${res}alt-unset "
test "${#HAVE}" = 5 && echo "  ok length" || res="${res}length "
unset ASN
: "${ASN:=assigned}"
test "$ASN" = assigned && echo "  ok assign" || res="${res}assign "
# ${x:?} must fail a non-interactive shell (run in a child so it can't abort us)
sh -c 'unset Q; : "${Q:?missing}"' 2>/dev/null
test "$?" != 0 && echo "  ok qmark-fatal" || res="${res}qmark-fatal "

# --- prefix/suffix trims ----------------------------------------------------
P=foo.bar.baz
test "${P#*.}" = bar.baz && echo "  ok trim-hash" || res="${res}trim-hash "
test "${P##*.}" = baz && echo "  ok trim-dhash" || res="${res}trim-dhash "
test "${P%.*}" = foo.bar && echo "  ok trim-pct" || res="${res}trim-pct "
test "${P%%.*}" = foo && echo "  ok trim-dpct" || res="${res}trim-dpct "

# --- the : null builtin -----------------------------------------------------
: && echo "  ok colon" || res="${res}colon "

# --- command substitution ---------------------------------------------------
test "$(echo hello)" = hello && echo "  ok cmdsub" || res="${res}cmdsub "
test "$(echo a b c)" = "a b c" && echo "  ok cmdsub-multi" || res="${res}cmdsub-multi "
CS=$(echo assigned)
test "$CS" = assigned && echo "  ok cmdsub-assign" || res="${res}cmdsub-assign "
test "pre$(echo mid)post" = premidpost && echo "  ok cmdsub-embed" || res="${res}cmdsub-embed "
test "`echo btick`" = btick && echo "  ok backtick" || res="${res}backtick "
test "$(echo $(echo nest))" = nest && echo "  ok cmdsub-nest" || res="${res}cmdsub-nest "
V=seen
test "$(echo $V)" = seen && echo "  ok cmdsub-var" || res="${res}cmdsub-var "
# field splitting over a substitution, and a `;` inside must not split the word
n=""
for w in $(echo one; echo two; echo three)
do
    n="x$n"
done
test "$n" = xxx && echo "  ok cmdsub-split" || res="${res}cmdsub-split "
# empty unquoted substitution drops out; quoted yields one empty field
set -- a $(true) b
test "$#" = 2 && echo "  ok cmdsub-empty-drop" || res="${res}cmdsub-empty-drop "
set -- "$(true)"
test "$#" = 1 && echo "  ok cmdsub-empty-quoted" || res="${res}cmdsub-empty-quoted "
# a ) inside quotes within a substitution must not close it early
test "$(echo "a)b")" = "a)b" && echo "  ok cmdsub-paren-q" || res="${res}cmdsub-paren-q "

# --- while / until / break / continue ---------------------------------------
# (string-concat counters, since arithmetic is not implemented yet)
wi=""
while test "$wi" != xxx
do
    wi="x$wi"
done
test "$wi" = xxx && echo "  ok while" || res="${res}while "
ui=""
until test "$ui" = xx
do
    ui="x$ui"
done
test "$ui" = xx && echo "  ok until" || res="${res}until "
bk=""
for w in a b c d e
do
    test "$w" = c && break
    bk="x$bk"
done
test "$bk" = xx && echo "  ok break" || res="${res}break "
ct=""
for w in a b c d
do
    test "$w" = b && continue
    ct="x$ct"
done
test "$ct" = xxx && echo "  ok continue" || res="${res}continue "
b2=""
for a in 1 2 3
do
    for b in 1 2 3
    do
        b2="x$b2"
        break 2
    done
done
test "$b2" = x && echo "  ok break2" || res="${res}break2 "
wb=""
while true
do
    wb="x$wb"
    test "$wb" = xxx && break
done
test "$wb" = xxx && echo "  ok while-break" || res="${res}while-break "

# --- functions + return -----------------------------------------------------
# (functions communicate via variables here: they are not visible inside $(),
#  which spawns a fresh shell)
greet() {
    G=hello
}
greet
test "$G" = hello && echo "  ok func" || res="${res}func "
addpair() {
    AP="$1-$2"
}
addpair a b
test "$AP" = a-b && echo "  ok func-args" || res="${res}func-args "
set -- outer1 outer2
inner() {
    IN="$1"
}
inner innerarg
test "$IN" = innerarg && echo "  ok func-posn" || res="${res}func-posn "
test "$1" = outer1 && echo "  ok func-restore" || res="${res}func-restore "
retval() {
    return 3
}
retval
test "$?" = 3 && echo "  ok return" || res="${res}return "
early() {
    EARLY=first
    return 0
    EARLY=second
}
early
test "$EARLY" = first && echo "  ok return-early" || res="${res}return-early "
oneliner() { OL=oneline; }
oneliner
test "$OL" = oneline && echo "  ok func-oneline" || res="${res}func-oneline "
# a function using control flow internally
classify() {
    if test "$1" = big
    then
        CL=large
    else
        CL=small
    fi
}
classify big
test "$CL" = large && echo "  ok func-if" || res="${res}func-if "
# return outside a function is a no-op and must not stop the script
return 5
test done = done && echo "  ok return-toplevel" || res="${res}return-toplevel "
# a redefinition replaces the earlier body
rf() { RF=one; }
rf() { RF=two; }
rf
test "$RF" = two && echo "  ok func-redef" || res="${res}func-redef "

# --- arithmetic $(( )) and eval ---------------------------------------------
test "$(( 2 + 3 * 4 ))" = 14 && echo "  ok arith-prec" || res="${res}arith-prec "
test "$(( (2 + 3) * 4 ))" = 20 && echo "  ok arith-paren" || res="${res}arith-paren "
test "$(( 10 / 3 ))" = 3 && echo "  ok arith-div" || res="${res}arith-div "
test "$(( 10 % 3 ))" = 1 && echo "  ok arith-mod" || res="${res}arith-mod "
AX=5
test "$(( AX * 2 ))" = 10 && echo "  ok arith-var" || res="${res}arith-var "
test "$(( $AX + 1 ))" = 6 && echo "  ok arith-dollar" || res="${res}arith-dollar "
test "$(( 3 > 2 ))" = 1 && echo "  ok arith-cmp" || res="${res}arith-cmp "
test "$(( 1 && 0 ))" = 0 && echo "  ok arith-and" || res="${res}arith-and "
test "$(( 1 << 4 ))" = 16 && echo "  ok arith-shift" || res="${res}arith-shift "
test "$(( -5 + 2 ))" = -3 && echo "  ok arith-neg" || res="${res}arith-neg "
test "$(( 7 & 3 ))" = 3 && echo "  ok arith-bitand" || res="${res}arith-bitand "
test "$(( 5 | 2 ))" = 7 && echo "  ok arith-bitor" || res="${res}arith-bitor "
# && / || short-circuit: the dead 1/0 branch must not raise an error
test "$(( 1 || 1 / 0 ))" = 1 && echo "  ok arith-shortcirc" || res="${res}arith-shortcirc "

# --- pathname expansion (glob) and tilde ------------------------------------
# glob against /usr (romfs, which supports directory listing)
set -- /usr/b*
test "$1" = /usr/bin && echo "  ok glob-star" || res="${res}glob-star "
set -- /usr/bi?
test "$1" = /usr/bin && echo "  ok glob-quest" || res="${res}glob-quest "
# multiple matches expand to sorted words (/usr/bin then /usr/conf)
set -- /usr/*
test "$1" = /usr/bin && test "$2" = /usr/conf && echo "  ok glob-multi" || res="${res}glob-multi "
# no match -> the literal pattern is kept
set -- /usr/zzz_nomatch*
test "$1" = "/usr/zzz_nomatch*" && echo "  ok glob-nomatch" || res="${res}glob-nomatch "
# a quoted metacharacter is literal (no expansion)
set -- "/usr/*"
test "$1" = "/usr/*" && echo "  ok glob-quoted" || res="${res}glob-quoted "
# a word mixing an escaped and an unquoted metachar is not globbed (stays literal)
set -- /usr/bi\*n?
test "$1" = "/usr/bi*n?" && echo "  ok glob-mixed" || res="${res}glob-mixed "
# tilde at word start expands to $HOME
HOME=/testhome
test ~ = /testhome && echo "  ok tilde" || res="${res}tilde "
test ~/sub = /testhome/sub && echo "  ok tilde-path" || res="${res}tilde-path "
# tilde after an (empty) expansion is not at word start -> literal
EMPTY=
set -- "$EMPTY"~
test "$1" = "~" && echo "  ok tilde-notstart" || res="${res}tilde-notstart "

# --- special parameters and read --------------------------------------------
# $$ is the shell pid: nonempty and stable within the shell
sp1=$$
sp2=$$
test -n "$sp1" && test "$sp1" = "$sp2" && echo "  ok dollar-pid" || res="${res}dollar-pid "
# $- reflects the current set options
set -u
case "$-" in
    *u*) echo "  ok dollar-flags" ;;
    *) res="${res}dollar-flags " ;;
esac
set +u
# read returns nonzero at end of input (so while-read loops terminate)
read eofv < /dev/null
test "$?" != 0 && echo "  ok read-eof" || res="${res}read-eof "
# read splits on whitespace; the last variable gets the remainder. RTMux runs
# the last pipeline stage in-process (no fork), so the assignments persist here;
# subshell-scoping shells would not, so this line also documents that behavior.
echo "one two three four" | read r1 r2 r3
test "$r1" = one && test "$r3" = "three four" && echo "  ok read-split" || res="${res}read-split "
eval 'EV=evaluated'
test "$EV" = evaluated && echo "  ok eval" || res="${res}eval "
CMD=echo
eval "$CMD ok" >/dev/null && echo "  ok eval-var" || res="${res}eval-var "

# --- here-documents ---------------------------------------------------------
# an unquoted body expands parameters; `read` consumes it from stdin
HX=world
read hd1 <<EOF
hello $HX
EOF
test "$hd1" = "hello world" && echo "  ok heredoc" || res="${res}heredoc "
# a quoted delimiter suppresses expansion
read hd2 <<'EOF'
hello $HX
EOF
test "$hd2" = 'hello $HX' && echo "  ok heredoc-quoted" || res="${res}heredoc-quoted "
# <<- strips leading tabs from the body and the delimiter line
read hd3 <<-EOF
	indented
	EOF
test "$hd3" = indented && echo "  ok heredoc-strip" || res="${res}heredoc-strip "
# arithmetic and command substitution expand in an unquoted body
read hd4 <<EOF
$(( 6 * 7 ))
EOF
test "$hd4" = 42 && echo "  ok heredoc-arith" || res="${res}heredoc-arith "
# words left on the command line survive the redirect; read splits them
read hda hdb <<EOF
one two three
EOF
test "$hda" = one && test "$hdb" = "two three" && echo "  ok heredoc-args" || res="${res}heredoc-args "
# a body line that leads with a reserved word must not mis-cut an enclosing loop
hk=""
for w in a b
do
    read hdk <<EOF
done
EOF
    hk="x$hk"
done
test "$hk" = xx && test "$hdk" = done && echo "  ok heredoc-in-loop" || res="${res}heredoc-in-loop "

# --- set -u (nounset) and set -x (xtrace) -----------------------------------
# referencing an unset parameter under `set -u` fails (run in a child)
sh -c 'set -u; echo "$DEFINITELY_UNSET_XYZ"' >/dev/null 2>&1
test "$?" != 0 && echo "  ok nounset" || res="${res}nounset "
# the failed expansion aborts the command: nothing after it on the line runs
nsout=$(sh -c 'set -u; echo "$DEFINITELY_UNSET_XYZ"; echo after' 2>/dev/null)
test -z "$nsout" && echo "  ok nounset-suppress" || res="${res}nounset-suppress "
# a default-word form tolerates the unset parameter, so no error
sh -c 'set -u; echo "${DEFINITELY_UNSET_XYZ:-fallback}"' >/dev/null 2>&1
test "$?" = 0 && echo "  ok nounset-default" || res="${res}nounset-default "
# a set (even empty) parameter is fine under -u
sh -c 'EMPTY=; set -u; echo "$EMPTY"' >/dev/null 2>&1
test "$?" = 0 && echo "  ok nounset-empty-ok" || res="${res}nounset-empty-ok "
# set -x traces to stderr, leaving stdout clean
xt=$(sh -c 'set -x; echo traced' 2>/dev/null)
test "$xt" = traced && echo "  ok xtrace" || res="${res}xtrace "

# --- set -e (errexit) -------------------------------------------------------
# (run in children: errexit exits the shell, which would kill this suite)
# a failed command ending an AND-OR list exits before the next command
ee1=$(sh -c 'set -e; false; echo after' 2>/dev/null)
test -z "$ee1" && echo "  ok errexit" || res="${res}errexit "
# a non-final `&&` operand may fail without exiting; only the list end counts
ee2=$(sh -c 'set -e; false && true; echo after' 2>/dev/null)
test "$ee2" = after && echo "  ok errexit-andor" || res="${res}errexit-andor "
# the final operand failing does exit
ee3=$(sh -c 'set -e; true && false; echo after' 2>/dev/null)
test -z "$ee3" && echo "  ok errexit-and-last" || res="${res}errexit-and-last "
# a failure caught by `||` does not exit
ee4=$(sh -c 'set -e; false || echo recovered' 2>/dev/null)
test "$ee4" = recovered && echo "  ok errexit-or" || res="${res}errexit-or "
# set -e is ignored while testing an if/while condition. This runs in the test
# shell itself (sh -c cannot host a compound command; the parser needs the
# multi-line form). If suppression were broken, `if false` would exit the suite
# before it can print PASS.
eok=fail
set -e
if false
then
    eok="BUG if-body-ran"
fi
while false
do
    eok="BUG while-body-ran"
done
set +e
eok=ok
test "$eok" = ok && echo "  ok errexit-cond-suppressed" || res="${res}errexit-cond-suppressed "

# --- command -v / type ------------------------------------------------------
# a builtin resolves to its own name
test "$(command -v echo)" = echo && echo "  ok command-builtin" || res="${res}command-builtin "
# an external resolves to its PATH location
test "$(command -v sh)" = /bin/sh && echo "  ok command-path" || res="${res}command-path "
# an unknown name yields nonzero status (run directly: a $() assignment would
# not propagate the substitution's exit status to $?)
command -v definitely_not_a_command_xyz >/dev/null 2>&1
test "$?" != 0 && echo "  ok command-none" || res="${res}command-none "
# a function is found -- run in THIS shell, since $() spawns a child that lacks
# the definition (so its exit status, not its captured text, is what we check)
cvfn() { :; }
command -v cvfn >/dev/null && echo "  ok command-func" || res="${res}command-func "
type cvfn >/dev/null && echo "  ok type-func" || res="${res}type-func "
# type reports the kind (builtins and PATH names are visible inside $())
test "$(type test)" = "test is a shell builtin" && echo "  ok type-builtin" || res="${res}type-builtin "
test "$(type sh)" = "sh is /bin/sh" && echo "  ok type-path" || res="${res}type-path "
# the run form `command NAME args` executes NAME
test "$(command echo runform)" = runform && echo "  ok command-run" || res="${res}command-run "

# ----------------------------------------------------------------------------
if test -z "$res"; then
    echo "sh-posix: PASS"
else
    echo "sh-posix: FAIL: $res"
fi
test -z "$res" # final exit status reflects pass/fail for the init gate
