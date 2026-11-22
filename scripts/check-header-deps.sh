#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
# Header dependency tracking gate: a touched header must rebuild every object
# that includes it, in every component.
#
#   lint     Every Makefile compile recipe passes $(DEPFLAGS), and every Makefile
#            with one calls dep-include (mk/build.mk). Single logical recipe lines
#            only; the behavioural check below is the backstop for anything odd.
#            Run on the real tree and on mutated copies, so a broken lint cannot
#            pass silently.
#   behave   On a built tree, take the objects the generated .d files say depend
#            on a header, touch the header, and require `make -n` to rebuild every
#            one of them. A .d file that is never included, or an object missing
#            from a dep-include call, shows up here. The header's mtime is restored
#            on every exit path, and `make -n` must report nothing stale before and
#            after, so the check cannot pass vacuously on an unbuilt tree. A third
#            step touches every dep-include stamp and requires every object that
#            has a .d file to go stale, which catches an object left out of a list.
#
# Usage: scripts/check-header-deps.sh (needs a built tree: make)
set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd -P)"

# The build tree: make's OUT, the same default as mk/build.mk. Exported, so
# every inner make builds into it; made absolute like mk/build.mk does.
OUT=${OUT:-$ROOT/build}
case $OUT in /*) ;; *) OUT=$ROOT/$OUT ;; esac
export OUT
tmp=
hdr=
ref=
saved=

# Stamp times are saved as touch -r reference files ($saved/N, paths listed in
# $saved/list): GNU and BSD touch both take -r, unlike -d @epoch.
restore_stamps() {
    local i=0 p
    [ -n "$saved" ] && [ -s "$saved/list" ] || return 0
    while IFS= read -r p; do
        i=$((i + 1))
        # A save that failed part way left no reference for the rest.
        [ -e "$saved/$i" ] && touch -r "$saved/$i" "$p"
    done <"$saved/list"
    : >"$saved/list"
}
cleanup() {
    [ -n "$hdr" ] && [ -e "$ref" ] && touch -r "$ref" "$ROOT/$hdr"
    restore_stamps
    [ -n "$ref" ] && rm -f "$ref"
    [ -n "$saved" ] && rm -rf "$saved"
    [ -n "$tmp" ] && rm -rf "$tmp"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
trap 'exit 129' HUP

fail() { echo "check-header-deps: FAIL: $*"; exit 1; }

# lint FILE...: print each violation, return 1 if any.
lint() {
    local f joined recipes bad rc=0
    for f; do

        # Join backslash continuations, then find compile recipes (-c). Drop
        # comments too, so a mention of DEPFLAGS or dep-include in one is not
        # taken for a use.
        joined=$(awk '{ if (sub(/\\$/, "")) { b = b $0; next } l = b $0; b = ""
                        sub(/(^|[ \t])#.*/, "", l); print l }' "$f")

        # The host tools compile and link in one command and cannot use -MMD, so
        # they must depend on the shared header list instead.
        if grep -q '\$(HOST_CC)' <<<"$joined"; then
            grep -q '^\$(PROGS): \$(HOST_HDRS)' <<<"$joined" || {
                echo "lint: $f: host tool recipes without '\$(PROGS): \$(HOST_HDRS)'"
                rc=1
            }
        fi
        recipes=$(grep -E '\$\((CC|AS|CXX)\)[^#]* -c( |$)' <<<"$joined" || true)
        [ -z "$recipes" ] && continue
        bad=$(grep -vE '\$\((LKL_)?DEPFLAGS\)' <<<"$recipes" || true)
        if [ -n "$bad" ]; then
            echo "lint: $f: compile recipe without \$(DEPFLAGS):"
            sed 's/^/    /' <<<"$bad"
            rc=1
        fi

        # A recipe that reaches the LKL headers through -isystem must use
        # LKL_DEPFLAGS (-MD): $(DEPFLAGS) passes the DEPFLAGS lint above but
        # drops them.
        bad=$(grep 'LKL_CPPFLAGS' <<<"$recipes" | grep -vF '$(LKL_DEPFLAGS)' || true)
        if [ -n "$bad" ]; then
            echo "lint: $f: LKL compile recipe without \$(LKL_DEPFLAGS):"
            sed 's/^/    /' <<<"$bad"
            rc=1
        fi
        case $f in
        Makefile | */Makefile)
            grep -Eq '^[[:space:]]*\$\(eval[[:space:]]+\$\(call dep-include' <<<"$joined" || {
                echo "lint: $f: has compile recipes but no dep-include call"
                rc=1
            }
            ;;
        esac
    done
    return $rc
}

# A read loop, not mapfile: macOS ships Bash 3.2.
FILES=()
while IFS= read -r f; do
    FILES+=("$f")
done < <(git -C "$ROOT" ls-files -co --exclude-standard -- \
    'Makefile' '*/Makefile' '*.mk' ':!externals')
[ "${#FILES[@]}" -ge 20 ] || fail "found fewer than 20 Makefiles (not a git checkout?)"

# lint, then prove it catches two seeded mutations
out=$(lint "${FILES[@]/#/$ROOT/}") || { echo "$out"; fail "lint"; }

tmp=$(mktemp -d)
mkdir "$tmp/k" "$tmp/l"
grep -v 'call dep-include' "$ROOT/kernel/Makefile" >"$tmp/k/Makefile"
lint "$tmp/k/Makefile" >/dev/null && fail "self-test: removing a dep-include call was not caught"
sed 's/^\$(eval \$(call dep-include/# &/' "$ROOT/kernel/Makefile" >"$tmp/k/Makefile"
lint "$tmp/k/Makefile" >/dev/null && fail "self-test: a commented-out dep-include call was accepted"
sed 's/ \$(DEPFLAGS)//' "$ROOT/lib/libc/Makefile" >"$tmp/l/Makefile"
lint "$tmp/l/Makefile" >/dev/null && fail "self-test: removing \$(DEPFLAGS) from a recipe was not caught"
mkdir "$tmp/f"
sed '/^\t\$(Q)\$(CC)/s/LKL_DEPFLAGS/DEPFLAGS/' "$ROOT/mk/lkl-server.mk" >"$tmp/f/lkl-server.mk"
lint "$tmp/f/lkl-server.mk" >/dev/null && fail "self-test: an LKL recipe with \$(DEPFLAGS) instead of \$(LKL_DEPFLAGS) was not caught"
grep -q '^LKL_DEPFLAGS *= *-MD\b' "$ROOT/mk/lkl-server.mk" || fail "lint: LKL_DEPFLAGS in mk/lkl-server.mk must be -MD (-MMD drops the -isystem LKL headers)"
echo "check-header-deps: lint ok"

# behavioural check

# Objects `make -n` would compile, one absolute path per line. Extra arguments
# go to make (-B: every object the current config builds).
compiles() {
    local out
    out=$(env -u MAKEFLAGS make -C "$ROOT" -n "$@" 2>&1) || fail "make -n $* failed:
$(tail -5 <<<"$out")"
    grep -E ' -c ' <<<"$out" | grep -oE -- ' -o [^ ]+\.o' | sed 's/^ -o //' | sort -u
}

stale=$(compiles | wc -l)
[ "$stale" -eq 0 ] || fail "$stale objects are stale before the test; build the tree first (make)"

# The objects the current config builds. An object another config left in the
# same tree (kernel/debug/inject.o after a CONFIG_DEBUG_INJECT=y build) is in no
# dep-include call of this one and is never rebuilt, so it is not checked.
if ! graph=$(compiles -B); then
    echo "$graph" # the failure compiles printed
    exit 1
fi
[ -n "$graph" ] || fail "make -n -B plans no compiles"

# built LIST: the objects of LIST (one per line) that exist and that the current
# config builds.
built() {
    local o
    for o in $1; do [ -e "$o" ] && echo "$o"; done | sort -u | comm -12 - <(echo "$graph")
}

# header | text to find in .d files | build dir that must NOT rebuild (optional)
#        | minimum dependents (optional, default 3)
# The version header is a generated, per-commit file; keeping it out of the
# common headers means a new commit recompiles only its few users, not the tree.
CASES=(
    "kernel/include/core/args.h|core/args.h|"
    "include/errno.h|include/errno.h|"
    "internal/include/sys/rtmux-core.h|sys/rtmux-core.h|"
    "internal/include/rtmux-version.h|rtmux-version.h|$OUT/lib/libc/"
)

# LKL servers: with CONFIG_LKL_SERVERS=y over a tree built with them (and an LKL
# archive), a header of each LKL tree must also rebuild the server objects. It
# reaches them through -isystem, which -MMD omits, so the LKL recipes use -MD. A
# tree without LKL header entries fails here instead of skipping the case, or
# the leg would pass without testing anything. The flag comes from the
# environment or .config, so a configured tree is never skipped silently.
lkl_on=${CONFIG_LKL_SERVERS:-$(sed -n 's/^CONFIG_LKL_SERVERS=//p' "$ROOT/.config" 2>/dev/null)}
if [ "$lkl_on" = y ]; then
    for out in out-fsd out-netd; do

        # Take the header from host.d: host.o is one of the objects that had no
        # .d file before -MD, so the case cannot be satisfied by main.o alone.
        lkl_hdr=$(grep -hoE -m1 "prebuilt/lkl-rv64/$out/[^ ]+\.h" "$OUT/servers/lkl-${out#out-}/host.d" 2>/dev/null | head -1 || true)
        [ -n "$lkl_hdr" ] || fail "CONFIG_LKL_SERVERS=y but no $out header in lkl-${out#out-}/host.d (build the tree with the LKL servers first)"
        CASES+=("$lkl_hdr|$lkl_hdr||1")
    done
fi

# The default build's output only; other trees under build/ (lkl-regress,
# opts-*) come from other targets and are not part of `make`'s graph.
BUILD_DIRS=()
for d in arch kernel lib servers tests userland; do
    [ -d "$OUT/$d" ] || continue
    if [ "$d" = servers ] && [ "$lkl_on" != y ]; then

        # make -n only plans the LKL servers when they are configured in;
        # objects a past LKL build left behind are not part of this graph.
        for sd in "$OUT"/servers/*/; do
            [ -d "$sd" ] || continue
            case $sd in */lkl-*) ;; *) BUILD_DIRS+=("${sd%/}") ;; esac
        done
        continue
    fi
    BUILD_DIRS+=("$OUT/$d")
done
[ "${#BUILD_DIRS[@]}" -gt 0 ] || fail "no build output under $OUT (build first: make)"

ref=$(mktemp)
rc=0
for c in "${CASES[@]}"; do
    IFS='|' read -r h needle forbid min <<<"$c"
    want=$(grep -rlF --include='*.d' -- "$needle" "${BUILD_DIRS[@]}" | sed 's/\.d$/.o/' | sort -u)
    want=$(built "$want")
    n=$(grep -c . <<<"$want")
    [ "$n" -ge "${min:-3}" ] || fail "only $n built objects depend on $h (missing .d files? build first)"

    touch -r "$ROOT/$h" "$ref"
    hdr=$h
    touch "$ROOT/$hdr"
    got=$(compiles)
    touch -r "$ref" "$ROOT/$hdr"

    miss=$(comm -23 <(echo "$want") <(echo "$got"))
    if [ -n "$miss" ]; then
        echo "check-header-deps: FAIL: touching $hdr does not rebuild $(grep -c . <<<"$miss") of $n dependents, e.g.:"
        head -3 <<<"$miss" | sed "s|$ROOT/||; s/^/    /"
        rc=1
    fi
    if [ -n "$forbid" ] && grep -q "^$forbid" <<<"$got"; then
        echo "check-header-deps: FAIL: touching $hdr rebuilds objects under ${forbid#"$ROOT"/}"
        rc=1
    fi
done
hdr=

# Host tools: every tool depends on every header in the trees it includes from.
# Build them first (cheap) so the baseline is clean, then touch one header.
host_stale() {
    env -u MAKEFLAGS make -C "$ROOT/tools/host" -n 2>&1 | grep -oE 'HOSTCC [A-Za-z0-9-]+' | sort -u
}
env -u MAKEFLAGS make -C "$ROOT/tools/host" >/dev/null 2>&1 || fail "host tools do not build"
[ -z "$(host_stale)" ] || fail "host tools are stale before the test"
touch -r "$ROOT/include/errno.h" "$ref"
hdr=include/errno.h
touch "$ROOT/$hdr"
got=$(host_stale)
touch -r "$ref" "$ROOT/$hdr"
hdr=
for tool in mkasmoff mkromfs outcomes-test; do
    grep -qx "HOSTCC $tool" <<<"$got" || {
        echo "check-header-deps: FAIL: touching include/errno.h does not rebuild host tool $tool"
        rc=1
    }
done

# Completeness: every object that has a .d file must be listed in a dep-include
# call. Touching every stamp makes exactly the listed objects stale, so any
# built object missing from the result is one whose header changes would be
# ignored.
saved=$(mktemp -d)
find "${BUILD_DIRS[@]}" -name .deps >"$saved/list" || fail "cannot list the .deps stamps"
n=0
while IFS= read -r p; do
    n=$((n + 1))
    touch -r "$p" "$saved/$n" || fail "cannot save the time of $p"
done <"$saved/list"
[ "$n" -gt 0 ] || fail "no .deps stamps found"
tr '\n' '\0' <"$saved/list" | xargs -0 touch
got=$(compiles)
restore_stamps
all=$(for o in $(find "${BUILD_DIRS[@]}" -name '*.d' | sed 's/\.d$/.o/'); do
    [ -e "$o" ] && echo "$o"
done | sort -u)
others=$(comm -23 <(echo "$all") <(echo "$graph"))
all=$(comm -12 <(echo "$all") <(echo "$graph"))
if [ -n "$others" ]; then
    echo "check-header-deps: skipping $(grep -c . <<<"$others") objects the current config does not build, e.g.:"
    head -3 <<<"$others" | sed "s|$ROOT/||; s/^/    /"
fi
miss=$(comm -13 <(echo "$got") <(echo "$all"))
if [ -n "$miss" ]; then
    echo "check-header-deps: FAIL: $(grep -c . <<<"$miss") built objects are not in any dep-include call, e.g.:"
    head -3 <<<"$miss" | sed "s|$ROOT/||; s/^/    /"
    rc=1
fi

after=$(compiles | wc -l)
[ "$after" -eq 0 ] || { echo "check-header-deps: FAIL: timestamps were not restored ($after stale)"; rc=1; }
[ "$rc" -eq 0 ] && echo "check-header-deps: ok"
exit $rc
