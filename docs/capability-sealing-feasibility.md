# Capability Sealing Feasibility

Decision: park sealing for now.

RTMux already has attenuation and group-scoped revoke. A kernel seal/unseal pair
would only earn ABI space for a concrete bearer-token consumer. The plausible
consumer is a short-lived driver-client queue token: a driver could hand a client
an opaque token that names a connection or memory-object right without exposing
metadata to intermediaries. That does not yet exist as a measured service path.

If sealing is revived, the minimal primitive is a new typed sealed-cap object
that carries the original cap id, rights, generation, and seal key generation.
`MsgSend` and memory mapping must reject sealed caps until the holder presents a
matching unseal right. Unseal returns an attenuated live cap; it never widens
rights. Revoke on the original group kills sealed and unsealed bearers, and
revoke of the seal key makes future unseal fail closed.

Security invariant: a sealed cap never authorizes `MsgSend` directly, and no
payload-named token can bypass the endpoint cap lookup. If a future bearer-token
consumer validates a seal per message, that path must add a measured row against
the native IPC `MsgSend` round-trip classes in `docs/rt-budgets.md`, not only
the existing revoke-latency rows.

Efficiency gate:

- TCB delta: one cap type plus lookup/unseal checks, no scheduler or IPC queue
  object.
- Hot-path cost: zero unless a consumer opts into per-message unseal.
- Authority source: the held unseal right and the sealed cap generation.
- Rollback: delete the sealed cap type and keep ordinary endpoint/memory-object
  cap transfer.

Revisit trigger: a driver-client or service-front bearer-token design that cannot
be expressed as ordinary attenuated endpoint or memory-object caps, and that
ships with fresh revoke-latency and per-message IPC cost evidence.
