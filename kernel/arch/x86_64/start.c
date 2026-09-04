/*
 * start.c - x86-64 boot CPU initialisation.
 *
 * Order matters and is the whole content of this file:
 *   1. console, so every later failure can be seen
 *   2. GDT + TSS, so the IDT can reference the double-fault IST
 *   3. IDT, so any fault from here on is reported instead of triple-faulting
 *   4. PIC remap + mask, so nothing spurious lands on an exception vector
 *   5. CPU feature identification and protection features
 * then hand over to the generic kernel.
 */

#include <kernel/kernel.h>
#include <kernel/log.h>

#include <arch/console.h>

#include <x86/cpu.h>
#include <x86/gdt.h>
#include <x86/idt.h>
#include <x86/pic.h>

void x86_start(const struct cosmoboot_info *info)
{
    arch_console_early_init();
    kdebug("x86: console up");

    gdt_init();
    kdebug("x86: GDT/TSS loaded");

    idt_init();
    kdebug("x86: IDT loaded");

    pic_init_masked();
    kdebug("x86: legacy PIC masked");

    x86_cpu_init();
    const struct x86_cpu_info *c = x86_cpu_info();
    kdebug("x86: %s family %u model %u stepping %u", c->vendor, c->family, c->model, c->stepping);
    kdebug("x86: nx=%d smep=%d smap=%d umip=%d pge=%d apic=%d x2apic=%d",
           c->has_nx, c->has_smep, c->has_smap, c->has_umip, c->has_pge, c->has_apic, c->has_x2apic);
    kdebug("x86: cr0=0x%llx cr3=0x%llx cr4=0x%llx efer=0x%llx",
           (unsigned long long)read_cr0(), (unsigned long long)read_cr3(),
           (unsigned long long)read_cr4(), (unsigned long long)rdmsr(MSR_EFER));

    kernel_main(info);
}
