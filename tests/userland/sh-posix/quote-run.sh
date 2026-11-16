#! /bin/sh
#
# Run by sh-posix-test: every quoting case of the table in the file named by
# $1 (two lines each: the command line, then the count and fields a reference
# POSIX shell made of it) goes through this shell's own tokenizer, with eval, in
# an empty directory so that no pathname expansion matches. Prints the cases
# that differ, then "quote-table: PASS" or "quote-table: FAIL".
#
# Regenerate the table with tests/host/gen-sh-quote-cases.py.
#
# SPDX-License-Identifier: Apache-2.0
#
table=$1
# Pathname expansion must match nothing: use a directory with no files in it.
dir=
for d in /dev/shmem /tmp; do
    if test -z "$dir" && cd "$d" 2>/dev/null; then
        set -- *
        if test "$1" = "*"; then
            dir=$d
        fi
    fi
done
if test -z "$dir"; then
    echo "quote-table: FAIL: no empty directory"
    exit 1
fi
cd "$dir" || exit 1
A='x y'
B=
C=' a  b '
D='p*q'
unset Z
n=0
bad=0
while IFS= read -r line && IFS= read -r want; do
    n=$((n + 1))
    set --
    eval "set -- $line"
    got=$#
    for f in "$@"; do
        got="$got<$f>"
    done
    if test "$got" != "$want"; then
        bad=$((bad + 1))
        echo "  quote-case: [$line] want [$want] got [$got]"
    fi
done < "$table"
echo "  $n quoting cases, $bad differ"
if test "$bad" = 0 && test "$n" -ge 200; then
    echo "quote-table: PASS"
else
    echo "quote-table: FAIL"
    exit 1
fi
