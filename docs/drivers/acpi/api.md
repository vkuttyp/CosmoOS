# ACPI Tables: API

Header `kernel/include/kernel/acpi.h`, implementation
`drivers/acpi/acpi.c`. **ABI stability: kernel-internal.** No symbol is
exported to modules (`acpi.c` has no `EXPORT_SYMBOL`), none is reachable
from user space, and the structs may change with the kernel. The
on-firmware layouts the driver reads are fixed by the ACPI
specification.

Common properties: after `acpi_init`, every accessor below runs in any
context (thread, interrupt, any CPU), takes no lock, does no I/O,
does not allocate and does not sleep; it only reads statics written
once at boot. Before `acpi_init` they return empty results (count 0,
NULL, 0, `false`).

## Constants

| Name | Value | Meaning |
|---|---|---|
| `ACPI_MAX_CPUS` | `64u` | Capacity of the processor array |
| `ACPI_MAX_IOAPICS` | `8u` | Capacity of the I/O APIC array |
| `ACPI_MAX_OVERRIDES` | `24u` | Capacity of the interrupt source override array |

`ACPI_MAX_TABLES` (64) and the `MADT_*` entry-type constants are
private to `acpi.c`.

## Records

### `struct acpi_sdt_header`
The 36-byte common table header, `__packed`. `signature` is four bytes,
not NUL-terminated; `length` covers the whole table including the
header. Every header returned by `acpi_find_table` has passed the
checksum over `length` bytes and has `length >= 36`.

### `struct acpi_madt_cpu`
`apic_id`: hardware CPU id — the APIC id (type 0), the x2APIC id
(type 9), or on AArch64 the MPIDR affinity packed as Aff0-2 in bits
0-23 and Aff3 in 24-31 (type 11). `acpi_id`: the ACPI processor UID.
`x2apic`: true only for type 9 entries.

### `struct acpi_madt_ioapic`
`id`, `address` (physical MMIO base), `gsi_base` (first GSI it serves).

### `struct acpi_madt_override`
`bus` (0 is ISA), `source` (the ISA IRQ), `gsi`, `flags` (MPS INTI
flags: polarity in bits 0-1, trigger mode in bits 2-3). Decoded by
`irq_legacy_to_gsi` in `kernel/interrupt/irq.c`.

### `struct acpi_gic`
The MADT's GIC description. `version` (the GICD entry's version byte,
copied unchecked: 0 when firmware leaves it unset, 1-4 by the ACPI
specification), `gicd_base`, `gicc_base` (first GICC physical base),
`v2m_base` with `v2m_spi_base`/`v2m_spi_count` (non-zero count only
when the MSI frame entry overrides the frame's TYPER), `gicr_base` and
`gicr_length` (GICR entry), `gicc_gicr_base` (first non-zero GICC
redistributor base), `its_base`/`its_id`, and `gicv_base`, `gich_base`,
`maint_gsiv`. A zero field means the MADT did not provide it. Field
selection rules are in [design.md](design.md#data-structures).
Consumers: `kernel/arch/aarch64/irqc.c`, `gic.c`, `gicv3.c`,
`gicv2m.c`. `gicv_base`, `gich_base`, `maint_gsiv` and `its_id` are only
logged by `acpi_init`; no other code reads them.

## Functions

### `void acpi_init(void)`
Purpose: locate and validate the RSDP and root table, record every
checksum-valid table, decode the MADT. Inputs:
`bootinfo_get()->acpi_rsdp`. Called exactly once, from `kernel_main`
(`kernel/core/main.c`) on the boot CPU after `vmm_init` and before
`irq_init`; not safe to call twice (arrays would gain duplicates).
May take `kernel_space.lock` and allocate a `vm_region` (through
`vm_map_phys`) for a table outside RAM. Does not sleep. Failure modes:
panics when there is no RSDP, the RSDP signature or either checksum is
wrong, the root table is unusable, or there is no MADT; see the table
in [design.md](design.md#error-handling) for the cases that only warn
or skip.

### `bool acpi_available(void)`
True once `acpi_init` has parsed the MADT. Because `acpi_init` panics
on every path that would leave it false, it is always true after boot;
only the `acpi` self-test calls it.

### `const struct acpi_sdt_header *acpi_find_table(const char *signature)`
Purpose: the first recorded table whose signature equals the first
four bytes of `signature`. Inputs: `signature` must point at at least
four readable bytes (compared with `memcmp`, so no NUL is needed).
Output: a kernel virtual pointer valid for the life of the kernel, or
NULL when no table listed by the XSDT/RSDT has that signature, the
table failed its checksum, or it was not among the first 64 tables
`acpi_init` recorded (entries that are zero, unmappable, short or fail
their checksum are skipped and do not count toward the 64).
Tables reached only through another table (DSDT, FACS) are never
found. The caller casts to the table's own layout and must bound every
read by `hdr.length`. Linear scan of at most 64 entries.
Callers: `drivers/pci/pci.c` (`MCFG`), `drivers/iommu/intel_vtd.c`
(`DMAR`), `drivers/iommu/arm_smmuv3.c` (`IORT`),
`kernel/arch/aarch64/timer.c` (`GTDT`), `smp.c` (`FACP`). The SPCR
is read by the loader, not here (`docs/boot/design.md`, "The serial
console").

### `bool acpi_rsdp_oem(char out[7])` / `bool acpi_platform_is_qemu(void)`

`acpi_rsdp_oem` copies the RSDP's six-byte OEM id, NUL terminated with
trailing spaces kept, read with byte reads through the loader's direct
map the first time it is called and cached; usable from `bootinfo_init`
on, before `acpi_init`. False when there is no RSDP or its signature is
wrong. `acpi_platform_is_qemu` is true when that id is `"BOCHS "`, the
id QEMU builds its tables with: the one firmware on which the AArch64
kernel may use `virt`'s fixed addresses for a device the tables leave
out (`docs/kernel/arch/aarch64/design.md`, "Platform discovery").
Neither sleeps, allocates nor takes a lock.

### `paddr_t acpi_madt_lapic_base(void)`
The local APIC physical base: MADT `lapic_address`, or the 64-bit
address of a type 5 entry when present (the last one wins). 0 when the
MADT reports none. `kernel/arch/x86_64/irqc.c` panics on 0.

### `size_t acpi_madt_cpus(const struct acpi_madt_cpu **out)`
Stores a pointer to the processor array in `*out` (non-NULL `out`
required) and returns the count, at most `ACPI_MAX_CPUS`. Only entries
flagged enabled or online capable are included (invariant AC7), in MADT
order.
The array is owned by the driver and never changes after boot.
Callers: `smp_init` (`kernel/core/smp.c`), the `smp-online` self-test.

### `size_t acpi_madt_ioapics(const struct acpi_madt_ioapic **out)`
As above for I/O APICs, at most `ACPI_MAX_IOAPICS`. Caller:
`kernel/arch/x86_64/irqc.c`.

### `size_t acpi_madt_overrides(const struct acpi_madt_override **out)`
As above for interrupt source overrides, at most `ACPI_MAX_OVERRIDES`.
Caller: `irq_legacy_to_gsi` (`kernel/interrupt/irq.c`).

### `bool acpi_madt_gic(struct acpi_gic *out)`
Copies the decoded `struct acpi_gic` into `*out` (non-NULL required)
whether or not a distributor was found, and returns true only when the
MADT had a GIC distributor (type 12) entry. A false return can still
leave GICC-derived fields non-zero in `*out`; the only caller,
`arch_irqc_init` in `kernel/arch/aarch64/irqc.c`, then replaces the
whole struct with the QEMU `virt` defaults.

### `const void *acpi_map(paddr_t pa, size_t len)`
Purpose: a kernel virtual pointer to physical `[pa, pa+len)`. Returns
`phys_to_virt(pa)` when the first and last byte are RAM in the boot
memory map; otherwise a new read-only write-back window from
`vm_map_phys` over the page-rounded range, offset to `pa`. Returns NULL
for `len == 0`, for a range whose end wraps, or when `vm_map_phys`
fails. The window is never unmapped (there is no `acpi_unmap`); each
call outside RAM consumes kernel arena space and a `vm_region`. Takes
`kernel_space.lock` with interrupts saved and may allocate on that
path; requires `vmm_init` to have run (`vm_map_phys` asserts it). Not
for interrupt context on the non-RAM path. Only `acpi.c` calls it.
