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
  0.4 s at that rate), not a looser estimate. Parallels needs 5 or 6
  batches (12-16 ms), its lowest estimate down from 0.67 to 0.27 bits per
  sample.
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
