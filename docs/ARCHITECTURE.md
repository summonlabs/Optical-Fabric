# Architecture

## Boundary

```
   Transceiver Registry ─┐
   Cable/Attachment Reg. ─┤   typed evidence (IEvidenceSource)
   Link Quality Fabric  ─┼──────────────────────────────────┐
   Wavelength Fabric    ─┘                                  v
   Optical Path Planner ──── route proposals (IPlannerPort) ─┴─> Optical Fabric
                                                                  │
                                       governed connectivity object │ lifecycle
                                       + authority + commit index    v
                                                        authoritative answer:
                                                        active path, authorized set,
                                                        fence requirements
```

Optical Fabric owns the connectivity object, its lifecycle, its authority and the
commit index. It never owns a registry or a planner, and it re-derives nothing those
runtimes already own. A planner proposal is a proposal: it tells the runtime which
resources to consider and never promotes anything. Evidence is a fact with
provenance and never carries authority.

## Layers

| Layer | Types | Notes |
| --- | --- | --- |
| Identity | `StrongId`, `ResourceRef`, `Names`, `Digest128` | Named identities are FNV-1a-64 digests of a validated canonical name, domain separated by resource kind. |
| Time | `Tick`, `Epoch`, `Generation`, `Incarnation` | The logical clock is persisted, so a lease expiry survives a restart. Zero means never. |
| Evidence | `EvidenceState`, `EvidenceKind`, `EvidenceRecord`, `EvidenceProvenance` | Conservative join; provenance is mandatory. |
| Topology | registration records, `ResourceState` | Physical and structural identity only. |
| Path | `PathSegment`, `PathDescriptor`, `CanonicalPath` | Ordered segments with generations; identity is the digest of the canonical text. |
| Lifecycle | `ConnectivityState`, `TransitionRecord`, `RefusalCode` | An explicit successor table; illegal moves are refused. |
| Authority | `AuthorityToken`, `AuthorityScope`, `FenceRecord` | Epoch, incarnation and scope bound. |
| Runtime | `OpticalFabric` | One mutex, no callbacks under it, all state changes through one commit path. |

## Commit path

Every mutation builds a `CommitBundle`: a list of typed items describing the state
the commit installs. The bundle is encoded, persisted as exactly one store record,
then decoded and applied through the single `apply_item` path. Live mutation and
recovery therefore cannot diverge: recovery applies the same items in the same order.
A torn write can only ever lose a whole commit, never half of one.

## Recovery

On open the runtime replays the store, then conservatively repairs:

1. Every grant from a previous incarnation is fenced, and the epoch advances if any
   grant was live.
2. Objects left ACTIVATING become FAILED with their pending activation digest
   cleared; objects left WITHDRAWING are retired; committed objects lose their
   authority confirmation.
3. Observations from previous incarnations stay in the store but are no longer live,
   so they can never silently become fresh.

The result is a new boot sequence, a new incarnation, no authorized path, and a
diagnostics report that says exactly why.

## Locking

* One mutex guards all authoritative state. It is never held across a call into
  caller code, a producer, a planner or a socket.
* Producers and planners run outside the lock; their results are re-validated under
  the lock before they are applied, so a concurrent topology change cannot smuggle
  an unregistered resource into a path.
* The store has its own mutex, always acquired after the state mutex, and never
  calls back into the runtime.

## Persistence format

```
header : magic u32 | format version u32 | flags u32 | header crc u32 | boot u64
record : magic u32 | kind u16 | reserved u16 | payload length u32 | crc u32 | payload
```

Payloads are canonical key/value text with strict decoding: unknown keys, duplicate
keys, missing keys, unparsable numbers and truncated groups are all errors. Recovery
discards only a trailing run of bytes that does not form complete, checksummed
records; corruption with valid records after it is refused and the file is left
untouched. Compaction rewrites the store through a temporary file and an atomic
rename, and is driven by growth since the last checkpoint so a large checkpoint never
causes a rewrite per commit. A sidecar lock file makes the store single-writer across
processes.
