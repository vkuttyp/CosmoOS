/*
 * acpi_internal.h - The ACPI table walker's private structures, and its
 * decoders of what firmware wrote (roadmap 2.0 SA5, the virtq/USB/ACPI
 * family): the checksum, the RSDP, the root table's entries and the
 * MADT, in CeeSafe (acpi_parse.cee). acpi.c maps the tables, panics and
 * logs; the decoders read only the bytes they are given, each within the
 * length it was checked to have. tests/host/test_acpi.c holds them to a
 * recorded digest.
 */

#ifndef DRIVERS_ACPI_INTERNAL_H
#define DRIVERS_ACPI_INTERNAL_H

#include <kernel/acpi.h>

struct acpi_rsdp {
    char     signature[8];   /* "RSD PTR " */
    uint8_t  checksum;       /* covers first 20 bytes */
    char     oem_id[6];
    uint8_t  revision;
    uint32_t rsdt_address;
    /* revision >= 2 */
    uint32_t length;
    uint64_t xsdt_address;
    uint8_t  ext_checksum;   /* covers `length` bytes */
    uint8_t  reserved[3];
} __packed;

struct acpi_madt {
    struct acpi_sdt_header hdr;
    uint32_t lapic_address;
    uint32_t flags;
    /* variable entries follow */
} __packed;

/* What the MADT says, decoded: the accessors' answers, and what the walk
 * found wrong, for acpi.c to log. */
struct acpi_madt_state {
    paddr_t lapic_base;
    struct acpi_madt_cpu cpus[ACPI_MAX_CPUS];
    size_t cpu_count;
    struct acpi_madt_ioapic ioapics[ACPI_MAX_IOAPICS];
    size_t ioapic_count;
    struct acpi_madt_override overrides[ACPI_MAX_OVERRIDES];
    size_t override_count;
    struct acpi_gic gic;
    bool gic_present;
    bool too_short;          /* shorter than its fixed fields: nothing decoded */
    bool stopped;            /* the walk stopped at an entry whose length is wrong: */
    unsigned bad_entry_len;  /* that length */
    unsigned extra_cpus;     /* enabled CPUs past ACPI_MAX_CPUS, ignored */
    unsigned extra_ioapics;  /* IOAPICs past ACPI_MAX_IOAPICS, ignored */
};

/* Why an RSDP is refused. */
enum { ACPI_RSDP_OK = 0, ACPI_RSDP_SIGNATURE, ACPI_RSDP_CHECKSUM, ACPI_RSDP_EXT_CHECKSUM };

/* The byte sum of `len` bytes: 0 for a table that checks. */
uint8_t acpi_checksum(const void *p, size_t len);
/* The RSDP's signature and checksums; on ACPI_RSDP_OK the root table's
 * address and the width of its entries (8 for an XSDT, 4 for an RSDT). */
int acpi_rsdp_decode(const struct acpi_rsdp *rsdp, paddr_t *root, unsigned *entry_size);
/* A checked root table's entries: how many, and the i-th table's address. */
size_t acpi_root_count(const struct acpi_sdt_header *root, unsigned entry_size);
paddr_t acpi_root_entry(const struct acpi_sdt_header *root, unsigned entry_size, size_t i);
/* The MADT (hdr.length bytes, checksummed) into `st`, which starts zeroed. */
void acpi_madt_decode(const struct acpi_madt *madt, struct acpi_madt_state *st);

#endif /* DRIVERS_ACPI_INTERNAL_H */
