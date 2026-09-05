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

#include <kernel/acpi.h>
#include <kernel/bootarchive.h>
#include <kernel/blk.h>
#include <kernel/bootinfo.h>
#include <kernel/cosmofs.h>
#include <kernel/device.h>
#include <kernel/interrupt.h>
#include <kernel/ipi.h>
#include <kernel/irq.h>
#include <kernel/kernel.h>
#include <kernel/kmalloc.h>
#include <kernel/log.h>
#include <kernel/module.h>
#include <kernel/netif.h>
#include <kernel/pmm.h>
#include <kernel/futex.h>
#include <kernel/hv.h>
#include <kernel/process.h>
#include <kernel/quiesce.h>
#include <kernel/random.h>
#include <kernel/sched.h>
#include <kernel/selftest.h>
#include <kernel/tty.h>
#include <arch/console.h>
#include <kernel/smp.h>
#include <kernel/timer.h>
#include <kernel/vmm.h>
#include <kernel/shutdown.h>
#include <kernel/string.h>
#include <kernel/version.h>
#include <kernel/vfs.h>

#include <arch/cpu.h>
#include <arch/irq.h>

#include <drivers/pci.h>

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
    bootarchive_init();

    interrupt_init();

    /* Memory, in dependency order: frames, then a heap on the bootstrap
     * direct map, then kernel-owned page tables (which need the heap for
     * region records and install the page-fault handler). */
    pmm_init();
    kmalloc_init();
    vmm_init();

    /* Execution: firmware tables, interrupt controllers, the clock and
     * tick, then the scheduler (which adopts this context as thread 0). */
    acpi_init();
    irq_init();
    ipi_init();
    timer_init();
    sched_init();
    quiesce_init();
    tty_init();
    process_init();
    futex_init();
    module_init();

    /* Devices: the model, then the PCI bus (ECAM from ACPI, BARs, MSI
     * capabilities), then the services drivers feed: block registry and
     * the entropy pool. Drivers themselves arrive as boot modules. */
    device_init();
    pci_init();
    blk_init();
    random_init();

    /* The namespace: a ramfs root with the boot archive under /boot. */
    vfs_init();
    cosmofs_init();
    ramfs_populate_boot();

    /* The network stack: mbufs, the worker thread, loopback. NIC drivers
     * are boot modules and register their interfaces when they load. */
    net_init();

    arch_irq_enable();
    kinfo("interrupts enabled");

    /* Virtualization: probe the backend and run its self-check guest (needs interrupts). */
    hv_init();
    arch_console_input_init();

    /* Bring up the other CPUs now that this one can take interrupts:
     * the shootdowns and cross-CPU calls bring-up needs require it. */
    smp_init();

    /* Boot-time kernel modules from the archive, before the self-tests
     * (which load and unload their own fixtures) and before init. */
    int failed = (int)module_load_boot();
#if CONFIG_SELFTEST
    failed += selftest_run_all();
#else
    kinfo("self-tests disabled in this build");
#endif

#if CONFIG_CRASH_TEST
    /* Deliberate page fault on a canonical but unmapped address. Proves
     * the exception path, the panic report, and the harness's failure
     * detection. Built only with CRASH_TEST=1 (make test-crash). Runs
     * before init: since Phase 9 init waits for a console shell that the
     * crash harness never types at. */
    kinfo("crash test: writing to an unmapped address on purpose");
    volatile uint64_t *unmapped = (volatile uint64_t *)0xFFFF900000000000ULL;
    *unmapped = 1;
    kerror("crash test: write did not fault; page tables are wrong");
    failed++;
#endif

    /* The first user process: init from the boot archive (the kernel
     * finds it by name; the rest of the archive is the filesystem). */
    const void *image;
    size_t image_size;
    if (bootarchive_find("init", &image, &image_size)) {
        static const char *const argv[] = { "init", NULL };
        struct process *init = NULL;
        int rc = process_create_from_elf(image, image_size, "init", argv, NULL, NULL, &init);
        if (rc == 0) {
            process_set_init(init);
            int status = process_wait_exit(init);
            process_set_init(NULL);
            kinfo("init exited with status %d", status);
            process_put(init);
            if (status != 0)
                failed++;
        } else {
            kerror("cannot start init (%d)", rc);
            failed++;
        }
    } else {
        kwarn("no init in the boot archive: init not started");
    }


    kinfo("boot complete; nothing more to do in this phase");
    kernel_shutdown(failed ? KERNEL_EXIT_FAILURE : KERNEL_EXIT_SUCCESS);
}
