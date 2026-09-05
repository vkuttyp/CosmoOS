/*
 * acpi.c - Static ACPI table walker: RSDP -> XSDT/RSDT -> tables, and a
 * decoded view of the MADT.
 *
 * Every table is checksum-verified before use and every entry length is
 * bounds-checked against its table. Tables are firmware data: trusted in
 * origin, still validated in shape.
 */

#include <kernel/acpi.h>
#include <kernel/bootinfo.h>
#include <kernel/log.h>
#include <kernel/page.h>
#include <kernel/panic.h>
#include <kernel/string.h>
#include <kernel/vmm.h>

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

struct madt_entry {
    uint8_t type;
    uint8_t length;
} __packed;

#define MADT_LAPIC          0
#define MADT_IOAPIC         1
#define MADT_ISO            2
#define MADT_LAPIC_OVERRIDE 5
#define MADT_X2APIC         9
#define MADT_GICC           11
#define MADT_GICD           12
#define MADT_GIC_MSI_FRAME  13

#define MADT_CPU_ENABLED        (1u << 0)
#define MADT_CPU_ONLINE_CAPABLE (1u << 1)

#define ACPI_MAX_TABLES 64

static bool g_available;
static const struct acpi_sdt_header *g_tables[ACPI_MAX_TABLES];
static size_t g_table_count;

static paddr_t g_lapic_base;
static struct acpi_madt_cpu g_cpus[ACPI_MAX_CPUS];
static size_t g_cpu_count;
static struct acpi_madt_ioapic g_ioapics[ACPI_MAX_IOAPICS];
static size_t g_ioapic_count;
static struct acpi_madt_override g_overrides[ACPI_MAX_OVERRIDES];
static size_t g_override_count;
static struct acpi_gic g_gic;
static bool g_gic_present;

static uint8_t checksum(const void *p, size_t len)
{
    const uint8_t *b = p;
    uint8_t sum = 0;
    for (size_t i = 0; i < len; i++)
        sum = (uint8_t)(sum + b[i]);
    return sum;
}

const void *acpi_map(paddr_t pa, size_t len)
{
    if (len == 0)
        return NULL;
    /* Firmware-supplied address and length: refuse a span that wraps. */
    if (pa + len < pa || page_align_up(pa + len) < pa)
        return NULL;
    if (bootinfo_phys_is_ram(pa) && bootinfo_phys_is_ram(pa + len - 1))
        return phys_to_virt(pa);

    paddr_t base = page_align_down(pa);
    size_t span = (size_t)page_align_up(pa + len) - (size_t)base;
    vaddr_t win = vm_map_phys(base, span, VM_PROT_READ, VM_CACHE_WB);
    if (win == 0)
        return NULL;
    return (const void *)(win + (pa - base));
}

/* Map a table header, then remap with its full length if needed. */
static const struct acpi_sdt_header *map_table(paddr_t pa)
{
    const struct acpi_sdt_header *h = acpi_map(pa, sizeof(*h));
    if (h == NULL)
        return NULL;
    if (h->length < sizeof(*h))
        return NULL;
    const struct acpi_sdt_header *full = acpi_map(pa, h->length);
    if (full == NULL)
        return NULL;
    if (checksum(full, full->length) != 0) {
        kwarn("acpi: table %.4s at 0x%llx has a bad checksum; ignored", full->signature,
              (unsigned long long)pa);
        return NULL;
    }
    return full;
}

static void parse_madt(const struct acpi_madt *madt)
{
    g_lapic_base = madt->lapic_address;

    const uint8_t *p = (const uint8_t *)madt + sizeof(*madt);
    const uint8_t *end = (const uint8_t *)madt + madt->hdr.length;

    while (p + sizeof(struct madt_entry) <= end) {
        const struct madt_entry *e = (const struct madt_entry *)p;
        if (e->length < sizeof(*e) || p + e->length > end) {
            kwarn("acpi: MADT entry with bad length %u; stopping", e->length);
            break;
        }

        switch (e->type) {
        case MADT_LAPIC:
            if (e->length >= 8) {
                uint32_t flags;
                memcpy(&flags, p + 4, 4);
                if (flags & (MADT_CPU_ENABLED | MADT_CPU_ONLINE_CAPABLE)) {
                    if (g_cpu_count < ACPI_MAX_CPUS) {
                        g_cpus[g_cpu_count].acpi_id = p[2];
                        g_cpus[g_cpu_count].apic_id = p[3];
                        g_cpus[g_cpu_count].x2apic = false;
                        g_cpu_count++;
                    } else {
                        kwarn("acpi: more than %u CPUs; extra ignored", ACPI_MAX_CPUS);
                    }
                }
            }
            break;
        case MADT_X2APIC:
            if (e->length >= 16) {
                uint32_t apic_id, flags, acpi_id;
                memcpy(&apic_id, p + 4, 4);
                memcpy(&flags, p + 8, 4);
                memcpy(&acpi_id, p + 12, 4);
                if (flags & (MADT_CPU_ENABLED | MADT_CPU_ONLINE_CAPABLE)) {
                    if (g_cpu_count < ACPI_MAX_CPUS) {
                        g_cpus[g_cpu_count].acpi_id = acpi_id;
                        g_cpus[g_cpu_count].apic_id = apic_id;
                        g_cpus[g_cpu_count].x2apic = true;
                        g_cpu_count++;
                    }
                }
            }
            break;
        case MADT_IOAPIC:
            if (e->length >= 12) {
                if (g_ioapic_count < ACPI_MAX_IOAPICS) {
                    uint32_t addr, gsi;
                    memcpy(&addr, p + 4, 4);
                    memcpy(&gsi, p + 8, 4);
                    g_ioapics[g_ioapic_count].id = p[2];
                    g_ioapics[g_ioapic_count].address = addr;
                    g_ioapics[g_ioapic_count].gsi_base = gsi;
                    g_ioapic_count++;
                } else {
                    kwarn("acpi: more than %u IOAPICs; extra ignored", ACPI_MAX_IOAPICS);
                }
            }
            break;
        case MADT_ISO:
            if (e->length >= 10) {
                if (g_override_count < ACPI_MAX_OVERRIDES) {
                    uint32_t gsi;
                    uint16_t flags;
                    memcpy(&gsi, p + 4, 4);
                    memcpy(&flags, p + 8, 2);
                    g_overrides[g_override_count].bus = p[2];
                    g_overrides[g_override_count].source = p[3];
                    g_overrides[g_override_count].gsi = gsi;
                    g_overrides[g_override_count].flags = flags;
                    g_override_count++;
                }
            }
            break;
        case MADT_LAPIC_OVERRIDE:
            if (e->length >= 12) {
                uint64_t addr;
                memcpy(&addr, p + 4, 8);
                g_lapic_base = addr;
            }
            break;
        case MADT_GICC:
            /* GICC: CPU interface number 4, ACPI UID 8, flags 12, parking version 16,
             * performance GSIV 20, parked address 24, physical base 32, GICV 40, GICH 48,
             * VGIC maintenance 56, GICR base 60, MPIDR 68 (76 bytes in ACPI 5.1, 80 in 6.x). */
            if (e->length >= 76) {
                uint32_t flags;
                uint64_t base, mpidr;
                memcpy(&flags, p + 12, 4);
                memcpy(&base, p + 32, 8);
                memcpy(&mpidr, p + 68, 8);
                if (g_gic.gicc_base == 0)
                    g_gic.gicc_base = base;
                if (flags & (MADT_CPU_ENABLED | MADT_CPU_ONLINE_CAPABLE)) {
                    if (g_cpu_count < ACPI_MAX_CPUS) {
                        uint32_t acpi_id;
                        memcpy(&acpi_id, p + 8, 4);
                        g_cpus[g_cpu_count].acpi_id = acpi_id;
                        /* hw_id: the MPIDR affinity fields (Aff0-2 in bits 0-23, Aff3 in 24-31) */
                        g_cpus[g_cpu_count].apic_id = (uint32_t)((mpidr & 0xFFFFFFull) | ((mpidr >> 8) & 0xFF000000ull));
                        g_cpus[g_cpu_count].x2apic = false;
                        g_cpu_count++;
                    } else {
                        kwarn("acpi: more than %u CPUs; extra ignored", ACPI_MAX_CPUS);
                    }
                }
            }
            break;
        case MADT_GICD:
            if (e->length >= 24) {
                uint64_t base;
                memcpy(&base, p + 8, 8);
                g_gic.gicd_base = base;
                g_gic.version = p[20];
                g_gic_present = true;
            }
            break;
        case MADT_GIC_MSI_FRAME:
            if (e->length >= 24) {
                uint64_t base;
                uint32_t flags;
                uint16_t count, sbase;
                memcpy(&base, p + 8, 8);
                memcpy(&flags, p + 16, 4);
                memcpy(&count, p + 20, 2);
                memcpy(&sbase, p + 22, 2);
                if (g_gic.v2m_base == 0) {
                    g_gic.v2m_base = base;
                    if (flags & 1) {
                        g_gic.v2m_spi_count = count;
                        g_gic.v2m_spi_base = sbase;
                    }
                }
            }
            break;
        default:
            break;
        }
        p += e->length;
    }
}

void acpi_init(void)
{
    const struct cosmoboot_info *info = bootinfo_get();

    if (info->acpi_rsdp == 0)
        panic("acpi: firmware provided no RSDP; this kernel requires ACPI");

    const struct acpi_rsdp *rsdp = acpi_map(info->acpi_rsdp, sizeof(*rsdp));
    if (rsdp == NULL || memcmp(rsdp->signature, "RSD PTR ", 8) != 0)
        panic("acpi: bad RSDP signature at 0x%llx", (unsigned long long)info->acpi_rsdp);
    if (checksum(rsdp, 20) != 0)
        panic("acpi: RSDP checksum failed");

    paddr_t root_pa;
    size_t entry_size;
    if (rsdp->revision >= 2 && rsdp->xsdt_address != 0) {
        if (checksum(rsdp, rsdp->length < sizeof(*rsdp) ? rsdp->length : sizeof(*rsdp)) != 0)
            panic("acpi: RSDP extended checksum failed");
        root_pa = rsdp->xsdt_address;
        entry_size = 8;
    } else {
        root_pa = rsdp->rsdt_address;
        entry_size = 4;
    }

    const struct acpi_sdt_header *root = map_table(root_pa);
    if (root == NULL)
        panic("acpi: root table at 0x%llx is unusable", (unsigned long long)root_pa);

    const uint8_t *entries = (const uint8_t *)root + sizeof(*root);
    size_t n = (root->length - sizeof(*root)) / entry_size;
    for (size_t i = 0; i < n && g_table_count < ACPI_MAX_TABLES; i++) {
        paddr_t pa = 0;
        if (entry_size == 8) {
            uint64_t v;
            memcpy(&v, entries + i * 8, 8);
            pa = v;
        } else {
            uint32_t v;
            memcpy(&v, entries + i * 4, 4);
            pa = v;
        }
        if (pa == 0)
            continue;
        const struct acpi_sdt_header *t = map_table(pa);
        if (t != NULL)
            g_tables[g_table_count++] = t;
    }

    const struct acpi_madt *madt = (const struct acpi_madt *)acpi_find_table("APIC");
    if (madt == NULL)
        panic("acpi: no MADT; this kernel requires an interrupt-controller description");
    parse_madt(madt);
    g_available = true;

    if (g_gic_present)
        kinfo("acpi: %.4s rev %u, %zu tables, GICv%u at 0x%llx, %zu CPUs, MSI frame 0x%llx", root->signature,
              rsdp->revision, g_table_count, g_gic.version, (unsigned long long)g_gic.gicd_base, g_cpu_count,
              (unsigned long long)g_gic.v2m_base);
    else
        kinfo("acpi: %.4s rev %u, %zu tables, LAPIC at 0x%llx, %zu CPUs, %zu IOAPICs, %zu overrides",
              root->signature, rsdp->revision, g_table_count, (unsigned long long)g_lapic_base,
              g_cpu_count, g_ioapic_count, g_override_count);
}

bool acpi_available(void)
{
    return g_available;
}

const struct acpi_sdt_header *acpi_find_table(const char *signature)
{
    for (size_t i = 0; i < g_table_count; i++) {
        if (memcmp(g_tables[i]->signature, signature, 4) == 0)
            return g_tables[i];
    }
    return NULL;
}

paddr_t acpi_madt_lapic_base(void)
{
    return g_lapic_base;
}

size_t acpi_madt_cpus(const struct acpi_madt_cpu **out)
{
    *out = g_cpus;
    return g_cpu_count;
}

size_t acpi_madt_ioapics(const struct acpi_madt_ioapic **out)
{
    *out = g_ioapics;
    return g_ioapic_count;
}

size_t acpi_madt_overrides(const struct acpi_madt_override **out)
{
    *out = g_overrides;
    return g_override_count;
}

bool acpi_madt_gic(struct acpi_gic *out)
{
    *out = g_gic;
    return g_gic_present;
}
