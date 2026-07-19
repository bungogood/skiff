# Skiff Design

## 1. Purpose

Skiff is a C++26 replicated key-value service exploring a faster-than-Raft primary-replication path alongside a Raft consensus mode. It is a systems project first: correctness, failure behaviour, durable recovery, and reproducible performance evidence take priority over database features.

The first release runs three voting replicas on a four-node RK1 cluster. The fourth node is reserved for load generation, trace collection, and fault orchestration.

## 2. Success Criteria

The first release is complete only when it provides all of the following:

- A three-node Raft cluster elects leaders, replicates writes, commits through a quorum, repairs divergent logs, and applies entries to a deterministic KV state machine.
- Acknowledged writes survive a process crash and restart in durable mode.
- Deterministic fault-injection tests cover elections, partitions, message delay/reordering, crashes, recovery, and conflict repair.
- A real TCP cluster passes integration tests on the RK1 hardware.
- Benchmarks publish committed operations/s, MiB/s, p50/p95/p99/p99.9 end-to-end commit latency, and complete environment details.

## 3. Explicit Non-Goals

Version one does not include:

- Dynamic membership, joint consensus, learner nodes, or sharding.
- TLS, authentication, SQL, transactions, secondary indexes, or multi-key operations.
- Linearizable reads. Version one benchmarks `PUT`; reads are added later using ReadIndex or a documented lease protocol.
- UDP as the initial correctness transport.
- Claims of fault tolerance, consensus performance, or low latency before the relevant test and benchmark gates pass.

## 4. Client Contract

### Benchmark Baselines

Benchmarking uses three intentionally non-equivalent modes:

- `single_node`: a fixed primary appends, persists, and applies a command
  locally. This establishes the storage and state-machine baseline.
- `async_primary_replication`: a fixed primary does the same local work, returns
  success, then best-effort fans the command out to two replicas. It does not
  wait for replica acknowledgements and has no election, retry, log repair, or
  convergence guarantee. It isolates the steady-state cost of replication
  fan-out from quorum acknowledgement latency.
- `raft`: a dynamic Raft leader commits only after quorum replication. This is
  the only mode that may claim consensus or quorum commit behaviour.

Each result records the mode, cluster size, acknowledgement point, durability
policy, payload size, and client concurrency. The asynchronous three-node mode
must never be described as a weaker Raft configuration.

### Commands

The state machine supports:

```text
PUT key value
DELETE key
GET key                 # Local diagnostic read only in version one.
```

`PUT` and `DELETE` are submitted to the current leader. A successful client response means:

1. The command has a log index and term.
2. The entry is replicated to a quorum.
3. The entry is committed.
4. The local leader state machine has applied the entry in order.

In durable mode, quorum replicas must persist the entry before returning successful replication acknowledgements. In asynchronous mode, replication is still quorum based, but persistence is only to the OS page cache and is explicitly not power-loss durable.

Requests to a follower return a `NOT_LEADER` response with the latest known leader ID when available. Client retry behaviour belongs in the benchmark client, not the Raft core.

## 5. Cluster Topology

```text
RK1 node 1: Raft voter
RK1 node 2: Raft voter
RK1 node 3: Raft voter
RK1 node 4: benchmark client, observer, fault controller
```

Three voters provide a quorum of two and tolerate one voter failure. Four voters would still require a quorum of three and do not improve single-failure availability.

## 6. Architecture

```text
                    +------------------+
                    | Client / Loadgen |
                    +--------+---------+
                             |
                       client protocol
                             |
 +---------------------------------------------------+
 |                    Raft Node                      |
 |                                                   |
 |  Client API -> Raft Core -> State Machine (KV)    |
 |                    |              |               |
 |                Transport        Storage            |
 |             TCP / later UDP    mmap WAL/snapshot   |
 +---------------------------------------------------+
```

The Raft core must not directly depend on operating-system time, sockets, or mmap files. It depends on injected interfaces:

```cpp
class Clock;
class Transport;
class Storage;
class StateMachine;
class Scheduler;
```

Production implementations use a monotonic clock, Asio TCP transport, mmap storage, and a single event-loop owner for Raft state. Tests replace them with deterministic versions.

## 7. Concurrency Model

- One event-loop thread owns all Raft state: term, vote, role, log indexes, peer progress, commit index, and applied index.
- Storage completion and network receive events are posted back to that event loop.
- The KV state machine applies committed entries on the same ordered execution path in version one.
- Benchmark clients may use many threads or asynchronous connections, but no client thread mutates Raft state directly.
- Use bounded queues and backpressure. Do not accept unbounded client requests during a slow follower or persistence stall.

This keeps the consensus state machine race-free by construction. Parallelism is introduced only around IO, clients, and observability after ownership boundaries are explicit.

## 8. Raft State and Protocol

### Persistent State

Every node persists before the relevant response is sent:

```text
current_term
voted_for
log[index] = {term, command}
```

The service also persists enough metadata to reconstruct `commit_index` and replay the applied state after restart.

### Volatile State

```text
role: follower | candidate | leader
leader_id
commit_index
last_applied
next_index[peer]
match_index[peer]
election_deadline
heartbeat_deadline
```

### Election Rules

- Election timeouts are randomized from a documented range using an injectable random source.
- A candidate increments and persists its term, votes for itself, and requests votes.
- It becomes leader only after a majority of votes in its current term.
- A node steps down on receiving a valid higher term.
- The test harness asserts at most one leader per term.

### Replication Rules

- The leader sends AppendEntries heartbeats and log entries to every follower.
- Followers verify `prev_log_index` and `prev_log_term` before accepting entries.
- On mismatch, the leader uses follower conflict information to move `next_index` backwards efficiently.
- A leader advances `commit_index` only after a majority have replicated an entry from its current term.
- Every node applies entries from `last_applied + 1` through `commit_index` in index order.

## 9. Transport Plan

### Phase 1: TCP Reference Transport

Use framed TCP RPCs through Asio. TCP provides ordered, reliable byte streams, allowing early work to validate Raft rather than reinvent transport reliability.

Protocol requirements:

- Explicit message type, protocol version, sender, recipient, term, correlation ID, and payload length.
- Bounded frame size and input validation before allocation.
- Connection loss maps to a peer-unavailable event; Raft heartbeat/election rules handle recovery.
- RequestVote, AppendEntries, and their responses are observable in traces.

### Phase 2: UDP Multicast Replication Experiment

UDP multicast is an experiment, not the initial Raft transport.

- The leader multicasts common AppendEntries payloads.
- Every follower sends unicast acknowledgements containing its durable `match_index`.
- The leader retains peer-specific progress and uses TCP replay for lost, delayed, or conflicting entries.
- Benchmark TCP and UDP modes using identical commands, payload sizes, durability policies, cluster topology, and client load.
- Do not claim that multicast is faster until the throughput-versus-tail-latency results are published.

## 10. Mmap Write-Ahead Log

### Segment Layout

The log uses fixed-size segment files. Each segment has a header containing:

```text
magic, format_version, segment_id, first_index, capacity, header_checksum
```

Each record contains:

```text
record_length, log_index, term, command_length, command_bytes, crc32c, commit_marker
```

The commit marker is written last and includes the index plus a fixed magic value. A record is valid only when its length is bounded, its marker is present, and its checksum matches.

### Write and Recovery Contract

1. Reserve space in the current segment.
2. Write the record body and checksum.
3. Write the commit marker last.
4. In durable mode, synchronise changed pages with `msync(MS_SYNC)` and then call `fdatasync` on the segment file before acknowledging persistence.
5. Persist metadata through a double-buffered metadata file with generation number and checksum.
6. On startup, load the newest valid metadata record, scan segments, stop at the first invalid record, and truncate the logical tail.

The implementation must document the platform assumptions behind mmap and `fdatasync`. Crash tests must demonstrate that partially written or corrupted tails do not become committed commands.

### Snapshots

Snapshots are added after log recovery works:

- Serialize the KV state machine at a committed index.
- Persist and checksum the snapshot before updating snapshot metadata.
- Compact only log entries covered by a valid snapshot.
- Test restart from snapshot plus subsequent log tail.

## 11. Deterministic Fault Injection

The deterministic harness has no real sockets, threads, wall clock, or disk.

### Simulated Components

- `FakeClock`: advances only when the test tells it to.
- `SimulatedTransport`: holds message envelopes in an event queue.
- `SimulatedStorage`: provides durable images, flush points, partial writes, and corruption injection.
- `DeterministicScheduler`: selects the next delivery, timeout, crash, restart, or storage event.

### Fault Actions

```text
deliver(message)
drop(message)
delay(message, duration)
duplicate(message)
reorder(messages)
partition(node_set_a, node_set_b)
heal_partition()
crash(node)
restart(node, durable_image)
corrupt_log_tail(node)
advance_time(duration)
```

### Invariants Checked After Every Event

- At most one leader exists in a term.
- Terms and commit indexes never decrease.
- Nodes with the same `(index, term)` have identical log prefixes.
- Every committed entry was accepted by a quorum.
- Applied entries are contiguous and ordered.
- Identical committed logs produce identical KV state.
- Restart recovery never applies an invalid or uncommitted tail record.

Generated fault tests use a fixed seed. A failing run writes the seed and complete event trace so it can be replayed exactly. Start with hand-written scenarios, then add thousands of seeded randomized schedules.

## 12. Real Integration Tests

Run after deterministic tests pass:

- Start a real three-node TCP cluster.
- Confirm leader election and write replication.
- Kill a follower; continue committing through the remaining quorum.
- Kill the leader; measure and assert successful failover.
- Partition the old leader from a majority; assert it cannot commit new writes.
- Heal the partition and verify log repair and KV convergence.
- Restart a node after durable writes and verify recovery.

The UDP experiment must pass the same semantic test suite before performance comparison.

## 13. Benchmark Methodology

### Metrics

For committed `PUT`s, record:

- Operations/s and MiB/s.
- Client-send to commit-response latency: p50, p95, p99, p99.9, maximum.
- Offered load, achieved load, error rate, and backpressure events.
- Leader CPU, follower CPU, RSS, network bytes, disk writes, fsync/msync time, and replication lag.

### Test Matrix

Run the complete matrix for TCP. Run the same matrix for UDP only after correctness gates pass.

```text
Durability:       durable quorum persistence | asynchronous page-cache
Payload:          64 B | 256 B | 1 KiB | 4 KiB
Client concurrency: 1 | 8 | 64 | 256
Cluster:           1 node | 3 voters + 1 load generator
Transport:         TCP | UDP multicast experiment
```

Use one low-concurrency run to expose latency and rising offered load to produce throughput-versus-p99 curves. Do not report only the highest operations/s result.

### Reproducibility

Every benchmark result includes:

- Git commit SHA and dirty/clean state.
- Exact Clang/libc++ version, CMake preset, compiler flags, and linked library versions.
- RK1 hardware, kernel, storage device, network topology, CPU governor, IRQ affinity, and CPU pinning.
- Raft settings: election timeout, heartbeat interval, batch size, segment size, and durability mode.
- Raw latency histogram and machine-readable result file.

## 14. C++26 Toolchain

- Pin the exact ARM64 Linux Clang/libc++ version in a CMake toolchain file and CI container/image.
- Use C++26 only where that pinned toolchain supports the feature reliably.
- Prefer simple, measurable primitives over novelty: RAII ownership, `std::span`, `std::expected`, stop tokens, atomics, bounded queues, and carefully scoped Asio coroutines.
- Build with warnings as errors, sanitizers in tests, and optimisation flags only in benchmark presets.

## 15. Performance Engineering Rules

Performance work starts only after the deterministic and real-cluster correctness suites pass. Every optimisation must have a benchmark result and a stated trade-off.

### Hot Path

- The Raft event-loop thread is the sole owner of consensus state: no mutex acquisition or shared-state contention on the steady-state replication path.
- Production code uses concrete adapters or compile-time composition on the hot path; the virtual test interfaces exist for deterministic tests, not as a benchmark target.
- Use fixed binary frames, bounded queues, preallocated buffers, and buffer reuse. Avoid per-message allocation, string formatting, and logging on the steady-state path.
- Keep per-peer progress and counters on separate cache lines where profiling shows false sharing.
- Batch AppendEntries and group persistence deliberately. Report batch size and group-commit delay with every result.

### Measurement Discipline

- Separate client-to-commit latency from network-only, storage-only, and state-machine-apply latency.
- Use a dedicated fourth RK1 node for load generation so client work does not consume voter CPU or network capacity.
- Pin event-loop, storage, and benchmark threads; record CPU frequency governor, IRQ affinity, and NUMA/cache topology where applicable.
- Publish `perf stat` counters and flamegraphs for the durable p99 bottleneck and saturated-throughput run.
- Profile before changing data structures. A claimed optimisation must improve a named workload without regressing stated correctness or tail-latency goals.

### Benchmark Narrative

The final report answers four questions:

1. What does quorum durability cost versus asynchronous replication?
2. Where is the TCP baseline limited: CPU, network, batching, or persistence?
3. Does UDP multicast improve the throughput-versus-p99 curve after accounting for acknowledgement and repair traffic?
4. How does the system behave during a follower failure, leader failover, and catch-up?

## 16. Repository Layout

```text
 skiff/
  CMakeLists.txt
  cmake/
   include/skiff/
    raft/
    storage/
    transport/
    kv/
    test/
  src/
    raft/
    storage/
    transport/tcp/
    transport/udp_experiment/
    kv/
  tests/
    deterministic/
    integration/
    persistence/
  benchmarks/
    client/
    scripts/
    results/
  docs/
    protocol.md
    benchmark-methodology.md
    failure-model.md
```

## 17. Delivery Milestones

1. Repository scaffold, pinned C++26 toolchain, message model, fake clock/transport/storage.
2. Single-node append and deterministic KV application.
3. Three-node election and TCP AppendEntries.
4. Quorum commit, conflict repair, and deterministic failure matrix.
5. Mmap durability, recovery, corruption handling, and snapshots.
6. Real RK1 integration suite and TCP benchmarks.
7. UDP multicast experiment with TCP repair, equivalent correctness tests, and comparative benchmarks.
8. Public documentation, benchmark artifacts, and CV update using measured results.
