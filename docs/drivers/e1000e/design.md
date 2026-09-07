# e1000e driver: design

The first network device this system drives that is not virtio.
`docs/audit/next-subsystem.md` names it and says why: every interface
the stack has ever had was virtio, so its claim not to depend on a
specific NIC (Invariant 5 in `drivers/network/README.md`) had one
implementation, and an interface with one implementation is a guess.
The driver is the module `e1000e` (`drivers/network/e1000e.c`,
`e1000e.h`), bound through the `pci` bus to Intel `8086:10d3`, the
82574L that QEMU's `-device e1000e` models, and it registers one
`struct netif`. It reaches the kernel only through exported symbols.

**Nothing in the kernel changes shape for it.** That was the hypothesis
the report set out to test, and it held: `netif_ops` (transmit,
release), `netif_rx`, `pci_*`, `dma_*` and `timer_*` were enough. Where
the two devices differ is recorded below as facts about this one, not as
new interface.

## Bring-up (82574 datasheet §4.6, §14)

1. `pci_enable_device(pdev, true)`, `dma_set_mask(64)` (the 82574
   addresses 64 bits), map BAR0.
2. Mask every interrupt (`IMC` all ones), read `ICR` to drain it, then
   `CTRL.RST`. Wait 10 ms (the datasheet's post-reset settle), mask
   again: reset clears `IMS` but the order costs nothing and a stray
   cause during setup would be delivered to a handler that has no rings.
3. `CTRL.SLU` ("set link up"): the MAC does not report link until told
   to try. `CTRL.ASDE` with speed and duplex left to autonegotiation.
4. The station address from `RAL0`/`RAH0`, which hardware loads from
   the EEPROM at reset (`RAH0.AV` says it is valid). The EEPROM is not
   read directly: the address registers are the documented result of
   that read, and parsing the EEPROM would be a second way to learn one
   fact.
5. The multicast table array (`MTA`, 128 words) zeroed; no multicast
   filtering is programmed, and `RCTL.MPE` is off, so only unicast to
   the station address and broadcast are accepted.

## Rings

Two legacy descriptor rings of 256 entries each, from `dma_alloc`
(coherent, so no sync on the descriptors themselves).

**Receive.** Every descriptor owns one cluster mbuf (`m_getcl`, 2 KiB),
mapped `DMA_FROM_DEVICE`; `RCTL.BSIZE` is 2048 with `BSEX` off, so the
device never writes past the cluster. `RDT` is the last descriptor
software has filled; hardware writes `DD` and `EOP` into `status` as it
fills them. The handler walks from its own head while `DD` is set,
unmaps, trims to `length`, and hands the frame to `netif_rx`; a fresh
cluster replaces it before the descriptor is given back. When there is
no fresh cluster the frame is dropped and its buffer serves again
without being unmapped: the descriptor keeps the address it has, so
there is no remap that could fail and no way for the device to be handed
a descriptor pointing at memory the driver has freed. Frames whose
`errors` byte is set, or shorter than an Ethernet header, are counted
and dropped. `RCTL.SECRC` strips the CRC so `length` is the frame.

**Transmit.** One descriptor per mbuf in the chain, each segment mapped
`DMA_TO_DEVICE`; the last carries `EOP`, every one carries `IFCS` (the
device appends the CRC) and `RS` (report status, so `DD` is written back
and the slot can be reclaimed). `TCTL.PSP` pads frames shorter than 64
bytes. A chain that needs more free descriptors than the ring has is
refused with `-ENOBUFS` rather than queued: the stack owns queuing
policy, and a driver that buffers is a driver that hides a full link.
Completed descriptors are reclaimed in the interrupt handler, oldest
first, stopping at the first without `DD`.

The ring registers (`RDBAL/RDBAH/RDLEN/RDH/RDT`, `TDBAL/...`) are
programmed with the ring empty and heads at zero. `RXDCTL`/`TXDCTL` are
left at their reset values: the first version of this driver set bit 25
in each as a "queue enable", from memory of a later Intel family where
it is one, and a test with the bit clear showed QEMU's model does not
need it. On the 82574 that bit is not a queue enable, so writing it was
programming a register from the wrong datasheet.

## Interrupts

One MSI-X vector. The 82574 maps causes to vectors through `IVAR`;
receive queue 0, transmit queue 0 and "other" are all pointed at entry
0, which `pci_msix_request` routes to CPU 0. `IMS` enables `RXQ0`,
`TXQ0`, `RXT0`, `RXDMT0`, `RXO`, `TXDW`, `LSC` and `OTHER`. Without
MSI-X the driver falls back to single-message MSI, where the same
handler serves the same `ICR`.

The handler reads `ICR` (read clears it) and then services both rings
regardless of which cause bits were set. That is deliberate: the
datasheet's cause bits differ between the legacy and queue-mapped
schemes, QEMU's model raises one or the other depending on how it was
built, and a handler that trusted the bits would work on one and not
the other. Servicing both is a few register reads on a ring that is
usually empty, and it is right on both.

`LSC` (link status change) reads `STATUS.LU` and logs the transition;
the interface stays registered either way, since a link that comes back
should not need a re-probe.

## The watchdog

A one-second timer. If transmit descriptors are outstanding and the
hardware head (`TDH`) has not moved since the last tick for five ticks,
the transmit path is declared hung: it is logged with the counts, the
ring is disabled, every outstanding mbuf is unmapped and freed (counted
as dropped), the ring is re-programmed empty and re-enabled. Receive is
left alone. This mirrors NVMe's timeout-and-reset rather than inventing
a second idiom, and it is bounded: a device that keeps hanging keeps
being reset, and each reset says so.

## Offloads: measured, and not worth it

`nif->caps` is 0: neither `NETIF_CAP_RXCSUM` nor `NETIF_CAP_TXCSUM` is
claimed. The 82574 can do both (`RXCSUM.TUOFL`, context descriptors),
and §21 forbids that complexity without a benchmark showing it pays.
`net-nicbench` (`docs/kernel-services/network/design.md`, "The NIC-path
benchmark") is that benchmark, and it says no. Measured 2026-09-07 on
the boot test (QEMU TCG, 4 CPUs, Apple Silicon host; noisy, indicative):

| Shape | Interface | ARP round trips | UDP 1 KiB sends | sw checksum of 1 KiB |
|---|---|---|---|---|
| x86_64, both | virtio-net `eth0` | 12 568/s, 80 µs | 22 567/s, 44 µs | 1.1 µs = 2 % of a send |
| x86_64, both | e1000e `eth1` | 14 299/s, 70 µs | 21 855/s, 46 µs | 1.1 µs = 2 % |
| x86_64, e1000e only | e1000e `eth0` | 12 870/s, 78 µs | 20 072/s, 50 µs | 1.2 µs = 2 % |
| aarch64, both | virtio-net `eth0` | 7 915/s, 126 µs | 13 976/s, 72 µs | 1.0 µs = 1 % |
| aarch64, both | e1000e `eth1` | 7 583/s, 132 µs | 15 559/s, 64 µs | 1.0 µs = 1 % |

A transmit checksum offload can save at most the checksum's share of a
send, and that share is one to two percent of a path dominated by the
stack and the device model. Receive offload saves the same order per
inbound packet. Context descriptors, the transmit context state machine
and the receive status-bit handling would buy two percent on a machine
where they can be measured at all, so they are not written, and this
table is why. The decision is revisited when the path gets ten times
cheaper or the traffic gets ten times larger, not before.

The two drivers are within noise of each other, each faster on one
column. That is a result too: the driver is not where this path spends
its time, so a faster driver would not show.

The benchmark also found a bug in this driver that nothing else had: it
incremented `rx_packets`, `rx_bytes`, `tx_packets` and `tx_bytes` that
`netif_rx` and `netif_transmit` already count, so every figure was
doubled. Beside virtio-net's numbers in the same boot it was obvious;
alone it had looked plausible. The driver now counts only what the layer
cannot see: its own ring-level drops and hardware-reported errors.

## Naming and coexistence

The interface takes the first free `ethN`, checked with `netif_find`, so
a machine with virtio-net gets `eth0` there and `eth1` here, and one
without gets `eth0` here. `netif_register` refuses a duplicate name and
this driver never asks for one. Two interfaces coexist because the
registry and the receive path were written for a list; which one
carries traffic is `netif_default()`, the first non-loopback interface
that is up — there is no per-interface route, and this driver does not
add one.

## Teardown

`remove`: `netif_unregister` first (no transmit or receive reaches the
rings after it returns), `RCTL.EN` and `TCTL.EN` cleared, `IMC` all
ones, the watchdog cancelled synchronously, the vector released and
`synchronize_irq` waited on, then every mbuf the rings still hold is
unmapped and freed, the rings freed, BAR0 unmapped, and the creator's
`netif_put`. The `release` callback frees the driver structure when the
last holder is gone, which may be after `remove` — a queued packet or a
route lookup can outlive the device (`docs/kernel/quiesce/`).

## What is not here

Multiple queues (the 82574 has two of each), interrupt moderation
(`ITR`), jumbo frames, VLAN offload, wake-on-LAN, EEPROM access, and
`igb`: the 82576 is a different family with advanced descriptors and a
different queue layout, and one datasheet at a time is enough.
