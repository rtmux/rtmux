# Capability Law

This checklist is the executable contract for the current RTMux capability
model. A property is complete only when it has a runtime/host regression and a
formal-claims row; prose alone is not evidence.

| property | law | runtime or host evidence | formal evidence |
| --- | --- | --- | --- |
| unforgeability | Integers not minted by the cap table, wrong generation handles, malformed handles, and stale handles authorize nothing. | `tools/host/cap-table-test`; `tests/userland/capgate.c`; `make check-cap-namespace` | `docs/formal-claims.md#capability-law-unforgeability` |
| type-discrimination | A capability's object type is part of authority; endpoint and process caps for the same object are not interchangeable. | `tools/host/cap-table-test`; `make check-cap-namespace` | `docs/formal-claims.md#capability-law-type-discrimination` |
| rights-enforcement | Lookup requires a non-empty requested-rights subset and rejects absent rights. | `tools/host/cap-table-test`; `tests/userland/capgate.c`; `make check-cap-namespace` | `docs/formal-claims.md#capability-law-rights-enforcement` |
| attenuation-only | Delegation and transfer can preserve or reduce rights, never amplify them. | `tools/host/cap-table-test`; `make check-cap-smt` | `docs/formal-claims.md#capability-law-attenuation-only` |
| transfer-without-grant | Cross-table transfer uses held caps and bounded transfer vectors; malformed transfer flags fail without partial allocation. | `tools/host/cap-table-test`; `make check-cap-smt` | `docs/formal-claims.md#capability-law-transfer-without-grant` |
| revoke-before-free | Revocation kills the whole revocation group before the object can be recycled; pinned objects are returned only on final unpin. | `tools/host/cap-table-test`; `make check-cap-smt` | `docs/formal-claims.md#capability-law-revoke-before-free` |
| stale-handle-rejection | Generation changes, detach, object death, and revoke make old handles fail closed. | `tools/host/cap-table-test`; `tests/userland/capgate.c`; `make check-cap-namespace` | `docs/formal-claims.md#capability-law-stale-handle-rejection` |
| no-ambient-lookup | Name, pid, uid, global id, and guessed integer lookup do not grant kernel authority without a held capability. | `tests/userland/capgate.c`; `docs/capability-abi.md`; `make check-capability-abi` | `docs/formal-claims.md#capability-law-no-ambient-lookup` |

Run `make check-capability-law` after touching cap-table semantics, endpoint
capability routing, process/thread caps, or the capability ABI ledger.
