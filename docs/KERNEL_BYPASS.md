# Kernel Bypass Research

This document records the feasibility of DPDK and AF_XDP for Skiff on the
Turing Pi RK3588 nodes. It distinguishes installing a package from actually
bypassing the kernel data path.

## Conclusion

DPDK is a worthwhile long-term systems goal, but the built-in RK3588 Ethernet
interface is not currently a viable native DPDK target. The immediate latency
path is Linux TCP with the opt-in busy-poll profile. A real DPDK experiment
needs either driver work or a DPDK-supported NIC.

## Observed Hardware

Both tested nodes expose their direct Ethernet interface through:

```text
driver:        st_gmac
platform driver: rk_gmac-dwmac
compatible:    rockchip,rk3588-gmac
               snps,dwmac-4.20a
link:          1 Gb/s full duplex
```

This is a platform-integrated Synopsys DWMAC device, not a PCIe NIC. It is
different from the Intel, NVIDIA, Marvell, and other server NICs usually used
with DPDK poll-mode drivers.

## DPDK Feasibility

Ubuntu 24.04 provides DPDK 23.11 packages for ARM64, but no DPDK tools are
currently installed. The official DPDK NIC driver list has no native
`st_gmac`, `stmmac`, or DWMAC poll-mode driver.

The common DPDK direct-device setup also does not fit this NIC:

- The NIC is platform-integrated, not PCIe.
- `CONFIG_VFIO_PLATFORM` is disabled, so it cannot use VFIO platform binding.
- `CONFIG_STMMAC_UIO` is disabled, so it cannot use the stmmac UIO path.
- Huge pages and generic VFIO support exist, but zero huge pages are reserved.

DPDK offers AF_PACKET, TAP, PCAP, and other virtual drivers. Those can be
useful for functional experiments, but they still route through the kernel and
are not the kernel-bypass result we want.

## AF_XDP Feasibility

AF_XDP is the smaller Linux-oriented kernel-bypass experiment. An XDP program
redirects packets from a network queue into a user-space UMEM and descriptor
rings. In zero-copy mode, the NIC DMA buffers can be shared with user space.

The running Rockchip kernel has BPF syscalls and BPF JIT enabled, but:

```text
CONFIG_XDP_SOCKETS is not set
```

AF_XDP therefore cannot run on this kernel today. However, the installed 6.1
kernel exports `stmmac_xdp_setup_pool`, the stmmac zero-copy XSK pool setup
function. This is strong evidence that the driver-side implementation is linked
into this Rockchip kernel and only the AF_XDP socket API is configured out.

Upstream stmmac AF_XDP zero-copy support was published in a 2021 seven-patch
series and is present in current Linux `stmmac_xdp.c`. The RK3588 uses the same
stmmac/DWMAC driver family. This makes an AF_XDP zero-copy prototype feasible,
but not guaranteed: it must be validated at runtime on the RK3588 MAC by
forcing zero-copy mode. If bind fails in forced zero-copy mode, the platform
cannot provide the path and must not be silently measured in copy mode.

The DPDK AF_XDP PMD has the same prerequisite: `CONFIG_XDP_SOCKETS=y`, plus
libbpf/libxdp. It does not bypass the need for driver support.

## What DPDK Would Change

Current Skiff uses:

```text
Skiff -> send/recv -> Linux TCP/IP -> st_gmac driver -> NIC
```

A native DPDK path would use:

```text
Skiff packet loop -> DPDK PMD -> NIC DMA rings -> NIC
```

The application would own packet buffers, polling, and NIC queues. Normal
Linux TCP is not present in that path. Skiff would need a custom Ethernet/UDP
transport or a user-space TCP stack, including framing, acknowledgements,
ordering, retransmissions, flow control, congestion control, and recovery.

This is why DPDK is a transport experiment, not a flag added to the current
TCP server.

## Measured Baseline

Before bypass work, the current direct-LAN latency profile is already strong:

| Path | Pipeline | p50 | p99 | p99.9 |
|---|---:|---:|---:|---:|
| Linux TCP, interrupt driven | 1 | 258 us | 269 us | 333 us |
| Linux TCP, 50 us busy poll | 1 | 54 us | 61 us | 107 us |

The busy-poll profile spends a dedicated A76 core polling for packets. It is
the baseline a kernel-bypass prototype must beat, including its CPU cost and
its acknowledgement semantics.

## Recommended Path

1. Keep TCP plus busy polling as the latency baseline and record CPU use.
2. Do not install DPDK merely to claim kernel bypass; it has no native PMD for
   the current NIC.
3. Rebuild the Rockchip kernel on a sacrificial node with
   `CONFIG_XDP_SOCKETS=y`, then force AF_XDP zero-copy against `end0`. Use the
   upstream `xdpsock` test before building Skiff transport code. A forced
   zero-copy bind is the go/no-go check; do not accept copy-mode fallback as a
   kernel-bypass result.
4. For a genuine DPDK experiment, attach a supported PCIe NIC and dedicate it
   to Skiff. This provides a maintained PMD, VFIO binding, queues, and a clean
   comparison against the onboard 1 Gb/s path.
5. Build the first raw-packet prototype as a separate transport mode. Start
   with one-way packets and explicit acknowledgements; do not replace the TCP
   baseline until ordering, loss recovery, and flow control are measured.

## Sources

- [DPDK NIC driver list](https://doc.dpdk.org/guides/nics/index.html)
- [DPDK AF_XDP PMD requirements](https://doc.dpdk.org/guides/nics/af_xdp.html)
- [Linux AF_XDP documentation](https://docs.kernel.org/networking/af_xdp.html)
- [stmmac AF_XDP zero-copy patch series](https://lwn.net/Articles/852357/)
- [Upstream stmmac AF_XDP implementation](https://codebrowser.dev/linux/linux/drivers/net/ethernet/stmicro/stmmac/stmmac_xdp.c.html)
