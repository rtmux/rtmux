#!/usr/bin/env python3
# RTMux is freely redistributable under the Apache License. See the file
# "LICENSE" for information on usage and redistribution of this file.
#
# SPDX-License-Identifier: Apache-2.0
"""Static lock-rank acquisition-order check over the real kernel source.

Complements CONFIG_LOCK_DEBUG (kernel/debug/lock-debug.c), which only catches an
inversion on a schedule that actually runs.  This gate reads the LOCK_CLASS_*
ranks from kernel/include/lock.h, resolves the class of every kspinlock_lock()
target from the kspinlock_init() sites, and walks each function to flag a nested
acquire that goes from a higher rank down to a lower one (a rank inversion) or a
cycle.

Scope and honesty:
  - Intra-procedural.  A lock held across a call into another function that
    acquires a second lock is not tracked here; runtime LOCK_DEBUG covers that.
  - Flow-insensitive.  The per-function scan is a straight-line walk, so a lock
    that is released on an early error path but still held on the fall-through
    path (unlock nested in a conditional, then a later acquire) is treated as
    released.  This under-reports rather than over-reports: it never invents a
    false inversion, but it can miss an inversion split across a conditional
    unlock.  That residual is the runtime LOCK_DEBUG's job.  The guarantee here
    is "no inversion in straight-line / properly-nested acquire regions", not a
    sound whole-program proof.
  - kspinlock_lock / kspinlock_lock_irqsave sites plus the SCHED_LOCK* wrapper
    macros (MACRO_LOCKS) that expand to kspinlock_lock on the dispatch lock.
  - Same-class distinct-instance nesting (two channels, two vec_slocks, the
    sync_lock_two turnstile pair, two per-hart dispatch locks) is NOT a rank
    inversion; it is collected and printed as an allowlist (-v) so a reviewer
    sees it was considered, matching the runtime checker which permits
    same-class distinct-instance acquires.
  - An acquire whose class cannot be resolved fails the gate (fail closed): it
    could mask a real inversion, so it must be annotated (ANNOTATIONS) or the
    resolver extended, never silently passed.

Exit status: non-zero on any rank inversion OR any unresolved acquire site.
"""

import os
import re
import sys

KERNEL = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                      "kernel")

# Acquire whose target class the resolver cannot reach on its own.  Keying an
# entry here is the documented "annotate" path; value is the LOCK_CLASS_* name.
ANNOTATIONS = {
    # remote_ipc is a file-scope anonymous struct in kernel/ipc/channel.c whose
    # .chp field is a channel_t*, so remote_ipc.chp->lock is a CHANNEL lock.
    "remote_ipc.chp->lock": "LOCK_CLASS_CHANNEL",
}

# Lock primitive / runtime checker itself — not lock consumers.  Excluded from
# the acquire scan but lock.h is still read for the enum ranks.
SKIP = {os.path.join("include", "lock.h"), os.path.join("debug", "lock-debug.c")}

ACQUIRE_RE = re.compile(
    r'\bkspinlock_lock(?:_irqsave)?\s*\(\s*(&?[^,;()]+?)\s*\)')
RELEASE_RE = re.compile(
    r'\bkspinlock_unlock(?:_irqrestore)?\s*\(\s*(&?[^,;()]+?)\s*[,)]')
INIT_RE = re.compile(
    r'\bkspinlock_init\s*\(\s*&?\s*([^,]+?)\s*,\s*(LOCK_CLASS_\w+)\s*,')

# Lock-macro wrappers that expand to kspinlock_lock on a fixed class, so the
# unexpanded source never shows kspinlock_lock (kernel/include/core/macros.h).
# name -> (kind, class, fixed_instance_or_None).  A None instance uses the
# macro argument, so distinct dispatch objects stay distinct instances.
MACRO_LOCKS = {
    "SCHED_LOCK_LOCAL":   ("acq", "LOCK_CLASS_SCHED", "LOCAL_DPP"),
    "SCHED_UNLOCK_LOCAL": ("rel", "LOCK_CLASS_SCHED", "LOCAL_DPP"),
    "SCHED_LOCK":         ("acq", "LOCK_CLASS_SCHED", None),
    "SCHED_UNLOCK":       ("rel", "LOCK_CLASS_SCHED", None),
}
# Longest-name-first so SCHED_LOCK_LOCAL wins over SCHED_LOCK.
MACRO_RE = re.compile(r'\b(' + '|'.join(
    sorted(MACRO_LOCKS, key=len, reverse=True)) + r')\s*\(([^;)]*)\)')


def c_files():
    for root, _dirs, files in os.walk(KERNEL):
        for f in files:
            if f.endswith((".c", ".h")):
                yield os.path.join(root, f)


def read(path):
    with open(path, encoding="utf-8", errors="replace") as fh:
        return fh.read()


def _blank(m):
    # Replace a comment with spaces but keep its newlines, so byte offsets and
    # line counts downstream stay correct.
    return re.sub(r'[^\n]', ' ', m.group(0))


def strip_comments(text):
    text = re.sub(r'/\*.*?\*/', _blank, text, flags=re.S)
    text = re.sub(r'//[^\n]*', _blank, text)
    return text


def parse_ranks():
    """LOCK_CLASS_* enum declaration order -> integer rank."""
    text = strip_comments(read(os.path.join(KERNEL, "include", "lock.h")))
    m = re.search(r'enum\s+lock_class\s*\{(.*?)\}', text, re.S)
    if not m:
        sys.exit("check-lock-rank: cannot find enum lock_class")
    ranks, nxt = {}, 0
    for name in re.findall(r'\b(LOCK_CLASS_\w+)\b', m.group(1)):
        if name == "LOCK_CLASS_NR":
            continue
        ranks[name] = nxt
        nxt += 1
    return ranks


# lightweight type resolution

KEYWORDS = frozenset((
    "return", "sizeof", "if", "for", "while", "switch", "typedef", "do",
    "else", "case", "goto", "const", "static", "volatile", "unsigned",
    "signed", "struct", "union", "enum"))


def base_type(tok):
    return re.sub(r'^struct\s+', '', tok).strip()


def matched_brace(text, open_idx):
    depth, j = 0, open_idx
    while j < len(text):
        if text[j] == '{':
            depth += 1
        elif text[j] == '}':
            depth -= 1
            if depth == 0:
                return j
        j += 1
    return len(text)


def top_level_fields(body):
    """Split a struct body on top-level ';' (skip nested union/struct bodies)."""
    out, depth, cur = {}, 0, []
    for ch in body:
        if ch == '{':
            depth += 1
        elif ch == '}':
            depth -= 1
        if ch == ';' and depth == 0:
            line = ''.join(cur).strip()
            m = re.search(r'\b((?:struct\s+)?[A-Za-z_]\w*)\s*\*?\s*'
                          r'([A-Za-z_]\w*)\s*(?:\[[^\]]*\])?\s*$', line)
            if m:
                out[m.group(2)] = base_type(m.group(1))
            cur = []
        else:
            cur.append(ch)
    return out


def parse_struct_fields(files_text):
    """Returns (fields, aliases): (tag)->{field:type}, typedef_name->tag."""
    fields, aliases = {}, {}
    for text in files_text.values():
        # typedef struct tag name;   (alias only)
        for m in re.finditer(r'\btypedef\s+struct\s+([A-Za-z_]\w*)\s+'
                             r'([A-Za-z_]\w*)\s*;', text):
            aliases[m.group(2)] = m.group(1)
        # struct tag { ... }   and   typedef struct [tag] { ... } name;
        for m in re.finditer(r'\b(?:typedef\s+)?struct\s+([A-Za-z_]\w*)?\s*\{', text):
            tag = m.group(1)
            open_idx = m.end() - 1    # regex ends at the '{'
            close = matched_brace(text, open_idx)
            body = text[open_idx + 1:close]
            trailer = text[close + 1:close + 80]
            tm = re.match(r'\s*([A-Za-z_]\w*)\s*;', trailer)
            name = tag or (tm.group(1) if tm else None)
            if not name:
                continue
            fields.setdefault(name, {}).update(top_level_fields(body))
            if tag and tm:               # typedef struct tag { } name;
                aliases[tm.group(1)] = tag
    return fields, aliases


def parse_globals(text):
    """File-scope var -> base type (best effort, for arrays/objects locks index)."""
    g = {}
    depth = 0
    for line in text.split('\n'):
        depth += line.count('{') - line.count('}')
        if depth != 0:
            continue
        m = re.match(r'\s*(?:static\s+|const\s+|volatile\s+)*'
                     r'((?:struct\s+)?[A-Za-z_]\w*)\s*\*?\s*'
                     r'([A-Za-z_]\w*)\s*(?:\[[^\]]*\])?\s*[;=]', line)
        if m and m.group(1) not in ("return", "typedef"):
            g[m.group(2)] = base_type(m.group(1))
    return g


def parse_functions(text):
    """Yield (name, start, end, scope-vars) for each function body."""
    for m in re.finditer(r'(^|\n)[A-Za-z_][\w\s\*]*?\b([A-Za-z_]\w*)\s*\('
                         r'([^;{]*?)\)\s*\{', text):
        name = m.group(2)
        i = text.index('{', m.end() - 1)
        j = matched_brace(text, i)
        body = text[i:j]
        # Build var->type from every "TYPE [*]var" occurrence in params + body.
        # A shared-type multi-declarator ("connect_t *cop, *cop_master") only
        # names the type once; the trailing "*var" continuations are caught by
        # the second pattern, which reuses the most recent declared type.
        scope = {}
        text2 = m.group(3) + ';' + body
        for d in re.finditer(
                r'\b((?:struct\s+)?[A-Za-z_]\w*)\s+\*?\s*([A-Za-z_]\w*)'
                r'\s*(?=[;,)=\[])', text2):
            t = d.group(1)
            if t in KEYWORDS:
                continue
            scope.setdefault(d.group(2), base_type(t))
        for d in re.finditer(
                r'\b((?:struct\s+)?[A-Za-z_]\w*)\s+((?:\*?\s*[A-Za-z_]\w*'
                r'\s*(?:=[^,;]+)?\s*,\s*)+)\*\s*([A-Za-z_]\w*)', text2):
            if d.group(1) not in KEYWORDS:
                scope.setdefault(d.group(3), base_type(d.group(1)))
        yield name, i, j, scope, body


class Resolver:
    def __init__(self, struct_fields, aliases, file_globals):
        self.sf = struct_fields
        self.al = aliases
        self.g = file_globals

    def canon(self, t):
        seen = set()
        while t in self.al and t not in seen:
            seen.add(t)
            t = self.al[t]
        return t

    def type_of(self, chain, scope):
        parts = re.split(r'->|\.', chain)
        first = re.sub(r'\[[^\]]*\]', '', parts[0]).strip()
        t = scope.get(first) or self.g.get(first)
        if t is None:
            return None
        for f in parts[1:]:
            f = re.sub(r'\[[^\]]*\]', '', f).strip()
            t = self.sf.get(self.canon(t), {}).get(f)
            if t is None:
                return None
        return self.canon(t)


def normalize(expr):
    return expr.strip().lstrip('&').strip()


def main():
    ranks = parse_ranks()
    files_text = {p: strip_comments(read(p)) for p in c_files()}
    struct_fields, aliases = parse_struct_fields(files_text)
    scan = {p: t for p, t in files_text.items()
            if os.path.relpath(p, KERNEL) not in SKIP}

    # class maps
    global_class = {}          # bare-ident lock -> LOCK_CLASS_*
    type_field_class = {}      # (base_type, field) -> LOCK_CLASS_*
    file_globals = {}

    for path, text in scan.items():
        file_globals[path] = parse_globals(text)

    # First pass: record globals-by-name from init sites (position independent).
    for path, text in scan.items():
        res = Resolver(struct_fields, aliases, file_globals[path])
        for _n, _s, _e, scope, body in parse_functions(text):
            for m in INIT_RE.finditer(body):
                expr, cls = normalize(m.group(1)), m.group(2)
                if re.fullmatch(r'[A-Za-z_]\w*', expr):
                    global_class[expr] = cls
                    continue
                if '->' in expr or '.' in expr:
                    field = re.split(r'->|\.', expr)[-1]
                    field = re.sub(r'\[[^\]]*\]', '', field).strip()
                    base = expr[:expr.rfind(field)].rstrip('.->')
                    t = res.type_of(base, scope)
                    if t:
                        type_field_class[(t, field)] = cls

    def classify(expr, scope, res, alias):
        expr = normalize(expr)
        if expr in ANNOTATIONS:
            return ANNOTATIONS[expr]
        if re.fullmatch(r'[A-Za-z_]\w*', expr):
            if expr in global_class:
                return global_class[expr]
            if expr in alias:
                return alias[expr]
            return None
        field = re.split(r'->|\.', expr)[-1]
        field = re.sub(r'\[[^\]]*\]', '', field).strip()
        base = expr[:expr.rfind(field)].rstrip('.->')
        t = res.type_of(base, scope)
        if t and (t, field) in type_field_class:
            return type_field_class[(t, field)]
        # vec_slock only exists on process_t today; fall back when the base type
        # cannot be resolved (e.g. kap->prp->vec_slock).  A resolved base of a
        # different type would have matched type_field_class above, so this
        # cannot silently misclassify an existing lock.
        if field == "vec_slock":
            return "LOCK_CLASS_PROCVEC"
        return None

    inversions = []
    warnings = []
    same_class = []

    for path, text in scan.items():
        res = Resolver(struct_fields, aliases, file_globals[path])
        line_at = lambda off: text.count('\n', 0, off) + 1
        rel = os.path.relpath(path, os.path.dirname(KERNEL))
        for _n, start, _end, scope, body in parse_functions(text):
            # local kspinlock_t* aliases: var = &something
            alias = {}
            for am in re.finditer(r'\b([A-Za-z_]\w*)\s*=\s*([^;]+;)', body):
                v = am.group(1)
                if v in scope and scope[v] == "kspinlock_t":
                    tgts = re.findall(r'&\s*([A-Za-z_][\w.\->\[\]]*)', am.group(2))
                    classes = {classify(t, scope, res, {}) for t in tgts}
                    classes.discard(None)
                    if len(classes) == 1:
                        alias[v] = classes.pop()

            held = []  # list of (rank, cls, inst, line)
            # events: (offset, kind, cls_or_None, instance).  kspinlock sites
            # resolve their class here; macro sites carry a fixed class.
            events = []
            for m in ACQUIRE_RE.finditer(body):
                e = normalize(m.group(1))
                events.append((m.start(), "acq", classify(e, scope, res, alias), e))
            for m in RELEASE_RE.finditer(body):
                e = normalize(m.group(1))
                events.append((m.start(), "rel", classify(e, scope, res, alias), e))
            for m in MACRO_RE.finditer(body):
                kind, cls, fixed = MACRO_LOCKS[m.group(1)]
                inst = fixed or normalize(m.group(2)).strip('()')
                events.append((m.start(), kind, cls, inst))
            events.sort(key=lambda e: e[0])

            for off, kind, cls, expr in events:
                line = line_at(start + off)
                if kind == "rel":
                    for i in range(len(held) - 1, -1, -1):
                        if held[i][2] == expr:
                            held.pop(i)
                            break
                    else:
                        # alias/direct mismatch: drop nearest same-class
                        for i in range(len(held) - 1, -1, -1):
                            if held[i][1] == cls:
                                held.pop(i)
                                break
                    continue
                if cls is None:
                    warnings.append(f"{rel}:{line}: unresolved lock class for "
                                    f"'{expr}' (annotate in ANNOTATIONS)")
                    continue
                rank = ranks.get(cls)
                if rank is None:
                    warnings.append(f"{rel}:{line}: unknown class {cls}")
                    continue
                for hrank, hcls, hexpr, hline in held:
                    if rank < hrank:
                        inversions.append(
                            f"{rel}:{line}: rank inversion: acquire {cls}"
                            f"({rank}) '{expr}' while holding {hcls}({hrank}) "
                            f"'{hexpr}' taken at {rel}:{hline}")
                    elif rank == hrank and hexpr != expr:
                        same_class.append(
                            f"{rel}:{line}: same-class nest {cls}: '{expr}' "
                            f"under '{hexpr}' ({rel}:{hline})")
                held.append((rank, cls, expr, line))

    verbose = "-v" in sys.argv or "--verbose" in sys.argv
    if same_class and verbose:
        print("Same-class distinct-instance nests (allowlist — reviewed, "
              "must be address-ordered, e.g. sync_lock_two):")
        for s in sorted(set(same_class)):
            print("  " + s)
        print()
    if warnings:
        # Fail closed: an acquire whose class we cannot resolve could mask a
        # real inversion, so an unannotated site is a hard failure, not a note.
        print("Unresolved acquire sites (annotate in ANNOTATIONS or extend the "
              "resolver):")
        for w in sorted(set(warnings)):
            print("  " + w)
        print()
    if inversions:
        print("LOCK-RANK INVERSIONS:")
        for v in sorted(set(inversions)):
            print("  " + v)
    if inversions or warnings:
        print(f"\ncheck-lock-rank: FAIL ({len(set(inversions))} inversion(s), "
              f"{len(set(warnings))} unresolved site(s))")
        return 1
    print("check-lock-rank: ok (0 rank inversions, "
          f"{len(set(same_class))} same-class nest(s) [-v to list])")
    return 0


if __name__ == "__main__":
    sys.exit(main())
