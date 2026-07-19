# Skiff Performance Guide

This guide explains how the first Skiff network baseline works, how to measure
it on the Turing Pi cluster, and which host settings trade latency for
throughput. The benchmark is an in-memory, single-primary baseline. It does
not measure persistence, replication, or Raft.

## What One Operation Means

One `PUT` is one 24-byte request containing `{id, key, value}`. The server
updates its local hash map and returns one 8-byte response containing `id`.
The client verifies the response IDs are ordered and complete.

Pipelining does not execute writes in parallel. It allows several writes to be
in flight on one TCP connection while the single-threaded server still applies
them in order.

```text
pipeline 1:       send PUT 1, wait for ACK 1
pipeline 1,024:   keep at most 1,024 PUTs awaiting their ACKs
```

Batching is separate from pipelining. Skiff reads, writes, and replies to up to
256 already-in-flight frames per syscall. It reduces syscall overhead without
changing the per-`PUT` acknowledgement contract.

## Running A Baseline

Build the release binary, not the debug binary:

```sh
cmake --preset macos-llvm22-release
cmake --build --preset macos-llvm22-release
```

On the primary:

```sh
taskset -c 4 /tmp/skiff-bench/skiff_node --port 19000
```

On the load node, use the direct cluster LAN address rather than the Tailscale
name:

```sh
taskset -c 4 /tmp/skiff-bench/skiff_bench tpn1.local 19000 1000000 1024
```

`tpn1.local` resolved to `192.168.1.165` during the initial tests. Confirm the
address from the load node before every run:

```sh
getent ahostsv4 tpn1.local
```

Warm the server before recording steady-state results. The first million keys
allocate hash-table entries; later updates measure the transport path instead.

```sh
taskset -c 4 /tmp/skiff-bench/skiff_bench tpn1.local 19000 1000000 1024 >/dev/null
```

## Reading The Metrics

- `operations_per_second`: completed, acknowledged `PUT`s per second.
- `p50`: half of operations completed faster than this latency.
- `p95`, `p99`, and `p99.9`: tail latency; queueing, scheduling, and interrupt
  delay appear here first.
- `max`: the single slowest sample. Treat it as an outlier signal, not as a
  stable latency target.
- `pipeline`: maximum outstanding operations, not server parallelism.

For a bounded pipeline, throughput is limited by the number of in-flight
requests divided by their observed latency. This is why a pipeline of 64
reached about 60k operations/s before batching: `64 / 1.056 ms` is about 60.6k.

Increasing the pipeline improves throughput until the CPU, NIC, or link is
saturated. Beyond that point it only increases queueing latency.

## Turing Pi Hardware Ceiling

The tested nodes are Rockchip RK3588 systems:

- CPUs `0-3`: Cortex-A55 efficiency cores, up to 1.8 GHz.
- CPUs `4-7`: Cortex-A76 performance cores, about 2.3 GHz.
- `end0` and `end1`: 1 Gb/s full-duplex Ethernet links.
- The NIC supports MTU up to 9000, but the active MTU is 1500.

The request direction is the limiting direction: each acknowledged operation
sends 24 bytes from the client to the primary. The raw payload ceiling of a
1 Gb/s link is about 5.21M operations/s. TCP/IP/Ethernet framing and ACKs
lower the practical ceiling. The best measured one-connection throughput was
4.58M operations/s, so there is no remaining 2x software win on this link.

Pipeline 1 has a separate round-trip latency ceiling. At a 279 us p50, it can
complete only about 3.6k operations/s even if the link is otherwise idle.

## CPU And IRQ Affinity

The biggest host-side improvement was running both Skiff and its NIC interrupt
on an A76 core. The NIC had one active RX/TX queue, its IRQ was serviced on
CPU 0, and RPS/XPS were disabled. Running Skiff on CPU 4 while leaving the IRQ
on CPU 0 forces cross-core packet handoff between an A55 and an A76.

Discover the NIC IRQs; do not copy the IRQ numbers from another boot or host:

```sh
awk '/end0/ {print}' /proc/interrupts   # primary
awk '/end1/ {print}' /proc/interrupts   # load node
```

Record the current policy before changing it:

```sh
cat /proc/irq/IRQ_NUMBER/smp_affinity_list
```

For a dedicated benchmark run, place each displayed `end0` or `end1` IRQ on
the same A76 core as its Skiff process:

```sh
sudo sh -c 'echo 4 > /proc/irq/IRQ_NUMBER/smp_affinity_list'
taskset -c 4 /tmp/skiff-bench/skiff_node --port 19000
```

Restore the previous affinity after the run. The initial nodes used `0-7`:

```sh
sudo sh -c 'echo 0-7 > /proc/irq/IRQ_NUMBER/smp_affinity_list'
```

This is a host-wide networking setting. Do not leave it pinned on a shared
Kubernetes node without deciding which workloads own that core and NIC queue.

CPU 4 was the first tested A76 core, not a guaranteed best core. A 5,000-write
pipeline-one sweep found CPU 6 had the best stable p99.9 at 297 us. Repeat this
sweep after major workload or thermal changes; core choice is a measurement,
not a hardware rule.

## Interrupt Coalescing Profiles

Coalescing waits briefly before raising an interrupt, reducing interrupt rate
and increasing batching. Inspect the current setting with:

```sh
ethtool -c end0
```

The initial driver settings were roughly RX 327 us, TX 1000 us, and TX 25
frames. They favor maximum throughput. Low-latency trial settings were RX 50
us, TX 50 us, and TX one frame:

```sh
sudo ethtool -C end0 rx-usecs 50 tx-usecs 50 tx-frames 1
```

Measured results with A76 process and IRQ affinity:

| Profile | Pipeline | Operations/s | p50 | p99 |
|---|---:|---:|---:|---:|
| Default coalescing | 1,024 | 2.00M | 431 us | 680 us |
| Default coalescing | 4,096 | 4.58M | 837 us | 973 us |
| Low-latency coalescing | 1 | 4.01k | 261 us | 281 us |
| Low-latency coalescing | 1,024 | 2.62M | 349 us | 571 us |
| Low-latency coalescing | 4,096 | 2.47M | 486 us | 775 us |

Use default coalescing for maximum throughput. Use lower coalescing for a
latency-sensitive or moderate-pipeline workload. Restore the original values
after an experiment:

```sh
sudo ethtool -C end0 rx-usecs 327 tx-usecs 1000 tx-frames 25
```

For a pipeline-one latency sweep with CPU and IRQ affinity on A76 core 4, the
50 us setting produced p50 262 us, p99 276 us, and p99.9 606 us. Zero
coalescing was worse: p50 356 us and a 25 ms maximum. Interrupts still consume
CPU, so the minimum setting is not necessarily the minimum latency.

## Offloads And Socket Settings

The useful offloads are already enabled on both nodes:

- TCP segmentation offload (TSO)
- Generic segmentation offload (GSO)
- Generic receive offload (GRO)
- RX/TX checksum offload

Check them with `ethtool -k end0`. Keep them enabled for the throughput
baseline. `TCP_NODELAY` was tested and made this tiny-frame workload slower:
it disabled useful coalescing and reduced peak throughput.

The existing TCP socket buffers are sufficient for the measured RTT and batch
sizes. Increase buffers only after a measurement shows a buffer limit; larger
values are not a generic speed setting.

## Busy Polling

The RK3588 kernel has `CONFIG_NET_RX_BUSY_POLL=y`. Busy polling replaces part
of the interrupt wake-up path with a short NIC polling budget. It can lower
latency substantially, but it consumes a dedicated CPU while traffic is idle.
It is therefore an opt-in benchmark profile, not a cluster-wide default.

For an isolated experiment, stop K3s on the primary and load node, choose the
measured A76 core, and align the NIC IRQ before enabling the budget:

```sh
sudo systemctl stop k3s                 # tpn1
sudo systemctl stop k3s-agent           # tpn4
sudo sysctl -w net.core.busy_read=50 net.core.busy_poll=50
```

Start both endpoints with an explicit socket budget:

```sh
taskset -c 6 /tmp/skiff-bench/skiff_node --port 19000 --busy-poll-us 50
taskset -c 6 /tmp/skiff-bench/skiff_bench tpn1.local 19000 20000 1 --busy-poll-us 50
```

With CPU 6, matching NIC IRQs, 50 us coalescing, and K3s paused, normal
interrupts measured p50 258 us, p99 269 us, and p99.9 333 us. The 50 us
busy-poll profile measured p50 54 us, p99 61 us, and p99.9 107 us.

Restore the hosts immediately after the experiment:

```sh
sudo sysctl -w net.core.busy_read=0 net.core.busy_poll=0
sudo systemctl start k3s                 # tpn1
sudo systemctl start k3s-agent           # tpn4
```

## Switch And Jumbo Frames

The cluster switch path is already much faster than Tailscale. The direct LAN
test uses ordinary TCP over IPv4 and Ethernet, not TCP-over-UDP.

Jumbo frames are the only likely switch-side optimization. The NICs advertise
an MTU capability of 9000, but the tested board-switch path did not accept it:
setting `tpn1` `end0` to MTU 9000 immediately dropped carrier. The interface
was restored to MTU 1500 before restarting K3s, and no jumbo benchmark was
run. Do not retry on this shared network until the board-switch configuration
and supported port MTUs are known.

To exceed the current wire-rate result materially, the cluster needs a faster
than 1 Gb/s network path or larger logical client commands. UDP or QUIC do not
remove the physical bandwidth limit, and a reliable UDP protocol would need to
reimplement ordering, acknowledgements, loss recovery, and flow control.

## Recommended Profiles

| Goal | CPU and IRQ | Coalescing | Pipeline |
|---|---|---|---:|
| Lowest one-request latency | A76 core 4 | Low-latency | 1 |
| Balanced throughput and tail latency | A76 core 4 | Low-latency | 1,024 |
| Maximum one-connection throughput | A76 core 4 | Default | 4,096 |

Always record the Git commit, compiler flags, hostnames, address used, CPU and
IRQ affinity, coalescing settings, MTU, pipeline, warm-up policy, and complete
latency results with each benchmark.
