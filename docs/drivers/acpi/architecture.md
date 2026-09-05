# ACPI Tables: Architecture

## Purpose

Read the static ACPI tables the firmware leaves in memory so the kernel
can enumerate CPUs and interrupt controllers. Nothing more: no AML
interpreter, no power management, no device tree of ACPI objects.

## Position

`drivers/acpi/acpi.c` sits below the interrupt controllers and SMP
bring-up and above the memory subsystem. Its only input is
`cosmoboot_info.acpi_rsdp`. It reads tables through the direct map; ACPI
tables are RAM (types ACPI reclaim and NVS) and the direct map covers
all RAM after `vmm_init`.

## Responsibilities

- Validate the RSDP (signature, checksum, revision) and choose the XSDT
  (revision ≥ 2) or the RSDT.
- Validate every referenced table header checksum before exposing it.
- `acpi_find_table("SIG")` for consumers.
- Decode the MADT into typed arrays: local APIC base (with the 64-bit
  override entry), processor entries (LAPIC and x2APIC, only those
  flagged enabled or online-capable), I/O APICs, ISA interrupt source
  overrides; since Phase 13 also the GIC entries: CPU interfaces (type
  11, the MPIDR affinity fields become the processor `hw_id`, the first
  entry's base is the GICv2 CPU interface), the distributor (type 12,
  base and version) and the MSI frame (type 13, base and optional SPI
  range), exposed as `struct acpi_gic` through `acpi_madt_gic()`.

## Non-responsibilities

AML, FADT power fields, HPET, MCFG (PCIe, Phase 6), SRAT (NUMA, later).
The AArch64 backend reads its own tables through `acpi_find_table`
(GTDT for the timer interrupts, SPCR for the console, the FADT's ARM
boot flags for the PSCI conduit); this driver only locates them.

## Interfaces

`kernel/acpi.h`: `acpi_init`, `acpi_find_table`, `acpi_madt_*` accessors
(including `acpi_madt_gic`) and the `struct acpi_madt_*` / `struct
acpi_gic` records.

## Concurrency and ownership

Initialised once on the BSP before any other CPU exists; read-only
afterwards, no locks. Decoded arrays are static with fixed maxima
(`ACPI_MAX_CPUS` 64, `ACPI_MAX_IOAPICS` 8, `ACPI_MAX_OVERRIDES` 24);
excess entries are logged and dropped.

## Error handling

Missing RSDP or MADT is a panic: this kernel targets APIC (x86-64) and
GIC (AArch64) platforms only. A bad checksum on an individual table skips
that table with a warning.

## Security

Tables come from firmware, part of the trusted computing base, but
lengths are still bounds-checked against the table header before any
entry is read.

## Testing

Self-test: the MADT reports at least one CPU and either the LAPIC base
(x86-64) or the GIC distributor (AArch64) is present; under QEMU with
`-smp N` exactly N processor entries appear.
