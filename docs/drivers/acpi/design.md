# ACPI Tables: design

## Data structures

Public records are in `kernel/include/kernel/acpi.h`; the private ones
and all state are in `drivers/acpi/acpi.c`.

```c
struct acpi_sdt_header { char signature[4]; uint32_t length; uint8_t revision, checksum;
                         char oem_id[6], oem_table_id[8]; uint32_t oem_revision,
                         creator_id, creator_revision; } __packed;      /* 36 bytes */
struct acpi_madt_cpu      { uint32_t apic_id, acpi_id; bool x2apic; };
struct acpi_madt_ioapic   { uint8_t id; paddr_t address; uint32_t gsi_base; };
struct acpi_madt_override { uint8_t bus, source; uint32_t gsi; uint16_t flags; };
struct acpi_gic { unsigned version; paddr_t gicd_base, gicc_base, v2m_base;
                  unsigned v2m_spi_base, v2m_spi_count;
                  paddr_t gicr_base; uint64_t gicr_length; paddr_t gicc_gicr_base;
                  paddr_t its_base; unsigned its_id;
                  paddr_t gicv_base, gich_base; unsigned maint_gsiv; };
```

Private to `acpi.c`: `struct acpi_rsdp` (the 36-byte ACPI 2.0 RSDP),
`struct acpi_madt` (header, `lapic_address`, `flags`), and `struct
madt_entry` (`type`, `length`). The MADT entry types decoded are the
`MADT_*` constants 0, 1, 2, 5, 9, 11, 12, 13, 14 and 15; every other
type is skipped by its length.

State, all file-scope statics in `acpi.c`:

| Variable | Contents |
|---|---|
| `g_tables[ACPI_MAX_TABLES]`, `g_table_count` | Pointers to the checksum-valid tables named by the root table, in root-table order; `ACPI_MAX_TABLES` is 64 |
| `g_lapic_base` | MADT `lapic_address`, replaced by a type 5 (64-bit override) entry if one appears |
| `g_cpus[ACPI_MAX_CPUS]`, `g_cpu_count` | Processor entries (types 0, 9, 11) whose flags have bit 0 or bit 1 set; at most 64 |
| `g_ioapics[ACPI_MAX_IOAPICS]`, `g_ioapic_count` | Type 1 entries; at most 8 |
| `g_overrides[ACPI_MAX_OVERRIDES]`, `g_override_count` | Type 2 entries; at most 24 |
| `g_gic`, `g_gic_present` | The GIC description; `g_gic_present` is set by a type 12 (distributor) entry only |
| `g_available` | Set at the end of `acpi_init`, after the MADT is parsed |

`struct acpi_madt_cpu.apic_id` is the CPU's hardware id, not always an
APIC id: for a type 0 entry it is the 8-bit APIC id, for type 9 the
32-bit x2APIC id, for type 11 (GICC) the MPIDR affinity fields packed
as Aff0-Aff2 in bits 0-23 and Aff3 in bits 24-31. `kernel/core/smp.c`
compares it with `arch_smp_boot_hw_id()` to skip the boot CPU.

For `struct acpi_gic`, each field is written only while it is still
zero, so the first entry giving a non-zero value wins: `gicc_base`
(GICC physical base), `gicc_gicr_base` (GICC redistributor base),
`v2m_base` (MSI frame; its SPI range is copied only when the frame's
flags bit 0 says the entry overrides the frame's own TYPER),
`gicr_base`/`gicr_length` (GICR entry), `its_base`/`its_id` (ITS
entry). `gicv_base`, `gich_base` and `maint_gsiv` are written together
by every GICC entry until one leaves `gich_base` non-zero; on a GICv3,
which reports GICH as 0, the last GICC entry's values remain.
`gicd_base` and `version` are written by every type 12 entry, so the
last one wins.

## Initialisation sequence (`acpi_init`)

1. `bootinfo_get()->acpi_rsdp` must be non-zero. The UEFI loader
   (`find_acpi_rsdp` in `boot/uefi/main.c`) fills it with the ACPI 2.0
   configuration table, falling back to the ACPI 1.0 one.
2. `acpi_map` the RSDP (36 bytes), compare the signature with
   `"RSD PTR "`, verify the checksum over the first 20 bytes.
3. If `revision >= 2` and `xsdt_address != 0`: verify the extended
   checksum over `min(rsdp->length, 36)` bytes and use the XSDT with
   8-byte entries. Otherwise use the RSDT with 4-byte entries; the
   extended checksum is then not checked.
4. `map_table` the root table. Its signature is not compared with
   `"XSDT"`/`"RSDT"`; it only has to have `length >= 36` and a zero
   checksum.
5. For each non-zero entry, `map_table` it and append it to `g_tables`
   while there is room. A table with a bad checksum is logged with
   `kwarn` and skipped; one that cannot be mapped or has a length below
   36 is skipped silently; entries past the 64th are not examined.
6. `acpi_find_table("APIC")` must find the MADT; `parse_madt` decodes
   it; `g_available` is set; one `kinfo` line summarises the result
   (GIC form when `g_gic_present`, LAPIC form otherwise), with extra
   lines for GICv3 redistributor/ITS and GIC virtualisation fields
   when any of them is non-zero.

`map_table` maps the 36-byte header first, reads `length`, maps the
full length and checksums it. `parse_madt` walks entries from offset
44 while two bytes of entry header fit, stops (with a `kwarn`) at an
entry whose length is below 2 or runs past the table end, and reads
each entry's fields only if the entry is long enough for them (for
example 8 bytes for type 0, 16 for type 9, 76 for type 11). All
multi-byte fields are read with `memcpy`, so unaligned entries are
safe.

Tables only reachable through another table (the DSDT and FACS through
the FADT) are not in `g_tables`; `acpi_find_table("DSDT")` returns
NULL. When the root table lists two tables with one signature (several
SSDTs, for instance), `acpi_find_table` returns the first.

## Ownership and lifetime

The driver owns nothing it allocates explicitly. Table pointers are
either direct-map addresses (`phys_to_virt`) or windows created by
`vm_map_phys`; neither is ever unmapped, so every pointer
`acpi_find_table` returns is valid for the life of the kernel. The
memory behind a RAM-resident table stays reserved because
`kernel/memory/pmm.c` and `kernel/memory/bootmem.c` take free memory
only from
`COSMOBOOT_MEM_USABLE` and `COSMOBOOT_MEM_LOADER_RECLAIMABLE` ranges;
`COSMOBOOT_MEM_ACPI_RECLAIMABLE` and `COSMOBOOT_MEM_ACPI_NVS` are never
handed to the allocator.

The decoded arrays are static. `acpi_madt_cpus`, `acpi_madt_ioapics`
and `acpi_madt_overrides` hand out pointers to them; `acpi_madt_gic`
copies `g_gic` into the caller's struct. Callers never free anything.

## Concurrency model

`acpi_init` runs once from `kernel_main` (`kernel/core/main.c`) on the
boot CPU, after `vmm_init` and before `irq_init`, `smp_init` and any
other thread. After it returns nothing writes the state again, so all
accessors read without locks or atomics. Nothing guards against a
second `acpi_init` call; one would append duplicate entries to every
array.

Accessors called before `acpi_init` return the zero-initialised
statics: no tables, count 0, LAPIC base 0, `acpi_madt_gic` false.

## Memory

Static footprint: 64 table pointers, 64 `acpi_madt_cpu`, 8
`acpi_madt_ioapic`, 24 `acpi_madt_override`, one `acpi_gic`. No heap
allocation by the driver itself. `acpi_map` uses the direct map when
the first and last byte of the range are RAM in the boot memory map
(`bootinfo_phys_is_ram`); otherwise it calls `vm_map_phys` with
`VM_PROT_READ` and `VM_CACHE_WB` over the page-rounded range, which
takes `kernel_space.lock` and allocates a `vm_region` record. On a
table outside RAM `map_table` therefore creates two windows (header,
then full length), and the header window is never released.

## Error handling

| Condition | Result |
|---|---|
| `acpi_rsdp == 0` | `panic("acpi: firmware provided no RSDP ...")` |
| RSDP unmappable or wrong signature | `panic("acpi: bad RSDP signature ...")` |
| RSDP checksum (20 bytes) wrong | `panic("acpi: RSDP checksum failed")` |
| Extended checksum wrong (XSDT path) | `panic("acpi: RSDP extended checksum failed")` |
| Root table unmappable, short or bad checksum | `panic("acpi: root table ... is unusable")` |
| No `APIC` table | `panic("acpi: no MADT ...")` |
| Other table with a bad checksum | `kwarn`, table skipped |
| Other table unmappable or `length < 36` | Skipped, no message |
| More than 64 root-table entries | Rest ignored, no message |
| MADT entry length < 2 or past the end | `kwarn`, MADT walk stops; entries already decoded are kept |
| Entry shorter than its fields need | Entry skipped, no message |
| LAPIC/GICC CPU entries past 64 | `kwarn` per extra entry, dropped |
| x2APIC CPU entries past 64 | Dropped, no message |
| I/O APIC entries past 8 | `kwarn`, dropped |
| Interrupt source overrides past 24 | Dropped, no message |

No function returns an error code; the accessors cannot fail.

## Performance

All work happens once at boot: one linear pass over the root table's
entries (each table checksummed byte by byte) and one over the MADT.
`acpi_find_table` is a linear `memcmp` scan of at most 64 pointers; its
callers (`pci.c`, `arm_smmuv3.c`, `intel_vtd.c`, the AArch64 `timer.c`,
`pl011.c` and `smp.c`) each call it once during initialisation.

## Security

Firmware tables are part of the trusted computing base, but their shape
is still checked: checksums on every table before it is recorded,
`acpi_map` refuses a zero length and a range whose end wraps,
`map_table` refuses a length below the header size, and every MADT
entry is checked against the table end and against its own minimum
length before a field is read.

Not checked: the MADT's own fixed fields (`lapic_address`, `flags` at
offsets 36-43) are read without checking `hdr.length >= 44`; the root
table's signature is not compared; and consumers of `acpi_find_table`
must bound their own reads by `hdr.length` (the AArch64 `timer.c`,
`pl011.c` and `smp.c` and `arm_smmuv3.c` compare it with a minimum
length; `pci.c`'s `setup_ecam` subtracts `sizeof(struct acpi_mcfg)`
from `hdr.length` without first checking that it is at least that
large).

The table contents are not reachable from user space: no system call or
device file exposes them.

## Future extensibility

Fuzzing the table parser on the host is listed as remaining work under
"Implementation — missing fuzz/property coverage" in
[docs/plan.md](../../plan.md). Nothing else is planned for this
driver. Not implemented: an AML interpreter, the FADT's power-management
fields, HPET, SRAT/SLIT (NUMA), the DSDT/FACS reached through the
FADT, per-CPU GICR/GICV/GICH frames (only the first entry's are kept),
more than one ITS or MSI frame, and hot-added CPUs (online-capable
entries are recorded but nothing brings them up later). A new table
consumer needs no driver change: it calls `acpi_find_table` with the
signature and parses the table itself, as `drivers/pci/pci.c` (MCFG),
`drivers/iommu/intel_vtd.c` (DMAR) and `drivers/iommu/arm_smmuv3.c`
(IORT) do. A new MADT entry type is added as a `case` in `parse_madt`
with a minimum-length check and a field in one of the static records.
