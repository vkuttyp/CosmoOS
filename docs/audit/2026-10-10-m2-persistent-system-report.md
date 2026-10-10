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

### PR 1 in CI: a translation block across a page

PR #342's x86-64 job failed four runs in a row in the crash boot
(`make test-crash`): `syscall-fuzz` 8.3-11.0 s against its 8 s budget, and
twice the 180 s timeout, with every other boot of the same jobs normal.
Ruled out in turn: the runner (an ordinary debug boot right after the
crash boot in the same job ran at that runner's normal speed), the step's
position, and other load on the machine (a sampler in the step: idle
before, QEMU alone during). The CI-built crash kernel was slow on this
machine too, the CI-built debug kernel was not. vCPU PC sampling over QMP
put the difference in the framebuffer console: the scroll's per-pixel
loop straddled the page boundary at `0xffffffff80002000` in that binary
(same code, 64 bytes later than in the debug kernel), and a 12-row scroll
cost 118.8 ms instead of 10.9 ms. QEMU's TCG cannot chain a translation
block across a page, so the loop paid a lookup on every pixel. Fixed by
layout rule, not by budget: fbcon's three drawing loops are 256-byte
aligned functions of at most 256 bytes, and `check-kernel-elf.sh` fails
the link if one spans a page (`e0869ad8`). Full account in flakes.md
("syscall-fuzz", fourth sighting and attribution).

There is no `--old` probe for it: the defect is a placement that only one
compiler produced for one build, and the evidence for the old behaviour is
those two CI binaries (`debug-elfs-x86_64` of run 38037450806); the fix
removes the placement by construction and the link check holds it.

After the fix (`e0869ad8`) moved code, the local compiler put
`lockdep-graph-bench`'s quadratic edge loop across a page (8-10 s instead
of 3.7 s; the local `test-chaos` boot failed its 20 s budget and the
watchdog). The rule became `__page_local` in `compiler.h`, applied to that
loop, and the link check covers `page_local_*` too (`02ccce52`). Because
CI's compiler lays code out differently, every later tree was also built
with CI's toolchain (Debian clang 19.1.7 in `debian:trixie`, which
reproduced CI's crash kernel layout to the byte) and its debug, crash and
chaos images booted here.

PR 1 final validation (local, after `02ccce52`): host-test, fuzz,
analyze pass on both architectures; x86-64 debug 131.4 s (SMP 1), 143.9 s
(`test-smp2`), 149.7 s (SMP 4), chaos 142.7 s, retry 151.3 s, release
16.7 s, crash 122.7 s; AArch64 130.9 s, 142.2 s, 151.8 s, chaos 155.5 s,
retry 152.9 s, release 20.7 s, crash 129.8 s -- all PASS. CI-toolchain
x86-64 builds: debug 142.5 s, crash 128.1 s, chaos 146.5 s, PASS. PR #342
CI green on both architectures; merged as `765bd579`.

Two further CI sightings on the branch were recorded, not attributed to
it: `irqpoll-boost` over its gap bound (x86-64 `test-smp2`, first
sighting) and, on PR #343, `net-neigh-down-race` step 2 (a candidate
mechanism in the test's park hook; inventory).
## PR 2 (as built)

| Area | Change | Files |
|---|---|---|
| `/dev/blkctl` | LIST (partitions, the `BOOT` partition, `MOUNTED`), READ/WRITE of up to 32 KiB (a WRITE refused while the device, its disk or a partition of it is mounted, V36), FLUSH, RESCAN, FORMAT through a new `fs_type.format` (cosmofs: `cosmofs_format`); privileged at open and at every write; results read back whole | `kernel-services/vfs/blkctl.c`, `kernel/include/uapi/cosmo/blkctl.h`, `kernel-services/vfs/vfs.c` (`vfs_bdev_mounted`, `vfs_format`) |
| Randomness for user space | native `COSMO_AT_RANDOM` (25) naming the 16 bytes already placed on every initial stack | `kernel/process/process.c`, `uapi/cosmo/syscall.h` |
| Installer | `/sbin/cosmo-install` (design: `docs/userland/design.md`, "The installer"): GPT from `gpt_build`, checked with `gpt_parse`, backup then primary; the boot ESP copied with its one slot rewritten; cosmofs on partition 2 with `/bin`, `/sbin`, `/etc`, `/usr` and `/var/db` when present; refusal exit 3; failure wipes the table and unmounts | `userland/system/cosmo-install.c`, `install_gpt.c`, `install_sha512.c` |
| Acceptance (stage 1) | `make test-install`: install from the live release image onto a blank 256 MiB disk, refusals (a table without `--force`, a mounted disk), the new root mounted and listed; on the host, both GPT copies, the ESP equal to the booted ESP but for the slot, the slot's `root=PARTUUID=`; CI job `install` | `tests/boot/install_test.py`, `Makefile`, `.github/workflows/ci.yml` |

### Two stack overflows on the installer's path

The installer's first runs double-faulted twice, each a 16 KiB array on a
16 KiB kernel stack:

1. **`format_at`** kept `struct cfs_member mem[255]` (64 bytes each) on
   the stack. Only self-tests had formatted a cosmofs; a FORMAT from a
   system call double-faulted (`format_at`, `cosmofs_core.c:1836`). The
   path is new in this PR (no system call reached `cosmofs_format` on
   main), so `make test-install` is its test. Fixed by allocating the
   `n` members (`eb760d7d`).
2. **`lz4_compress`** kept its 4096-entry match table (16 KiB) on the
   stack. cosmofs compresses every multi-block record of a regular file
   at write-back, so on main a user process that writes such a file to a
   cosmofs mount and calls `fsync` or `sync` overflows the stack. A
   defect on main: `init --selftest` now writes 64 KiB of a repeated line
   to the scratch cosmofs and `fsync`s it, and the harness requires its
   line in every debug boot. The table is now `LZ4_WORK_BYTES` the caller
   provides; cosmofs allocates it per record (`5cdcb682`).
   `tools/lz4-stack-probe.py --old` reverts that whole commit in a
   throwaway worktree and boots debug:

   | | x86-64 | AArch64 |
   |---|---|---|
   | `--old` | FAIL 108.9 s: `#DF double fault` in `init`, PC `lz4_compress` (`lz4.c:68`, the table's `memset`, RDX=0x4000) | FAIL 109.2 s: kernel write fault at `0xffffc000103f4000`, the guard page below `init`'s kernel stack, X2=0x4000 (the `memset`'s length); AArch64 has no separate fault stack, so the report is the guard page, taken in the exception entry |
   | fixed | PASS 155.1 s | PASS 144.6 s |

   The first version of the probe read the PC from the first `#0` frame
   in the log, which on x86-64 was a lockup sample's, and reported the
   expectation failed; it now reads the panic report.

The shell expands `$?` for a whole line before running it (`cmd; echo
$?` reports the line before); the harness types the status check as its
own line. x86 firmware without a variable store writes `\NvVars` into
the ESP it boots from, so the installed ESP is compared with the live
disk's ESP after its boot, not with the built image (34 sectors differ
between those two).

### PR 2 validation (local)

| Item | x86-64 | AArch64 |
|---|---|---|
| `host-test` | pass | pass |
| `fuzz` | pass | pass |
| `analyze` | clean | clean |
| debug `test`, `QEMU_SMP=1` | PASS 139.4 s | PASS 131.7 s |
| debug `test-smp2` | first boot hung in OVMF before the loader (no `BdsDxe:` line, 240 s; flakes.md, second sighting of the firmware-handover stop); rerun PASS 143.5 s | PASS 155.6 s |
| debug `test`, `QEMU_SMP=4` | PASS 150.4 s | PASS 142.8 s |
| `test-chaos` | PASS 142.0 s | PASS 143.9 s |
| `test-harness-retry` | PASS 143.8 s | PASS 144.9 s |
| `BUILD=release test` | PASS 16.9 s | PASS 20.4 s |
| `BUILD=release test-install` (install stage) | PASS 7.6 s (`cosmo-install` 1.8 s) | PASS 11.8 s (1.6 s) |

Every debug boot now prints `usertest: cosmofs compressed a file
committed from user mode`, which the harness requires.

PR 2 after rebasing onto PR 1's final head (local): x86-64 debug `test`
PASS 145.1 s, `test-crash` PASS 129.6 s, release `test-install` PASS
7.9 s; AArch64 154.8 s, 130.8 s, 11.3 s. CI-toolchain x86-64 builds:
debug 143.3 s, crash 127.9 s, chaos 141.1 s, PASS.
## PR 3 (as built)

| Area | Change | Files |
|---|---|---|
| Switch root | `vfs_switch_root`: three passes (decide under the table lock; look up the target directories without it; re-validate and commit, which cannot fail); top-level mounts of the old root (`/proc`, `/dev`) move to the same names; the new root leaves its mountpoint and its namespace references; `vfs_release_old_root` releases the live ramfs when nothing references it (V37) | `kernel-services/vfs/vfs.c` |
| System call | `SYS_switch_root` (102): init alone, privileged, unconfined, the only process; init's directory moves to `/` before the old root is released; libc `cosmo_switch_root` | `kernel/syscall/native.c`, `kernel/process/process.c` (`process_is_init`), `uapi/cosmo/syscall.h`, `libc/include/cosmo/syscall.h` |
| Boot composition | `/dev` a ramfs mount of its own; anonymous files (memfd, shm) on a detached ramfs (`vfs_mount_internal`, `ramfs_set_anon_mount`) instead of the root | `kernel/core/bootfs.c`, `kernel-services/vfs/ramfs.c` |
| init | `root=` → mount `kernel.rootdev` at `/sysroot`, switch, ramfs on `/tmp`; any failure says `staying on the live root` and the boot goes on live; `sync` before exiting on a disk root | `userland/init/init.c` |
| Userland | `/bin/sync` (there was no way to commit from the shell) | `userland/coreutils/sync.c` |
| Harness | `QEMU_DISKS=boot`: the installed disk alone, writable on both machines; `make test-install` stages reboot, persist, fallback; CI step renamed | `scripts/qemu-run.sh`, `tests/boot/install_test.py`, `.github/workflows/ci.yml` |

## Disk layout and boot flow, as built

```text
installed disk (GPT, 512-byte sectors; 256 MiB in the test)
  LBA 0            protective MBR
  LBA 1..33        primary header + 128 entries
  LBA 2048..       partition 1  EFI System (c12a7328-...), FAT32, a byte copy of the
                   boot ESP: \EFI\BOOT\BOOT{X64,AA64}.EFI, \cosmo\kernel.elf,
                   \cosmo\boot.tar, \cosmo\cmdline = "#cosmo-cmdline v1\nroot=PARTUUID=<p2>\n"
  next MiB..last   partition 2  CosmoOS root (c9b09224-e00d-418e-846a-9f1d9a61bfd5),
                   cosmofs: /bin /sbin /etc (/usr /var/db) /dev /proc /tmp /mnt /var /boot
  last 33 sectors  backup entries + backup header
```

Boot: firmware → loader reads `\cosmo\cmdline` and its own partition's
GUID (protocol v7) → kernel boots on the live ramfs root as always,
scans partitions, resolves `root=PARTUUID=` to the partition (`vda2` on
virt, `ahci0p0p2` on q35) → init mounts it at `/sysroot`,
`switch_root("/sysroot")` (`/proc` and `/dev` move, the live ramfs is
released), mounts a ramfs on `/tmp`, runs the installed `/etc/rc` and the
shell → at the shell's exit init `sync`s. Without `root=` the boot is the
live boot; with a `root=` that cannot be mounted, init says so and stays
live.

## Acceptance (`make BUILD=release test-install`, local, QEMU 11.1.1, TCG)

| Stage | x86-64 | AArch64 |
|---|---|---|
| install (live image + blank 256 MiB disk; `cosmo-install`) | PASS 8.0 s (`cosmo-install` 1.8 s) | PASS 11.2 s (1.7 s) |
| host check of the disk | GPT valid, ESP = booted ESP but the slot, slot names partition 2 | same |
| reboot (installed disk alone), write `/persist.txt`, power off | PASS 5.0 s, root `ahci0p0p2` | PASS 8.5 s, root `vda2` |
| persist (boot again, read it back) | PASS 4.6 s | PASS 8.1 s |
| fallback (`root=vda`, a blank disk) | PASS 5.0 s: `cannot mount vda ... staying on the live root`, live shell | PASS 9.2 s |
| whole test | PASS 23.1 s | PASS 37.2 s |

### PR 3 validation (local)

| Item | x86-64 | AArch64 |
|---|---|---|
| `host-test` | pass | pass |
| `fuzz` | pass | pass |
| `analyze` | first run 3 unexpected in `vfs_switch_root` (a NULL root, a NULL mountpoint, the move table's `dir` not visibly initialised); stated explicitly, rerun clean | clean (with the fix) |
| debug `test`, `QEMU_SMP=1` | PASS 135.6 s | PASS 143.6 s |
| debug `test-smp2` | PASS 153.1 s | PASS 145.2 s |
| debug `test`, `QEMU_SMP=4` | PASS 150.4 s; after the analyzer fix PASS 143.9 s | PASS 168.5 s |
| `test-chaos` | PASS 149.1 s | PASS 143.7 s |
| `test-harness-retry` | PASS 143.8 s | PASS 153.9 s |
| `BUILD=release test` | PASS 17.8 s | PASS 20.3 s |
| `BUILD=release test-install` (all four stages) | PASS 23.7 s (again after the fix: 23.7 s) | PASS 37.0 s |

The analyzer fix landed while the AArch64 half of the matrix was running,
so every AArch64 item ran with it; the x86-64 items that ran before it
(analyze, a debug boot, test-install) were rerun after it.

PR 3 after rebasing onto PR 2's final head (local): x86-64 debug `test`
PASS 142.0 s, `test-crash` PASS 122.9 s, release `test-install` PASS
23.3 s (install 8.0, reboot 5.3, persist 4.6, fallback 5.0); AArch64
144.5 s, 125.6 s, 36.4 s (11.1, 8.4, 8.1, 8.6). CI-toolchain x86-64
builds: debug 152.6 s, crash 128.9 s, chaos 142.6 s, PASS.
