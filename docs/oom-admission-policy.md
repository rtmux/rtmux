# RTMux Memory-Pressure and OOM Victim-Selection Policy

This note fixes what RTMux does under memory pressure, so the behavior is a
gated policy rather than an open question. It is a section 18 (Core-Path
Realtime Characterization) artifact and is paired with the memory-growth block
in docs/rt-budgets.md and the mem-pressure regression.

## The policy in one sentence

RTMux never selects a victim: memory pressure is resolved by refusing the new
allocation with ENOMEM to the requesting thread, never by reclaiming, paging
out, swapping, or killing any already-admitted process.

## Why there is no victim to select

RTMux backs a mapping with physical memory eagerly, at allocation time. There
is no demand-paging handler: a user page fault against an unbacked address goes
straight to the fatal path, not to a pager that would fault memory in later. So
a mapping either wins its physical pages when it is created or it does not exist
at all.

That single design choice removes the whole reclaim problem:

- There is no page-out or swap path. Once a process owns physical pages, no
  kernel or roottask code path takes them back while the process lives. Memory
  is released only when the owner unmaps it or exits.
- There is no OOM killer. Nothing scans for a process to terminate to free
  memory, so no victim-selection heuristic exists to get wrong.
- Admission is the only pressure valve. When the physical quantum allocator is
  exhausted, pa_alloc() returns NULL, the roottask memory path turns that into a
  clean ENOMEM (servers/roottask/mem/mm-map.c, mm-memobj.c), and the requesting
  thread gets the failure. No other process is disturbed.

A system that never reclaims cannot victimize the wrong process. Protected
classes (realtime driver and IST processes, critical servers) are safe by
construction, not by a priority comparison that could be tuned wrong: their
already-allocated working set is never a reclaim target because nothing is ever
reclaimed. The rule that protects them is structural.

## What "protected by construction" requires of a realtime process

Because memory is never reclaimed, a realtime process is safe if and only if it
owns its full working set before its deadline-sensitive phase begins. Allocate
(and touch, so the eager backing is committed) up front; after that point a
later burst of pressure from any other process cannot take a page away. A
realtime process that instead allocates lazily during its critical phase is
exposed not to reclaim but to its own allocation returning ENOMEM, which is a
program bug under this policy, not a kernel behavior to tune.

## Current caps and the honest gap

Per-map admission is the enforced control today. The RLIMIT_AS check at map time
(rlimit_blown in servers/roottask/mem/mm-rlimit.c) is currently a stub that
always passes, so there is no per-process address-space or resident-set cap
actually enforced: a single process can consume physical memory up to global
exhaustion, at which point it, and any concurrent allocator, sees ENOMEM. This
is safe for the protected classes (they are never reclaimed regardless) but it
is not fair: one greedy non-realtime process can deny memory to another
non-realtime process. Closing that gap means enforcing the RLIMIT_AS check or
adding a resident-memory budget per process or per class; it does not change the
victim-selection policy, which stays "no victim, admission failure only."

## Regression

tests/userland/rt-latency/mem-pressure.c drives the system into this ENOMEM
boundary and asserts the policy holds. A background hog thread maps and touches
anonymous memory until the map returns MAP_FAILED, while a SCHED_FIFO realtime
thread runs a periodic loop that measures its wake deviation and re-verifies a
canary buffer it committed before the pressure began. It runs as the final
stage of make check-rt, so the whole realtime suite runs under the resulting
pressure.

Pass criteria, all asserted by the probe:

- the hog reaches ENOMEM without a crash, proving admission failure is bounded
  and clean rather than a fault or a panic;
- the canary stays intact through peak pressure, proving no page owned by the
  realtime thread was reclaimed;
- the realtime loop keeps running, proving pressure does not starve it.

The realtime wake deviation is reported but not gated: absolute latency under
QEMU/TCG is inflated by host descheduling of vCPU threads, the same measurement
caveat that applies to every other section 18 block. The gated facts are the
three structural ones above; the latency number is characterization only.
