/*
 * main.c - Architecture-independent kernel entry.
 *
 * What exists at this point: a valid stack, a working console, the
 * architecture's descriptor tables and exception vectors, interrupts
 * masked. What does not exist yet: memory allocation, threads, timers.
 * kernel_main therefore does only what the first engineering task asks:
 * validate the boot data, announce itself, prove the interrupt path
 * works, and stop cleanly with a verdict.
 */

#include <kernel/bootinfo.h>
#include <kernel/interrupt.h>
#include <kernel/kernel.h>
#include <kernel/kmalloc.h>
#include <kernel/log.h>
#include <kernel/pmm.h>
#include <kernel/selftest.h>
#include <kernel/vmm.h>
#include <kernel/shutdown.h>
#include <kernel/string.h>
#include <kernel/version.h>

#include <arch/cpu.h>
#include <arch/irq.h>

static const char *firmware_name(uint32_t firmware)
{
    switch (firmware) {
    case COSMOBOOT_FIRMWARE_UEFI: return "UEFI";
    default:                      return "unknown";
    }
}

static void print_banner(void)
{
    const struct cosmoboot_info *info = bootinfo_get();
    char cpu[64];
    uint32_t regions;

    arch_cpu_brand_string(cpu, sizeof(cpu));
    bootinfo_mem_map(&regions);

    /* Build type upper-cased for the banner. */
    char build[16];
    strlcpy(build, COSMO_BUILD_TYPE, sizeof(build));
    for (char *p = build; *p; p++) {
        if (*p >= 'a' && *p <= 'z')
            *p = (char)(*p - 'a' + 'A');
    }

    kprintf("\n");
    kprintf("%s kernel %s (build %s)\n", KERNEL_NAME, KERNEL_VERSION, COSMO_BUILD_ID);
    kprintf("Architecture: %s\n", arch_name());
    kprintf("Build: %s\n", build);
    kprintf("Boot: %s (%s v%u, protocol v%u)\n", firmware_name(info->firmware),
            info->loader_name, info->loader_version, info->version);
    kprintf("CPU: %s\n", cpu);
    kprintf("Memory: %llu MiB usable in %u regions, RAM ends at %llu MiB\n",
            (unsigned long long)(bootinfo_usable_bytes() >> 20), regions,
            (unsigned long long)(bootinfo_phys_limit() >> 20));
    kprintf("\n");
}

static void log_memory_map(void)
{
    uint32_t n;
    const struct cosmoboot_mem_entry *map = bootinfo_mem_map(&n);
    const struct cosmoboot_info *info = bootinfo_get();

    kdebug("kernel image: virt %p phys 0x%llx size 0x%llx",
           (void *)(uintptr_t)info->kernel_virt_base,
           (unsigned long long)info->kernel_phys_base,
           (unsigned long long)info->kernel_size);
    kdebug("direct map: virt %p size 0x%llx",
           (void *)(uintptr_t)info->hhdm_base, (unsigned long long)info->hhdm_size);
    kdebug("bootstrap page tables at phys 0x%llx", (unsigned long long)info->boot_pagetable_root);
    if (info->acpi_rsdp)
        kdebug("ACPI RSDP at phys 0x%llx", (unsigned long long)info->acpi_rsdp);

    kdebug("memory map (%u entries):", n);
    for (uint32_t i = 0; i < n; i++) {
        kdebug("  0x%016llx-0x%016llx %-12s %llu KiB",
               (unsigned long long)map[i].base,
               (unsigned long long)(map[i].base + map[i].length),
               bootinfo_mem_type_name(map[i].type),
               (unsigned long long)(map[i].length >> 10));
    }
}

void kernel_main(const struct cosmoboot_info *info)
{
    bootinfo_init(info);
    print_banner();
    log_memory_map();

    interrupt_init();

    /* Memory, in dependency order: frames, then a heap on the bootstrap
     * direct map, then kernel-owned page tables (which need the heap for
     * region records and install the page-fault handler). */
    pmm_init();
    kmalloc_init();
    vmm_init();

    arch_irq_enable();
    kinfo("interrupts enabled");

    int failed = 0;
#if CONFIG_SELFTEST
    failed = selftest_run_all();
#else
    kinfo("self-tests disabled in this build");
#endif

#if CONFIG_CRASH_TEST
    /* Deliberate page fault on a canonical but unmapped address. Proves
     * the exception path, the panic report, and the harness's failure
     * detection. Built only with CRASH_TEST=1 (make test-crash). */
    kinfo("crash test: writing to an unmapped address on purpose");
    volatile uint64_t *unmapped = (volatile uint64_t *)0xFFFF900000000000ULL;
    *unmapped = 1;
    kerror("crash test: write did not fault; page tables are wrong");
    failed++;
#endif

    kinfo("boot complete; nothing more to do in this phase");
    kernel_shutdown(failed ? KERNEL_EXIT_FAILURE : KERNEL_EXIT_SUCCESS);
}
