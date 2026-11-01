/*
 * acpi.c - Static ACPI table walker: RSDP -> XSDT/RSDT -> tables, and a
 * decoded view of the MADT.
 *
 * Every table is checksum-verified before use and every entry length is
 * bounds-checked against its table. Tables are firmware data: trusted in
 * origin, still validated in shape. What is decoded from their bytes --
 * the checksum, the RSDP, the root table's entries, the MADT -- is
 * acpi_parse (acpi_internal.h); this file maps, panics and logs.
 */

#include <kernel/acpi.h>
#include <kernel/bootinfo.h>
#include <kernel/log.h>
#include <kernel/page.h>
#include <kernel/panic.h>
#include <kernel/string.h>
#include <kernel/vmm.h>

#include "acpi_internal.h"

#define ACPI_MAX_TABLES 64

static bool g_available;
static const struct acpi_sdt_header *g_tables[ACPI_MAX_TABLES];
static size_t g_table_count;

static struct acpi_madt_state g_madt;

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
    if (acpi_checksum(full, full->length) != 0) {
        kwarn("acpi: table %.4s at 0x%llx has a bad checksum; ignored", full->signature,
              (unsigned long long)pa);
        return NULL;
    }
    return full;
}

static void parse_madt(const struct acpi_madt *madt)
{
    acpi_madt_decode(madt, &g_madt);
    if (g_madt.too_short)
        kwarn("acpi: MADT of %u bytes is shorter than its fixed fields; ignored", madt->hdr.length);
    if (g_madt.stopped)
        kwarn("acpi: MADT entry with bad length %u; stopping", g_madt.bad_entry_len);
    if (g_madt.extra_cpus)
        kwarn("acpi: more than %u CPUs; %u extra ignored", ACPI_MAX_CPUS, g_madt.extra_cpus);
    if (g_madt.extra_ioapics)
        kwarn("acpi: more than %u IOAPICs; %u extra ignored", ACPI_MAX_IOAPICS, g_madt.extra_ioapics);
}

/* Read once, the first time it is asked -- which kernel_main arranges to
 * be before the kernel's own page tables, whose direct map covers RAM
 * alone -- and kept: an RSDP outside RAM is not mapped afterwards. */
static char g_oem[7];
static int g_oem_state;   /* 0 unread, 1 read, -1 absent */

bool acpi_rsdp_oem(char out[7])
{
    if (g_oem_state != 0) {
        memcpy(out, g_oem, 7);
        return g_oem_state > 0;
    }
    out[0] = '\0';
    if (!bootinfo_ready())
        return false;   /* asked from a panic before bootinfo_init: unknown, and not cached */
    g_oem_state = -1;
    const struct cosmoboot_info *info = bootinfo_get();
    /* An RSDP outside the direct map is not QEMU's (its tables are low):
     * no OEM, rather than the panic bootinfo_phys_to_virt would raise. */
    if (info->acpi_rsdp == 0 || info->acpi_rsdp + 36 > info->hhdm_size)
        return false;
    /* Byte reads: before the kernel's own tables the direct map may give
     * a non-RAM page device attributes, where wider unaligned reads fault. */
    const volatile uint8_t *p = bootinfo_phys_to_virt(info->acpi_rsdp);
    static const char sig[] = "RSD PTR ";
    for (unsigned i = 0; i < 8; i++)
        if (p[i] != (uint8_t)sig[i])
            return false;
    for (unsigned i = 0; i < 6; i++)
        g_oem[i] = (char)p[9 + i];
    g_oem[6] = '\0';
    g_oem_state = 1;
    memcpy(out, g_oem, 7);
    return true;
}

bool acpi_platform_is_qemu(void)
{
    char oem[7];
    return acpi_rsdp_oem(oem) && memcmp(oem, "BOCHS ", 6) == 0;
}

void acpi_init(void)
{
    const struct cosmoboot_info *info = bootinfo_get();

    if (info->acpi_rsdp == 0)
        panic("acpi: firmware provided no RSDP; this kernel requires ACPI");

    const struct acpi_rsdp *rsdp = acpi_map(info->acpi_rsdp, sizeof(*rsdp));
    paddr_t root_pa = 0;
    unsigned entry_size = 0;
    switch (rsdp == NULL ? ACPI_RSDP_SIGNATURE : acpi_rsdp_decode(rsdp, &root_pa, &entry_size)) {
    case ACPI_RSDP_SIGNATURE:
        panic("acpi: bad RSDP signature at 0x%llx", (unsigned long long)info->acpi_rsdp);
    case ACPI_RSDP_CHECKSUM:
        panic("acpi: RSDP checksum failed");
    case ACPI_RSDP_EXT_CHECKSUM:
        panic("acpi: RSDP extended checksum failed");
    default:
        break;
    }

    const struct acpi_sdt_header *root = map_table(root_pa);
    if (root == NULL)
        panic("acpi: root table at 0x%llx is unusable", (unsigned long long)root_pa);

    size_t n = acpi_root_count(root, entry_size);
    for (size_t i = 0; i < n && g_table_count < ACPI_MAX_TABLES; i++) {
        paddr_t pa = acpi_root_entry(root, entry_size, i);
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

    if (g_madt.gic_present) {
        kinfo("acpi: %.4s rev %u, %zu tables, GICv%u at 0x%llx, %zu CPUs, MSI frame 0x%llx", root->signature,
              rsdp->revision, g_table_count, g_madt.gic.version, (unsigned long long)g_madt.gic.gicd_base,
              g_madt.cpu_count, (unsigned long long)g_madt.gic.v2m_base);
        /* What a GICv3 needs, whether or not this kernel can drive it
         * yet: where the redistributors are and whether there is an ITS
         * to translate an MSI. Logged so that a boot on such a machine
         * says what it found before it decides what to do about it. */
        if (g_madt.gic.gicr_base || g_madt.gic.gicc_gicr_base || g_madt.gic.its_base)
            kinfo("acpi: GICR window 0x%llx+0x%llx, GICC redistributor 0x%llx, ITS %u at 0x%llx",
                  (unsigned long long)g_madt.gic.gicr_base, (unsigned long long)g_madt.gic.gicr_length,
                  (unsigned long long)g_madt.gic.gicc_gicr_base, g_madt.gic.its_id,
                  (unsigned long long)g_madt.gic.its_base);
        /* What a hypervisor needs to give a guest interrupts. A GICv3
         * reports no GICH and no GICV -- it has EL2 system registers
         * instead -- but does report the maintenance GSIV, which is a
         * PPI on both. Saying what firmware offered is the first step in
         * deciding what can be done with it. */
        if (g_madt.gic.gich_base || g_madt.gic.gicv_base || g_madt.gic.maint_gsiv)
            kinfo("acpi: GICH 0x%llx, GICV 0x%llx, VGIC maintenance GSIV %u",
                  (unsigned long long)g_madt.gic.gich_base, (unsigned long long)g_madt.gic.gicv_base,
                  g_madt.gic.maint_gsiv);
    }
    else
        kinfo("acpi: %.4s rev %u, %zu tables, LAPIC at 0x%llx, %zu CPUs, %zu IOAPICs, %zu overrides",
              root->signature, rsdp->revision, g_table_count, (unsigned long long)g_madt.lapic_base,
              g_madt.cpu_count, g_madt.ioapic_count, g_madt.override_count);
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
    return g_madt.lapic_base;
}

size_t acpi_madt_cpus(const struct acpi_madt_cpu **out)
{
    *out = g_madt.cpus;
    return g_madt.cpu_count;
}

size_t acpi_madt_ioapics(const struct acpi_madt_ioapic **out)
{
    *out = g_madt.ioapics;
    return g_madt.ioapic_count;
}

size_t acpi_madt_overrides(const struct acpi_madt_override **out)
{
    *out = g_madt.overrides;
    return g_madt.override_count;
}

bool acpi_madt_gic(struct acpi_gic *out)
{
    *out = g_madt.gic;
    return g_madt.gic_present;
}
