#!/usr/bin/env python3
"""Check docs/ipc-send-ranks.tsv, the acyclic send-rank table (Q43, Q71).

  check-ipc-send-ranks.py [--table FILE] [--root DIR] [--emit-promela FILE]
                          [--omit-sinks] [--table-only] [--expect-fail] [--emit-only]

Table columns (tab separated, '#' comments): node, source paths (comma list of
directories or files), rank, sends_to (comma list of nodes, or '-').  sends_to
holds only edges that have no call site in the tree (a send made by libc on a
caller's behalf); call-site edges come from the tree.  A blocking send must
target a strictly higher rank, so every wait-for path ends at the top rank and
no cycle exists.

The tree is scanned per call site.  Every blocking msg_send*/MsgSend* call and
every open()-family call (open, openat, creat, fopen, freopen, opendir, the 64
forms) under servers/ and userland/ must be covered by an annotation comment:

    ipc-send: SRC[,SRC] -> DST[,DST]   on the call line or within the 4 lines
                                       above it; one per call site

There is no file-wide cover (`ipc-send-file:` is rejected).  Files are spliced
(backslash-newline) and lexed whole: comments, string and char literals are
blanked, annotations are read from comments only, and a call may span lines.
A member call (`ops->open(`) and a field declarator are not matched.  A use of a
send name that is not a direct call (function pointer, `(open)(...)`, macro
operand, any preprocessor line) is rejected, since it can hide a send.  A file
the lexer cannot trust (unterminated comment/string/char literal, C23 digit
separator) is rejected, not skipped.

For open() with a string-literal path the targets are derived from the
`ioserv_attach(..., "/path", ...)` literals in the tree (the resolver
roottask-core plus the owner of the longest attached prefix); an annotation that
omits one is rejected.  Other targets stay asserted.  Not scanned: libc wrappers
outside the open family, inline assembly.  The scan is a lexical aid, not a
proof of coverage (docs/ipc-deadlock-freedom.md).

SRC must be a table node whose paths cover the file.  The edge set is the union
of the annotations and the table's sends_to, and every edge is rank-checked, so
a new call site to a lower rank is rejected without editing the table.

--emit-promela writes the graph as an include for tests/spin/ipc-deadlock.pml.
--emit-only emits without checking (the planted cyclic graph).
--expect-fail inverts the exit status (negative fixtures).
"""

import argparse
import bisect
import re
import sys
from pathlib import Path

import gate_cert

ROOT = Path(__file__).resolve().parent.parent
PI_WALK_MAX = 16  # SYNC_PI_MAX_DEPTH, kernel/sync/wait.c
SCAN_PARENTS = ("servers", "userland")
SCAN_SKIP = {"userland/lib", "userland/include"}
# Names that reach a blocking send: the raw send calls and the libc wrappers that
# resolve a path through roottask (open and its relatives).  Every use of one of
# these must be a direct call with its own annotation; any other use (function
# pointer, parenthesized call, macro) is rejected, so it cannot hide a send.
SEND_NAME = re.compile(
    r"\b(msg_send(?!_notification)\w*|MsgSend\w*|open(?:at)?(?:64)?|creat(?:64)?|fopen(?:64)?|"
    r"freopen(?:64)?|opendir|popen)\b")
LIST = r"[\w-]+(?:[ \t]*,[ \t]*[\w-]+)*"  # a comma list; blanks after a comma are allowed
NOTE = re.compile(rf"ipc-send(-file)?:\s*({LIST})\s*->\s*({LIST})")
NOTE_REACH = 4  # lines above a call that an ipc-send annotation may sit
SPLICE = re.compile(r"\\[ \t]*\r?\n")
STRING_ARGS = re.compile(r'("[^"\\]*"\s*)+')
ATTACH = re.compile(r"\bioserv_attach\s*\(")
NOT_A_TYPE = {"return", "case", "else", "do", "sizeof", "goto"}
RESOLVER = "roottask-core"  # every path open is resolved by the roottask endpoint


class Source:
    """One lexed C file.

    text is the file after C line splicing (backslash-newline removed), so a call
    split by a splice (`op\\<nl>en(`) is one token.  code is text with every
    comment, string and char literal blanked to spaces.  comments is a list of
    (offset, text); strings is a list of literal start offsets; problems is a
    list of (offset, reason) for input the lexer cannot trust: an unclosed /*
    comment, an unterminated string or char literal, or a C23 digit separator
    (1'000).  The caller fails closed on any of them, since a mis-lexed tail could
    hide calls.  Handles escapes.  Not handled: trigraphs, raw strings.  Offsets
    are into text; line() maps one back to its original line.
    """

    def __init__(self, raw):
        self.nl = [i for i, c in enumerate(raw) if c == "\n"]
        kept, self.origin, last = [], [], 0
        for m in SPLICE.finditer(raw):
            kept.append((last, m.start()))
            last = m.end()
        kept.append((last, len(raw)))
        self.text = "".join(raw[a:b] for a, b in kept)
        for a, b in kept:
            self.origin.extend(range(a, b))
        self.comments, self.strings, self.problems = [], [], []
        self.code = self._lex()

    def line(self, off):
        """Original 1-based line of an offset into text."""
        return bisect.bisect_left(self.nl, self.origin[min(off, len(self.origin) - 1)]) + 1

    def _lex(self):
        text, n = self.text, len(self.text)
        out = list(text)
        i = 0

        def blank(a, b):
            for k in range(a, b):
                if out[k] != "\n":
                    out[k] = " "

        while i < n:
            c = text[i]
            pair = text[i:i + 2]
            if pair == "/*":
                j = text.find("*/", i + 2)
                if j < 0:
                    self.problems.append((i, "unterminated /* comment"))
                end = n if j < 0 else j + 2
                self.comments.append((i, text[i:end]))
                blank(i, end)
                i = end
            elif pair == "//":
                nl = text.find("\n", i)
                end = n if nl < 0 else nl
                self.comments.append((i, text[i:end]))
                blank(i, end)
                i = end
            elif c in "\"'":
                if c == "'" and i and (text[i - 1].isalnum() or text[i - 1] == "_"):
                    k = i
                    while k and (text[k - 1].isalnum() or text[k - 1] == "_"):
                        k -= 1
                    if text[k:i] not in ("L", "u", "U", "u8"):  # else an encoding prefix
                        self.problems.append((i, "C23 digit separator is not supported"))
                        i += 1
                        continue
                j = i + 1
                while j < n and text[j] != c and text[j] != "\n":
                    j += 2 if text[j] == "\\" else 1
                if j >= n or text[j] != c:
                    what = "string" if c == '"' else "char"
                    self.problems.append((i, f"unterminated {what} literal"))
                    end = min(j, n)
                else:
                    end = j + 1
                if c == '"':
                    self.strings.append(i)
                blank(i, end)
                i = end
            else:
                i += 1
        return "".join(out)

    def directive(self, off):
        """The preprocessor line (text from its #) holding this offset, or None."""
        start = self.code.rfind("\n", 0, off) + 1
        end = self.code.find("\n", off)
        line = self.code[start:len(self.code) if end < 0 else end].lstrip()
        return line if line.startswith("#") else None

    def args(self, paren):
        """Spans [(start, end)] of the arguments of the call whose ( is at paren."""
        spans, depth, start = [], 0, paren + 1
        for i in range(paren, len(self.code)):
            c = self.code[i]
            if c in "([{":
                depth += 1
            elif c in ")]}":
                depth -= 1
                if depth == 0:
                    if self.code[start:i].strip() or spans:
                        spans.append((start, i))
                    return spans
            elif c == "," and depth == 1:
                spans.append((start, i))
                start = i + 1
        return spans

    def literal(self, span):
        """The text of an argument made only of string literals, else None."""
        s = self.text[span[0]:span[1]].strip()
        if not STRING_ARGS.fullmatch(s):
            return None
        return "".join(re.findall(r'"([^"]*)"', s))

    def declares(self, m):
        """Is this match a field or variable declarator (`io_open_t open;`)?"""
        prev = re.search(r"([A-Za-z_]\w*)\s*$", self.code[max(0, m.start() - 256):m.start()])
        return bool(prev and prev.group(1) not in NOT_A_TYPE
                    and self.code[m.end():].lstrip().startswith(";"))

    def sites(self):
        """Yield (offset, name, kind): "call" (a direct call), "ref" (a use that is
        not a direct call) or "macro" (on a preprocessor line)."""
        for m in SEND_NAME.finditer(self.code):
            before = self.code[max(0, m.start() - 256):m.start()].rstrip()
            if before.endswith("->") or before.endswith("."):
                continue  # a member, not the libc function
            d = self.directive(m.start())
            if d and re.match(r"#\s*include\b", d):
                continue  # a header name is not a call
            if d:
                kind = "macro"
            elif self.declares(m):
                continue
            elif self.code[m.end():].lstrip().startswith("("):
                kind = "call"
            else:
                kind = "ref"
            yield m.start(), m.group(1), kind


def load(path):
    nodes = {}
    errors = []
    for n, line in enumerate(path.read_text().splitlines(), 1):
        if not line.strip() or line.startswith("#"):
            continue
        f = line.split("\t")
        if len(f) != 4 or not f[2].isdigit():
            errors.append(f"{path}:{n}: want node<TAB>dirs<TAB>rank<TAB>sends_to")
            continue
        if f[0] in nodes:
            errors.append(f"{path}:{n}: duplicate node {f[0]}")
            continue
        sends = [] if f[3] == "-" else f[3].split(",")
        nodes[f[0]] = (f[1].split(","), int(f[2]), sends)
    return nodes, errors


def source_files(root):
    for parent in SCAN_PARENTS:
        for d in sorted((root / parent).glob("*")):
            if d.is_dir() and f"{parent}/{d.name}" not in SCAN_SKIP:
                yield from sorted(list(d.rglob("*.c")) + list(d.rglob("*.h")))


def covers(paths, rel):
    return any(rel == p or rel.startswith(p + "/") for p in paths)


def top_owner(nodes, rel):
    """The highest-rank node whose source paths cover rel, or None.

    servers/roottask holds both roottask nodes; they are one endpoint, so the
    highest rank (roottask-core) names it."""
    held = [n for n, (paths, _, _) in nodes.items() if covers(paths, rel)]
    return max(held, key=lambda n: nodes[n][1]) if held else None


def attach_table(nodes, sources):
    """{path: {node}} from every `ioserv_attach(dpp, attr, "/path", ...)` whose
    path is a string literal: the file that attaches a path is its owner."""
    table = {}
    for rel, src in sources.items():
        owner = top_owner(nodes, rel)
        for m in ATTACH.finditer(src.code):
            if src.directive(m.start()):
                continue
            spans = src.args(m.end() - 1)
            path = src.literal(spans[2]) if len(spans) > 2 else None
            if owner and path and path != "/":
                table.setdefault(path, set()).add(owner)
    return table


def derive(table, path):
    """Targets of open(path): the resolver plus the owner of the longest attached
    prefix, or None when the path is not under a literal attach (a root-filesystem
    path, whose mount point is chosen at run time, stays asserted)."""
    best = max((a for a in table if path == a or path.startswith(a.rstrip("/") + "/")),
               key=len, default=None)
    return None if best is None else {RESOLVER} | table[best]


def path_arg(name):
    """Index of the path argument of a path-opening call, None if it has none."""
    if name.startswith("msg_send") or name.startswith("MsgSend") or name == "popen":
        return None
    return 1 if name.startswith("openat") else 0


def scan(nodes, root):
    """Per-call-site scan: return ({(src, dst): "file:line"}, errors, derived).

    derived counts the open() sites whose annotated targets were checked against
    the attach table rather than only against the ranks."""
    edges, errors, derived = {}, [], 0
    sources = {}
    for path in source_files(root):
        sources[path.relative_to(root).as_posix()] = Source(path.read_text(errors="replace"))
    table = attach_table(nodes, sources)

    def declare(rel, line, srcs, dsts):
        ok = False
        for src in srcs.split(","):
            if src not in nodes:
                errors.append(f"{rel}:{line}: ipc-send names unknown node {src}")
            elif not covers(nodes[src][0], rel):
                errors.append(f"{rel}:{line}: ipc-send node {src} does not cover "
                              "this file (not one of its source paths)")
            else:
                ok = True
                for dst in dsts.split(","):
                    edges.setdefault((src, dst), f"{rel}:{line}")
        return ok

    for rel, src in sources.items():
        for off, why in src.problems:  # fail closed: the rest of the file is untrusted
            errors.append(f"{rel}:{src.line(off)}: {why}; cannot scan this file")
        notes = {}  # line number -> (annotation was valid, its targets)
        for off, body in src.comments:  # annotations are read from comments only
            for m in NOTE.finditer(body):
                at = src.line(off + m.start())
                if m.group(1):
                    errors.append(f"{rel}:{at}: ipc-send-file is retired; annotate "
                                  "each open() call with its own ipc-send")
                    continue
                srcs = re.sub(r"\s+", "", m.group(2))
                dsts = re.sub(r"\s+", "", m.group(3))
                notes[at] = (declare(rel, at, srcs, dsts), set(dsts.split(",")))
        for off, name, kind in src.sites():  # every use needs its own note
            at = src.line(off)
            if kind == "macro":
                errors.append(f"{rel}:{at}: `{name}` on a preprocessor line: a macro "
                              "or directive can hide a send; call it directly")
                continue
            if kind == "ref":
                errors.append(f"{rel}:{at}: `{name}` used other than as a direct call "
                              "(function pointer, parenthesized call or macro operand) "
                              "can hide a send")
                continue
            near = [k for k in range(at, at - NOTE_REACH - 1, -1) if notes.get(k, (0,))[0]]
            if not near:
                errors.append(f"{rel}:{at}: blocking send `{name}(` has no "
                              "ipc-send annotation")
                continue
            dsts = notes.pop(near[0])[1]  # one annotation covers one call site
            idx = path_arg(name)
            if idx is None:
                continue
            spans = src.args(src.code.index("(", off))
            lit = src.literal(spans[idx]) if len(spans) > idx else None
            want = derive(table, lit) if lit else None
            if want is not None:
                derived += 1
                if not want <= dsts:
                    errors.append(f"{rel}:{at}: `{name}(\"{lit}\")` reaches "
                                  f"{','.join(sorted(want - dsts))} by the ioserv_attach "
                                  "table, which the annotation omits")
        for at, (ok, _) in sorted(notes.items()):  # a stale note still adds edges
            if ok:
                errors.append(f"{rel}:{at}: ipc-send annotation covers no call site "
                              "(stale, or more than 4 lines from its call)")
    return edges, errors, derived


def check_dirs(nodes, root):
    return [f"{name}: source path {d} does not exist"
            for name, (dirs, _, _) in nodes.items()
            for d in dirs if not (root / d).exists()]


def merge_edges(nodes, found):
    """Union of the table's sends_to and the annotated call sites."""
    edges = {(n, d): "table" for n, (_, _, sends) in nodes.items() for d in sends}
    for k, v in found.items():
        edges.setdefault(k, v)
    return edges


def check_ranks(nodes, edges):
    errors = []
    for (src, dst), where in sorted(edges.items()):
        if src not in nodes or dst not in nodes:
            errors.append(f"{where}: {src} -> {dst}: unknown node")
        elif nodes[dst][1] <= nodes[src][1]:
            errors.append(f"{where}: {src} (rank {nodes[src][1]}) -> {dst} (rank "
                          f"{nodes[dst][1]}): edge to an equal or lower rank")
    return errors


def depth(nodes, edges):
    """Longest wait-for chain in edges; only meaningful once ranks pass."""
    memo = {}
    succ = {n: [d for (s, d) in edges if s == n] for n in nodes}

    def walk(n):
        if n not in memo:
            memo[n] = max((1 + walk(d) for d in succ[n]), default=0)
        return memo[n]
    return max(walk(n) for n in nodes)


def emit_promela(nodes, edges, path, omit_sinks=False):
    """Write the graph for ipc-deadlock.pml.

    Clients (no incoming edge, at least one outgoing) are listed so the gate can
    run them in combinations.  Sinks (no outgoing edge) only receive and reply,
    so they cannot lie on a wait-for cycle; omit_sinks drops them and the edges
    into them, which shrinks the state space enough to check more clients at
    once.
    """
    sources = {s for (s, _) in edges}
    names = [n for n in nodes if not omit_sinks or n in sources]
    idx = {n: i for i, n in enumerate(names)}
    pairs = {(idx[s], idx[d]) for (s, d) in edges if s in idx and d in idx}
    succ = {i for i, _ in pairs}
    preds = {j for _, j in pairs}
    out = ["/* generated from the send-rank table and call sites; do not edit */",
           f"#define NNODES {len(names)}"]
    out += [f"/* {i} = {n} */" for i, n in enumerate(names)]
    out.append("/* clients: " + " ".join(
        str(i) for i in range(len(names)) if i in succ and i not in preds) + " */")
    out.append("#define EDGE_SET " + " ".join(
        f"edge[{i * len(names) + j}] = 1;" for i, j in sorted(pairs)))
    Path(path).write_text("\n".join(out) + "\n")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--table", default=str(ROOT / "docs/ipc-send-ranks.tsv"))
    ap.add_argument("--root", default=str(ROOT))
    ap.add_argument("--emit-promela")
    ap.add_argument("--expect-fail", action="store_true")
    ap.add_argument("--omit-sinks", action="store_true",
                    help="with --emit-promela: leave sink nodes out of the model")
    ap.add_argument("--table-only", action="store_true",
                    help="check the table's own edges; skip the tree scan (fixtures)")
    ap.add_argument("--emit-only", action="store_true",
                    help="with --emit-promela: skip all checks (planted graphs)")
    a = ap.parse_args()
    root = Path(a.root)
    nodes, errors = load(Path(a.table))
    if a.emit_only and a.emit_promela and not errors:
        emit_promela(nodes, merge_edges(nodes, {}), a.emit_promela,
                     a.omit_sinks)
        return 0
    found, derived = {}, 0
    if not errors and not a.table_only:
        errors = check_dirs(nodes, root)
    if not errors and not a.table_only:
        found, errors, derived = scan(nodes, root)
    edges = merge_edges(nodes, found)
    if not errors:
        errors = check_ranks(nodes, edges)
    d = 0 if errors else depth(nodes, edges)
    if d > PI_WALK_MAX:
        errors.append(f"wait-for depth {d} exceeds PI walk bound {PI_WALK_MAX}")
    for e in errors:
        print(f"check-ipc-send-ranks: {e}", file=sys.stderr)
    if a.expect_fail:
        if not errors:
            print("check-ipc-send-ranks: expected rejection, table accepted",
                  file=sys.stderr)
        return 0 if errors else 1
    if errors:
        return 1
    if a.emit_promela:
        emit_promela(nodes, edges, a.emit_promela, a.omit_sinks)
    gate_cert.emit(f"{Path(a.table).name}: {len(nodes)} nodes, {len(edges)} edges "
                   f"({len(edges) - sum(len(v[2]) for v in nodes.values())} "
                   f"beyond the table from call-site annotations), all to strictly "
                   f"higher rank, longest chain {d} (PI walk bound {PI_WALK_MAX}); "
                   f"{derived} literal-path open sites checked against the attach table")
    print(f"check-ipc-send-ranks: ok ({len(nodes)} nodes, {len(edges)} edges, "
          f"depth {d})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
