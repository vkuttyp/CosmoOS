# AArch64 port: testing

## The machine

`scripts/qemu-run.sh` with `QEMU_ARCH=aarch64` (set by `make
ARCH=aarch64 run|test|test-crash`):

```text
qemu-system-aarch64 -machine virt,gic-version=2,accel=tcg -cpu cortex-a72 -smp 4 -m 256M
  -drive if=pflash,format=raw,readonly=on,file=out/aarch64-debug/firmware-aarch64.fd
  -drive if=none,id=testdisk,... -device virtio-blk-pci,drive=testdisk      # vda: the scratch disk
  -drive if=none,id=boot,readonly=on,file=cosmoos.img -device virtio-blk-pci,drive=boot
  -device virtio-rng-pci -device virtio-serial-pci -device virtconsole,chardev=vcon
  -netdev user,... -device virtio-net-pci,netdev=n0
  -semihosting-config enable=on,target=native -serial stdio -display none -no-reboot
```

- `QEMU_CPU`, `QEMU_SMP`, `QEMU_MEM`, `QEMU_ACCEL`, `QEMU_EXTRA`,
  `QEMU_TESTDISK`, `QEMU_VCON`, `QEMU_NET_HOSTFWD`, `QEMU_FWCFG_NETTEST`,
  `QEMU_PCAP` and `OVMF_CODE` keep their meanings. `-cpu max` adds PAN and
  is also supported.
- The firmware (`scripts/find-firmware.sh aarch64`: `AAVMF`,
  `qemu-efi-aarch64`, Homebrew `edk2-aarch64-code.fd`) is copied and
  padded to the 64 MiB flash size the `virt` machine expects, into
  `$(OUT)/firmware-aarch64.fd`; QEMU refuses a smaller image.
- The scratch disk is attached before the boot image so it is `vda`, as
  on x86, and the storage self-tests format the right device; the boot
  image is read-only.
- The kernel ends the run with semihosting `SYS_EXIT_EXTENDED`; QEMU exits
  with `(code << 1) | 1`, the same encoding the x86 `isa-debug-exit`
  device produces, so `tests/boot/run_boot_test.py` is unchanged there.
- EDK2 hands over at EL1; `virtualization=on` would start the loader at
  EL2 and it refuses (`cosmoboot: started at EL2; this loader requires
  EL1`).

A debug boot to `boot complete` takes about 15 s under TCG on the
development host (an Apple Silicon Mac).

## Boot test (`make ARCH=aarch64 test`)

The harness reads `COSMO_ARCH` from the environment (the Makefile
exports it) and requires the generic markers with `Architecture: aarch64`
in the banner: the loader banner, `jumping to kernel entry`, the module
load lines, `eth0`, `vda`, the console sink, `USERTEST: PASS`,
`SELFTEST: PASS`, `SHTEST: PASS`, the package markers, `interactive-ok`,
`boot complete` in the virtio console, and exit status 33. The network
harness and the shell harness run unchanged.

Architecture-dependent markers:

| Marker | x86-64 | AArch64 |
|---|---|---|
| Linux ABI (`hello from linux abi`, `LINUXTEST: PASS`, musl line) | required | not checked |
| Virtualization (`HVTEST: PASS`, `hv-*` self-tests) | required; `HVTEST: skipped` and `selftest: hv: skipped` forbidden | not checked |
| `LINUXTEST: skipped`, `HVTEST: skipped` | forbidden | **required** (the x86-only sections must report themselves skipped, proving `rc.test` reached them) |

`rc.test` runs `/etc/rc.linux` only if `/boot/tests/linux/lxhello`
exists (the Makefile includes `tests/linux` and `tests/hv` only for
`ARCH=x86_64`) and prints `LINUXTEST: skipped` otherwise; `vmctl probe`
fails without `/dev/vmm` and prints `HVTEST: skipped`.

The same chain as x86 is run before a phase is declared complete:
`QEMU_SMP=1 make ARCH=aarch64 test`, `make ARCH=aarch64 BUILD=release
test`, `make ARCH=aarch64 test-crash`, `make ARCH=aarch64
MODULE_SIG_ENFORCE=0 test`, `make ARCH=aarch64 host-test`, `make
ARCH=aarch64 analyze`, `make ARCH=aarch64 reproducible`
(`check-reproducible.sh` compares `boot/BOOTAA64.EFI`).

## Kernel self-tests

All architecture-independent self-tests run unchanged on AArch64. What
each of them exercises in this backend:

- `irq-state`: `DAIF` save/restore nesting.
- `breakpoint-trap`: `brk #0` → the lower-EL/current-EL sync slot,
  `save_frame`, `aarch64_trap_entry`, `classify`, `interrupt_dispatch`,
  the `ELR + 4` resume, `eret`; the handler sees vector 1024 and an `elr`
  in kernel text.
- Interrupt/IRQ tests: dynamic vector allocation in 1056..1311, GSI
  routing to SPIs, mask/unmask, MSI compose through the GICv2m frame,
  the periodic test IRQ on the virtual timer (INTID 27).
- Timer tests: `CNTPCT` monotonicity and rate, the tick on `CNTP_CVAL`
  compares (the rate windows are what caught the `TVAL` drift), one-shot
  timers and sleeps.
- SMP tests: PSCI bring-up of CPUs 1–3, per-CPU ticks (banked PPI
  enables), SGI-based IPIs and cross-CPU calls, the TLB shootdown
  counters (`initiated` and `acks_received` without an IPI), stop and
  restart.
- Memory tests: the PMM test accepts an empty DMA (`< 16 MiB`) zone,
  which `virt` has because RAM starts at 1 GiB; the DMA32 zone is
  populated and checked; the VMM tests cover 2 MiB blocks, `protect`,
  user/kernel fault classification through `FAR_EL1`/`ESR_EL1`, and the
  near arena bounds from `arch_mmu_near_arena`.
- Process and user tests: `svc #0` dispatch, the initial EL0 entry with
  zeroed registers, TLS through `TPIDR_EL0`, kill delivery through the
  return-to-user hook, the `ELF_MACHINE_NATIVE` check (an x86 binary is
  refused as the wrong machine).
- Module tests: `R_AARCH64_*` relocation of the fixture modules
  (`cosmotest` calls kernel exports through `CALL26`), the module ELF
  validator with `e_machine` 183 (`scripts/check-module-elf.py module.ko
  aarch64` at build time), signature enforcement.
- Device tests: the DMA test's 24-bit allocation is checked only when a
  DMA zone exists; `arch_dma_barrier` runs on every `dma_sync_for_device`.
- `linux-elf` and `hv-*`: log `skipped`.

The scheduler's ACPI check accepts either a local APIC or a GIC
distributor in the MADT.

## Crash test (`make ARCH=aarch64 test-crash`)

`CRASH_TEST=1` writes to `0xFFFF900000000000`, an unmapped kernel-half
address: a data abort from EL1 with a level-0 translation fault. The
harness requires, besides the generic lines (`KERNEL PANIC: page fault:
kernel write at 0xffff900000000000 (not present): no region`, `stack
trace:`, a frame in `0xffffffff8xxxxxxx`, `halting.`), the
AArch64-specific ones:

```text
^trap 1029 
^ELR=[0-9a-f]{16} SPSR=
^FAR=ffff900000000000 \(not-present write kernel\)
```

1029 is `VEC_SYNC_BASE + ARCH_TRAP_PAGE_FAULT`. The flag decoding proves
`arch_trap_fault_flags` (a translation fault is "not-present", `WnR` is
"write", a current-EL abort is "kernel").

## Host tests

`make host-test` builds `tests/host/test_reloc_aarch64` on every host
(any architecture): `kernel/arch/aarch64/modreloc.c` compiled natively
under ASan/UBSan, checking each relocation type's encoding against
hand-assembled expectations and each range limit (`ABS32/ABS16/PREL32/
PREL16`, `ADR_PREL_LO21`, `ADR_PREL_PG_HI21` ±4 GiB, `LD_PREL_LO19`,
`TSTBR14`, `CONDBR19` ±1 MiB, `CALL26/JUMP26` ±128 MiB), the
`-ERANGE`/`-ENOEXEC` results and `aarch64_reloc_width`. `test_modelf`'s
wrong-machine expectation is architecture neutral. The other host tests
are unchanged.

## CI

`.github/workflows/ci.yml` runs a matrix over `arch: [x86_64, aarch64]`
in the `debian:trixie` container (QEMU 10, `qemu-system-arm`,
`qemu-efi-aarch64`), the same steps for both: `check-tools`, debug
`all` + `test`, release `all test`, `host-test`, `analyze`,
`reproducible`, `test-crash`; serial logs and images are uploaded per
architecture.

## Bugs the tests caught during bring-up

Recorded so their symptoms are recognisable:

- **Silent hang at VMM takeover**: the new kernel root had no PL011 page
  (the direct map covers RAM only). Fix: `map_early_devices` in
  `arch_mmu_activate` (A7).
- **Secondary CPUs never came up**: the mailbox's physical address was
  computed with `virt_to_phys` on an image address; then the trampoline
  read the mailbox after enabling the MMU with only its own page
  identity-mapped. Fixes: `kernel_va_to_pa`, read-before-MMU (A13).
- **`breakpoint-trap` looped forever**: BRK is fault-class; `ELR` pointed
  at the `brk`. Fix: resume at `ELR + 4` (A15).
- **Module load failed with `CALL26 out of range`**: the x86 near arena
  is ~2 GiB above the image. Fix: `arch_mmu_near_arena` (A16).
- **Three CPUs and a garbage GICC base**: wrong MADT GICC field offsets.
  Fix: flags at 12, base at 32, MPIDR at 68.
- **Arena faults reported as user faults**: `arch_mmu_kernel_base`
  returned the image base. Fix: return the start of TTBR1's half.
- **PMM/DMA tests failed on an empty DMA zone**: made tolerant.
- **Timer rate tests missed their window**: `TVAL` reload drift. Fix:
  absolute `CVAL` compares (A11).
- **Shootdown test counters**: no IPI meant no acks. Fix: count `n−1`
  acks per broadcast (A12).
- **Storage tests formatted the boot image**: it was `vda`. Fix: attach
  the scratch disk first, boot image read-only.
- **Module ELF check rejected `e_machine` 183** and `test_modelf` expected
  the x86 message: both made architecture aware.

## Not tested

Real hardware, GICv3, KVM/HVF acceleration (the CI machine has none for
AArch64 and the macOS host would need `virtualization=on`, which the
loader refuses today), `-cpu max` in CI, big-endian, AArch32 EL0,
FP/SIMD in userland, PSCI `CPU_OFF`/`SYSTEM_OFF` (shutdown is
semihosting only).
