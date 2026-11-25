#! /bin/sh
#
# POSIX Shell Command Language conformance regression for RTMux /bin/sh.
# Run by the `sh-posix` qemu-regress mode. Each check prints "  ok NAME" or
# records NAME in $res; the final line is "sh-posix: PASS" iff every check
# passed.
#
# The suite uses only features the shell has; the cases where it once differed
# from dash are in gaps.sh, which also runs under dash.
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
test "$*" = "alpha beta gamma" && echo "  ok pstar" || res="${res}pstar "
test "${*}" = "alpha beta gamma" && echo "  ok braced-star" || res="${res}braced-star "
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
# (functions communicate via variables here, which also shows that a plain
#  call runs in this shell, not a subshell)
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
# a function is found (its exit status, not captured text, is what we check)
cvfn() { :; }
command -v cvfn >/dev/null && echo "  ok command-func" || res="${res}command-func "
type cvfn >/dev/null && echo "  ok type-func" || res="${res}type-func "
# type reports the kind (builtins and PATH names are visible inside $())
test "$(type test)" = "test is a shell builtin" && echo "  ok type-builtin" || res="${res}type-builtin "
test "$(type sh)" = "sh is /bin/sh" && echo "  ok type-path" || res="${res}type-path "
# the run form `command NAME args` executes NAME
test "$(command echo runform)" = runform && echo "  ok command-run" || res="${res}command-run "

# --- subshells: background and non-final pipeline builtins -----------------
# A builtin or function that is backgrounded, or feeds a pipe, runs in a child
# shell that gets this shell's functions, parameters and options. It used to
# run in the shell itself, and filling the pipe (PIPE_BUF bytes) before the
# reader was started hung; the qemu timeout catches that regressing.
set -- $(echo "$(seq 3000)" | wc -l)
test "$1" = 3000 && echo "  ok pipe-builtin-big" || res="${res}pipe-builtin-big "
bigf() {
    i=0
    while [ $i -lt 2000 ]
    do
        echo line$i
        i=$((i+1))
    done
}
set -- $(bigf | wc -l)
test "$1" = 2000 && echo "  ok pipe-func-big" || res="${res}pipe-func-big "
bigf1() { i=0; while [ $i -lt 2000 ]; do echo line$i; i=$((i+1)); done; }
set -- $(bigf1 | wc -l)
test "$1" = 2000 && echo "  ok pipe-func-oneline" || res="${res}pipe-func-oneline "
# the stage's assignments stay in the child; the status is the last stage's
i=keep
bigf | wc -l >/dev/null
test "$?" = 0 && test "$i" = keep && echo "  ok pipe-func-status" || res="${res}pipe-func-status "
true | false
test "$?" = 1 && echo "  ok pipe-status-last" || res="${res}pipe-status-last "
false | true
test "$?" = 0 && echo "  ok pipe-status-first" || res="${res}pipe-status-first "
# no shell process outlives its pipeline
set -- $(ps | grep -c sh)
np1=$1
bigf | wc -l >/dev/null
set -- $(ps | grep -c sh)
test "$np1" = "$1" && echo "  ok pipe-reaped" || res="${res}pipe-reaped "
# words reach the child intact: blanks, quotes, globs, $ and an empty word
set -- "a  b" "it's" '*' '$HOME' ""
qf() { echo "$#[$1][$2][$3][$4][$5]"; }
test "$(qf "$1" "$2" "$3" "$4" "$5" | cat)" = "5[a  b][it's][*][\$HOME][]" && echo "  ok sub-quoting" || res="${res}sub-quoting "
test "$(echo "$#" | cat)" = 5 && echo "  ok sub-params" || res="${res}sub-params "
test "$(qf x)" = "1[x][][][][]" && echo "  ok cmdsub-func" || res="${res}cmdsub-func "
set -u
test "$(echo "$-" | cat)" = u && echo "  ok sub-options" || res="${res}sub-options "
set +u
test "$(echo $$ | cat)" = "$$" && echo "  ok sub-pid" || res="${res}sub-pid "
test "$(cd /usr; pwd | cat)" = /usr && echo "  ok sub-cwd" || res="${res}sub-cwd "
# a here-document feeds a function in a non-final stage
hg() { read hv; echo "x$hv"; }
hg <<EOF | read hr
hdoc
EOF
test "$hr" = xhdoc && echo "  ok sub-heredoc" || res="${res}sub-heredoc "
# a backgrounded function runs; $(...) ends once it has exited
test "$(f(){ echo hi; }; f &)" = hi && echo "  ok bg-func" || res="${res}bg-func "
# a backgrounded loop returns at once, shows in jobs, and sets $!
while :; do :; done &
bgpid=$!
case "$(jobs)" in
    *Running*) echo "  ok bg-loop-jobs" ;;
    *) res="${res}bg-loop-jobs " ;;
esac
test -n "$bgpid" && kill "$bgpid" && echo "  ok bg-loop-kill" || res="${res}bg-loop-kill "

# --- quoting ----------------------------------------------------------------
# a backslash is literal inside single quotes, so it cannot escape the quote
test 'a\b' = "a\\b" && echo "  ok squote-backslash" || res="${res}squote-backslash "
test 'x\' = "x\\" && echo "  ok squote-trailing-backslash" || res="${res}squote-trailing-backslash "
# inside double quotes it escapes only $ ` " \ and is kept before anything else
test "a\qb" = 'a\qb' && echo "  ok dquote-backslash" || res="${res}dquote-backslash "
test "\$HOME\"\`" = '$HOME"`' && echo "  ok dquote-escape" || res="${res}dquote-escape "
# a # inside a word does not start a comment
set -- a#b
test "$1" = "a#b" && echo "  ok hash-in-word" || res="${res}hash-in-word "

# --- "$@" and field splitting -----------------------------------------------
nargs() { NA="$#:$1:$2"; }
set -- "a  b" c
nargs "$@"
test "$NA" = "2:a  b:c" && echo "  ok at-fields" || res="${res}at-fields "
nargs "x$@y"
test "$NA" = "2:xa  b:cy" && echo "  ok at-affix" || res="${res}at-affix "
nargs "${@}"
test "$NA" = "2:a  b:c" && echo "  ok at-braced" || res="${res}at-braced "
nargs "$*"
test "$NA" = "1:a  b c:" && echo "  ok star-joined" || res="${res}star-joined "
fa=""
for w in "$@"
do
    fa="$fa[$w]"
done
test "$fa" = "[a  b][c]" && echo "  ok for-at" || res="${res}for-at "
set --
nargs "$@"
test "$NA" = "0::" && echo "  ok at-none" || res="${res}at-none "
nargs "$@" ""
test "$NA" = "1::" && echo "  ok at-none-empty" || res="${res}at-none-empty "
# an unquoted expansion is split into fields; an empty one is no field
sv="1  2"
nargs $sv
test "$NA" = "2:1:2" && echo "  ok split-var" || res="${res}split-var "
ev=
nargs a $ev b
test "$NA" = "2:a:b" && echo "  ok split-empty" || res="${res}split-empty "
# an assignment word is neither split nor globbed
as=$(echo "one  two")
test "$as" = "one  two" && echo "  ok assign-cmdsub" || res="${res}assign-cmdsub "
as=$sv
test "$as" = "1  2" && echo "  ok assign-var" || res="${res}assign-var "
as=/usr/*
test "$as" = "/usr/*" && echo "  ok assign-noglob" || res="${res}assign-noglob "
export as=$(echo "x  y")
test "$as" = "x  y" && echo "  ok export-assign" || res="${res}export-assign "
case $sv in
    1*2) echo "  ok case-nosplit" ;;
    *) res="${res}case-nosplit " ;;
esac
ce=
cm=BAD
case $ce in
    *) cm=ok ;;
esac
test "$cm" = ok && echo "  ok case-empty" || res="${res}case-empty "

# --- redirections -----------------------------------------------------------
# >&2 and 1>&2 send stdout to stderr
test "$(sh -c 'echo e >&2' 2>&1 >/dev/null)" = e && echo "  ok dup-stderr" || res="${res}dup-stderr "
test "$(sh -c 'echo e 1>&2' 2>/dev/null)" = "" && echo "  ok dup-stderr-1" || res="${res}dup-stderr-1 "
# 2>&1 duplicates the pipe the command writes, not the shell's stdout
set -- $(sh -c 'echo e >&2' 2>&1 | wc -l)
test "$1" = 1 && echo "  ok dup-into-pipe" || res="${res}dup-into-pipe "
# a pipe or redirection after a closing done/fi/esac applies to the whole command
set -- $(for w in a b c; do echo $w; done | wc -l)
test "$1" = 3 && echo "  ok done-pipe" || res="${res}done-pipe "
for w in a b
do
    echo $w
done | wc -l | read dl
test "$dl" = 2 && echo "  ok done-pipe-multiline" || res="${res}done-pipe-multiline "
test "$(for w in a; do echo $w; done >/dev/null)" = "" && echo "  ok done-redir" || res="${res}done-redir "
test "$(if true; then echo e >&2; fi 2>&1)" = e && echo "  ok fi-redir" || res="${res}fi-redir "
nl=0
while read l; do nl=$((nl+1)); done < /etc/passwd
set -- $(wc -l < /etc/passwd)
test "$nl" = "$1" && echo "  ok done-input" || res="${res}done-input "
case x in
    x) echo a; echo b ;;
esac | wc -l | read cl
test "$cl" = 2 && echo "  ok esac-pipe" || res="${res}esac-pipe "
for w in a; do :; done && echo "  ok done-and" || res="${res}done-and "
test "$(for w in a; do echo hi; done 2>$(echo /dev/null | cat))" = hi && echo "  ok done-redir-cmdsub" || res="${res}done-redir-cmdsub "
set -- $(for w in a; do echo e >&2; done 2>&1 | wc -l)
test "$1" = 1 && echo "  ok done-dup-pipe" || res="${res}done-dup-pipe "

# --- prefix assignments -----------------------------------------------------
# X=1 cmd puts X in cmd's environment only
unset PX
test "$(PX=1 printenv PX)" = 1 && test -z "$PX" && echo "  ok prefix-external" || res="${res}prefix-external "
PX=old
test "$(PX=new printenv PX)" = new && test "$PX" = old && echo "  ok prefix-restore" || res="${res}prefix-restore "
PX=1 PX=2 true
test "$PX" = old && echo "  ok prefix-repeat" || res="${res}prefix-repeat "
test "$(_PU=u printenv _PU)" = u && echo "  ok prefix-underscore" || res="${res}prefix-underscore "
pxf() { PXF="$PX"; }
PX=infunc pxf
test "$PXF" = infunc && test "$PX" = old && echo "  ok prefix-func" || res="${res}prefix-func "
test "$(PX=piped printenv PX | cat)" = piped && echo "  ok prefix-pipe" || res="${res}prefix-pipe "
# several assignments alone set them all; before a special builtin they stay
PA=1 PB=2
test "$PA$PB" = 12 && echo "  ok assign-multi" || res="${res}assign-multi "
PC=3 :

# --- line continuation ------------------------------------------------------
# a trailing &&, || or | continues the list on the next line
lc=BAD
if true &&
    false; then
    lc=BAD
else
    lc=OK
fi
test "$lc" = OK && echo "  ok cont-and" || res="${res}cont-and "
lc=BAD
false ||
    lc=OK
test "$lc" = OK && echo "  ok cont-or" || res="${res}cont-or "
echo a b c |
    wc -w | read lw
test "$lw" = 3 && echo "  ok cont-pipe" || res="${res}cont-pipe "
lc=BAD
true && # a comment ends the line, not the list
    lc=OK
test "$lc" = OK && echo "  ok cont-comment" || res="${res}cont-comment "
lc=BAD
true &&

    # blank and comment lines may come before the operand
    lc=OK
test "$lc" = OK && echo "  ok cont-linebreak" || res="${res}cont-linebreak "
# a backslash before the newline joins the lines
lc=\
OK
test "$lc" = OK && echo "  ok cont-backslash" || res="${res}cont-backslash "
# a quoted or escaped | does not
set -- '|' x\|
set -- a b
test "$#" = 2 && echo "  ok cont-quoted" || res="${res}cont-quoted "

# a << in a comment does not start a here-document
hc=""
for w in a b
do
    # not a here-document: <<EOF
    hc="x$hc"
done
test "$hc" = xx && echo "  ok heredoc-comment" || res="${res}heredoc-comment "

# --- compound commands parsed at load time ----------------------------------
# eval and sh -c text is loaded like a script: one-line compounds, functions
# and several lines all work
ev=""
eval 'for w in a b; do ev="$ev$w"; done'
test "$ev" = ab && echo "  ok eval-compound" || res="${res}eval-compound "
two=$(printf 'f() { echo "f$1"; }\nfor w in 1 2; do f $w; done')
set -- $(sh -c "$two" | wc -l)
test "$1" = 2 && echo "  ok sh-c-lines" || res="${res}sh-c-lines "
# without `in`, for goes over "$@"
set -- x y
fl=""
for w; do fl="$fl$w"; done
test "$fl" = xy && echo "  ok for-no-in" || res="${res}for-no-in "
set --
fl=ok
for w in "$@"; do fl=BAD; done
test "$fl" = ok && echo "  ok for-empty" || res="${res}for-empty "
# a nested one-line loop inside a one-line if runs once per iteration
nn=""
for a in 1 2; do if true; then for b in x y; do nn="$nn$a$b"; done; fi; done
test "$nn" = 1x1y2x2y && echo "  ok nested-oneline" || res="${res}nested-oneline "

# --- trap -------------------------------------------------------------------
hibye=$(printf 'hi\nbye')
out=$(sh -c "trap 'echo bye' EXIT; echo hi")
test "$out" = "$hibye" && echo "  ok trap-exit" || res="${res}trap-exit "
# the subshells a pipeline stage or $(...) runs in do not run it again
out=$(sh -c "trap 'echo bye' EXIT; echo hi | cat; echo \"\$(echo s)\"" | wc -l)
set -- $out
test "$1" = 3 && echo "  ok trap-exit-once" || res="${res}trap-exit-once "
# one set inside a background subshell runs when that subshell exits
out=$(tf() { trap 'echo bye' EXIT; echo hi; }; tf &)
test "$out" = "$hibye" && echo "  ok trap-exit-bg" || res="${res}trap-exit-bg "
# $? in the EXIT action is the exit status, which it keeps
out=$(sh -c "trap 'echo \$?' EXIT; exit 3")
sh -c "trap : EXIT; exit 3"
test "$?" = 3 && test "$out" = 3 && echo "  ok trap-exit-status" || res="${res}trap-exit-status "
# a caught signal runs its action and the shell goes on
out=$(sh -c "trap 'echo caught' TERM; kill -TERM \$\$; echo after")
test "$out" = "$(printf 'caught\nafter')" && echo "  ok trap-term" || res="${res}trap-term "
out=$(sh -c "trap 'echo hup' HUP; kill -HUP \$\$; trap - HUP; echo after")
test "$out" = "$(printf 'hup\nafter')" && echo "  ok trap-hup" || res="${res}trap-hup "
out=$(sh -c "trap '' INT; kill -INT \$\$; echo survived")
test "$out" = survived && echo "  ok trap-ignore" || res="${res}trap-ignore "
# trap lists the traps, also in a subshell, in a form it can read back
trap 'echo it'\''s' HUP
test "$(trap)" = "trap -- 'echo it'\\''s' HUP" && echo "  ok trap-list" || res="${res}trap-list "
trap - HUP
test -z "$(trap)" && echo "  ok trap-reset" || res="${res}trap-reset "
trap bogus NOSUCHSIG 2>/dev/null
test "$?" != 0 && echo "  ok trap-bad" || res="${res}trap-bad "
# an action that starts with a digit is still an action
out=$(sh -c "trap '1; echo bye' EXIT; :" 2>/dev/null)
test "$out" = bye && echo "  ok trap-digit-action" || res="${res}trap-digit-action "
test "$PC" = 3 && echo "  ok prefix-special" || res="${res}prefix-special "
# --- IFS (POSIX 2.6.5) --------------------------------------------------------
# splitting at the characters of IFS; two non-blank delimiters bound an empty
# field, a trailing one does not
IFS=:
x="a:b::c:"
set -- $x
test "$#" = 4 && test "$1" = a && test "$3" = "" && test "$4" = c &&
    echo "  ok ifs-split" || res="${res}ifs-split "
x=":a"
set -- $x
test "$#" = 2 && test "$1" = "" && echo "  ok ifs-leading" || res="${res}ifs-leading "
# a quoted expansion is not split; "$*" joins with the first character of IFS
set -- "$x"
test "$#" = 1 && echo "  ok ifs-quoted" || res="${res}ifs-quoted "
set -- p q r
test "$*" = p:q:r && echo "  ok ifs-star" || res="${res}ifs-star "
set -- $(echo a:b)
test "$#" = 2 && echo "  ok ifs-cmdsub" || res="${res}ifs-cmdsub "
# blanks in IFS run together and trim; one beside another delimiter is part of it
IFS=' :'
x=" a  b : c "
set -- $x
test "$#" = 3 && test "$1" = a && test "$2" = b && test "$3" = c &&
    echo "  ok ifs-blank-delim" || res="${res}ifs-blank-delim "
x="a :b"
set -- $x
test "$#" = 2 && echo "  ok ifs-delim-pair" || res="${res}ifs-delim-pair "
x="a  :b"
set -- $x
test "$#" = 2 && test "$2" = b && echo "  ok ifs-blank-run-delim" || res="${res}ifs-blank-run-delim "
# a quoted word in ${...} is one field, even where it holds an IFS character
IFS=:
unset hv
set -- ${hv:-"a:b"}
test "$#" = 1 && test "$1" = a:b && echo "  ok ifs-quoted-word" || res="${res}ifs-quoted-word "
set -- ${hv:-c:d}
test "$#" = 2 && echo "  ok ifs-unquoted-word" || res="${res}ifs-unquoted-word "
IFS=' :'
# an empty IFS splits nothing; an unset one is blank, tab and newline
IFS=
x="a b:c"
set -- $x
test "$#" = 1 && echo "  ok ifs-empty" || res="${res}ifs-empty "
set -- p q
test "$*" = pq && echo "  ok ifs-empty-star" || res="${res}ifs-empty-star "
unset IFS
x="  a   b  "
set -- $x
test "$#" = 2 && test "$*" = "a b" && echo "  ok ifs-unset" || res="${res}ifs-unset "
# read splits the line the same way, and the last variable keeps the rest
IFS=: read ra rb rc <<EOF
a:b:c:d
EOF
test "$ra" = a && test "$rb" = b && test "$rc" = c:d && echo "  ok ifs-read" || res="${res}ifs-read "
test -z "${IFS+set}" && echo "  ok ifs-read-scoped" || res="${res}ifs-read-scoped "
IFS= read -r rl <<EOF
  lead and trail  
EOF
test "$rl" = "  lead and trail  " && echo "  ok ifs-read-keep" || res="${res}ifs-read-keep "
read ra rb <<EOF
   one   two three  
EOF
test "$ra" = one && test "$rb" = "two three" && echo "  ok read-trim" || res="${res}read-trim "
IFS=' :' read ra rb rc <<EOF
a :b::c
EOF
test "$ra" = a && test "$rb" = b && test "$rc" = :c && echo "  ok ifs-read-delim" || res="${res}ifs-read-delim "

# one delimiter at the end of the line ends the last field; a run keeps its empty ones
IFS=: read ra rb <<EOF
a:b:
EOF
test "$ra" = a && test "$rb" = b && echo "  ok ifs-read-trailing" || res="${res}ifs-read-trailing "
IFS=: read ra rb <<EOF
a:b::
EOF
test "$rb" = "b::" && echo "  ok ifs-read-trailing-run" || res="${res}ifs-read-trailing-run "

# --- closing and duplicating descriptors --------------------------------------
# >&WORD duplicates the descriptor WORD expands to; >&- closes: every write fails
out=$(sh -c 'echo hi >&$1' sh 1 2>/dev/null)
test "$out" = hi && echo "  ok dup-fd-var" || res="${res}dup-fd-var "
out=$(sh -c 'echo hi >&$1' sh 2 2>/dev/null)
test -z "$out" && echo "  ok dup-fd-err" || res="${res}dup-fd-err "
out=$(sh -c 'fd=2; echo hi >&$fd' 2>&1)
test "$out" = hi && echo "  ok dup-fd-expand" || res="${res}dup-fd-expand "
echo x | cat >&- 2>/dev/null
test "$?" != 0 && echo "  ok close-out" || res="${res}close-out "
echo x | cat 2>&- >/dev/null
test "$?" = 0 && echo "  ok close-err-unused" || res="${res}close-err-unused "
sh -c 'echo a >&99' 2>/dev/null
test "$?" != 0 && echo "  ok dup-bad-fd" || res="${res}dup-bad-fd "
sh -c 'echo a >&x' 2>/dev/null
test "$?" != 0 && echo "  ok dup-bad-word" || res="${res}dup-bad-word "
out=$(echo seen; echo gone >&- 2>/dev/null; echo after)
test "$out" = "$(printf 'seen\nafter')" && echo "  ok close-scoped" || res="${res}close-scoped "

# --- an assignment-only command's status --------------------------------------
x=$(false)
test "$?" = 1 && echo "  ok assign-status" || res="${res}assign-status "
x=$(exit 3)
test "$?" = 3 && echo "  ok assign-status-3" || res="${res}assign-status-3 "
x=$(true) y=$(exit 5)
test "$?" = 5 && echo "  ok assign-status-last" || res="${res}assign-status-last "
false
x=7
test "$?" = 0 && echo "  ok assign-status-plain" || res="${res}assign-status-plain "
false
x=$(echo ok)
test "$?" = 0 && echo "  ok assign-status-ok" || res="${res}assign-status-ok "

# --- "$@" with no parameters ----------------------------------------------------
set --
set -- "$@$@"
test "$#" = 0 && echo "  ok at-at-empty" || res="${res}at-at-empty "
set -- a "$@" b
test "$#" = 2 && echo "  ok at-empty-between" || res="${res}at-empty-between "
set --
set -- "$@" ""
test "$#" = 1 && echo "  ok at-empty-then-empty" || res="${res}at-empty-then-empty "
set -- 1 2
set -- "$@$@"
test "$#" = 3 && test "$2" = 21 && echo "  ok at-at-joined" || res="${res}at-at-joined "
set --

# --- trap: an action runs as soon as an interactive shell's read is interrupted
# The reader is a shell with a prompt reading commands from a pipe. A sibling
# kills it with TERM while it waits for its next line, and its trap action
# wakes the feeder, which sends that line only then: if the action had to wait
# for a line, the feeder would give up (30 s) and "after" would come first.
Q="'"
FEED="trap go=1 HUP; echo \"trap ${Q}echo trapped; kill -HUP \$\$${Q} TERM\";"
FEED="$FEED echo \"sh -c ${Q}sleep 1; kill -TERM \\\$1${Q} x \\\$\\\$ &\"; i=0;"
FEED="$FEED while test -z \"\$go\" && test \"\$i\" -lt 30; do sleep 1; i=\$((i + 1)); done;"
FEED="$FEED echo \"echo after\""
out=$(sh -c "$FEED" | sh 2>&1)
tail=${out#*trapped}
head=${out%%trapped*}
test "$tail" != "$out" && test "${tail#*after}" != "$tail" &&
    test "${head#*after}" = "$head" && echo "  ok trap-read-interrupt" ||
    res="${res}trap-read-interrupt "

# --- the quote and substitution scanner, against a reference shell ------------
# 260 cases: each line goes through eval here, and its fields must be the ones
# dash made of it (tests/host/gen-sh-quote-cases.py)
if test -f /bin/sh-posix-quote-run && test -f /bin/sh-posix-quote-cases; then
    qt=$(sh /bin/sh-posix-quote-run /bin/sh-posix-quote-cases 2>&1)
    qs=$?
    echo "$qt"
    test "$qs" = 0 && echo "  ok quote-table" || res="${res}quote-table "
else
    res="${res}quote-table-missing "
fi
# --- the gaps against a reference shell ---------------------------------------
# gaps.sh holds the cases where this shell once differed from dash, with what
# dash makes of each; it passes under dash as it is.
if test -f /bin/sh-posix-gaps; then
    SH=sh sh /bin/sh-posix-gaps 2>&1
    gs=$?
    test "$gs" = 0 && echo "  ok gaps" || res="${res}gaps "
else
    res="${res}gaps-missing "
fi
# ----------------------------------------------------------------------------
if test -z "$res"; then
    echo "sh-posix: PASS"
else
    echo "sh-posix: FAIL: $res"
fi
test -z "$res" # final exit status reflects pass/fail for the init gate
