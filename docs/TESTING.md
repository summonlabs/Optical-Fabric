# Testing

No test carries a timeout or a watchdog. A hanging test is a defect: it is
diagnosed and fixed, never masked. The only bounded waits are the ones used to
observe a real child process, and exceeding such a bound produces an explicit failed
assertion with a diagnostic.

Run everything with:

```
ctest --test-dir build/release --output-on-failure
```

| Suite | What it proves |
| --- | --- |
| `test_identity` | Name validation, derived identity determinism and collision freedom over 20k names, canonical path identity (order, generation, channel and direction sensitivity), digest and CRC round trips, scope and counter arithmetic. |
| `test_evidence` | The composition lattice never upgrades an indeterminate state, freshness policy, ingestion validation, out-of-order and contradictory attestations, disagreement between producers, and topology registration including idempotence, collisions and capacity. |
| `test_lifecycle` | The whole lifecycle, the refusal to activate without a reservation, evidence loss between prepare and commit, conflicting claims, replay and attempt reuse, reservation expiry, release, degradation and failure, provenance for every segment, and accounting closure. |
| `test_authority` | Acquisition fencing the previous holder, scope enforcement, global scope, renewal, expiry, explicit fencing, a stale controller unable to withdraw a live path, and forged incarnation/holder tokens. |
| `test_persistence` | Restart semantics, incomplete activation and withdrawal recovery, torn tail recovery under both salvage policies, mid-file corruption refusal, wrong magic and version refusal, read-only inspection, single-writer exclusion, compaction bounds and topology stability across a reopen. |
| `test_protocol` | Frame round trips, byte-at-a-time feeds, magic/version/CRC failures, a declared length beyond the bound refused from the header alone, message escaping and malformed payloads. |
| `test_concurrency` | A reservation race with exactly one winner, parallel activations on independent lines, real shutdown with typed refusals, and two claimants racing for one exclusive resource. |
| `test_property` | Seeded randomized operation sequences: invariants and accounting hold after every single operation, two runs of the same seed produce identical state digests, canonical identity is independent of registration order, and evidence composition is idempotent and order independent. Failing seeds are reported. |
| `test_adversarial` | Malformed and out-of-order requests, forged commit digests, attempt reuse, every capacity bound, extreme values rejected before allocation, and degenerate configuration refused at construction. |
| `test_scale` | Forty lines with full lifecycle, claim counts, invariants, retirement of every path with exact closure, and bounded histories under long churn. |
| `test_multiprocess` | Real operating-system processes over loopback TCP: kill and restart with fresh-incarnation fencing, a kill at the activation boundary leaving nothing active, two processes refused a shared store, and a recovered path keeping its claims against a conflicting cross-process request. |

Deterministic identity and stable serialization are additionally checked by
comparing `FabricSnapshot::digest` across independent runs of the same input. That
digest deliberately covers authoritative state only: caller-supplied attempt
identifiers and per-process entropy are excluded, while every identity, path,
generation, epoch, tick, refusal and claim is included.

## Sanitizers

`-DOPTICAL_FABRIC_ENABLE_ASAN=ON` builds the library, the applications, the examples
and the test suite with AddressSanitizer where the toolchain supports it. On MSVC
that is `/fsanitize=address`; on GCC and Clang it is `-fsanitize=address` with frame
pointers. The full suite passes under the sanitizer on this platform; see the release
report for the exact configuration that was run.
