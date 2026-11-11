# RTMux Named Channel Registry Design

The named channel registry is roottask-owned metadata for services that need a
mountable endpoint but should not invent ad hoc `/dev/*` control files.

## Interface

A service registers:

- endpoint name;
- owning channel ID / connection target;
- permissions and credentials policy;
- mountability flag;
- unlink policy.

Clients open the name through the namespace and receive a normal RTMux
connection.  The registry does not replace `channel_create()` or
`connect_attach()`; it gives roottask a namespace seam above those primitives.

`name_attach(dpp, path, flags)` and `name_open(name, flags)` are the libc entry
points.  The known attach flag is `NAME_FLAG_ATTACH_MOUNTABLE`; any other bits
are rejected with `EINVAL` so a typo doesn't silently downgrade the request.
`name_detach(attach, NAME_FLAG_DETACH_SAVEDPP)` removes the registry name
without tearing down the dispatch or channel the caller still holds.

## Namespace

Names resolve under `/dev/name/local/`.  A bare name (no leading `/`) is
normalized into that namespace so a client and server that disagree on CWD still
find each other.

Rooted names that fall outside `/dev/name/local/` are rejected with `EINVAL`.
Without that gate, `name_attach(NULL, "/dev/mem", 0)` could shadow an existing
I/O-service path and steer client opens to an attacker endpoint.  This is the
layered defense on top of the namespace
`NAMESPACE_FLAG_STICKY` ownership semantics.

## Lifetime

| Operation | Rule |
| --- | --- |
| create | fails if the name already exists unless replace is explicit |
| connect/open | returns a normal connection to the registered endpoint |
| unlink before close | removes the name; existing connections survive |
| close before unlink | endpoint remains discoverable until owner unregisters |
| owner death | roottask unregisters the name and future opens fail |
| detach with `SAVEDPP` | name unlinked, channel and dispatch left to the caller |

Mountability is explicit.  Endpoints that are only service control channels
stay unmounted; endpoints meant to appear in the path namespace opt in via
`NAME_FLAG_ATTACH_MOUNTABLE` (which sets `NAMESPACE_FLAG_DIR` on the underlying
namespace link).

Path-based `unlink()` of the registry name has a TOCTOU window if another
process recreates the same node between attach and detach.  Real fix is a
handle-based unregister; until then the registry relies on `STICKY` ownership
being unique by `(path, pid)` in practice.
