# Roadmap M2: persistent system — 2026-10-10

Milestone M2 of the [1.0 roadmap](../roadmap-1.0.md): install to a blank
disk image, reboot from it, write a file, reboot, read it, on both
architectures, in CI. Base: main `88e18272` (M1 done, CI images through
ECR, CI green). Decisions 1-6 of the task are the owner's and were not
reopened; this report records how each was built.

## Plan as worked

| PR | Content |
|---|---|
| 1 | GPT partitions as block devices; boot protocol v7 (command line, boot partition); `root=` resolution; filesystem registration and the root mount moved out of `vfs_init` into boot composition. The boot image becomes a GPT disk. |
| 2 | `/dev/blkctl` (raw sectors, rescan, format), native `COSMO_AT_RANDOM`, `/sbin/cosmo-install`; `make test-install`'s install stage and its CI job. |
| 3 | `SYS_switch_root`, `/dev` and anonymous files off the live root, init's disk root; the reboot, persist and fallback stages; M2 ticked. |

The task's plan put switch-root (with a disk made by a host tool) before
the installer. That order needed a host-side cosmofs writer used only
until the installer existed; the task allowed the installer to make the
disk instead, so the installer came first and the boot from it moved to
PR 3. Three PRs, within the limit of five.

**The ESP source (decision 1, "say which").** Read raw from the boot
device. The boot image is now itself a GPT disk whose partition 1 is the
64 MiB FAT32 ESP (`scripts/mkgpt.py` around the mtools image); the loader
reports the GUID of the partition it was read from (protocol v7), the
kernel registers that partition like any other, and the installer copies
it through `/dev/blkctl`. Shipping an ESP image in the boot archive was
rejected: the archive stays in memory and `ramfs_populate_boot` copies
it, so a ~13 MiB image (FAT16, which also departs from the ESP's FAT32)
costs ~26 MiB of a 256 MiB guest on every boot, and the archive inside
the ESP inside the archive needs a two-stage build whose installed
`boot.tar` differs from the live one. Reading the boot partition gives an
installed ESP byte-identical to the live one but for one sector, and an
installed system can install again.

## PR 1 (as built)

| Area | Change | Files |
|---|---|---|
| GPT | `gpt_parse`: protective MBR, both headers (signature, revision 1.x, size, CRC, `MyLBA`, entry array outside the usable range), backup on the last sector pointing at the primary and agreeing, both entry arrays' CRC, entries inside the usable range, no overlap, distinct non-zero unique GUIDs, ≤ 32 used; `gpt_build` (the writer the tests and the installer share); CRC-32; GUID text | `kernel/block/gpt.c`, `kernel/include/kernel/gpt.h` |
| Partitions | each entry a `struct blkdev` named by entry number (`vda1`, `nvme0n1p1`, `ahci0p0p1`), forwarding bios at its offset; bounds by the block layer against the partition's capacity and again before translating (D16); a disk's partitions removed first by `blk_unregister`, the last reference dropped from a thread when bios are still in flight (D17); rescans refused while a partition is held; scan at boot after `module_load_boot` and on request | `kernel/block/part.c`, `kernel/include/kernel/part.h`, `kernel/block/blk.c` |
| Boot protocol v7 | `cmdline_phys`/`cmdline_size` (spending `reserved2`; text in a sixth bootinfo page), `boot_flags`, `boot_partuuid` from the loader's device-path hard-drive node; kernel validates and copies (BT14) | `boot/protocol/cosmoboot.h`, `boot/uefi/{main.c,efi.h,loader.h}`, `kernel/core/bootinfo.c` |
| Boot image | GPT disk, ESP partition 1 from LBA 2048, `\cosmo\cmdline` one-sector slot (`#cosmo-cmdline v1` and NULs), fixed FAT serial, GUIDs from the content | `scripts/mkimage.sh`, `scripts/mkgpt.py` |
| Command line | tokens, `#` comments, first key wins, values never truncated | `kernel/core/cmdline.c` |
| Boot composition | `vfs_init` is the VFS alone; `bootfs_init` registers ramfs, procfs, cosmofs, mounts the ramfs root (`vfs_mount_root`), populates it, mounts `/proc`; `bootfs_disks_ready` scans partitions and resolves `root=PARTUUID=` / `root=<device>`; sysctls `kernel.cmdline`, `kernel.root`, `kernel.rootdev` | `kernel/core/bootfs.c`, `kernel-services/vfs/vfs.c`, `kernel/core/main.c`, `kernel/syscall/native.c` |
| Tests | `blk-gpt` self-test; `tests/host/test_gpt.c`; `fuzz_gpt`; harness requires the GPT boot volume, the boot ESP partition, the empty command line and the live-root line | `kernel/block/parttest.c`, `tests/` |

The module ABI is unchanged: no exported symbol and no structure modules
see changed (`struct blkdev` is untouched; partition state is in a
wrapper).

### PR 1 validation (local, macOS host, QEMU 11.1.1, TCG)

| Item | x86-64 | AArch64 |
|---|---|---|
| `host-test` (with `test_gpt`) | pass | pass |
| `fuzz` (with `fuzz_gpt`; also 200 000 runs of it alone) | pass | pass |
| `analyze` | clean (0 unexpected) | clean (0 unexpected) |
| debug `test`, `QEMU_SMP=1` | PASS 135.2 s | PASS 145.5 s |
| debug `test-smp2` | PASS 146.3 s | PASS 156.5 s |
| debug `test`, `QEMU_SMP=4` | PASS 153.5 s | PASS 155.4 s |
| `test-chaos` | PASS 144.2 s | PASS 155.1 s |
| `test-harness-retry` | PASS 150.7 s | PASS 158.0 s |
| `BUILD=release test` | PASS 16.9 s | PASS 20.7 s |

The network harness passes in every debug boot (part of the verdict).
Every boot shows `boot volume: a GPT partition`, the boot ESP registered
(`ahci0p0p1` on q35, `vdb1` on virt) and `root: no root= on the command
line; the live root stays`. The first `analyze` run failed the gate on
`parttest.c` leaking its buffers on an early `CHECK` return; the test was
restructured (`fb9589a7`) and the matrix rerun from the start.

Probe convention: M2 is a new feature, proved by its acceptance test
(`make test-install`, PRs 2 and 3); PR 1's own claims are proved by
`blk-gpt` (every damaged copy refused by its own check, bounds and
translation), the host test and the fuzzer.
