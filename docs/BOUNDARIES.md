# Adjacent runtime boundaries

Optical Fabric is deliberately narrow. It owns governed optical connectivity and
optical path state. Everything else stays with the runtime that owns it, and is
consumed through a typed interface.

| Adjacent runtime | What Optical Fabric consumes | Interface | What it does not do |
| --- | --- | --- | --- |
| Transceiver Registry | Port and transceiver capability facts | `IEvidenceSource` producing `port-capability`, `transceiver-capability` | Program, reset or query a transceiver |
| Cable & Attachment Registry | Physical attachment facts for endpoints | `IEvidenceSource` producing `cable-attachment` | Own cable or attachment identity |
| Wavelength Fabric | Channel and wavelength availability | `IEvidenceSource` producing `channel-availability`, `wavelength-availability` | Assign, tune or switch wavelengths |
| Link Quality Fabric | Span and line-system operational observations | `IEvidenceSource` producing `span-operational-state`, `line-system-operational-state` | Compute link quality |
| Optical Path Planner | Candidate routes | `IPlannerPort::propose_route` | Search, rank or optimise routes |

## What a proposal is

A `RouteProposal` is a list of resource references. The runtime validates every
reference against the registered topology, reads the current generation of each
resource, and refuses unknown or repeated resources. A proposal cannot carry
evidence, cannot carry authority, and cannot promote anything: an intent built from a
proposal is still PROPOSED.

## What evidence is

An `EvidenceRecord` is a typed observation with mandatory provenance: producing
runtime, producing instance, source identity, producer sequence, ingestion
incarnation, observed generation, observation tick, validity window and content
digest. It carries no authority. A required fact with no producer is UNSUPPORTED; a
required fact whose producer has not answered is UNKNOWN; neither is ever KNOWN, and
neither can promote a path.

## Declared-route resolution

When no planner is attached, the runtime resolves an intent by walking the
cross-connect opportunities and spans that operators declared. That walk is
deterministic, breadth first, bounded, and follows only registered structural links.
It does not optimise, cost, rank or invent anything. When the walk cannot connect the
endpoints, the intent is refused with `route-unresolved` and the caller may attach a
planner.
