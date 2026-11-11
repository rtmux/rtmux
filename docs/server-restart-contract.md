# User-Mode Server Restart Contract

RTMux keeps restart policy in user mode. The kernel reclaims a dead process's
channels, connections, endpoint caps, and delegated resources; a supervisor is
responsible for deciding whether to spawn a new server generation.

## Contract

For a restartable service:

- One namespace path is the health signal. When the path vanishes, the old
  generation is dead or no longer serving.
- Old client handles are stale. A client may see `EBADF`, `EPIPE`, timeout, or a
  failed reopen, but must not retain authority over the new generation.
- A new generation re-acquires its own caps and republishes the path. No kernel
  policy respawns it.
- The supervisor retries spawn instead of sleeping a fixed reclaim delay. A
  generation that cannot acquire its resources exits, leaving the path absent.

## Service Table

| Service | Restart status | Owner | Health path | Stale-handle rule | Gate |
| --- | --- | --- | --- | --- | --- |
| `lkl-fsd` | restartable | `lkl-supervisor` | `/fs/root` | old `/fs/root` handles fail closed after crash/revoke | `scripts/qemu-regress.sh --mode lkl-fsd-restart --timeout 600` |
| `lkl-netd` | restartable | `lkl-supervisor` | `/net/ipifc` | old `/net/*` handles fail closed after crash/revoke | `scripts/qemu-regress.sh --mode lkl-netd-restart --timeout 180`; stale control handle: `--mode lkl-netd-stale` |
| `logger` | restartable for `/dev/slog` | test supervisor (`logger-restart`) | `/dev/slog` | old `/dev/slog` handles fail `EBADF`; clients reopen the new generation | `scripts/qemu-regress.sh --mode logger-restart --timeout 180` |
| `pci-server` | fail-stop | init/boot | `/dev/pci` | clients must treat server loss as device-bus loss | no restart until PCI clients have reconnect/reset rules |

The logger restart gate covers `/dev/slog`, not `/dev/console`: the normal boot
logger also owns the console path, so a production logger supervisor would need a
console handoff rule before respawning that endpoint. `pci-server` is
intentionally not supervised today. It owns enumeration and resource assignment
for the live PCI bus; respawning it without a per-client device reset/reconnect
contract could duplicate ownership state. Add a PCI restart gate only when the
first PCI client can prove reconnect and stale BAR/MSI handles fail closed.

## Existing Supervisor

`tests/userland/rt-latency/lkl-supervisor.c` is generic despite its historical
name:

```text
lkl-supervisor WATCHPATH MAXRESTARTS CRASHINJECT|- BIN [ARG...]
```

It waits for `WATCHPATH`, optionally writes once to `CRASHINJECT` to fault the
current generation, waits for `WATCHPATH` to disappear, then spawns the next
generation. The restart gates require `lkl-supervisor: gen 1 up`, proving a full
crash-to-republish cycle rather than a first-boot pass.
