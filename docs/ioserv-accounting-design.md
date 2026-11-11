# RTMux I/O-Service Resource Accounting Design

Servers allocate memory on behalf of clients.  The first RTMux accounting step is
measurement and attribution, not reclaim policy.

## Accounting Record

Every persistent server-side allocation records:

- client pid and credential snapshot;
- accounting class;
- server pid;
- allocation size or object count;
- create and cleanup points.

The class names are server-local but reported through roottask so per-process
limits and counters can be compared across servers.

## Interface

Roottask exposes counters through procfs or a roottask query:

| Counter | Meaning |
| --- | --- |
| current bytes / objects | live server-side state charged to the client |
| high-water bytes / objects | diagnostic peak |
| cleanup-pending | state retained after client exit for server cleanup |

Client exit does not erase attribution immediately.  Counters remain until the
server frees the persistent state, so leak and cleanup tests can prove where
resources went.

## Memory-Pressure Classes

| Class | Allocation failure | Cache reclaim | Restartable | Victim policy |
| --- | --- | --- | --- | --- |
| critical server | avoid; fail only explicit bounded admission | yes, private caches only | no | never |
| realtime driver/IST process | explicit `ENOMEM`/`EDQUOT` before unbounded growth | driver-owned bounded cache only | no while IRQ attached | never |
| restartable I/O service | may fail persistent client state | yes, server-private cache | yes after cleanup regression | only with future class policy |
| best-effort user process | may fail allocation | no server cache ownership | yes | only if admission and cache reclaim are proven insufficient |

No global OOM killer belongs in this module.  Admission failure and
cache-specific reclaim are the current policy.

## Current Prototype

Roottask currently exposes persistent namespace-registration accounting at
`/sys/ioserv-accounting`.  The prototype counts durable namespace-object
registrations by creator pid and object type:

- total current and high-water object counts;
- per-type current and high-water object counts;
- per-creator-pid current and high-water object counts.

This is intentionally object-count accounting, not byte accounting.  It covers
the durable server-side state roottask can observe today: namespace registrations,
procfs symlinks, and memory-object records.  It does not yet charge
transient request buffers, credential snapshots, per-client server-private
allocations inside external I/O services, or cleanup-pending state after
client exit.

The exported file is now both a measurement surface and the first admission
gate.  Non-roottask creators have a fixed per-pid durable namespace-object ceiling;
over-limit attach fails with `EDQUOT` before linking new namespace state.
Counters must return to their prior value after cleanup.

## Reclaim

Reclaim stays cache-specific:

- kernel roottask stack cache: `kext_stack_purge()` purges cached roottask pool
  stacks and asserts it is not running from interrupt-mask context;
- roottask loader-context cache: bounded standard contexts stay cached; oversized
  contexts unmap immediately on free.

Anonymous process memory, server paging, and victim selection are not
implemented.  A future victim policy needs a concrete workload and must protect
critical servers plus realtime driver/IST processes.
