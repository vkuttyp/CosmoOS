# Boot: design

## The protocol structure

`struct cosmoboot_info` (`boot/protocol/cosmoboot.h`, version 9). All
fields are fixed-width integers; there are no pointers, enums, or
padding surprises, so the layout is identical on every architecture and
compiler.

| Field | Meaning |
|---|---|
| `magic` | `COSMOBOOT_MAGIC` = `0x3154424F4D534F43` ("COSMOBT1") |
| `version` | `COSMOBOOT_VERSION` = 10 (1: memory map, HHDM, kernel placement, page tables, RSDP; 2: added one raw boot module; 3: the module became the boot archive; 4: a second bootstrap table root for architectures with split roots; 5: the AArch64 EL2 stub; 6: the framebuffer; 7: the command line and the boot partition; 8: verified boot; 9: the serial console the firmware names; 10: the firmware's seed and time) |
| `size` | `sizeof(struct cosmoboot_info)` as written by the loader; lets a newer kernel detect an older loader |
| `arch` | `COSMOBOOT_ARCH_X86_64` = 1, `COSMOBOOT_ARCH_AARCH64` = 2 |
| `firmware` | `COSMOBOOT_FIRMWARE_UEFI` = 1 |
| `loader_name[32]`, `loader_version` | for the banner: `cosmoboot-uefi`, 1 |
| `hhdm_base`, `hhdm_size` | direct map: virtual `hhdm_base + p` == physical `p` for `p < hhdm_size` |
| `kernel_phys_base`, `kernel_virt_base`, `kernel_size` | where the image landed; `kernel_virt_base` is the lowest PT_LOAD vaddr |
| `boot_pagetable_root` | physical root of the bootstrap tables the kernel is running on: CR3 on x86-64, the TTBR1 (higher-half) table on AArch64 |
| `mem_map_phys`, `mem_map_entries`, `mem_map_entry_size` | array of `struct cosmoboot_mem_entry { base, length, type, reserved }` |
| `acpi_rsdp` | physical RSDP from the EFI configuration table (ACPI 2.0 preferred, 1.0 fallback), 0 if absent |
| `firmware_system_table` | physical `EFI_SYSTEM_TABLE *` for future runtime-services use |
| `archive_phys`, `archive_size` | (v3) the boot archive `\cosmo\boot.tar`, a ustar archive holding `init` and the boot-time kernel modules (`modules/<name>.ko`) and test fixtures (`tests/*.ko`), in `COSMOBOOT_MEM_ARCHIVE` memory below 4 GiB; both zero when the file is absent. Written by `scripts/mkbootarchive.py`, parsed by `kernel/core/bootarchive.c` |
| `boot_pagetable_root_user` | (v4) the second bootstrap root for architectures with split roots: the TTBR0 identity table on AArch64, 0 on x86-64. The kernel does not read it today; its pages are `COSMOBOOT_MEM_BOOT_PAGETABLES` and go with the rest at takeover |
| `el2_stub_phys` | (v5, AArch64) the page holding the EL2 stub the loader installed before dropping to EL1, in memory of type `COSMOBOOT_MEM_EL2_STUB` (never freed). 0 when firmware handed over at EL1, when the machine has no EL2, or when the page could not be reserved — the kernel then reports no EL2 and boots exactly as before (`docs/kernel/arch/aarch64/design.md`, "Exception level 2") |
| `fb_phys`, `fb_size`, `fb_width`, `fb_height`, `fb_pitch`, `fb_bpp`, and three (shift, bits) pairs | (v6) the Graphics Output Protocol's mode when the loader exits boot services: the firmware's own, unless the loader set the one the display's EDID prefers (`boot/uefi/display.c`; "The display mode" below). All zero when there is none, which is not an error. The pixel format is three (shift, bits) pairs rather than an enumeration, so a bit-mask format is described exactly like the two common ones. The range is not RAM: it is `EfiMemoryMappedIO` on a PCI display and `EfiReservedMemoryType` for a `ramfb`, and either way the PMM never hands it out |
| `cmdline_phys`, `cmdline_size` | (v7, spending `reserved2`) the text of `\cosmo\cmdline` up to its first NUL, NUL terminated, at most `COSMOBOOT_CMDLINE_MAX` (1023) bytes, in the last of the bootinfo pages (`COSMOBOOT_MEM_BOOTINFO`); both zero when the file is absent or empty. See "The command line" |
| `verify_flags`, `verify_key_id[8]` | (v8) `COSMOBOOT_VERIFIED` when the loader checked `kernel.elf`, `boot.tar` and `cmdline` against `\cosmo\manifest` under a signature by a key compiled into it, with that key's id (first 8 bytes of SHA-512 of the public key, the module ring's naming); both zero from a `VERIFY_BOOT=0` loader. See "Verified boot" |
| `boot_flags`, `boot_partuuid[16]` | (v7) `COSMOBOOT_BOOT_PARTUUID` set when the loader was read from a GPT partition: `boot_partuuid` is that partition's unique GUID in on-disk byte order, from the hard-drive node of the loader's device path. Clear for a volume with no partition table. See "The boot disk" |
| `console_type`, `console_gsiv`, `console_base`, `console_access`, `console_flags`, `console_stride` | (v9) the serial console the firmware's ACPI tables name, found by the loader (`boot/uefi/acpi.c`): `console_type` is `COSMOBOOT_CONSOLE_NONE` 0, `PL011` 1 (PL011 or SBSA generic UART, MMIO), `16550_MMIO` 2 or `16550_PORT` 3; `console_base` the register block's physical address (an I/O port for `16550_PORT`); `console_access` the width of one register access in bytes and `console_stride` the spacing of the registers in bytes, each 0 when the table leaves it undefined (below); `console_gsiv` the interrupt, 0 for a polled port; `console_flags` `COSMOBOOT_CONSOLE_FROM_DBG2` when the port came from the DBG2 rather than the SPCR. All zero when no table names a port the kernel drives. See "The serial console" |
| `fw_flags`, `fw_seed_len`, `fw_epoch`, `fw_seed[64]` | (v10) what the firmware offers before `ExitBootServices`: `fw_seed_len` bytes (64, or 0) from `EFI_RNG_PROTOCOL`'s default algorithm, which `bootinfo_init` copies and wipes in place; `fw_epoch` the time from `GetTime` in seconds since 1970 UTC, with `COSMOBOOT_FW_TIME` in `fw_flags`. All zero when the firmware offers neither. See "Firmware seed and time" |

Memory types (`COSMOBOOT_MEM_*`): `USABLE` 1, `RESERVED` 2,
`ACPI_RECLAIMABLE` 3, `ACPI_NVS` 4, `BAD` 5, `LOADER_RECLAIMABLE` 6,
`KERNEL` 7, `BOOTINFO` 8, `BOOT_PAGETABLES` 9, `FIRMWARE_RUNTIME` 10,
`MMIO` 11, `PERSISTENT` 12, `ARCHIVE` 13 (v3; the same value was
`MODULE` in v2). The PMM frees only `USABLE` and `LOADER_RECLAIMABLE`
ranges (`kernel/memory/bootmem.c`), so `ARCHIVE` memory, like `KERNEL`,
stays reserved for the life of the kernel: `init` is copied out of it
into the process's address space and modules are copied out of it into
their own regions, but the archive itself is never freed or written.

The ELF note: name `"COSMO\0"` (namesz 6), type `COSMOBOOT_NOTE_TYPE` = 1,
desc = `uint32_t COSMOBOOT_VERSION`. `entry.S` emits it into
`.note.cosmoboot`, `linker.ld` places that section in both the rodata
PT_LOAD and the PT_NOTE. The loader refuses a kernel without the note or
with a different version, so a protocol change can never produce a silent
mis-boot.

## The command line

`\cosmo\cmdline` on the boot volume (protocol v7). Optional: without it
the kernel boots exactly as before. The loader reads the file, keeps the
bytes up to the first NUL (at most `COSMOBOOT_CMDLINE_MAX`, truncating
with a line on the loader console), and copies them with a NUL into the
sixth bootinfo page, which exists for this; it neither parses nor
validates the text. The kernel's `bootinfo_init` checks the two fields
(size within the limit, the range inside the direct map, the NUL where
the size says) and copies the text into kernel memory (BT14).

Syntax (`kernel/core/cmdline.c`): tokens separated by white space, each
`key=value` or a bare `key`; `#` starts a comment to the end of the line;
the first token with a key wins; unknown keys are ignored. The kernel
logs the line once (`cmdline: ...`, line breaks shown as spaces) and
user space reads it as the sysctl `kernel.cmdline`.

Keys the kernel reads:

| Key | Meaning |
|---|---|
| `root=PARTUUID=<guid>` | the root filesystem is on the GPT partition with that unique GUID (any case) |
| `root=<device>` | the root filesystem is on the named block device (`vda2`); for development, since names depend on probe order |
| `random.firmware=off` | the kernel neither mixes nor credits the firmware's seed (boot protocol v10): for a machine whose firmware is not trusted to generate, and for the entropy boot that must be seeded by its virtio-rng alone (`make test-entropy`) |
| `random.jitter=off` | no timing-jitter collection (docs/kernel/security/design.md §6, "Timing jitter"): on a machine with no other source the pool then stays unseeded, which is what the no-source entropy boot tests |
| `serial=none` | AArch64: no serial console even when the firmware names one (boot protocol v9, "The serial console"): the framebuffer is the console from the first line. Read by the early console before `bootinfo_init`, with the same bounds. For testing the path a machine without a serial port takes (`make test-platforms`) |

The kernel only resolves `root=` (after its partition scan, `root:` log
line; sysctls `kernel.root` and `kernel.rootdev`); it always starts on
the live ramfs root, and init mounts the named device and switches to it
(`docs/kernel-services/vfs/design.md`, "Boot composition").

**The file.** `make image` writes `\cosmo\cmdline` from `CMDLINE`
(default `root=boot-disk`) and signs it with the kernel and the archive
(below), so the command line is fixed when the image is built; changing
it means signing again. Until roadmap M5 the file was a 512-byte slot the
installer rewrote in place with `root=PARTUUID=`; a signed command line
cannot be rewritten, so the installer now writes nothing on the ESP.

## Verified boot

Roadmap M5. The loader boots nothing it has not checked. `make image`
writes `\cosmo\manifest` (`scripts/mkmanifest.py`, layout in
`boot/protocol/cosmomanifest.h`): magic `COSMOMAN`, version 1, and for
`kernel.elf`, `boot.tar` and `cmdline`, in that order, the name, the size
and the SHA-512 of the file; then the module-signing trailer
(`scripts/modsign.py`: Ed25519 signature, key id, `COSMOSIG`) over those
280 bytes, signed with `MODSIGN_KEY` -- the per-machine developer key, or
the release key with `SIGNING=release`. The signature is deterministic,
so the manifest is reproducible. The rule rebuilds it whenever any of
the three files or the key changes, so a rebuilt image is never stale; a
stale or unsigned manifest in someone else's image is refused like any
other mismatch.

The loader (`boot/uefi/verify.c`) carries the key ring the kernel uses
for modules: `KEYRING_PUBS` through `scripts/gen-keyring.py`, compiled a
second time for the loader, together with the kernel's own
`kernel/security/sha512.c`, `ed25519.c` and `keyring.c` (there is no
second implementation; `boot/uefi/boot.mk`). In order:

1. Read `\cosmo\manifest`. Missing, the wrong size, no trailer, a key id
   the ring does not hold, a signature that does not verify, or a payload
   that is not three files named as above: refused.
2. Read `kernel.elf`, check its size and SHA-512 against the manifest
   *before* the ELF parser sees it; then `boot.tar`, then `cmdline`, each
   checked as soon as it is in memory. A missing file the manifest lists
   is refused.
3. Set `COSMOBOOT_VERIFIED` and `verify_key_id` in the boot information.

A refusal prints `verified boot: rejected \cosmo\<file>: <reason>` (or
`cannot read`, or `is signed by key <id>, which this loader does not
trust`) and `cosmoboot: FATAL: verified boot: refusing to boot (status
0x800000000000001a)` (`EFI_SECURITY_VIOLATION`), then returns to the
firmware as every `die()` does. The kernel never runs.

The kernel logs `boot: verified boot, key <id> (<name>)` (the name from
its own ring), prints `Verified boot, key <id> (<name>)` in the banner,
and answers the sysctl `kernel.verified_boot` with `yes <id>` or `no`.

**The developer override** is a build-time knob, `VERIFY_BOOT=0`
(`build/config.mk`, its own output tree `out/<arch>-<build>-noverify`): a
loader that compiles none of the checks, says `verified boot: OFF` on its
console and writes zeros into the v8 fields. The kernel then prints
`UNVERIFIED BOOT: the loader was built with VERIFY_BOOT=0 and checked
nothing` in the banner, warns the same in the log, and the sysctl says
`no`. There is no runtime switch: the only way to boot an unchecked
kernel is to put a different loader on the ESP, which is exactly what
Secure Boot of the loader itself (future work) would close.
`$(OUT)/boot/noverify/` holds such a loader in every tree for `make
test-security`; it is never put in an image unless the build says
`VERIFY_BOOT=0`.

**Where the root is.** The installed disk's root partition is chosen by
the installer, after the build, but the command line naming it is signed
by the build. The signed command line therefore names it relative to the
loader: `root=boot-disk` is the one partition of the CosmoOS root type on
the disk the loader was read from (`boot_partuuid`; `bootfs.c`,
`blk_find_sibling_typed`). The live image's disk has only its ESP, so
the live root stays (`root: root=boot-disk: the boot disk has no CosmoOS
root partition; the live root stays`); the installed disk has exactly
one, so init switches to it. The installer copies the ESP byte for byte
and signs nothing. This was chosen over an unsigned file holding the
PARTUUID because it leaves no unsigned input at all and makes the
installer simpler; the GPT itself is no more and no less trusted than
before (the root filesystem is not verified in 1.0). `root=PARTUUID=`
and `root=<device>` still work, for images built with another `CMDLINE`.

What verified boot does not cover: the loader itself (`BOOTX64.EFI` is
not signed for UEFI Secure Boot), the root filesystem, and measured boot;
see the inventory. Since shell completeness S7 the archive carries a copy
of the loader, `efi/BOOTX64.EFI` (`efi/BOOTAA64.EFI`), so the manifest's
signature covers the loader that `cosmo-upgrade` installs
(docs/userland/design.md, "Updating"); the loader the firmware starts is
still unchecked at boot.

## The boot disk

`make image` builds a GPT disk (`scripts/mkgpt.py` around the FAT32 image
`mkimage.sh` makes with mtools): protective MBR, 128 entries, partition 1
the EFI System Partition from LBA 2048 for exactly the 64 MiB of the FAT
image, backup table on the last sectors. Firmware boots it as it booted
the "superfloppy" before; the GUIDs are derived from the FAT image's
contents so the disk is a function of its inputs. The loader reports
the partition it was read from (`boot_partuuid`), which is how the
installer finds the ESP to copy (roadmap M2), and the kernel registers
it as a block device like any other partition (`ahci0p0p1` on q35,
`vdb1` on virt; `docs/kernel/device/design.md`, "Partitions").

## The serial console

The kernel cannot find a serial port before it has a console to report
from, and before it has its own page tables it cannot read an ACPI table
outside RAM. The loader can: it runs on the firmware's identity map. So
`acpi_scan` (`boot/uefi/acpi.c`), called in step 7 while boot services
are up, walks RSDP → XSDT (the RSDT when the RSDP is revision 1 or has
no XSDT; at most 64 entries, every table checksum-checked and read within
its length) and records one port in the boot info (protocol v9):

- **SPCR** (Serial Port Console Redirection), when present and at least
  58 bytes: interface type at offset 36, the base address's generic
  address structure at 40 (address space at 40, access size at 43,
  address at 44), the GSIV at 54. Types 0x03 (PL011), 0x0D and 0x0E (SBSA
  generic UART) in memory space become `COSMOBOOT_CONSOLE_PL011`; 0x00,
  0x01 and 0x12 (16550 variants) become `16550_MMIO` in memory space or
  `16550_PORT` in I/O space; any other type, space, or a zero base is
  `NONE`. An SPCR that names a port of another kind is final: the DBG2 is
  not consulted.
- **DBG2** (Debug Port Table), only when there is no SPCR: the first
  device entry of port type 0x8000 (serial) with an address descriptor,
  its subtype mapped as above. The DBG2 names no interrupt, so
  `console_gsiv` is 0 and `console_flags` has `COSMOBOOT_CONSOLE_FROM_DBG2`.
- Neither: `console_type` stays `NONE` and the loader prints `acpi: no
  SPCR and no DBG2 serial port: no serial console`.

`console_access` is the descriptor's access size translated to bytes
(1, 2, 3, 4 → 1, 2, 4, 8; 0 "undefined") and `console_stride` its
register bit width in bytes (8, 16, 32, 64 → 1, 2, 4, 8; anything else
0). They differ on a 16550 read a byte at a time from registers 4 bytes
apart, which the DBG2 specification allows. Either left undefined
follows the other, and both undefined is a byte-wide, byte-spaced 16550.
A 16550 whose accesses are 2 or 8 bytes wide is not one the loader or
the kernel drives: the loader says so and records no console. A PL011
is always driven with 32-bit accesses.

While boot services are up `acpi_scan` prints, on the firmware console,
the OEM id and the table signatures (`acpi: OEM 'BOCHS ', N tables: ...`)
and the SPCR or DBG2 line with its verdict. After `ExitBootServices`,
`acpi_report` prints on the recorded port what the kernel will take from
the MADT (GICC count, GICD and its version, GICR, ITS and MSI frames, or
the local APIC count on x86-64), the MCFG (segment, buses, ECAM), the GTDT
(EL1 physical and virtual GSIVs at offsets 56 and 64) and the FADT's ARM
boot flags (PSCI and the conduit). It is printed after, not before,
because firmware that mirrors its console to serial would show it twice,
and on a machine that then stays silent it is the evidence.

The loader's own output after `ExitBootServices` goes to that port and
nowhere else (BT16): on AArch64 `arch_serial_configure`
(`boot/uefi/arch/aarch64/serial.c`) drives a PL011 or a memory-mapped
16550 (spacing and width as above) at
`console_base`, which the firmware's identity map still covers; with no
record it writes nothing. No address is assumed: until this version it
wrote QEMU virt's 0x09000000, a store to whatever another machine has
there. x86-64 ignores the record and keeps COM1, as before.

The kernel's AArch64 serial console is the same port
(`docs/kernel/arch/aarch64/design.md`, "Platform discovery");
`serial=none` on the command line makes it ignore the record, and with no
serial console the framebuffer is the console from the first line
(`docs/kernel/diagnostics/design.md`).

## The display mode

`display_choose_mode` (`boot/uefi/display.c`), before the framebuffer is
read for the boot info. Both take the Graphics Output Protocol from
`display_gop`: the first handle carrying one with a mode. The display's
preferred mode is the first detailed timing of the EDID on that same
handle (`EFI_EDID_ACTIVE_PROTOCOL`, else `EFI_EDID_DISCOVERED_PROTOCOL`;
the block's header and checksum are checked); an EDID on another handle
describes another output and is not read, so with two displays the mode
set and the framebuffer handed over are the same output's. When the firmware offers that resolution with a linear
framebuffer the loader sets it with `SetMode`; with no EDID, a mode the
firmware does not offer or a failed `SetMode`, the firmware's mode stays,
as before this was written. After `ExitBootServices` the serial port gets
one line saying what it saw: `display: modes 1024x768; firmware's
1024x768; no EDID: kept` on Parallels Desktop 27, whose firmware offers
one mode and no EDID. The kernel never sets a mode; that is a display
driver's job.

## Firmware seed and time

Boot protocol v10 (`boot/uefi/firmware.c`). Just before
`ExitBootServices`, after the loader's last allocation, `firmware_gather`
asks the firmware for two things the kernel cannot get for itself on
every machine:

- **A seed.** `LocateProtocol(EFI_RNG_PROTOCOL)` and 64 bytes from
  `GetRNG` with the default algorithm into `fw_seed`, `fw_seed_len` 64.
  None (`firmware: no EFI_RNG_PROTOCOL: no seed`) or a failed call leaves
  the length 0. QEMU's OVMF and AAVMF offer the protocol over the
  virtio-rng (and on the CPU's random instructions); Parallels Desktop
  27's firmware does not. The kernel absorbs it in `random_init`,
  credited half (docs/kernel/security/design.md §6), unless the command
  line says `random.firmware=off`. `bootinfo_init` copies it and wipes the
  boot info's copy; `random_init` wipes the kernel's.
- **The time.** `GetTime` through the runtime services table, converted
  to seconds since 1970 UTC: a `TimeZone` other than unspecified is
  applied (UEFI: local = UTC − TimeZone minutes); unspecified is taken as
  UTC. An impossible date (a day past its month's length, leap years
  counted) and any year after 2500 are refused: the kernel keeps the wall
  clock in 64-bit nanoseconds, which end in 2554 (`timer_init` checks the
  product too). The kernel's wall clock uses it
  only when the architecture has no RTC it can read (`timer_init`: AArch64
  machines without QEMU's PL031, such as Parallels), anchored at the
  monotonic clock's zero, so it is late by the time between `GetTime` and
  the kernel's clock start, well under a second.

The loader prints both on the firmware console and, after
`ExitBootServices`, `firmware: seed N bytes; time from GetTime|none` on
the serial port. Neither is required.

## Entry state on x86-64

Set by `cpu_jump_to_kernel()` in `boot/uefi/cpu.c`:

- Long mode, CR3 = `boot_pagetable_root`, `CR0.WP` = 1, `EFER.NXE` = 1,
  interrupts disabled (`cli`). NX is mandatory: a processor without it
  (`cpu_has_nx()` false) makes the loader `die()` before any table is
  built, because the kernel's W^X guarantee cannot be met without it.
- `RDI` = HHDM virtual address of the info struct, `RSP` = top of a 16 KiB
  loader stack (4 pages, `EfiLoaderData`), `RBP` = 0.
- Segment registers are still the firmware's flat descriptors; the kernel
  loads its own GDT before anything else that depends on selectors.
- The kernel's `.bss` is zero because `alloc_pages_low()` zeroes every
  allocation before `elf_load()` copies file bytes into it.

Register roles are fixed by inline-asm constraints (`"a"` cr3, `"d"`
stack, `"D"` info, `"S"` entry) so the sequence cannot clobber an input
before consuming it. The jump is a `jmp`, not a `call`; there is no return
address and no calling convention across the boundary.

## Loader sequence (`boot/uefi/main.c`, `efi_main`)

1. `console_init()`: probe COM1 with the 16550 loopback test; note whether
   `ConOut` exists. `SetWatchdogTimer(0, ...)` so firmware does not reset a
   slow boot.
2. `HandleProtocol(LoadedImage)` on the loader's own handle: needed for the
   device handle (step 3) and for the image base/size (step 6).
3. `read_boot_file(KERNEL_PATH, EfiLoaderData, ...)`: LoadedImage →
   `DeviceHandle` → SimpleFileSystem → `OpenVolume` →
   `Open(L"\\cosmo\\kernel.elf", READ)` → `GetInfo` for the size
   (rejected if 0 or above 64 MiB) → `alloc_pages_low` → `Read`,
   verified to return exactly `FileSize`. Failure is fatal.
3a. The same function with `ARCHIVE_PATH` (`\cosmo\boot.tar`) and type
   `EFI_MEMORY_TYPE_COSMO_ARCHIVE`: a missing file is *not* fatal (the
   loader prints `archive: \cosmo\boot.tar not found; the kernel will
   run without init or modules` and passes zeros); any other error is.
   The archive is not parsed by the loader at all: it is opaque bytes
   for the kernel's tar parser (`bootarchive_init`), whose entries are
   in turn opaque bytes for the ELF validators (`elf_validate` for
   `init`, `modelf_validate` for modules).
4. `elf_load()` (below). Produces `struct elf_image`.
5. Allocations that must precede ExitBootServices, in order: a snapshot
   of the memory map, bootinfo (`BOOTINFO_PAGES` = 6 pages, type
   `COSMO_BOOTINFO`: the struct followed by up to `BOOTINFO_MAX_ENTRIES`
   map entries, then the command line), handoff stack
   (`HANDOFF_STACK_PAGES` = 4, `EfiLoaderData`).
6. Fill the info header, including `archive_phys`/`archive_size`.
   `find_acpi_rsdp()` scans `ConfigurationTable` for the ACPI 2.0 GUID,
   remembering a 1.0 hit as fallback. Then `acpi_scan(info)` records the
   serial console the tables name in `console_*` (v9) and
   `arch_serial_configure(info)` points the loader's own later output at
   it (AArch64; x86-64 keeps COM1); see "The serial console". The
   framebuffer is chosen and recorded.
7. `direct_map_size()` sizes the direct map ("The direct map", below);
   the page-table pool is allocated (`paging_pool_size()` pages, type
   `COSMO_PAGETABLES`) and `paging_build()` fills it; the roots go into
   the info header.
8. Memory map: `GetMemoryMap(size=0)` to learn the size, add 16
   descriptors of slack, allocate the buffer (`EfiLoaderData`, itself a
   map change the slack absorbs). Then up to two rounds of
   `GetMemoryMap` + `ExitBootServices(key)`; the second round exists
   because the first `GetMemoryMap` can itself change the map on some
   firmware. No allocation happens after this point.
9. `console_firmware_gone()`; `g_bs = NULL` so any later boot-services
   call is a null-pointer fault rather than undefined firmware behaviour.
10. `translate_memory_map()` into the bootinfo entry array; 0 entries
    means overflow and is fatal.
11. `acpi_report()` (what the MADT, MCFG, GTDT and FADT say), then the
    translated map, on the serial line.
12. `cpu_enable_nx()` then `cpu_enable_wp()` (NX presence was verified in
    step 4, before any allocation): NXE must be
    set before a CR3 whose entries carry bit 63 is loaded, otherwise the
    CPU raises a reserved-bit page fault.
13. `cpu_jump_to_kernel()`.

## ELF validation and loading (`boot/uefi/elf.c`)

`elf_load(file, size, img, fallback_used)` treats the file as hostile.
Checks, in order, each fatal with a one-line reason on the console:

- `size >= sizeof(Elf64_Ehdr)`; magic `\177ELF`; class 64; little-endian;
  version 1; `e_type == ET_EXEC`; `e_machine == EM_X86_64`.
- `e_phentsize == sizeof(Elf64_Phdr)`, `e_phnum > 0`, program-header
  table inside the file (`range_in_file`, overflow-safe).
- For each PT_NOTE inside the file: `parse_notes()` walks the note
  records with 4-byte alignment and bounds checks, looking for the
  cosmoboot note.
- For each PT_LOAD: `p_memsz >= p_filesz`; file range inside the buffer;
  `p_vaddr + p_memsz` does not wrap; at most `ELF_MAX_SEGMENTS` (8);
  **not both PF_W and PF_X** (W^X refused at load, not merely at link);
  page-rounded range does not overlap any earlier segment's page-rounded
  range (two permissions cannot share a page).
- At least one PT_LOAD; `e_entry` inside `[lo, hi)`; note present; note
  version equals `COSMOBOOT_VERSION`.

Loading: one `alloc_pages_low()` of `hi - lo` bytes (type `COSMO_KERNEL`)
gives `phys_base`; each PT_LOAD's `p_filesz` bytes are copied to
`phys_base + (p_vaddr - lo)`. Gaps and `.bss` are already zero.

## The direct map

`hhdm_size` is the loader's choice, and the kernel assumes none
(`cosmoboot.h`). `direct_map_size()` (`boot/uefi/main.c`) takes the
highest end of everything the kernel reaches through the map before it
builds its own:

- every RAM descriptor of the firmware's memory map (all types but
  MMIO, MMIO port space, reserved and unusable -- the AArch64 table
  builder's notion of RAM, so ACPI, runtime-services and boot-services
  memory count);
- the loader's own image (`LoadedImage->ImageBase/ImageSize`): the code
  that switches the tables keeps running from it;
- the kernel image, the boot archive, the boot-info pages, the handoff
  stack and the memory-map snapshot;
- the RSDP and every table `acpi_scan` read (`acpi_tables_end()`);
- the framebuffer;
- a memory-mapped serial console's register page (PL011 or 16550 MMIO;
  closes the inventory line about a console above the 4 GiB map).

The end is rounded up to 1 GiB and mapped from 0 with 2 MiB blocks
(AArch64: RAM normal, everything else device, mixed blocks as pages;
x86-64: write-back throughout). Holes between RAM and a device above it
are mapped like any other device memory. The loader prints

```
direct map: 0x0-0x140000000 (5 GiB), to the end of RAM at 0x140000000
```

naming what decided the size. If that end is past what this CPU's
tables can map -- `paging_pa_limit()`, 2^MAXPHYADDR on x86-64
(CPUID 0x80000008) or 2^PARange on AArch64 (at most 48 bits: these
tables use the 48-bit descriptor format), and at most `BOOT_HHDM_MAX`
(64 TiB, where the kernel's arena begins) -- the loader prints what and
where, and stops (`die`) while boot services are up, so the reason is on
the screen rather than lost to a silent hang. A pool the firmware cannot
provide stops it the same way, with the size it asked for.

Why: until 2026-10-18 the map was a fixed 4 GiB. Firmware loads the
loader where it likes, and with 4 GiB of RAM or more that is often
above 4 GiB (EDK2 allocates top-down). On AArch64 the TTBR0 identity
map then lacked the loader's own code, and the instruction after the
`msr ttbr0_el1` faulted with no handler: nothing after "jumping to
kernel entry". Parallels Desktop's default 4 GB VM did this on every boot
of v1.0.0-v1.0.4; QEMU virt with 4 GiB or more is the same machine shape
(`make test-platforms`, variants `mem4g`, `mem8g`).

Compatibility: no boot-protocol change. A v1.0.4 loader hands a new
kernel `hhdm_size` = 4 GiB, which it handles as before (frames above it
deferred until `vmm_init`, a console above it unused and said); a new
loader hands a v1.0.4 kernel a larger size, which that kernel's code
already accepted. `cosmo-upgrade` may therefore switch the set or the
loader first.

## Bootstrap page tables (`boot/uefi/arch/<arch>/paging.c`)

Four-level tables built from a pre-sized pool so the walk never allocates.
With N = `hhdm_size` / 1 GiB and P = ceil(N / 512) on x86-64:

```
PML4[0..P)    → PDPT_low[i] → PD_low[0..N) : 2 MiB pages, phys 0-hhdm_size, P|RW|PS|NX
                                             (loader image pages: P|RW|PS, no NX)
PML4[256..+P) → PDPT_low[i]                : same tables, HHDM at 0xFFFF800000000000
PML4[511]     → PDPT_k → PD_k → PT_k*      : kernel, 4 KiB pages, P|G, RW from PF_W,
                                             NX unless PF_X
```

### AArch64: a 2 MiB block may not mix RAM and device memory

The AArch64 loader gives the direct map RAM attributes (normal
write-back) or device attributes per 2 MiB block, from the firmware
memory map. A block that *mixes* the two -- firmware reserving a range
inside RAM without 2 MiB alignment -- cannot have one answer: device
attributes on RAM make every unaligned access fault, and normal
attributes on MMIO let the CPU reorder and merge accesses to a device.
Such a block is therefore mapped as 4 KiB pages, each with its own
attributes, and the loader says how many it found.

This is not hypothetical: a `ramfb` framebuffer is RAM the firmware
reserves, and adding one to the AArch64 machine put a reserved range in
the middle of memory for the first time. The kernel then took a page
from the same 2 MiB block for a slab object, and the compiler's
eight-byte store into a four-byte-aligned field of it raised an
alignment fault inside `vmm_init` -- before the kernel had installed
tables of its own. `paging_pool_size` accounts for the extra tables:
one per root per mixed block, bounded by the number of memory-map
descriptors, since a mixed block needs a boundary and a boundary needs
a descriptor.

`paging_pool_size(img, descriptors, hhdm_size)` on x86-64 = 1 (PML4) +
(P + N) (identity PDPTs + one PD per GiB) + (1 + 1 + span/2MiB + 1)
(kernel PDPT, PD, page tables) + 4 slack; on AArch64 2 × (1 + P + N)
(L0, L1s, L2s of each root) + (2 + span/2MiB) (kernel L1, L2, L3s) + 4
slack + 2 × descriptors (mixed blocks). `pool_take()` calls `die()` if
the estimate is ever wrong. AArch64 classifies a block by summing the
RAM descriptors' overlap with it, not page by page: a direct map tens
of GiB wide has that many thousand blocks.

Intermediate entries are always P|RW without NX; only leaves carry
permissions. `next_level()` creates tables on demand from the pool.
Sharing `PDPT_low` between PML4[0] and PML4[256] keeps identity and HHDM
identical by construction and costs no pages.

The loader's own image (`LoadedImage->ImageBase/ImageSize`, rounded out
to 2 MiB) is the one identity range left executable, because the final
`jmp` executes there after CR3 has switched. Those 2 MiB pages are
therefore writable and executable, through both the identity map and the
HHDM, until the kernel discards the bootstrap tables. This is the single
documented W^X exception; see `invariants.md` BT4.

## Memory allocation policy (`boot/uefi/memory.c`)

`alloc_pages_low(pages, type, out, fallback_used)`:
`AllocatePages(AllocateMaxAddress, type, pages, &addr)` with `addr` =
`LOADER_ALLOC_LIMIT - 1` (4 GiB), so everything the loader hands to the
kernel is inside the identity map and HHDM. Loader-defined types
(`EFI_MEMORY_TYPE_COSMO_KERNEL` `0x80000000`, `_BOOTINFO` `0x80000001`,
`_PAGETABLES` `0x80000002`, `_ARCHIVE` `0x80000003`) live in the range
UEFI reserves for OS loaders; a firmware that returns `EFI_INVALID_PARAMETER` gets a retry
with `EfiLoaderData` and `*fallback_used` is set, which the loader
reports as a warning. The result is always zeroed.

## Memory map translation (`translate_type`, `translate_memory_map`)

Any descriptor with `EFI_MEMORY_RUNTIME` becomes `FIRMWARE_RUNTIME`
regardless of type. Otherwise: Conventional → `USABLE`; LoaderCode/Data
and BootServicesCode/Data → `LOADER_RECLAIMABLE`; RuntimeServices* →
`FIRMWARE_RUNTIME`; ACPIReclaim → `ACPI_RECLAIMABLE`; ACPIMemoryNVS →
`ACPI_NVS`; Unusable → `BAD`; MemoryMappedIO(PortSpace) → `MMIO`;
PersistentMemory → `PERSISTENT`; the four loader types → `KERNEL`,
`BOOTINFO`, `BOOT_PAGETABLES`, `ARCHIVE`; everything else (Reserved, PalCode,
Unaccepted, unknown) → `RESERVED`. Adjacent descriptors with the same
translated type are merged. Descriptors with zero pages are skipped.

## Kernel-side consumer

`entry.S` `_start`: `cli; cld`, load `RSP` with `x86_boot_stack_top`
(64 KiB in `.bss.boot_stack`), zero `RBP`, push two zero words (fake
outermost frame, keeps 16-byte alignment), `call x86_start` with `RDI`
untouched.

`bootinfo_init(info)` (`kernel/core/bootinfo.c`) panics unless: pointer
non-null; magic and version match; `size >= sizeof`; `hhdm_base` and
`hhdm_size` non-zero; `mem_map_entry_size == sizeof(cosmoboot_mem_entry)`;
at least one entry; the map array lies inside the direct map; every entry
has non-zero, page-aligned base and length and does not wrap. Only then
are the accessors usable. `bootinfo_phys_to_virt()` panics for addresses
at or beyond `hhdm_size`.

## Concurrency

None. The loader is single-threaded, never re-entered, and runs on the
bootstrap processor with firmware interrupts still enabled until step 13;
none of its code runs from an interrupt. The kernel-side `bootinfo_*`
accessors are read-only after `bootinfo_init()` and need no locking.

## Error handling

Every firmware call's status is checked. Any failure goes through
`die(what, status)`: prints `cosmoboot: FATAL: <what> (status 0x..)`,
and while boot services are up, stalls 3 s so the message is readable on
screen and returns to firmware with `BS->Exit`; afterwards, halts with
`cli; hlt`. Nothing is ever ignored or retried silently except the
documented single `ExitBootServices` retry.

## Performance

Irrelevant at this stage: the whole loader path is a few milliseconds
under TCG; byte-wise `memcpy` of a 115 KiB kernel is not measurable.

## Security considerations

- Everything read from the file is bounds-checked before use (`elf.c`).
- W^X is enforced at three points: the ELF loader refuses W+X segments,
  the page-table builder derives permissions from the same flags, and the
  build's `check-kernel-elf.sh` catches it earlier still.
- NX is enabled before the tables that use it are loaded; WP is set so
  the kernel cannot write its own read-only mappings by accident.
- The transient loader W+X pages are a known gap closed by the Phase 2
  VMM.
- Verified boot (above): the loader refuses a kernel, archive or command
  line that does not match the manifest signed by a key in its ring.
  Secure Boot signing of `BOOTX64.EFI` itself is future work.

## Known limitations

- The loader allocates only below 4 GiB (`AllocateMaxAddress`); a
  machine with no free RAM there cannot boot. Memory above is mapped
  ("The direct map") but not allocated from.
- On x86-64, MMIO ranges inside the identity map are mapped write-back like
  RAM. The kernel must not access device memory through the bootstrap
  tables without remapping it first.
- No framebuffer or command line; `reserved1` is zero. Exactly one
  archive, always read from the fixed path `\cosmo\boot.tar`; the
  loader knows nothing about its contents (names, order, or what is a
  module) and passes no command line for it. Entry naming and load
  order are a kernel/build convention (`docs/kernel/module/design.md`).
- If the loader-defined memory types fall back to `EfiLoaderData`
  (firmware rejected `0x8000000x`), the translated map would list the
  kernel image, bootinfo, page-table pool, and archive as
  `LOADER_RECLAIMABLE`. The loader therefore retypes those four ranges
  from their known placements after translation (`mark_range` in
  `main.c`, splitting entries as needed), so the map the kernel sees is
  identical to the non-fallback case. As a second line, `pmm_init`
  refuses to boot if the entry covering the kernel image is usable or
  reclaimable. OVMF accepts the types, so the fallback path is covered
  by review only.
- A memory map with more than about 630 merged entries would not fit in
  `BOOTINFO_PAGES` and is fatal; real firmware produces well under 200.
- `BOOTX64.EFI` only; no 32-bit UEFI, no legacy BIOS.

## Future extensibility

- **AArch64**: the struct is unchanged. The header gains an "Entry state
  (AArch64)" section: EL1, MMU on with TTBR1 covering the kernel and the
  HHDM, `X0` = info, `SP` = handoff stack, DAIF masked. An AArch64 loader
  (`boot/uefi` compiled for `aarch64-unknown-windows`) reuses `main.c`,
  `elf.c` (with `EM_AARCH64`), `memory.c`, and `console.c` (UART via
  the EFI SerialIo or a PL011 at a firmware-reported address);
  `paging.c` and `cpu.c` become per-architecture files.
- **Protocol version 4** (framebuffer, command line): bump
  `COSMOBOOT_VERSION`, replace `reserved1` fields, and keep `size` so a
  kernel can read an older loader's struct and refuse it cleanly. The
  kernel refuses any version other than its own (`bootinfo_init`).
  Version 3 already answered the multi-module need with the archive, so
  no module list field is planned: more boot files mean more archive
  entries, not more protocol.
- **Memory manager handoff**: reservation is by map type, and the
  loader guarantees the types are right even under the `EfiLoaderData`
  fallback by retyping its own ranges (`mark_range`). The PMM's
  kernel-image check is the safety net.
