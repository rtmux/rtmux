#! /bin/sh
#
# Functional regression for the /bin toolbox applets. Run by the `toolbox`
# qemu-regress mode, which starts the pipe service and then execs this script.
# Each check prints "  ok NAME" or records NAME in $res; the final line is
# "toolbox: PASS" iff every check passed.
#
# The target /bin/sh is deliberately small; this suite uses only the shell
# features it actually implements (all verified on the target):
#   * $(cmd) and a single-stage $(feeder | applet); NOT `a | b | c`.
#   * `applet | grep -q PATTERN` for presence checks (exit status only).
#   * file arguments and command-line arguments for input.
#   * functions and && / || chains.
# It avoids the unimplemented corners: multi-stage pipes, `;` inside $(),
# `case` over multi-line values, here-docs feeding applets, `{ ...; }` groups,
# backslash escapes inside quotes, and writing files to /dev/shmem. Inputs are
# therefore drawn from /etc/passwd (root + rtmuxuser) and literal arguments.
#
# Applets intentionally NOT exercised here, with the reason:
#   crash, shutdown          - terminate the guest
#   login, getty, sh, vi,    - interactive / own a terminal
#     top, more, less, clear
#   setconsole, authtest     - reconfigure console / need a password prompt
#   dns, host, httpget, ip,  - network applets, covered by verify-lkl-net
#     ifconfig, nc, netstat,
#     nslookup, ping, route,
#     wget, hello
#   cp, mv, touch            - create files; /dev/shmem write-back is not
#                              observable from the shell, so only smoke-run.
#   yes                      - infinite output; the pipe service does not
#                              deliver a reader-close signal, so `yes | head`
#                              never terminates and cannot be bounded here.
#
# SPDX-License-Identifier: Apache-2.0
#
ROOTLINE="root:x:0:0:root:/root:/bin/sh"
USERLINE="rtmuxuser:x:1000:1000:RTMux default user:/home/rtmuxuser:/bin/sh"
res=""
ok() { echo "  ok $1"; }
no() { res="${res}$1 "; echo "  FAIL $1"; }

echo "toolbox: BEGIN"

# --- trivial status applets -------------------------------------------------
true && ok true || no true
false && no false || ok false
not false && ok not-true || no not-true
not true && no not-false || ok not-false
test abc = abc && ok test-eq || no test-eq

# --- echo -------------------------------------------------------------------
test "$(echo hello world)" = "hello world" && ok echo || no echo
test "$(echo -n hi)" = hi && ok echo-n || no echo-n

# --- cat / head / tail / wc (input: /etc/passwd) ----------------------------
cat /etc/passwd | grep -q rtmuxuser && ok cat || no cat
test "$(head -n 1 /etc/passwd)" = "$ROOTLINE" && ok head || no head
test "$(tail -n 1 /etc/passwd)" = "$USERLINE" && ok tail || no tail
set -- $(wc /etc/passwd)
test "$1" = 2 && ok wc || no "wc=[$1]"

# --- tee (passthrough; file write-back is not observable) -------------------
test "$(echo teed | tee /dev/null)" = teed && ok tee || no tee

# --- basename / dirname / seq -----------------------------------------------
test "$(basename /usr/lib/thing)" = thing && ok basename || no basename
test "$(dirname /usr/lib/thing)" = /usr/lib && ok dirname || no dirname
test "$(seq 1 3 | xargs)" = "1 2 3" && ok seq || no seq

# --- grep -------------------------------------------------------------------
test "$(grep root /etc/passwd)" = "$ROOTLINE" && ok grep || no grep
test "$(grep -v root /etc/passwd)" = "$USERLINE" && ok grep-v || no grep-v
test "$(grep -c : /etc/passwd)" = 2 && ok grep-c || no grep-c
test "$(grep -i ROOT /etc/passwd)" = "$ROOTLINE" && ok grep-i || no grep-i
test "$(grep -n rtmuxuser /etc/passwd)" = "2:$USERLINE" && ok grep-n || no grep-n
grep -q root /etc/passwd && ok grep-q || no grep-q
grep -q zzzzz /etc/passwd && no grep-q-miss || ok grep-q-miss
test "$(grep '^root' /etc/passwd)" = "$ROOTLINE" && ok grep-anchor || no grep-anchor

# --- tr (input via a single pipe) -------------------------------------------
test "$(echo abc | tr a-z A-Z)" = ABC && ok tr || no tr
test "$(echo a-b-c | tr -d -)" = abc && ok tr-d || no tr-d
test "$(echo aabbcc | tr -s abc)" = abc && ok tr-s || no tr-s

# --- cut --------------------------------------------------------------------
test "$(echo a:b:c:d | cut -d : -f 2)" = b && ok cut-f || no cut-f
test "$(echo a:b:c:d | cut -d : -f 2-3)" = "b:c" && ok cut-frange || no cut-frange
test "$(echo abcdef | cut -c 2-4)" = bcd && ok cut-c || no cut-c

# --- sort / uniq (limited to what single-pipe inputs allow) -----------------
test "$(sort /etc/passwd | head -n 1)" = "$ROOTLINE" && ok sort || no sort
test "$(sort -r /etc/passwd | head -n 1)" = "$USERLINE" && ok sort-r || no sort-r
sort -n /etc/passwd | grep -q root && ok sort-n || no sort-n
test "$(sort -u /etc/passwd | head -n 1)" = "$ROOTLINE" && ok sort-u || no sort-u
uniq /etc/passwd | grep -q rtmuxuser && ok uniq || no uniq
test -z "$(uniq -d /etc/passwd)" && ok uniq-d || no uniq-d
uniq -c /etc/passwd | grep -q root && ok uniq-c || no uniq-c

# --- xargs ------------------------------------------------------------------
test "$(echo one two three | xargs echo)" = "one two three" && ok xargs || no xargs

# --- expr -------------------------------------------------------------------
test "$(expr 2 + 3 '*' 4)" = 14 && ok expr-prec || no expr-prec
test "$(expr '(' 2 + 3 ')' '*' 4)" = 20 && ok expr-paren || no expr-paren
test "$(expr 10 / 3)" = 3 && ok expr-div || no expr-div
test "$(expr 7 = 7)" = 1 && ok expr-cmp || no expr-cmp
expr 1 / 0 >/dev/null 2>&1; test "$?" = 2 && ok expr-div0 || no expr-div0
expr 9223372036854775807 + 1 >/dev/null 2>&1
test "$?" != 0 && ok expr-overflow || no expr-overflow

# --- printf -----------------------------------------------------------------
test "$(printf '%s-%d' hi 5)" = "hi-5" && ok printf || no printf
test "$(printf '[%c]' abc)" = "[a]" && ok printf-c || no printf-c
test "$(printf '[%c]' '')" = "[]" && ok printf-c-empty || no printf-c-empty

# --- kill (rejection paths validate the pid guard) --------------------------
kill notapid >/dev/null 2>&1; test "$?" != 0 && ok kill-badpid || no kill-badpid
kill "" >/dev/null 2>&1; test "$?" != 0 && ok kill-emptypid || no kill-emptypid

# --- readlink (no symlinks on this fs → must fail, not crash) ----------------
readlink /etc/passwd >/dev/null 2>&1; test "$?" != 0 && ok readlink || no readlink

# --- identity / environment -------------------------------------------------
test "$(whoami)" = root && ok whoami || no whoami
id | grep -q uid=0 && ok id || no id
test "$(which sh)" = /bin/sh && ok which || no which
test -n "$(pwd)" && ok pwd || no pwd
test -n "$(printenv PATH)" && ok printenv || no printenv
test "$(env FOO=bar printenv FOO)" = bar && ok env || no env

# --- filesystem inspection --------------------------------------------------
ls /bin/grep >/dev/null 2>&1 && ok ls || no ls
test "$(find /etc/passwd)" = /etc/passwd && ok find || no find
stat /etc/passwd | grep -q size && ok stat || no stat
test -n "$(df /)" && ok df || no df
test -n "$(help)" && ok help || no help
test -n "$(mount)" && ok mount || no mount
test -n "$(ps)" && ok ps || no ps
test -n "$(date)" && ok date || no date
test -n "$(dmesg)" && ok dmesg || no dmesg

# --- timing / sync ----------------------------------------------------------
sleep 0 && ok sleep || no sleep
sync && ok sync || no sync

# --- file-mutating applets (smoke-run; write-back not observable) -----------
touch /dev/shmem/tb.a && ok touch || no touch
cp /etc/passwd /dev/null && ok cp || no cp
touch /dev/shmem/tb.m && mv /dev/shmem/tb.m /dev/shmem/tb.n && ok mv || no mv
rm /dev/shmem/tb.a >/dev/null 2>&1 && ok rm || no rm

if test -z "$res"; then
    echo "toolbox: PASS"
else
    echo "toolbox: FAIL: $res"
fi
test -z "$res" # final exit status reflects pass/fail for the init gate
