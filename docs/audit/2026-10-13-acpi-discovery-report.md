# AArch64 platform discovery from the firmware tables — 2026-10-13

The first unit after 1.0, chosen by the owner: the v1.0.0 AArch64 image did
not boot on Parallels Desktop 27 (Apple Silicon). The loader verified and
loaded everything, printed `exiting boot services`, and then nothing
appeared on the serial line or the screen. Base: main `df1b6539`
(v1.0.0). Goal: the kernel discovers its platform from the firmware
tables the loader hands it, before its first output, and never assumes
QEMU virt; v1.0.1 boots to the shell on Parallels and passes every CI job.

## The cause

Everything before the first output assumed QEMU virt's memory map:

- the loader, after `ExitBootServices`, wrote its own lines to a PL011 at
  0x09000000 (`boot/uefi/arch/aarch64/serial.c`);
- the kernel's first act, `arch_console_early_init`, did the same;
- `map_early_devices` mapped that page and fw_cfg's (0x09020000);
- the GIC drivers, the timer, the PL031 RTC, fw_cfg and the SMMU fell back
  to virt's addresses and interrupt numbers when a table lacked them;
- the semihosting exit (`hlt #0xF000`) ran on every machine.

On Parallels the store to 0x09000000 faults before any handler exists.

## What was built (decisions 1-5 of the task)

| Area | Change | Files |
|---|---|---|
| Boot protocol v9 | `console_type` (none, PL011, MMIO 16550, port 16550), `console_base`, `console_gsiv`, `console_access` (access width), `console_stride` (register spacing), `console_flags` (`FROM_DBG2`). Loader and kernel already refuse a protocol mismatch (the ELF note; `bootinfo_init`), and the AArch64 early console checks the version before it reads the record | `boot/protocol/cosmoboot.h` |
| Loader | `acpi_scan` walks RSDP -> XSDT, finds the SPCR, or when there is none the DBG2's debug serial port (no interrupt), records the port and prints its choice; the AArch64 loader's output after `ExitBootServices` goes to that port and nowhere else (x86-64 keeps COM1); `acpi_report` prints what the MADT, MCFG, GTDT and FADT say, after `ExitBootServices`, on that port | `boot/uefi/acpi.c`, `arch/aarch64/serial.c`, `main.c` |
| Early console | the recorded PL011 or MMIO 16550 through the direct map; input on the recorded interrupt or, with none, polled every 10 ms from a kernel timer; `serial=none` on the command line disables it. With no serial console `kernel_main` brings the framebuffer console up before its first line (`fbcon_early_init`, static buffer, the loader's direct map, which `map_early_devices` carries into the kernel's tables) | `kernel/arch/aarch64/pl011.c`, `kernel/core/fbcon.c`, `main.c`, `console.c` |
| No fixed addresses | QEMU is identified by the RSDP's OEM id, `BOCHS ` (`acpi_platform_is_qemu`); virt's GIC, fw_cfg, PL031, SMMU interrupts and the semihosting exit are used only there. Elsewhere a MADT without a GIC or an unusable GTDT is a panic naming the table; fw_cfg is absent and every `opt/cosmo/` key reads unset; no RTC; the emulator exit returns | `drivers/acpi/acpi.c`, `kernel/arch/aarch64/{irqc,gic,gicv3,timer,fwcfg,shutdown,mmu}.c`, `drivers/iommu/arm_smmuv3.c` |
| PSCI | the conduit is the FADT's (`aarch64_psci_conduit`); QEMU's firmware alone keeps the HVC default when the FADT says nothing; elsewhere no PSCI means no secondary CPUs, said once | `kernel/arch/aarch64/smp.c` |
| Platform line | logged by `arch_irqc_init`, the first line after the tables are parsed: OEM, console and its source, GIC version and bases, timer PPIs, PSCI conduit | `kernel/arch/aarch64/platform.c` |
| Tests | `make test-platforms` (`tests/boot/platform_test.py`, CI job `platforms`): GICv2, GICv3, highmem off, a CPU with RNDR, one without and no virtio-rng, and no serial port; each must reach the shell and print the platform line it should. The virtio console replays the log ring when it registers, so the no-serial boot's first lines can be read | `tests/boot/platform_test.py`, `scripts/qemu-run.sh` (`QEMU_SERIAL`, `QEMU_VIRT_OPTS`), `drivers/virtio/virtio_console.c` |

**`-serial none` does not remove the SPCR.** QEMU's virt describes its
PL011 in the SPCR whether or not anything is connected to it, so the
`noserial` variant also signs `serial=none` into the command line. That is
the path a machine without a serial port takes.

## Defects found and fixed

1. **The GTDT was never read.** `read_gtdt` took the timer interrupts from
   offsets 80 and 88: CntReadBase and the platform timer count, not the
   non-secure EL1 and virtual GSIVs, which are at 56 and 64. The values
   always failed the PPI range check and the virt defaults (30/27) were
   used, correct only because they are QEMU's numbers too.
2. **`arch_mmu_map` lacked an ISB.** The table stores were followed by
   `dsb ishst` only. On an Apple CPU under Parallels, the first read of a
   freshly mapped xHCI register page faulted (`page fault: kernel read at
   0xffffc0000048b000 (not present)` in `xhci_probe`). Release image,
   same tree, six boots each: **without the ISB 4 of 6 panicked there,
   with it 0 of 6.** QEMU's TCG never shows it. Linux pairs DSB and ISB
   the same way for kernel mappings.
3. **PCI 64-bit BAR sizing.** `decode_bars` computed `~mask + 1` over 64
   bits. A 64-bit BAR whose upper half is read-only zero, which the
   specification allows (Parallels' NVMe controller: BAR0 `0x14000004`,
   low mask `0xffffc004`), was sized about 16 EiB and could not be mapped.
   Now the mask's lowest set bit (16 KiB). Every BAR with writable upper
   bits, which is every one QEMU offers, gets the same answer as before.

**The proofs** (`tools/acpi-discovery-probe.py`). The three fixes are part
of the unit's one commit, so `--old` puts back exactly the defect's own
expression in a throwaway worktree, refusing if it is not where the fix
left it:

| Check | `--old` | The test that fails with it | Architectures |
|---|---|---|---|
| `bar` | `pci_bar_mem_size` returns `~mask + 1` (the sizing now lives in `kernel/include/kernel/pci_bar.h`) | host test `test_pci_bar`: "a 64-bit BAR whose upper half reads zero" (Parallels' NVMe masks) | both host builds |
| `gtdt` | the GTDT read at 80 and 88 | `test-platforms`' gicv3 variant: timer PPIs not 30/27, `timer: GTDT unusable` | AArch64 (x86-64 reads no GTDT) |
| `isb` | `dsb ishst` without the ISB | none on QEMU (TCG never shows it): the check boots a Parallels VM N times and counts panics | AArch64 on Parallels, a rate |

## The Parallels iteration log

VM "CosmoOS 1.0 test": EFI ARM64, 2 CPUs, 1 GiB, the image on a plain
SATA disk, serial port to a file or a socket.

| # | Seen | Cause | Fix |
|---|---|---|---|
| 1 | v1.0.0: loader verifies, `exiting boot services`, then nothing | loader and kernel write virt's PL011 at 0x09000000 | decisions 1-2 |
| 2 | the loader's new scan: `acpi: OEM 'PRLS  ', 7 tables: FACP DBG2 GTDT MCFG PCCT APIC PPTT`; **no SPCR**; DBG2: serial, subtype 0x3 (PL011), base 0x2110000, no interrupt; MADT: GICv3, GICD 0x2010000, one GICR region 0x2500000, 2 GICC, a GICv2m frame; GTDT 30/27; FADT: PSCI, SMC; MCFG: segment 0, buses 0-0 | Parallels has no SPCR | owner's decision: the DBG2 port as fallback, input polled |
| 3 | the debug kernel boots, prints the platform line, runs its self-tests; some fail (`pci`: the host bridge is not at 00:00.0; `net-firewall`, `net-tap-release-order`, `blk-submit-unregister`) | the debug self-test suite assumes QEMU's machine | not fixed: the suite runs on QEMU; the release image runs none (inventory) |
| 4 | release boots: 2 of 3 panic in `xhci_probe` on a fresh mapping | missing ISB (defect 2) | ISB; 0 of 6 since |
| 5 | release boots to `cosmo$`; over the serial socket `echo`, `ls /bin`, `sysctl kernel.verified_boot` (`yes`) work: input polled from the DBG2 PL011 | -- | -- |
| 6 | `cosmo-install --list` lists no disk: no storage controller on PCI | Parallels' SATA disks are not PCI devices (MCFG covers bus 0, where there is no AHCI); presumably a platform AHCI described only in the DSDT, which needs an AML interpreter | not in this unit; the owner chose to ship the shell |
| 7 | with the VM's disk switched to NVMe (`prlctl --iface nvme`): 1ab8:4007, class 01.08, `cannot map BAR0` | defect 3 | BAR sizing; then `MSI-X unavailable`: the controller offers MSI only and the NVMe driver requires MSI-X (next unit) |
| 8 | the NIC is 1af4:1000, a legacy-only virtio-net (`no modern capabilities`); `--adapter-type e1000e` is ignored on ARM | the virtio-pci driver is modern-only | next unit |
| 9 | `random: cpu source: none`, pool never seeded | Parallels exposes no RNDR to the guest and offers no virtio-rng | not in this unit (the M1 limitation: no jitter source) |

The platform lines, as printed:

```text
QEMU virt (make test):
[ INFO] platform: firmware OEM 'BOCHS ' (QEMU); console PL011 at 0x9000000 (SPCR, IRQ 33); GICv2 GICD 0x8000000 GICC 0x8010000 MSI frame 0x8020000; timer PPIs 30 (physical) 27 (virtual); PSCI SMC

Parallels Desktop 27, Apple Silicon:
[ INFO] platform: firmware OEM 'PRLS  '; console PL011 at 0x2110000 (DBG2, polled); GICv3 GICD 0x2010000 GICR 0x2500000 ITS 0x0 MSI frame 0x2250000; timer PPIs 30 (physical) 27 (virtual); PSCI SMC
```

QEMU's PSCI conduit is now SMC: its FADT says so with
`virtualization=on`, where the kernel used to assume HVC.

## What works on Parallels, and what does not

Works: verified boot, the kernel, both CPUs, the GICv3 and its v2m MSI
frame, the timer, the console in and out on the DBG2 port and on the
screen, the xHCI controller and its USB keyboard and tablet, virtio-gpu
and virtio-vsock enumerate, the live shell.

Does not: installing (no disk this kernel drives: SATA is not PCI, NVMe
needs MSI), networking (legacy virtio-net), ssh (no network, and no
entropy for a host key), anything that needs randomness (`getrandom`
blocks), the wall clock (no RTC found: it starts at 1970), the debug
self-test suite.

## Validation

The AGENTS.md matrix on this branch, both architectures (see the PR),
plus `make test-platforms` and `test-gic`, and the release-set
reproducibility check. Parallels: six release boots to the shell, the
shell used over the serial socket.
