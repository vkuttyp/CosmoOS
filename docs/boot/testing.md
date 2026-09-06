# Boot: testing

The boot subsystem is tested end to end under QEMU, at link time by the
build, and at run time by the kernel's own checks. There is no unit-test
harness for the loader yet; the items under "Not yet tested" are the
plan.

## End-to-end boot test

```sh
make test                  # debug image
make BUILD=release test    # release image
```

`tests/boot/run_boot_test.py` proves the loader path by requiring these
serial-log markers, in a run that ends with QEMU exit value `0x10`:

| Marker | Proves |
|---|---|
| `^cosmoboot-uefi v\d+` | firmware found and started `BOOTX64.EFI`; console works |
| `^jumping to kernel entry` | file read, ELF accepted, page tables built, ExitBootServices succeeded, memory map translated |
| `^CosmoOS kernel ` | the jump landed at `_start`, the higher-half mapping is right, the stack is usable |
| `^Boot: UEFI` | `bootinfo_init()` accepted the structure (it panics otherwise) and `loader_name`/`version` arrived intact |
| `^\[ INFO\] boot complete` | the kernel ran to its end on the loader-provided state |

The loader also prints, between the first two markers, the kernel's
virtual and physical placement, the page-table pool usage, `NX on|unsupported`,
and the full translated memory map. These are not asserted but are in
`out/<arch>-<build>/boot-test.log` for inspection.

## In-kernel self-test `bootinfo` (`kernel/core/selftest.c`)

Runs in debug builds after the banner and checks properties
`bootinfo_init()` does not: the map has at least one entry; usable memory
is non-zero; `kernel_size` is non-zero; `kernel_virt_base` equals the
linker's `__kernel_start`; no two map entries overlap (O(n²) over the
merged map); and the entry containing `kernel_phys_base` is not typed
`USABLE` (it is `KERNEL`, or `LOADER_RECLAIMABLE` on firmware that
rejected loader types). A failure prints `SELFTEST: bootinfo ... FAIL:
<check>` and the harness fails the run.

## Panic-path test through the boot mappings

```sh
make test-crash
```

The `CRASH_TEST=1` kernel writes to `0xFFFF900000000000`, an address in
neither the HHDM (`0xFFFF8000...`, 4 GiB) nor the kernel PML4 slot. The
expected `#PF` with `CR2=ffff900000000000 (not-present write kernel)`
confirms that the bootstrap tables map exactly what `design.md` says and
nothing more, and that NX/WP did not interfere with the report. See
`docs/build/testing.md` for the full marker list.

## Link-time check

`scripts/check-kernel-elf.sh` (run by the kernel link rule) verifies the
three PT_LOAD segments are W^X and that PT_NOTE exists, which is exactly
what `elf_load()` will check again at boot. A regression is caught
without a QEMU run.

## Manual inspection

```sh
llvm-objdump -p out/x86_64-debug/kernel/kernel.elf      # segments and PT_NOTE
llvm-readelf -n out/x86_64-debug/kernel/kernel.elf      # COSMO note, build-id
make run                                                # watch the loader on the terminal
make run QEMU_EXTRA="-d int,cpu_reset -D out/qemu.log"  # trace faults before the IDT is up
```

A triple fault during the handoff shows in QEMU's `-d int` log as a
`#PF`/`#GP` followed by `#DF` and a reset; `-no-reboot` (set by
`qemu-run.sh`) makes QEMU exit instead of looping.

## Failure modes and what they mean

| Output | Meaning |
|---|---|
| firmware shell prompt, no `cosmoboot` banner | image lacks `/EFI/BOOT/BOOTX64.EFI`, or the firmware did not enumerate the disk |
| `cannot locate own loaded image` | LoadedImage protocol missing: not a UEFI environment the loader understands |
| `cannot read \cosmo\kernel.elf from the boot volume` | file missing from the image, or the boot volume has no SimpleFileSystem (booting from a medium the firmware has no FAT driver for) |
| `kernel ELF rejected` preceded by a `cosmoboot: ...` reason | malformed, wrong architecture, W+X segment, overlapping segments, entry outside segments, missing or mismatched protocol note |
| `cannot allocate page-table pool` / `bootinfo` / `handoff stack` / `memory map buffer` | firmware could not find pages below 4 GiB: less than a few MiB of low RAM, or a firmware that rejects `AllocateMaxAddress` |
| `page-table pool exhausted` | `paging_pool_size()` under-estimated: a loader bug, usually after changing the identity-map size or kernel layout |
| `GetMemoryMap size query failed` / `GetMemoryMap failed` | firmware fault; not recoverable |
| `ExitBootServices failed` | the map changed during both attempts; extremely rare, indicates firmware activity during boot |
| `memory map does not fit in bootinfo` | more than ~630 merged entries; raise `BOOTINFO_PAGES` |
| `warning: firmware rejected loader memory types` | not a failure; the map shows kernel/bootinfo/page tables as `loader`, and the explicit fields in `cosmoboot_info` are authoritative |
| `jumping to kernel entry` then silence | the jump faulted before the kernel's IDT was loaded: wrong CR3, NX without NXE, or an unmapped entry address; use `-d int,cpu_reset` |
| `bootinfo: ...` panic | the kernel rejected the structure; the message names the field |

## AArch64 exception level 2

`scripts/qemu-run.sh` passes `virtualization=on` unless `QEMU_EL2=0`, so
the default AArch64 run has firmware hand over at EL2 and the loader
keep it (`docs/kernel/arch/aarch64/design.md`, "Exception level 2").
Two markers are required in that configuration —
`cosmoboot: EL2 stub at 0x… (N bytes)` from the loader and
`el2: stub v1 at 0x…; EL2 available` from the kernel — so a change that
silently loses EL2 fails the boot test instead of passing quietly. The
`el2` self-test then asks the stub for its version, hands it a different
vector table and takes it back, and checks an unknown selector is
refused; `QEMU_EL2=0` exercises the other path, where the same test
asserts that there is no stub and that `el2_set_vectors` refuses. Both
configurations run four CPUs, which is what proves the secondary
trampoline's own drop from EL2 works: PSCI starts every AP at EL2 too.

## Not yet tested

- Host-side fuzzing of `elf_load()` and `parse_notes()` with malformed
  inputs. Both are pure functions of a byte buffer apart from the
  allocation call, so a harness that stubs `alloc_pages_low()` can run
  them natively; this is the highest-value next test for this subsystem.
- Firmware that rejects loader-defined memory types (the fallback path is
  implemented but only exercised by inspection; OVMF accepts them).
- Machines with more than 4 GiB of RAM (QEMU runs use 256 MiB). The
  loader is expected to work, with the memory above 4 GiB reported in
  the map but not mapped.
- Real hardware and Parallels. QEMU/OVMF is the defined test platform;
  a hardware boot is a later milestone and will be a manual test until a
  serial-capture rig exists.
