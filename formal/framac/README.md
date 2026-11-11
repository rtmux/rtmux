# Frama-C on the RTMux kernel

Frama-C with the WP plugin, Alt-Ergo and Z3, from the opam switch named in
`scripts/verify-framac.sh`. That header is the one place the required
versions are written down; nothing here restates them.

```
make verify                          # the gate: parse, link, then wp
scripts/verify-framac.sh parse       # each source through the front end
scripts/verify-framac.sh link        # all sources as one program
scripts/verify-framac.sh wp          # discharge the ACSL contracts

formal/framac/fc.sh <frama-c args>   # front end: kernel include path + flags
formal/framac/preprocess.sh [file…]  # kernel sources -> pp/*.i for the MCP server
formal/framac/sweep-arith.sh         # whole-kernel arithmetic sweep -> sweep/*.log
formal/framac/machdep_gcc_rv64.yaml  # the rv64 machine model, see below
```

`pp/` and `sweep/` are generated and stay out of git.

The sweep is not part of the gate: it runs far too long, and most of what it
reports is missing contracts rather than defects, so it is an audit tool.

## Where the contracts live

In the kernel sources, as ACSL comments that every compiler ignores.
`kernel/cap/table.c` is the first. A source opts in with three markers:

```c
/* framac-verify: cap_lock,cap_unlock,cap_lookup_pin_typed */
/* framac-scan: kernel/cap/table.c kernel/include/cap-table.h */
/* framac-min-goals: 90 */
```

`framac-verify` is the proof set. `framac-scan` lists the files searched for
contracts that ought to be in it. `framac-min-goals` is a floor on how many
obligations the run must generate.

The last two exist because "Proved goals: N / N" is not evidence on its own.
Gut a function body and it proves 0 of 0. Leave a contracted helper out of the
proof set and WP assumes its contract instead of checking it, so every goal
above it rests on an unchecked axiom while the run still reports success.
`scripts/framac-verdict.py` applies both checks along with the exit status and
a scan for input errors; its header explains each one. Both ideas come from
../elfuse, which has a recorded case where replacing a contracted helper's body
with a constant contradicting its own ensures still printed a full pass.

Raising `framac-min-goals` when adding proved functions is deliberate: it
should only ever move because someone decided it should.

Function-scoped rather than file-scoped for a reason worth knowing before you
add the second one: a contract creates a proof obligation at every call site.
Give `cap_lock` a `requires \valid(lock)` and every caller must now establish
it, so an uncontracted public wrapper starts failing. Contracts spread
downward-up, and the marker is what lets that happen one function at a time
instead of all at once.

An earlier version kept a copy of the proved function in a separate file.
That is worth avoiding: the copy went green while the kernel moved, which is
the failure mode a verification gate exists to prevent.

## What the front end changes, and what that costs

The kernel is freestanding and RISC-V; Frama-C is neither. `fc.sh` bridges
the gap, and each bridge bounds what a result means:

| Deviation | Why | What it costs |
| --- | --- | --- |
| `-machdep gcc_rv64` | Frama-C ships no RISC-V machdep | nothing known. Built from the x86-64 model with `char_is_unsigned` flipped, since RISC-V `char` is unsigned; all other sizes and alignments already matched |
| `shim/prelude.h` erases C11 atomics | Frama-C has neither `_Atomic` nor the `__atomic_*` builtins | every result is single-threaded. No WP goal here says anything about SMP races; those belong to the litmus gates. Weak CAS is also modeled as strong, which removes spurious-failure executions |
| `-std c23` | gcc defaults to gnu23 and the tree uses it | none |
| `-D_GCC_NULLPTR_T` | Frama-C cannot type `nullptr_t` | none; the tree does not use it |
| `errno` and `setjmp` defined as macros | both trip Frama-C's CERT MSC38-C check as a hard error | none. The `setjmp` macro rewrites only the unused declaration: the kernel calls `_setjmp` through `xfer_setjmp`, never bare `setjmp` |

Do not substitute `gcc_x86_64` for the rv64 machdep. It models `char` as
signed, and `<sys/compiler-lp64.h>` branches on `__CHAR_UNSIGNED__` to pick
`CHAR_MIN`, so the analysis would be of a different program than the one that
ships. `fc.sh` installs the machdep into Frama-C's share directory on first
use, because that is the only place Frama-C loads them from; it says so when
it does, and falls back with a warning if that directory is not writable.

## Coverage

The gate's three stages differ in what they can see, and the difference
matters:

- parse types each source alone. It catches per-translation-unit regressions
  and nothing else.
- link loads them as one program with `linker:drop-conflicting-unused`
  escalated to an error. This is the only stage that sees across translation
  units. `kernel/startup/lib/mem.c` contradicting `<string.h>` is invisible
  to the parse stage, because mem.c never includes the header it disagrees
  with, and Frama-C reports the conflict as a warning and exits 0 unless the
  key is escalated.
- wp discharges contracts for the functions named in `framac-verify` markers,
  with RTE obligations included. Today that is a handful in
  `kernel/cap/table.c`. A green run says nothing about anything else.

`kernel/ipc/fastmsg.c` is excluded from every stage: Frama-C's parser rejects
a declaration placed directly after a label, which C23 allows and gcc
accepts. That is the largest IPC fast path and it is entirely unchecked.
Hoisting those declarations above the label would be behavior-preserving and
would close the hole.

Parse coverage is also relative to the active `.config`: anything behind a
disabled `CONFIG_*` is never seen.

## Reading a sweep result

`sweep-arith.sh` runs WP over RTE's division, shift and overflow obligations
with memory-validity obligations off. That filter is the point. A
`mem_access` obligation asks whether a pointer is valid, and nothing in this
tree states that a `thread_t *` points anywhere in particular, so every one
of them comes back unproved and tells you only that the function has no
contract. An unproved overflow obligation usually depends on local
arithmetic alone, so it is a question worth following up.

Unproved is not the same as wrong, and it does not mean one thing. Every
unproved goal chased to ground so far fell into one of three classes, and
they want completely different responses:

1. A real defect. The unary negations that were undefined at `INT_MIN`, and
   the unvalidated `DT_SYMENT` divisor in the ELF loader. Rare, and worth the
   sweep on its own.
2. A true fact the code does not state. Usually a bound that lives in a loop
   header or an enclosing `if`, which a prover will not carry into the body.
   `radix != 0` in `kvsnprintf`, `0 <= level <= 2` in the Sv39 walk, and
   `dparts >= 1` in `xfer_cpy_diov` were each one line, and each closed
   every goal in its group.
3. A true fact drowned in the verification condition. `xfermsg` has goals as
   trivial as "an `int` is not `INT64_MIN`" that fail at a 120-second prover
   timeout with 53 seconds spent in Qed alone. The function calls several
   uncontracted helpers and one function pointer, each of which WP must treat
   as writing everything, so the path condition explodes. No amount of local
   annotation fixes this; the callees need contracts first.

Class 3 is why a raw count is a poor progress metric. A large function with
uncontracted callees contributes unproved goals in proportion to its size,
not to its risk.

Two more things the count does not separate, both of which cost real time to
discover: goals that need bitwise reasoning (`0 <= (priority & 7) < 32` is
trivially true and neither Alt-Ergo nor Z3 discharges it, because `land` is
uninterpreted), and goals blocked behind an `extern const` table whose
initializer lives in another translation unit.
