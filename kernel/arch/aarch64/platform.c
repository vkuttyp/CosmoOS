/*
 * platform.c - The boot-time platform line (docs/kernel/arch/aarch64/
 * design.md, "Platform discovery").
 *
 * One line, logged as soon as the firmware tables are parsed and before
 * anything is driven from them: who made the firmware, which console the
 * kernel chose and from where, the GIC it will drive and where, the timer
 * interrupts and the PSCI conduit. A machine that then fails is
 * attributable from that line and the next. Everything in it comes from
 * the boot info and the ACPI tables; nothing is assumed.
 */

#include <kernel/acpi.h>
#include <kernel/bootinfo.h>
#include <kernel/fbcon.h>
#include <kernel/log.h>
#include <kernel/printf.h>
#include <aarch64/platform.h>

#include <cosmoboot.h>

void aarch64_platform_report(void)
{
    char oem[7];
    if (!acpi_rsdp_oem(oem))
        oem[0] = '\0';
    bool qemu = acpi_platform_is_qemu();

    char console[64];
    paddr_t base;
    unsigned intid;
    const char *kind;
    const struct cosmoboot_info *info = bootinfo_get();
    if (aarch64_console_serial(&base, &intid, &kind)) {
        const char *from = (info->console_flags & COSMOBOOT_CONSOLE_FROM_DBG2) ? "DBG2" : "SPCR";
        if (intid != 0)
            ksnprintf(console, sizeof(console), "%s at 0x%llx (%s, IRQ %u)", kind, (unsigned long long)base, from,
                      intid);
        else
            ksnprintf(console, sizeof(console), "%s at 0x%llx (%s, polled)", kind, (unsigned long long)base,
                      from);
    } else {
        ksnprintf(console, sizeof(console), "%s", fbcon_present() ? "framebuffer only" : "none");
    }

    char gic[96];
    struct acpi_gic g;
    if (acpi_madt_gic(&g)) {
        if (g.version >= 3)
            ksnprintf(gic, sizeof(gic), "GICv%u GICD 0x%llx GICR 0x%llx ITS 0x%llx MSI frame 0x%llx", g.version,
                      (unsigned long long)g.gicd_base,
                      (unsigned long long)(g.gicr_base ? g.gicr_base : g.gicc_gicr_base),
                      (unsigned long long)g.its_base, (unsigned long long)g.v2m_base);
        else
            ksnprintf(gic, sizeof(gic), "GICv%u GICD 0x%llx GICC 0x%llx MSI frame 0x%llx", g.version ? g.version : 2,
                      (unsigned long long)g.gicd_base, (unsigned long long)g.gicc_base,
                      (unsigned long long)g.v2m_base);
    } else {
        ksnprintf(gic, sizeof(gic), "no GIC in the MADT");
    }

    aarch64_timer_read_gtdt();
    unsigned tphys, tvirt;
    aarch64_timer_intids(&tphys, &tvirt);
    int conduit = aarch64_psci_conduit();

    kinfo("platform: firmware OEM '%s'%s; console %s; %s; timer PPIs %u (physical) %u (virtual); PSCI %s", oem,
          qemu ? " (QEMU)" : "", console, gic, tphys, tvirt,
          conduit == 1 ? "HVC" : conduit == 0 ? "SMC" : "none");
}
