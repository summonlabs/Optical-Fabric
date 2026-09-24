# Optical Fabric

Optical Fabric is a vendor-neutral runtime that owns governed optical connectivity
and optical path state as an explicit control-plane boundary. Given registered
optical resources, current authoritative evidence, a connectivity request and exact
generations, it answers one question: what optical connectivity is currently
authorized, which path is active, and what must be fenced before that answer may
change.

## Hardware posture

All optical devices, signals and observations in this distribution are SYNTHETIC.
The runtime does not drive photonic hardware and does not integrate with any vendor
SDK. It has no ROADM, coherent-optics, transceiver-programming, wavelength-switching
or optical-power-telemetry integration, and it claims none. Real evidence can only
enter through the documented typed interfaces, where its producer, instance,
sequence, generation, tick and content digest are recorded with it; if no producer
exists for a required fact, the runtime reports UNSUPPORTED and refuses to promote.

## What it owns

* Typed optical identity: sites, optical nodes, ports, line systems, spans, cross
  connects, channels and wavelength references, each with a generation.
* Evidence with provenance: UNKNOWN, UNSUPPORTED, STALE, CONFLICTING, INCOMPLETE and
  INVALID are preserved and never collapsed into success. Composition is a
  conservative join, and missing telemetry is never evidence of a healthy path.
* The connectivity object and its lifecycle: proposed, validated, reserved,
  activating, active, degraded, failed, withdrawing, retired, plus typed refusals.
* Authority: epoch, incarnation and generation bound grants, with real fencing of
  stale controllers and stale leases.
* Deterministic optical path identity from the canonical ordered segment and
  generation list, with stable serialization.
* Durable, versioned, integrity-checked and compacted state with conservative
  recovery.

It does NOT absorb the Transceiver Registry, Wavelength Fabric, Optical Path
Planner, Link Quality Fabric or Cable/Attachment Registry. Those are consumed
through typed interfaces (`IEvidenceSource`, `IPlannerPort`) and their outputs are
treated as evidence or proposals, never as authority. See `docs/ARCHITECTURE.md`.

## Lifecycle in one paragraph

A submitted intent becomes a PROPOSED object and nothing more. Validation checks
topology generations, blocking evidence and authority, and moves it to VALIDATED.
Reservation claims every resource of the path atomically under a bounded lease.
Activation is two-phase: `begin_activation` re-checks everything and returns an
activation digest; only `commit_activation` presenting that digest reaches ACTIVE.
Withdrawal releases every claim, then retires the object, and the accounting closes
exactly. A process that dies between the two activation phases leaves an object that
recovery turns into FAILED, never ACTIVE.

## Build, test, install

```
cmake -S . -B build/release -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/release
ctest --test-dir build/release --output-on-failure
cmake --install build/release --prefix <prefix>
```

Requires CMake 3.24 or newer and a C++20 compiler. First-party code builds
warning-clean under MSVC `/W4 /WX` and GCC/Clang `-Wall -Wextra -Wpedantic -Wshadow
-Werror`. No test carries a timeout: a hanging test is a defect and must be fixed,
not masked.

## Using the runtime

```cpp
#include "optical_fabric/optical_fabric.hpp"

optical_fabric::OpticalFabric fabric(options);   // options.store_path makes it durable
fabric.register_site(site);
// ... register nodes, ports, spans, cross connects, channels ...
fabric.register_evidence_source(producer);       // SYNTHETIC or real, with provenance
const auto grant = fabric.acquire_authority(request);
const auto submitted = fabric.submit_intent(intent);
fabric.validate({attempt, submitted.connectivity, grant.token});
fabric.reserve({attempt, submitted.connectivity, grant.token, ttl});
const auto began = fabric.begin_activation({attempt, submitted.connectivity, grant.token});
fabric.commit_activation({attempt, submitted.connectivity, grant.token, began.activation_digest});
const auto report = fabric.active_paths();       // what is active, what is authorized
```

An installed package is consumed with `find_package(OpticalFabric 1.0 CONFIG REQUIRED)`
and the target `SummonSoftwareLabs::OpticalFabric`. `consumer/` is an independent
project that proves exactly that against an installed prefix.

## Programs

* `of_cli inspect <store> [--verbose]` - read-only store inspection (header, records,
  checkpoints, torn tail, counters, digest).
* `of_cli demo [directory]` - runs a complete synthetic connectivity scenario and
  prints the path provenance, active paths, invariants and accounting.
* `of_node --store <path> [--port n] [--label name] [--salvage reject|discard-tail]` -
  the control-plane daemon. It binds loopback only and announces its port, boot
  sequence and instance on stdout. It is the runtime used for the multi-process
  proof; Optical Fabric does not claim multi-host operation.
* `examples/` - lifecycle, refusal and persistence/restart walkthroughs.
* `benchmarks/of_bench` - completed-work measurements of the synthetic model.

## Evidence and authority, precisely

* An observation carries its producer runtime, producer instance, source identity,
  producer sequence, ingestion incarnation, observed generation, observation tick,
  validity window and content digest. Re-using a producer sequence with different
  content is refused; an older sequence never replaces a newer one.
* Two producers that both claim to know the same fact must agree; disagreement is
  CONFLICTING and blocks promotion.
* An attestation from a previous incarnation or an older generation is outdated: it
  is superseded by a live re-attestation, and if nothing live remains the answer is
  STALE. Persisted evidence therefore never becomes fresh merely by being loaded.
* Authority is bound to a grant, an epoch, an incarnation and a scope. Acquiring
  authority advances the epoch, which fences every older token; a restart creates a
  new incarnation, which fences every token from the previous process. Fencing is
  durable state, not a transient answer.

## Guarantees the test suite proves

* Deterministic canonical connectivity and path identity, stable across processes
  and independent of registration order.
* Stale controller and stale incarnation fencing, including a restart that leaves
  every committed path unauthorized until it is revalidated.
* Conflicting resource claims cannot both become authoritative, within one process
  and across processes.
* Loss of required evidence prevents promotion to ACTIVE, at validation, at
  activation and at the commit boundary.
* Replayed activation, withdrawal and reservation messages are either idempotent or
  refused with a typed conflict.
* Real process kill and restart at the reservation, activation, commit and
  acknowledgement boundaries, over loopback TCP with independent operating-system
  processes.
* Persistence corruption, truncation and version mismatch are refused or recovered
  conservatively, and a refused store is left untouched.
* Concurrent reservation races over shared optical resources have exactly one
  winner, and no resource is ever committed to incompatible authoritative paths.
* Exact accounting closure after retire and release: no claims, no live leases, no
  drift.

See `docs/TESTING.md` for the suites and `BENCHMARKS.md` for measured numbers.

## Limits

Every collection, payload, queue and history is bounded by `optical_fabric::Limits`,
validated at construction. A limit violation is a typed refusal, never a silent
truncation. The store is single-writer: a second process is refused rather than
becoming a second writer.

## License

Apache License 2.0. Copyright 2026 Summon Software Labs. No telemetry transmission.
