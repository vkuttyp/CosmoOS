# Parallels devices: randomness, the clock, storage and network — 2026-10-13

The second unit after 1.0, chosen by the owner after v1.0.1: the
AArch64 image reached the live shell on Parallels Desktop 27 (Apple
Silicon) but could not install, had no network, never seeded its
randomness pool and kept time from 1970
(`docs/audit/2026-10-13-acpi-discovery-report.md`, "What works on
Parallels, and what does not"). Base: main `bfe009a9` (v1.0.1 plus a
flakes record). Goal: on Parallels, the pool seeds, the wall clock is
right, the live image installs to the VM's NVMe disk and boots from it,
and the network comes up; nothing Parallels-specific in the kernel.

## What Parallels offers (measured)

| Need | What QEMU gives | What Parallels gives |
|---|---|---|
| randomness | virtio-rng; RNDR on some CPU models; the firmware's `EFI_RNG_PROTOCOL` over either | none of the three: no RNDR (`ID_AA64ISAR0_EL1.RNDR` 0), no virtio-rng device, `firmware: seed 0 bytes` |
| wall clock | the PL031 at virt's address | no RTC the tables describe; UEFI `GetTime` works |
| disk | virtio-blk, NVMe with MSI-X | SATA not on PCI; NVMe 1ab8:4007 with MSI only, BAR0 16 KiB, 64-bit with a read-only upper half |
| network | modern virtio-net | 1af4:1000, legacy-only virtio-net, registers in a 4 KiB memory BAR0, MSI-X table in BAR1 |

The BARs were read with a scratch print in `decode_bars`, which on
Parallels hung the boot: enumeration turns a device's memory decoding off
while sizing its BARs, and the firmware's framebuffer, which the console
writes, is behind one of them (recorded in the inventory; nothing logs
there in the tree).

## PR 1: the firmware's seed and time, and a jitter source

| Area | Change | Files |
|---|---|---|
| Boot protocol v10 | `fw_flags` (`COSMOBOOT_FW_TIME`), `fw_seed_len`, `fw_epoch`, `fw_seed[64]` | `boot/protocol/cosmoboot.h` |
| Loader | `firmware_gather`, just before `ExitBootServices`: 64 bytes from `EFI_RNG_PROTOCOL`, the time from `GetTime` converted to UTC; `firmware_report` repeats both on the serial port after `ExitBootServices` | `boot/uefi/firmware.c`, `efi.h`, `main.c` |
| Kernel, boot info | `bootinfo_init` copies the seed and wipes the boot info's copy; `bootinfo_take_fw_seed`, `bootinfo_fw_epoch` | `kernel/core/bootinfo.c` |
| Randomness | the firmware's seed absorbed first, credited half like virtio-rng (`random.firmware=off` refuses it); with no CPU source and no firmware seed, a timing-jitter source (SP 800-90B most-common-value estimate per batch of 1024 samples, repetition count test, at most 1/8 bit per sample; `random.jitter=off`); the seeded line names `firmware` and `jitter` | `kernel/core/random.c`, `kernel/core/jitter.c` |
| Wall clock | with no RTC, the firmware's time at boot | `kernel/timer/timer.c` |
| Tests | host test `test_jitter`; `test-entropy` is four boots, one source each (none, jitter, CPU, device); the ordinary boots are now firmware-seeded | `tests/host/test_jitter.c`, `Makefile`, `tests/boot/run_boot_test.py` |

**QEMU's firmware seeds the ordinary boots.** OVMF and AAVMF offer
`EFI_RNG_PROTOCOL` whenever QEMU has a virtio-rng (and on the CPU's
random instructions), so `make test` now seeds from the firmware in the
first milliseconds (`cpu 0, firmware 256, jitter 0, devices 0`), before
the virtio-rng driver loads. The device-only configuration survives as
the fourth entropy boot, with `random.firmware=off random.jitter=off`.

**The jitter decision.** M1 chose no jitter source; the owner reversed
that for this unit (2026-10-13), as the only way to seed on Parallels.
It runs only with no CPU source and no firmware seed. Measured:

| Machine | Batches | Time | Credited | Lowest estimate |
|---|---|---|---|---|
| QEMU AArch64 (TCG), no virtio-rng | 5 | 3 ms | 301 bits | 0.46 bits per sample |
| QEMU x86-64 (TCG), no virtio-rng | 5 | 4 ms | 269 bits | 0.42 bits per sample |
| Parallels Desktop 27, Apple Silicon | 3 | 6 ms | 316 bits | 0.67 bits per sample |

On Parallels the release image now logs `timer: wall clock 1791869029 s
since 1970 from the firmware at boot (no RTC)` and `random: pool seeded
after 6 ms: 316 bits credited (cpu 0, firmware 0, jitter 316, devices
0)`, and reaches the shell.

## Review of PR 1 (#378)

Codex and Qodo, 13 findings. Changed:
- **The jitter credit trusted the histogram.** The most-common-value
  estimate sees only how widely the values spread, so a cycle through
  every byte value would have been credited the full bit per sample.
  `jitter_lag_h_q16` (SP 800-90B's lag predictor, lags 1-256, global
  rate and longest correct run) is now the second estimate, and the
  credit takes the lower. The host test shows the cycle refused.
- **The repetition count test reset at each batch:** its run now
  carries over.
- **The stricter estimate is stricter under emulation.** QEMU's TCG
  makes the timings regular enough that the lag predictor rated some
  batches at 0.01-0.05 bits per sample and refused half of them on
  x86-64: with the 32-batch limit the jitter-only boot ended at 226 bits,
  unseeded (AArch64 seeded after 22). The limit is now 128 batches (about
  0.4 s at that rate), not a looser estimate, and each sample's workload
  is heavier (256 steps through 64 KiB, not 64 through 16 KiB), which
  brought x86-64 release boots from 18-43 batches to 8-17. Parallels
  needs 5 or 6 batches (12-16 ms), its lowest estimate down from 0.67 to
  0.27 bits per sample.
- **x86-64 under TCG cannot carry the jitter boot.** With all of that,
  x86-64 debug boots seeded from jitter in 30-69 batches three times in
  four, and in the fourth every one of 128 batches was refused (0 bits):
  in some boots TCG makes x86-64's timings fully regular, and the
  estimator refuses them, as it should. Whether this is the per-boot TCG
  mode recorded for the x86 benchmarks (flakes.md) is not established. `test-entropy` runs the jitter boot on
  AArch64 only, where it seeded in 11-38 batches in all six boots; the
  source's logic is the same code on both and host-tested.
- **Jitter ran only when the CPU had no random instruction.** It now
  runs when the CPU credited nothing, which also covers an advertised
  instruction whose every read fails.
- **Firmware time:** an impossible day for its month (leap years
  counted) and any year after 2500 are refused by the loader, and
  `timer_init` checks the nanosecond product.
- **`test-entropy`'s CPU boot** also refuses the firmware seed: OVMF and
  AAVMF offer the CPU's instructions as `EFI_RNG_PROTOCOL`, so it was not
  proving the CPU alone. The source-less boots pin their CPU model. The
  harness reads the expected command line from the built file, so a
  quote in `CMDLINE` cannot break the shell.
- **`docs/boot/design.md`** still said version 9.

Not changed:
- **The firmware clock starts behind** by the time between `GetTime` and
  the kernel's clock start. `GetTime` gives whole seconds on most
  firmware, so the error is under a second either way, as the design
  says.
- **The plan and history entries** come in the unit's last PR.
## PR 2: NVMe with one vector, and the legacy virtio transport

| Area | Change | Files |
|---|---|---|
| NVMe | with fewer than two MSI-X vectors, one-vector mode: one MSI-X vector or, with none, plain MSI; one I/O queue; `nvme_irq_one` schedules the admin queue's and every created I/O queue's poll (`irq_ioq`, published after each queue is created); queues created with interrupt vector 0; teardown disables MSI or MSI-X as it enabled; the probe line names the mode | `drivers/nvme/nvme.c` |
| PCI | `pci_io_bar_usable`, `pci_io_read`, `pci_io_write` (exported): an I/O BAR's registers, bounds-checked, through `arch_pci_io_*` (x86-64 ports; AArch64 none) | `drivers/pci/pci.c`, `kernel/include/arch/pci.h`, `kernel/arch/*/pci*.c` |
| virtio | the legacy transport (spec 4.1.4.8): BAR0's register block in memory or I/O space, MSI-X, a 32-bit queue page number (44-bit DMA mask), queues of the device's size; `virtio_device_init` skips VERSION_1 and FEATURES_OK on it; a legacy device's length on a read-only chain is ignored | `drivers/virtio/virtio_pci.c`, `virtio.c`, `virtqueue.c`, `drivers/include/drivers/virtio.h` |
| virtio-net | on a legacy device asks for `MRG_RXBUF`, which makes the header the 12 bytes VERSION_1 has, and refuses a device without it; drops a frame whose `num_buffers` is not 1 | `drivers/virtio/virtio_net.c` |
| Tests | `make test-smp2` runs the NVMe with one MSI-X vector (`QEMU_NVME_OPTS=,msix_qsize=1`, both architectures) and on x86-64 a legacy-only virtio-net (`QEMU_VNET_OPTS=,disable-modern=on`); the harness requires the modes' lines; host test `virtq-readonly-chain-length` | `Makefile`, `scripts/qemu-run.sh`, `tests/boot/run_boot_test.py`, `tests/host/test_virtq.c` |

**What QEMU can and cannot show.** QEMU's NVMe has only MSI-X, so its
one-vector MSI-X mode is the CI test and plain MSI has run on Parallels
only. QEMU's legacy virtio devices put their registers in an I/O BAR,
which x86-64 reaches with port instructions and AArch64 cannot; so CI
tests the legacy transport through I/O space on x86-64, and the memory
BAR path (Parallels') has run on Parallels only.

**A legacy device reports transmit lengths.** On the first Parallels boot
with the transport, every transmit completed with `device wrote 354 bytes
into a 0-byte chain`: Parallels' adapter puts the frame's length in the
used ring for a buffer it may only read. The spec (2.7.8.2) says legacy
devices commonly do this and legacy drivers should ignore the length
where they can; `virtq_pop` now does, on a legacy transport, for a chain
with no device-writable bytes only.

**Installing needs the live image on NVMe too.** `cosmo-install` copies
the ESP from the partition the loader was read from. Parallels' SATA disk
is invisible to the kernel, so with the live image on SATA it refused
(`the loader was not read from a GPT partition; there is no ESP to
copy`). With both disks NVMe they are two namespaces of one controller,
`nvme0n2` (live, with the `boot` partition) and `nvme0n1` (blank). The
guide's Parallels section says to set the VM up that way.

**A legacy queue of 1024 on CI.** #379's first CI run failed x86-64's
`test-smp2`: CI's QEMU (10, the Debian trixie container) gives the legacy
virtio-net 1024-entry queues where Homebrew's gives 256, and the
transport refused anything over 256, so eth0 was the e1000e on the other
network and the harness could not reach the guest. A legacy queue's size
is the device's to choose; the transport now takes it whole up to 1024,
virtio-net keeps at most 256 transmits in flight, and `test-smp2` asks
QEMU for 1024 explicitly so both QEMUs run the same.

**A mutex that let a waiter lose for ever.** #379's CI failed
`sched-migrate-stress` on aarch64 twice more (it had once on #376). The
failure line this PR added showed the second mutex worker on a ready
list, being run, and making no round in 2.3 s: its partner unlocked,
woke it, and locked again before it ran, every time, and the kernel's
mutex had nothing to stop that. A waiter that was woken and finds the
mutex taken again now asks for it, and the next unlock hands it over
instead of releasing it (`m->handoff`, S33). `struct mutex` grows, so the
module ABI is 10. `mutex-handoff` reproduces the overtaking
deterministically on one CPU (the owner yields while it holds the mutex,
so the woken waiter runs only when the mutex is taken): the waiter gets
the mutex after the owner's 2nd round, and with the old unlock only
after its 20th; `tools/mutex-handoff-probe.py --old` shows that on both
architectures. It is outside this unit's scope, but it failed this PR's
CI in three runs of four, and merging with it would have turned main
red, which is the case in which AGENTS.md has a defect found along the
way fixed rather than recorded.

## Parallels iteration log

| # | Seen | Cause | Fix |
|---|---|---|---|
| 1 | `random: cpu source: none`, pool never seeded | no RNDR, no virtio-rng, no `EFI_RNG_PROTOCOL` (`firmware: seed 0 bytes`) | the jitter source (owner's decision) |
| 2 | wall clock 1970 | no RTC in the tables | `GetTime` at boot, protocol v10 |
| 3 | a scratch BAR print hung the boot after PCI enumeration | the framebuffer is behind a BAR whose decoding `decode_bars` turns off while sizing it | the print removed; recorded in the inventory |
| 4 | `nvme0: ... one MSI vector for every queue`, `nvme0n1` 1 GiB | -- | one-vector mode |
| 5 | `virtio device 1 as virtio0 (3 MSI-X vectors; legacy, registers in memory)`, DHCP bound | -- | the legacy transport |
| 6 | `device wrote 354 bytes into a 0-byte chain` per transmit | legacy length on a read-only chain | ignored on legacy (spec 2.7.8.2) |
| 7 | `cosmo-install`: no ESP to copy | the live SATA disk is not visible | the live image on NVMe (guide) |
| 8 | installed on `nvme0n1`; the installed system boots (`root=boot-disk is nvme0n1p2`), seeds in 12 ms, DHCP, `wget http://example.com/`, dropbear makes its key and answers on port 22 | -- | -- |
