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
- `QEMU_GIC` selects the interrupt controller: `2` (the default, the
  GICv2 driver in `gic.c`) or `3` (the GICv3 driver in `gicv3.c`).
  `QEMU_MSI` selects how an MSI reaches it: `its` (QEMU's default under
  `gic-version=3`, and what GICv3 hardware offers), `gicv2m` (the frame,
  the only path a GICv2 has and the fallback a GICv3 takes when firmware
  describes no ITS), or `off`. **`QEMU_MSI` needs QEMU 11 or newer**:
  the `virt` machine's `msi` property does not exist in 10 and earlier,
  where the boot fails half a second in with `Property
  'virt-10.0-machine.msi' not found`. `qemu-run.sh` checks and says so
  rather than passing it through. The ITS needs no property at all --
  `gic-version=3` builds one by default -- so only the GICv2m-under-
  GICv3 shape is affected. **`msi=off` cannot boot this tree**: no
  driver here falls back to INTx, so NVMe and AHCI fail to probe and the
  harness loses the device markers it requires; it exists so the decline
  path can be exercised deliberately. **`gic-version=2` caps the machine
  at eight CPUs**, which is why `QEMU_SMP` above 8 needs `QEMU_GIC=3`.
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
| Linux ABI (`hello from linux abi`, `LINUXTEST: PASS`, `lxinterp: ok`, `lxdyn: ok`, the seven `lxsig` lines) | required | **required** (milestone 10: the personality's AArch64 table) |
| musl line | required when `HAVE_MUSL=1` | not built (x86-64 machine code) |
| Virtualization (`HVTEST: PASS`, `hv-*` self-tests) | required; `HVTEST: skipped` and `selftest: hv: skipped` forbidden | not checked |
| `HVTEST: skipped` | forbidden | **required** (the x86-only section must report itself skipped, proving `rc.test` reached it) |

`rc.test` runs `/etc/rc.linux` when `/boot/tests/linux/lxhello` exists
(both architectures) and prints `LINUXTEST: skipped` otherwise; `vmctl
probe` fails without `/dev/vmm` and prints `HVTEST: skipped`.

The same chain as x86 is run before a phase is declared complete:
`QEMU_SMP=1 make ARCH=aarch64 test`, `make ARCH=aarch64 BUILD=release
test`, `make ARCH=aarch64 test-crash`, `make ARCH=aarch64
MODULE_SIG_ENFORCE=0 test`, `make ARCH=aarch64 host-test`, `make
ARCH=aarch64 analyze`, `make ARCH=aarch64 reproducible`
(`check-reproducible.sh` compares `boot/BOOTAA64.EFI`), plus the two
interrupt-controller shapes: `QEMU_GIC=3 QEMU_MSI=its`,
`QEMU_GIC=3 QEMU_MSI=gicv2m QEMU_SMP=1` and
`QEMU_GIC=3 QEMU_MSI=gicv2m QEMU_SMP=8`. The middle one exists because a
fallback nothing runs is a fallback that regresses.

### `make test-gic`

QEMU's virt defaults to `gic-version=2`, `make test` does not set
`QEMU_GIC`, and CI did not either -- so between the GICv3 unit landing
and this target existing, **nothing automated ran the GICv3 driver or
the ITS at all**. `make ARCH=aarch64 test-gic` runs the boot test on the
GICv3 machine with its default MSI path (an ITS) and then, where the
QEMU is new enough to have the `msi` property, with a GICv2m frame; on
an older QEMU it says which shape it skipped and why.
`.github/workflows/ci.yml` calls it for every architecture in the matrix
-- on x86-64 it prints that there is no GIC and succeeds.

That version split is the first thing this step found. CI's QEMU is 10
and the development host's is 11, so `QEMU_MSI` -- added by the GICv3
unit and never run by CI, because CI never set `QEMU_GIC` either --
worked everywhere it was tried and nowhere it was not.

The virtual GIC is GICv3-only, so the tests that deliver an interrupt to
a guest run there and nowhere else -- every `el2-guest-irq*`, and the two
timer *delivery* tests `el2-guest-timer` and `el2-guest-timer-ontime`;
this target is what keeps them run. The timer **state** tests --
`el2-guest-timer-isolated`, `el2-guest-timer-offset` and
`el2-guest-phys-timer` -- test only `CNTV`/`CNTVOFF`/`CNTHCTL` and touch
no interrupt path, so they run on both GIC machines and skip nowhere.
The coverage matrix is therefore:

| test | GICv2 | GICv3 |
|---|---|---|
| `el2-guest-irq*`, `el2-guest-timer`, `el2-guest-timer-ontime` | skip | run |
| `el2-guest-gicd-probe`, `-gic-config`, `-gic-timer`, `-sgi`, `-gicd-isolated` (a guest has a distributor only where it has a vGIC) | skip | run |
| `el2-guest-timer-isolated`, `-offset`, `-phys-timer` | run | run |

### Sixteen CPUs

`QEMU_GIC=3 QEMU_SMP=16` boots and brings all sixteen CPUs online --
the first configuration in this tree to do so, and the one that proves
the SGI target list is built from the right affinity fields (`smp-call`
sends to each CPU in turn and requires the callback to run *there*).

**It is worth running: it found the MSI/wired-SPI overlap.** At sixteen
CPUs the drivers ask for twenty-seven MSI vectors, which is where the
GICv2m frame's range (SPIs 80..143 on virt) reaches the lines firmware
wired to the SMMU (106 and 109), and the SMMU stopped being interrupted
-- `iommu` failed with `EVENTQ_PROD` at 256 and `CONS` at 0. Nine CPUs
ask for twenty-three and never reach it. Fixed, and `irq-msi-overlap`
now proves it at any CPU count; the boot logs `gicv3: MSI frame SPI 106
is wired to a device; not offering it`.

### What sixteen CPUs is worth, measured

The three things this port can honestly measure about the change --
interrupt latency under TCG is not one of them, for the reason
`docs/kernel/memory/testing.md` records:

| | 4 CPUs | 8 CPUs | 16 CPUs |
|---|---|---|---|
| CPUs online | 4 | 8 | **16** (was capped at 8) |
| device interrupts on CPU 0 / elsewhere | 14 / 13 | 13 / 22 | 12 / 36 |
| `net-nicbench` eth0 ARP round trips | 7590/s | 9348/s | 7595/s |
| `net-nicbench` eth0 UDP sends | 14320/s | 19148/s | 13171/s |

The interrupt column is the affinity change: what used to be entirely
CPU 0 is now spread, and CPU 0's remainder is the interrupts registered
before the APs are up, which have only one CPU to choose.

The network columns are the point the report made about what this unit
is worth to the ones after it: throughput improves from four CPUs to
eight and then **falls back** at sixteen. Some of that is a ten-core
host running sixteen MTTCG vCPUs, and none of it is a claim about
hardware -- but the shape is now visible at all, which it could not be
while eight was the ceiling. The single TCP lock and the single RX
worker are where to look next.

It is still **not** a chain step. One test remains over its budget at
sixteen: `process-user`'s fifteen-second "this is stuck" bound, at 16.3
s. That bound catches a hang, not slowness, and sixteen MTTCG vCPUs on a
ten-core development host is slowness -- the same test takes 3.6 s at
four CPUs and 6.6 s at eight, under both GIC drivers alike. Run sixteen
by hand when changing the SGI, affinity or MSI paths.

## Kernel self-tests

All architecture-independent self-tests run unchanged on AArch64. What
each of them exercises in this backend:

- `irq-state`: `DAIF` save/restore nesting.
- `breakpoint-trap`: `brk #0` → the lower-EL/current-EL sync slot,
  `save_frame`, `aarch64_trap_entry`, `classify`, `interrupt_dispatch`,
  the `ELR + 4` resume, `eret`; the handler sees vector 1024 and an `elr`
  in kernel text.
- Interrupt/IRQ tests: dynamic vector allocation in 1056..1311, GSI
  routing to SPIs, mask/unmask, MSI compose -- through whichever path
  the machine offers, an ITS translation to an LPI or a GICv2m frame's
  SPI -- and the periodic test IRQ, which is the distributor's spare
  SPI raised from a kernel timer. It was the virtual timer's PPI 27
  until the virtual-timer unit made `CNTV` a guest's and PPI 27 the
  hypervisor's; `irq-route` exercises the same request/enable/count/
  mask/release path on a line asserted by a callback instead.
- `irq-affinity` (`kernel/interrupt/irqtest.c`): the distributor's
  highest line -- which `virt` reports and wires to nothing -- is routed
  to each online CPU in turn, made pending with `GICD_ISPENDR` through
  `arch_test_irq_raise`, and the handler must report `arch_cpu_id()`
  equal to the CPU the route named. Every driver in the tree asks for
  CPU 0, so without this a controller that ignored the CPU argument
  would pass the whole suite. It runs on both GIC drivers and skips on
  x86-64, where an I/O APIC pin cannot be asserted by software.
- `irq-msi-devid` (same file): with an ITS, a device id no device table
  can hold must be refused, and one it can hold must still succeed -- so
  the refusal is about the id and not about MSIs being unavailable. It
  is the test that the id `irq_request_msi` now carries actually reaches
  the controller. Skips where the controller ignores it.
- `irq-msi-overlap` (same file): binds the line the MSI allocator would
  hand out *next* (`arch_test_msi_overlap_gsi`), asks for one MSI, and
  requires both that the MSI took a different line and that the wired
  one still delivers. This is the overlap above, provable without a
  machine large enough to reach it by accident. Skips on x86-64, where
  an MSI carries a vector rather than a GSI and cannot collide.
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
loader refuses today), `-cpu max` in CI, big-endian, AArch32 EL0, SVE
(a machine with it runs the FP/SIMD state fine; the wider registers are
not saved), PSCI `CPU_OFF`/`SYSTEM_OFF` (shutdown is semihosting
only).

## FP/SIMD

**`fpu-switch`** was true by default on this architecture until the
FP/SIMD unit: no thread could own vector state, so none could leak. It
is a real test now -- two threads pinned to one CPU hold different
patterns in `Q0`-`Q15` across sixty-four yields each and find their own
intact.

**`usertest: fpu isolation`** is the same property between processes,
from user mode: three processes each hold a pattern across three hundred
yields and sleeps. This is the test that would notice a switch hook that
saved into the wrong area or a signal frame that restored the wrong one.

**`LINUXTEST`** covers the signal frame: the handler walks the reserved
area's records and requires an `fpsimd_context` and an `esr_context`,
and `sig_vreg_roundtrip` puts a value in `d0`, takes a signal whose
handler clobbers `v0`, and requires the value back.

**`fpu-bench`** reports what owning state costs a context switch (about
1 000 ns of 21 600 on this architecture under TCG, against 270 of 2 700
on x86-64). It is what decided against lazy switching.

**`scripts/check-fpregs.sh`** runs in `make analyze` and fails the build
if the kernel names a vector register outside the save, the restore, the
guest swap and the test hooks. Proved by putting `movi v3.16b, #0` in
`console_set_panic_mode`: `check-fpregs: console_set_panic_mode: v3.16b`
and exit 1.

The bug this unit found in itself: `CPACR_EL1` is per-CPU, and the first
version set it only on the boot CPU. CPU 1 then took an FP trap inside
the context switch before any thread existed -- a panic at `EC=0x07`
with "context: boot (no threads yet)", which is as clear a message as
that mistake could produce.
