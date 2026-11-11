# RTMux Namespace Round-Trip Measurement

Batch RPC is not designed until namespace round-trip cost is measured.

## Counter

Roottask namespace service maintains a monotonically increasing counter for
namespace round trips.  A representative operation increments it once for each
namespace lookup/control exchange, not for transport-only `read` or `write`
calls.

The current implementation exposes a snapshot at `/sys/namespace-stats`:

```text
total <n>
open <n>
readlink <n>
unlink <n>
chdir <n>
chroot <n>
resolve <n>
root_resolve <n>
cache_hits <n>
cache_misses <n>
cache_uncacheable_bypasses <n>
namespace_version <n>
```

The counter intentionally records namespace/control entries, not data
transport on already-open file descriptors.  This gives later batch-RPC work a
stable denominator before it proposes a wider interface.

The cache counters are present even before a directory-entry cache exists:
normal lookups count as misses and uncacheable registrations count bypasses.
Cache hits remain zero until a real cache policy lands.

`namespace_version` is monotonic namespace state used by open-race hardening.  It
moves when namespace objects are attached or detached.  The namespace open path
samples it before lookup, retries once if the namespace changes before binding a
result, and then returns `ESTALE` rather than minting a stale result.

## Benchmark

`tests/userland/namespace-roundtrip/namespace-roundtrip [iterations]` builds as an
on-demand benchmark.  It reads `/sys/namespace-stats`, runs the currently
available namespace chain `openat(AT_FDCWD) -> fstatat(AT_FDCWD) ->
readlinkat(AT_FDCWD)`, reads the counters again, and prints elapsed
microseconds plus counter deltas.  fd-relative path lookup remains a separate
namespace design; the benchmark intentionally uses the `AT_FDCWD` form to measure
today's namespace round trips.

Representative follow-up workloads:

1. `find /`
2. `ls -R`
3. RTMux build/package workload where available

Each run records elapsed time, namespace round trips, p99 if histogram support is
available, and the RTMux config/git provenance block from `docs/rt-budgets.md`.

Only after a baseline exists may a batch envelope be designed.  Any batch RPC
proposal must cite the baseline it improves.
