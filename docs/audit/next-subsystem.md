# NEXT SUBSYSTEM

Constitution §68: after the audit, name the next subsystem in this shape
and wait for the instruction to build it. This is that report, and
nothing in it is implemented.

**Subsystem: an Intel gigabit NIC driver (`e1000e`/`igb` class), the
first non-virtio network device.**

## Problem

Every network interface this system has ever driven is virtio. The stack
claims not to depend on a specific NIC — `drivers/network/README.md`
states it as Invariant 5 — but nothing has ever tested that claim,
because there has only ever been one driver, written against the same
device model that the interface was designed around. `drivers/network/`
contains a README and no code.

An interface that has one implementation is a guess about what an
interface should be.

## Current implementation

- `drivers/virtio/virtio_net.c` (the only NIC), registering through
  `netif_register` with `struct netif_ops { transmit, release }`.
- The driver-facing kernel APIs a second driver would use already exist
  and are exercised by NVMe, which is not virtio: `pci_enable_device`,
  `pci_msix_request(pdev, index, fn, arg, cpu)` with per-CPU routing,
  `dma_alloc`/`dma_map`/`dma_unmap`, `COSMO_MODULE(...)` with signing
  and `MODULE_CAP_DRIVER`.
- The receive path takes packets on a per-CPU worker (network design.md,
  "post-audit unit 11"); the checksum capabilities `NETIF_CAP_TXCSUM`
  and `NETIF_CAP_RXCSUM` exist and virtio sets them from its features.
- The harness gives QEMU exactly one NIC: `scripts/qemu-run.sh` passes
  `-device virtio-net-pci`.

## Why it matters

§60 orders the hardware roadmap `NVMe, Intel/AMD modern NIC, USB, AHCI,
IOMMU, GPU later` and NVMe is done, so this is the next item on that
list. It is also the cheapest way to find out whether the driver
interface is honest:

- **It tests Invariant 5 for the first time.** A descriptor-ring device
  with hardware checksum flags, link-state interrupts and a rate limiter
  is shaped differently from a virtqueue. Whatever assumption `netif`
  has absorbed from virtio will surface here.
- **QEMU emulates it**, so §61's mandatory QEMU environment covers it
  (`-device e1000e` and `-device igb`, both in QEMU 8+). USB and AHCI
  are also emulated, but neither exercises an interface this project
  claims to have generalised.
- **It is the smallest of the remaining hardware items** — one device
  class, no new subsystem, no new user-visible API.

## Proposed design

A boot module, `drivers/network/e1000e.c`, matching Intel's 82574L
(`8086:10d3`, QEMU's `e1000e`) and refusing anything else. Nothing in
the kernel changes shape unless the work below proves it must; the point
of the exercise is to find out.

- **Rings.** Two descriptor rings in coherent memory from `dma_alloc`,
  legacy descriptors first (`RDESC`/`TDESC`), one RX and one TX to
  begin, sized to a page each. Receive buffers are mbuf-backed and
  mapped with `dma_map`, so packets reach the stack without a copy, as
  virtio's do.
- **Interrupts.** One MSI-X vector requested through `pci_msix_request`
  with a CPU, so received packets are queued on the worker of the CPU
  that took the interrupt — the path unit 11 built. A second vector for
  link state and errors if the extra vector proves worth its wiring.
- **Offloads.** `NETIF_CAP_RXCSUM` from the descriptor's status bits and
  `NETIF_CAP_TXCSUM` through context descriptors, both behind a
  measurement: §21 forbids complexity without a benchmark, and QEMU's
  user-mode backend has already been shown to give no offload, so this
  may end up deliberately not done with that written down.
- **Link state and reset.** A watchdog that reads the status register,
  and a controller reset on a transmit that does not complete, mirroring
  what NVMe's timeout path does rather than inventing a second idiom.

## Affected files

| File | Change |
| --- | --- |
| `drivers/network/e1000e.c` | new, the driver |
| `drivers/network/e1000e.h` | new, register and descriptor definitions |
| `kernel/kernel.mk`, `build/module.mk` | build and sign the module |
| `scripts/qemu-run.sh` | a second NIC for the test shapes that want it |
| `tests/boot/*.py` | recognise the interface in the boot output |
| `kernel-services/network/nettest.c` | the existing suite, run over it |
| `docs/drivers/network/` | new: design, api, invariants, testing |
| `kernel/include/kernel/netif.h` | **only if** the interface proves virtio-shaped |

## New APIs

None expected in the kernel. That is the hypothesis being tested: if
`netif_ops` needs a new callback (a link-state notification, a
per-queue transmit, a way to say "this buffer is mine again"), that is
the finding, and it is worth more than the driver.

The driver's own surface is a module entry point and nothing else; no
system call, no `sysctl` name, no `/proc` file.

## Migration plan

None. Nothing existing changes behaviour: virtio-net stays the default
in the harness, the new driver binds only its own PCI id, and a machine
without that device never loads it. The two can coexist on one boot,
which is itself a test that the stack holds two interfaces.

## Tests

- The existing `net-*` self-tests, run with the new NIC as the default
  interface: they are interface-agnostic by construction and this is the
  first time that has been true in fact rather than in intent.
- A boot shape with **both** NICs, requiring two interfaces to register,
  each to carry traffic, and `netif_unregister` of one to leave the
  other working.
- Ring wrap: enough packets to pass the ring's end in both directions.
- A transmit that never completes, injected, requiring the watchdog to
  reset and the interface to carry traffic afterwards.
- Unregister under load, which is where the lifetime rules bite.

## Benchmarks

`net-bench` already exists and reports loopback throughput. The numbers
that matter here are, per §21, the ones that would justify any
complexity added: packets per second and cycles per packet over the new
NIC against virtio-net on the same host, with and without each offload,
so that an offload that does not pay can be left out with a figure
beside the decision rather than an opinion.

## Risks

- **QEMU's `e1000e` is not a real 82574L.** Passing under emulation
  says less than it does for virtio, whose device model is the
  specification. The mitigation is to write to the datasheet rather
  than to observed QEMU behaviour, and to say in the docs which
  registers are untested on hardware.
- **The interface may need to change**, which turns a driver unit into a
  driver-plus-interface unit. That is the finding rather than a failure,
  but it makes the size uncertain in a way the last several units were
  not.
- **A second NIC in the harness costs boot time** in every shape that
  enables it, against a budget that has already flaked twice this week.
  Mitigation: the two-NIC shape is its own step in the chain rather than
  a change to the default.
- **Little of it is testable on real hardware here**, so "works" will
  mean "works in QEMU" for now, which §61's hardware matrix names as the
  eventual answer and not this unit's.

## Alternatives considered

- **USB (§60 #3)** — larger by a wide margin (host controller, hub, and
  a class driver before anything is observable) and it exercises no
  interface this project claims to have generalised.
- **AHCI (§60 #4)** — the block layer already has two drivers (virtio
  and NVMe), so a third proves less than a second NIC does.
- **The AArch64 follow-ups** (GICv3, ASID allocation, FP/SIMD at EL0) —
  named in the README, not in §60, and each is a self-contained
  improvement to something that works rather than a new capability.
  Worth doing; less informative than this.
