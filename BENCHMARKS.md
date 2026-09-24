# Benchmarks

`benchmarks/of_bench` measures completed useful work: every number counts an
operation that finished, including its durable commit where one applies. Nothing is
measured at submission time. The workload is a SYNTHETIC control-plane model; no
number here describes optical hardware, and none should be read as photonic device
performance.

## Measured results

Recorded on Windows, MSVC 19.44 (x64), CMake 4.3, `CMAKE_BUILD_TYPE=Release`, one
process, local filesystem for the durable case.

| Workload | Completed | Elapsed | Rate |
| --- | --- | --- | --- |
| Canonical path identity (3 segments, channel, generation) | 200000 | 0.140 s | 1432753 /s |
| Full lifecycle, memory only (register, validate, reserve, activate, commit, withdraw, retire) | 64 | 0.234 s | 274 /s |
| Full lifecycle with a durable store (fsync at every commit boundary) | 16 | 1.282 s | 12 /s |
| Concurrent reservation attempts from 8 threads on one object | 1600 | 0.008 s | 210405 /s |

The concurrent reservation figure counts *attempts*, not grants: exactly one of the
1600 attempts was granted and the other 1599 were refused with `already-satisfied` or
a typed conflict, which is the invariant the runtime must hold. It is reported here
because a refusal is completed, useful work in a governed control plane.

The durable lifecycle is two orders of magnitude slower than the memory-only one
because every commit is flushed to stable storage; that is the point of the mode, not
a defect. The same measurement in a Debug build (MSVC 19.44) is roughly 20x slower
across the board and is not reproduced here.

## Reproducing

```
cmake -S . -B build/release -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/release
./build/release/benchmarks/of_bench
```
