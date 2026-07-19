# Skiff

C++26 replicated key-value service with a faster-than-Raft benchmark baseline.

The implementation plan is in [`docs/DESIGN.md`](docs/DESIGN.md). The first
milestone establishes a deterministic Raft core with injected clock, transport,
storage, and state-machine interfaces before adding network or persistence code.

## Benchmark Modes

The intended benchmark configurations are explicitly distinct:

- `ReplicationMode::single_node`: one fixed primary appends, synchronises, and
  applies locally.
- `ReplicationMode::async_primary_replication`: one fixed primary acknowledges
  after the same local work, then fans out best-effort replication to two
  replicas. There is no election, acknowledgement wait, retry, repair, or
  convergence guarantee.
- `ReplicationMode::raft`: the eventual production mode, where successful
  writes require a Raft quorum. This mode does not accept writes yet.

The first two modes are baselines, not reduced-safety Raft modes. Benchmark
results must name the mode and must not compare their acknowledgement latency
as equivalent durability or availability guarantees.

## Build

```sh
cmake --preset macos-llvm22-debug
cmake --build --preset macos-llvm22-debug
ctest --preset macos-llvm22-debug
```

`CMakeLists.txt` requests C++26. The pinned Homebrew LLVM 22 preset uses
`/opt/homebrew/opt/llvm/bin/clang++`; the system Apple Clang preset is also
available for comparison.

## First Network Baseline

`skiff_node` is currently an in-memory, single-threaded TCP primary. It accepts
fixed-size binary writes and acknowledges each after updating its local map. It
is not the Raft node and has no persistence, replication, or crash recovery.

Start it on the target node:

```sh
./build/llvm22-release/skiff_node --port 9000
```

Run the load generator from another node:

```sh
./build/llvm22-release/skiff_bench TARGET_HOST 9000 1000000 64
```

The final argument is the maximum number of outstanding requests. Results
report request count, elapsed time, operations/s, and end-to-end p50/p95/p99
latency in microseconds. Begin with pipeline `1` for round-trip latency, then
increase it to find the throughput limit. The emitted mode is
`single_node_in_memory`; do not compare it as a durability or consensus result.

[`docs/PERFORMANCE.md`](docs/PERFORMANCE.md) explains the benchmark transport,
Turing Pi CPU/IRQ and coalescing profiles, measured hardware ceiling, and safe
host-tuning workflow.

[`docs/KERNEL_BYPASS.md`](docs/KERNEL_BYPASS.md) records the DPDK and AF_XDP
feasibility research for the RK3588 onboard NIC and the recommended path to a
real kernel-bypass experiment.
