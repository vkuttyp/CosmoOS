# Booting with any amount of RAM: the sized direct map — 2026-10-18

The project owner's task of 2026-10-18 ("Boot with any amount of RAM"):
Parallels Desktop at its default 4 GB hangs right after the handover on
v1.0.3 and v1.0.4 (inventory). Base: main `9c7ad9d8` (#402's merge;
v1.0.4 is `1ec37307`). Branch `direct-map`.

## The defect

The loader built its bootstrap tables over a fixed 4 GiB
(`BOOT_HHDM_SIZE`), on the reasoning that it allocates everything below
4 GiB itself (`LOADER_ALLOC_LIMIT`). It does not choose where the
firmware loads *it*. With 4 GiB of RAM or more the firmware puts the
loader image above 4 GiB (EDK2 allocates top-down):

| Machine (4 GiB of RAM) | Loader code at switch | Stack |
|---|---|---|
| QEMU virt (AArch64) | `0x13cb501d8` | `0x47686838` |
| QEMU q35 (x86-64) | `0x1400057f0` | `0x7fe55890` |

(from `tools/direct-map-probe.py --defect loader --old`, below). On
AArch64, `cpu_jump_to_kernel` writes TTBR0 with the identity map of 0-4
GiB and keeps executing from its own image; the next instruction fetch
has no translation and the CPU takes an exception with no usable vector.
Nothing follows `jumping to kernel entry`.

Reproduced on this Mac with QEMU under `-accel hvf` (QEMU_EL2=0,
QEMU_GIC=3, cortex `host`, 4G): v1.0.4-equivalent `e0e11424` prints the
loader's last line and nothing in 45 s, as Parallels does. Under TCG the
same image enters the kernel: TCG had the whole switch sequence
translated already, so the old binary alone shows nothing in CI. Hence
the loader check below, which turns the condition into a message TCG
shows.

Once the kernel ran with 4 GiB or more, a second defect followed: a
block device with no hardware behind it (a RAM disk, the self-tests'
fakes) was checked with `dma_mappable(NULL, ...)`, whose default mask is
32 bits. `kmalloc` hands out RAM above 4 GiB on such a machine, every
bio was `-EINVAL`, and `blk-submit-unregister`'s submitter never landed
one: the boot hung at the watchdog (`[WATCHDOG] no progress for 8001
ms`, `blkrace` running, on both architectures). `selftest_dma` had the
same assumption (a `kmalloc` buffer must map for the NULL device). The
inventory lists `blk-submit-unregister` among the self-tests that failed
on Parallels; this is a candidate cause, not verified there.

## Design (as built)

`docs/boot/design.md`, "The direct map"; invariant BT6 restated.

- `direct_map_size()` (`boot/uefi/main.c`) takes the highest end of:
  every RAM descriptor of the memory map (all but MMIO, MMIO port space,
  reserved, unusable), the loader image, the firmware stack, the kernel,
  archive, boot info, handoff stack and map snapshot, the RSDP and the
  tables `acpi_scan` read (`acpi_tables_end()`), the framebuffer and a
  memory-mapped serial console's page. Rounded up to 1 GiB; printed with
  what decided it.
- The limit is `paging_pa_limit()` (x86-64: 2^MAXPHYADDR from CPUID
  0x80000008; AArch64: 2^PARange, at most 48 bits) and `BOOT_HHDM_MAX`
  (64 TiB, where the kernel's arena begins). Past it, or with a pool the
  firmware cannot give, the loader prints why and `die`s before
  `ExitBootServices`.
- The tables are built after the framebuffer and console are known (main.c
  steps 6-7 reordered). x86-64 uses one PML4 entry per 512 GiB, shared by
  the identity map and the HHDM; AArch64 classifies a 2 MiB block by the
  RAM descriptors' overlap with it instead of 512 per-page lookups.
- After building, the loader checks that `cpu_jump_to_kernel` and its
  stack are below `hhdm_size`, and stops with `the loader would lose its
  own code at the table switch` otherwise.
- No boot-protocol change: `hhdm_size` was always the loader's to set and
  the kernel already accepted any value (frames above it deferred until
  `vmm_init`). A v1.0.4 loader with a new kernel, and a new loader with a
  v1.0.4 kernel, both boot as before; `cosmo-upgrade` may switch either
  first. The kernel's comments that said 4 GiB now name `hhdm_size`.
- `blk.c` `buf_ok`: a blkdev with no `struct device` needs only
  direct-map memory; every hardware blkdev (nvme, virtio-blk, ahci,
  usb-storage, partitions) sets `bd.dev`. `selftest_dma` maps its
  `kmalloc` buffer for the 64-bit test device.
- The SPCR console's page lies inside the sized map wherever it is, so
  the inventory line about a console above the 4 GiB map is closed.

## Changes

| Commit | Change |
|---|---|
| `a95f9c6c` | the sized direct map, its docs, `test-platforms` on both architectures with `mem1g`, `mem4g`, `mem8g`, CI's `platforms` job a matrix over both |
| `f7591b72` | `blk.c` `buf_ok`; `selftest_dma` on the 64-bit device |
| `00db045b` | `tools/direct-map-probe.py` |
| `3d01865f` | inventory |

## Acceptance

`make test-platforms` on both architectures: each memory variant boots
the debug image through the self-tests to the shell, requires the
loader's `direct map:` line to cover what it names, and `free`'s total
to be at least 95% of the RAM.

| | 1 GiB | 4 GiB | 8 GiB |
|---|---|---|---|
| AArch64 (virt, highmem on) | 2 GiB map, `free` 1006416 KiB | 5 GiB, 4127568 KiB | 9 GiB, 8289104 KiB |
| x86-64 (q35) | 3 GiB (to the end of the framebuffer at `0x803e8000`), 1016928 KiB | 6 GiB, 4121696 KiB | 10 GiB, 8283232 KiB |

x86-64 at 1 GiB is the "framebuffer placed high" case: q35's
framebuffer sits at 2 GiB, above all RAM, and the map reaches it. Nothing
in QEMU forces the ACPI tables above RAM; they lie at the top of RAM on
both machines.

Under hvf, the fixed image runs the kernel at 4G and 8G (`direct map:
0x0-0x140000000 (5 GiB)`, `0x0-0x240000000 (9 GiB)`) until the platform
line, where QEMU's hvf stops on an MMIO access without ISV
(`hvf_handle_exception`), a limitation of hvf, not of the kernel
(inventory).

## Proof

`tools/direct-map-probe.py --defect loader|blk [--old] [--arch A]`
builds the debug image in a throwaway worktree and boots `mem4g`.

- `loader --old` restores the fixed 4 GiB size and keeps the switch
  check (the whole old commit would hang only on hardware): AArch64 and
  x86-64 both stop with `the loader runs at 0x13cb501d8` /
  `0x1400057f0 ... outside the 0x100000000 its tables map`.
- `blk --old` reverse-applies `f7591b72`: both architectures hang at
  the watchdog in `blk-submit-unregister`.
- fixed: `mem4g` passes on both.

## Validation

The AGENTS.md matrix on `3d01865f`, both architectures, plus
`BUILD=release test-platforms`: host-test, fuzz, analyze, debug `test` at
`QEMU_SMP` 1, 2 and 4, test-smp2, test-chaos, test-harness-retry, the
release boot and test-platforms (eleven AArch64 variants, three x86-64)
all PASS by the harness's verdict, with one exception: the x86-64 release
boot failed once with the firmware stall (both attempts, no loader
banner; `docs/testing/flakes.md`, ninth sighting) and passed on rerun
(18.9 s).

## Parallels Desktop

The owner's VM "CosmoOS 1.0.4" (Parallels Desktop 27.0.3, 4 CPUs), this
branch's AArch64 release image (`389f173b`) copied onto its live NVMe
disk:

- 4096 MB, control first: the installed v1.0.4 prints `jumping to kernel
  entry 0xffffffff80000000, info at 0xffff8000fed33000` and nothing in 60
  s.
- 4096 MB, this image: the shell; `free` 4133256 KiB total; the kernel's
  summary `Memory: 4044 MiB usable in 29 regions, RAM ends at 5120 MiB`.
- 8192 MB, this image: the shell; `free` 8294792 KiB total; `ip a`
  shows the DHCP address 10.211.55.12.

The VM was put back as it was (the published live image, 2048 MB, the
installed disk first) for v1.0.5's in-place upgrade.

## Remaining

New inventory lines: no bounce buffers for 32-bit-DMA block
devices; hvf's ISV stop; the loader allocates only below 4 GiB; the
FADT's tables are outside `direct_map_size`.
