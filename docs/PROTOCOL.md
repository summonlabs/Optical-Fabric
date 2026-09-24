# Control-plane protocol

The node speaks a framed request/response protocol over loopback TCP. Each frame is
`magic u32 | version u16 | flags u16 | sequence u64 | payload length u32 | crc u32 |
payload`. A frame whose declared length exceeds the configured bound is refused from
the header, before anything is allocated. Each payload is a canonical key/value
message beginning with an operation token, and each response echoes the request id.

Identities travel in hexadecimal, quantities in decimal; a reader accepts the
runtime's own `kind:hex` resource reference form as well, so a value taken from one
response can be fed straight back into the next request.

| Operation | Purpose |
| --- | --- |
| `hello` | Version, boot sequence, instance, epoch, tick, recovery status |
| `register_site`, `register_node`, `register_port`, `register_span`, `register_line_system`, `register_cross_connect`, `register_channel` | Topology registration |
| `describe_resource`, `topology` | Registered identity and generations |
| `ingest_observation` | One observation from a remote producer; the runtime fills in the tick, ingestion incarnation and content digest itself |
| `ingest_evidence` | One fully specified observation record |
| `acquire_authority`, `renew_authority`, `fence` | Authority lifetime |
| `submit_intent`, `validate`, `reserve`, `renew_reservation`, `release_reservation` | Connectivity governance |
| `begin_activation`, `commit_activation` | Two-phase activation |
| `withdraw`, `revalidate`, `report_degraded`, `report_failed` | Lifecycle and post-restart reconfirmation |
| `active_paths`, `explain`, `describe_connectivity`, `claims`, `accounting`, `verify_invariants`, `snapshot`, `diagnose` | Queries |
| `advance_tick` | Logical clock |
| `shutdown` | Stop accepting work and close |

Responses carry `status` (`ok`, `refused`), `outcome` (`applied`, `already_satisfied`,
`idempotent_replay`, `refused`) and, on a refusal, the typed `refusal` code with a
human-readable `detail`. The protocol deliberately has no notion of a timeout inside
the runtime: a caller either gets a typed answer or a closed connection.
