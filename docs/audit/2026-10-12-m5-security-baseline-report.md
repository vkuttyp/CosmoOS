# Roadmap M5: security baseline — 2026-10-12

Milestone M5 of the [1.0 roadmap](../roadmap-1.0.md): a tampered kernel or
archive is refused; layouts differ across runs; an unprivileged process
cannot mount and a granted one can; both architectures, in CI. Base: main
`c09b26b1` (M1-M4 done, CI green). Decisions 1-4 of the task are the
owner's and were not reopened; this report records how each was built.

## Plan as worked

| PR | Content |
|---|---|
| 1 | Decision 1: the signed manifest, the loader's verification, boot protocol v8, the kernel's log line, banner and sysctl, `VERIFY_BOOT=0`, the install flow; `make test-security` with acceptance a-b and its CI job. |
| 2 | Decision 2 (user-space ASLR) and the W^X check of decision 4; acceptance c. |
| 3 | Decision 3 (operation-specific privileges), every call site converted, the service manager's configuration, `/proc` address hiding; acceptance d-e; M5 ticked. |

## PR 1 (as built): verified boot

| Area | Change | Files |
|---|---|---|
| Manifest | `\cosmo\manifest`: magic `COSMOMAN`, version 1, then for `kernel.elf`, `boot.tar`, `cmdline` in that order the 16-byte name, the size and the SHA-512; then the module-signing trailer (Ed25519 signature, key id, `COSMOSIG`) over those 280 bytes. Written by `scripts/mkmanifest.py sign` with `MODSIGN_KEY` (developer key, or the release key with `SIGNING=release`); deterministic, so reproducible. The make rule depends on the three files and the key, so a rebuilt image always carries a fresh manifest. | `boot/protocol/cosmomanifest.h`, `scripts/mkmanifest.py`, `Makefile`, `scripts/mkimage.sh` |
| Loader | `verify.c`: read the manifest first (missing, wrong size, no trailer, a key id outside the ring, a bad signature or the wrong layout: refused), then each file is checked against its size and SHA-512 as soon as it is read -- the kernel before the ELF parser sees it. Refusal: `verified boot: rejected \cosmo\<file>: <reason>` and `cosmoboot: FATAL: verified boot: refusing to boot (status 0x800000000000001a)`. The ring is `KEYRING_PUBS` (the module ring) through `gen-keyring.py`; hashing, signature check and ring lookup are the kernel's `kernel/security/sha512.c`, `ed25519.c`, `keyring.c`, compiled freestanding into `$(OUT)/boot/kcrypto/` (`crypto.h` and `keyring.h` now include standard headers only, for that reason). | `boot/uefi/verify.c`, `main.c`, `loader.h`, `boot.mk`, `efi.h`, `kernel/include/kernel/crypto.h`, `keyring.h` |
| Override | `VERIFY_BOOT=0` (`build/config.mk`, its own tree `out/<arch>-<build>-noverify`): a loader with no checks linked, `verified boot: OFF` on its console, zeros in the boot protocol. `$(OUT)/boot/noverify/` holds one in every tree for the test, never placed in an image unless the build asks. No runtime switch. | `build/config.mk`, `boot/uefi/boot.mk` |
| Protocol v8 | `verify_flags` (`COSMOBOOT_VERIFIED`), `reserved3`, `verify_key_id[8]`; `bootinfo_init` panics on an unknown flag; `bootinfo_verified()`. | `boot/protocol/cosmoboot.h`, `kernel/core/bootinfo.c`, `bootinfo.h` |
| Kernel | Banner `Verified boot, key <id> (<name>)` or `UNVERIFIED BOOT: the loader was built with VERIFY_BOOT=0 and checked nothing`; log `[ INFO] boot: verified boot, key <id> (<name>)` (a WARN when unverified); sysctl `kernel.verified_boot` = `yes <id>` / `no`. The key's name comes from the kernel's own ring. | `kernel/core/main.c`, `kernel/syscall/native.c` |
| Install flow | The command line is signed, so the build fixes it: `CMDLINE ?= root=boot-disk`. `root=boot-disk` is the one partition of the CosmoOS root type on the disk the loader was read from (`blk_find_sibling_typed`). The live image's disk has only its ESP: the live root stays (`root: root=boot-disk: the boot disk has no CosmoOS root partition`, init says the same). The installed disk has exactly one: init switches to it. `cosmo-install` copies the ESP byte for byte and writes and signs nothing; the 512-byte command-line slot and its marker are gone. | `Makefile`, `kernel/core/bootfs.c`, `kernel/block/part.c`, `part.h`, `userland/init/init.c`, `userland/system/cosmo-install.c` |
| Tests | `make test-security` stage `verify` (below); every ordinary boot requires the loader's four `verified boot` lines and the kernel's, and forbids `UNVERIFIED BOOT`; `test-install` checks the installed ESP is the boot ESP byte for byte, the installed boot resolves `root=boot-disk` and is verified, and its fallback image is re-signed with `--key`. | `tests/boot/security_test.py`, `run_boot_test.py`, `install_test.py`, CI job `security` |

**Why `root=boot-disk` and not an unsigned PARTUUID file.** The task
offered an unsigned file the kernel reads only for `root=`. Naming the
root relative to the loader's own partition needs no unsigned input at
all, no protocol field, and removes code from the installer instead of
moving it. The GPT that says which partition is the root is unsigned
either way, and the root filesystem is not verified in 1.0, so neither
variant protects the root more than the other; the chosen one has less
to get wrong. An image built with another `CMDLINE` (`root=PARTUUID=`,
`root=vda2`) is signed with it and works as before.

Design: `docs/boot/design.md`, "Verified boot" and "The command line";
invariants BT1 (restated), BT14, BT15; `docs/boot/testing.md`,
"Verified boot: make test-security"; `docs/userland/design.md`, "The
installer".

### Acceptance a-b (local, release builds)

| Boot | x86-64 | AArch64 |
|---|---|---|
| signed: `verified boot, key ec28a00bf331f50b (dev)`, sysctl `yes ec28a00bf331f50b` | PASS 5.7 s | PASS 9.3 s |
| `kernel.elf` one byte (`.strtab`) changed: refused naming it, no kernel output | PASS | PASS |
| `boot.tar` one byte changed | PASS | PASS |
| `cmdline` one byte changed | PASS | PASS |
| manifest signed by another key: refused naming that key | PASS | PASS |
| that manifest with the trusted key's id: signature does not verify | PASS | PASS |
| manifest deleted | PASS | PASS |
| VERIFY_BOOT=0 loader, tampered kernel: boots, UNVERIFIED BOOT, sysctl `no` | PASS 5.3 s | PASS 8.9 s |
| whole stage | 87.4 s | 113.3 s |

`make test-install` (release, x86-64): PASS, the installed ESP equal to
the boot ESP in every sector, `root=boot-disk` resolved to `ahci0p0p2`.

### PR 1 validation (local, 2026-10-12, at `418b9ac8`)

| Step | x86-64 | AArch64 |
|---|---|---|
| `make host-test` | PASS | PASS |
| `make fuzz` | PASS | PASS |
| `make analyze` | PASS | PASS |
| debug `make test`, `QEMU_SMP=1` | PASS 140.2 s | PASS 144.3 s |
| debug `make test`, `QEMU_SMP=2` | PASS 148.7 s | PASS 141.5 s |
| debug `make test`, `QEMU_SMP=4` | PASS 142.5 s | PASS 156.5 s |
| `make test-smp2` | PASS 149.8 s | PASS 151.0 s |
| `make test-chaos` | PASS 143.8 s | PASS 153.3 s |
| `make test-harness-retry` | PASS 142.0 s | PASS 157.4 s |
| `make BUILD=release test` | PASS 17.7 s | PASS 21.1 s |
| `make BUILD=release test-security` | PASS 87 s | PASS 112.9 s |
| `make BUILD=release test-install` | PASS 27.5 s | PASS 38.5 s |

The network harness passed in every debug boot (the boot test's verdict
requires it).

## PR 2 (as built): user-space ASLR and W^X

| Area | Change | Files |
|---|---|---|
| Layout draw | `image_build` draws 24 bytes from the pool (`random_get_bytes`: never blocks; a may-be-early caller in `docs/kernel/security/design.md` §6) at every spawn and exec: stack top `USER_STACK_TOP - r*4K`, `r < 2^22`; initial stack pointer a further `sp_offset`, a 16-byte multiple below 4 KiB (8 bits; the builder lays the frame out from `stack_top - sp_offset`); mmap base `USER_MMAP_BASE + r*4K`, `r < 2^28`; `ET_DYN` bias `USER_PIE_BASE + r*4K`, `r < 2^28`. Static asserts keep the four ranges apart. | `kernel/process/process.c`, `kernel/include/kernel/process.h` |
| mmap base | `struct vm_space` gains `mmap_base` (copied by `vm_space_fork`: a fork child keeps its parent's layout, as on Linux); `user_mmap_base(space)` replaces `USER_MMAP_BASE` in native and Linux `mmap` and in `shmat`. The `ET_DYN` interpreter now loads at the first free range above the mmap base, as Linux places it, instead of the fixed `USER_INTERP_BASE`. | `kernel/include/kernel/vmm.h`, `kernel/memory/vmm.c`, `kernel/syscall/native.c`, `compat/linux/syscalls.c` |
| Break | `linux_process_init`: `brk_start = page_up(info.hi) + r*4K`, `r < 2^13`, drawn at each exec; if that page is mapped (a fixed-address interpreter loaded above the executable), the first free page above it (PR #369 review). | `compat/linux/syscalls.c` |
| W^X | The map and protect paths refused W+X already (P3). New: `vm_user_wx_violations(space)` counts W+X regions and an executable stack; debug builds run it over every image `image_build` makes and panic on any. | `kernel/memory/vmm.c`, `vmm.h`, `process.c` |
| Tests | `tests/security/aslr.c` built as `aslr-static` (`ET_EXEC`) and `aslr-pie` (static PIE: `-static-pie`, musl's `rcrt1.o`) with the BusyBox port's musl sysroot -- so both architectures, unlike `hello_musl` -- and `aslr-native.c`; all three in every image under `/boot/tests/security/`. `test-security` stage `aslr`: ten runs each; every stack, stack pointer, mmap and break distinct; the varying bits inside the designed range and at least the designed count less four; the PIE's load address varying likewise, the `ET_EXEC` and native load addresses fixed; every run reports code on its stack faulting with SIGSEGV and W+X `mmap`/`mprotect` refused with EINVAL. `lxdyn`/`lxinterp` accept any page of their ranges instead of the old fixed addresses. | `tests/security/`, `tests/boot/security_test.py`, `tests/linux/lxdyn.c`, `lxinterp.c`, `Makefile` |

Design: `docs/kernel/process/design.md` §4a; invariants P3 (extended),
P37; `docs/compat/linux/design.md` (brk, dynamic executables), L9;
`docs/kernel/memory/design.md` (mmap placement).

What stays fixed: an `ET_EXEC` image (BusyBox, dropbear and every native
program are static `ET_EXEC`), the Linux signal trampoline page, and the
kernel (no KASLR in 1.0) -- inventory lines.

### A defect the matrix found in this PR: the stack headroom

The first local matrix of PR 2 failed `process-oom` in the AArch64
`test-chaos` and `test-harness-retry` boots (`check failed: status == 0`,
the `oom-copy` probe exiting 10). The probe expects the first
kernel-mode demand fault of its life to be its `read` into a fresh page,
where the kernel injects a failure; status 10 is its `pipe()` failing
first. The random sub-page offset had taken up to 4 KiB of the
populated stack below the initial frame, so the probe's `h[2]` could
land on a page not yet populated and the pipe's copy-out met the
injected failure. Forcing `sp_offset` to `0xFF0` made it fail on every
AArch64 debug boot; populating one page more whenever the offset is not
0 (the headroom below the frame is then never less than before) passed
under the same forcing. Commit "ASLR: keep the pre-populated stack
headroom below a moved initial frame".

### Acceptance c (local, release)

x86-64, ten runs each. "Bits" counts the bit positions in which any of
the ten values differed from the first: the random term is added, so a
carry can flip a bit above the drawn ones (the static break draws 13 bits
of pages and differed in 14 positions; the harness allows the designed
range plus the carry, `security_test.py` `EXPECT`):

| Probe | stack | sp | mmap | brk | load |
|---|---|---|---|---|---|
| `aslr-static` (musl, `ET_EXEC`) | 30 | 30 | 28 | 14 | fixed `0x400000` |
| `aslr-pie` (musl, static PIE) | 29 | 29 | 28 | 30 | 30 |
| `aslr-native` | 30 | 30 | 28 | -- | fixed `0x4001b0` |

AArch64, the same: static 30 / 30 / 28 / 13 bits and load fixed
`0x400000`; PIE 30 / 30 / 28 / 30 bits, load 30 bits; native 30 / 30 /
28 bits, load fixed `0x4001bc`.

W^X in every run: `stack-exec=SIGSEGV mmap-wx=EINVAL mprotect-wx=EINVAL`.

### PR 2 validation (local, 2026-10-12, at `4ff32b41`)

| Step | x86-64 | AArch64 |
|---|---|---|
| `make host-test`, `make fuzz`, `make analyze` | PASS | PASS |
| debug `make test`, `QEMU_SMP=1` / `2` / `4` | PASS 137.1 / 140.1 / 151.8 s | PASS 145.0 / 142.2 / 162.1 s |
| `make test-smp2` | PASS 142.5 s | PASS 142.6 s |
| `make test-chaos` | PASS 143.0 s | PASS 157.5 s |
| `make test-harness-retry` | FAIL (`signal-async` status 52, first sighting, `docs/testing/flakes.md`); then PASS three times, 167.3 / 141.9 / 142.1 s | PASS 151.1 s |
| `make BUILD=release test` | PASS 17.5 s | PASS 20.8 s |
| `make BUILD=release test-security` | PASS 93.9 s | PASS 125.8 s |
| `make BUILD=release test-install` | PASS 23.7 s | PASS 38.5 s |

