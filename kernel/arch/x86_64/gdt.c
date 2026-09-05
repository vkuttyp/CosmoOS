/*
 * gdt.c - GDT and TSS, one set per CPU.
 *
 * The GDT layout is identical on every CPU; only the TSS descriptor
 * points at a different TSS, whose IST1 is that CPU's double-fault
 * stack. CPU 0's tables are static so they exist before the heap; APs'
 * tables are allocated by the boot CPU before each AP is started.
 */

#include <kernel/errno.h>
#include <kernel/kmalloc.h>
#include <kernel/panic.h>
#include <kernel/percpu.h>
#include <kernel/string.h>
#include <kernel/vmm.h>

#include <arch/cpu.h>

#include <x86/gdt.h>

/* 64-bit code/data descriptors. Base and limit are ignored in long mode
 * except for the TSS. Flags: P, DPL, S, type; L for 64-bit code. */
#define DESC_KERNEL_CODE 0x00AF9A000000FFFFULL /* P DPL0 S code RX, L */
#define DESC_KERNEL_DATA 0x00CF92000000FFFFULL /* P DPL0 S data RW */
#define DESC_USER_DATA   0x00CFF2000000FFFFULL /* P DPL3 S data RW */
#define DESC_USER_CODE   0x00AFFA000000FFFFULL /* P DPL3 S code RX, L */

struct tss {
    uint32_t reserved0;
    uint64_t rsp[3];       /* stack for ring 0/1/2 on privilege change */
    uint64_t reserved1;
    uint64_t ist[7];       /* interrupt stack table, 1-based in the IDT */
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iomap_base;
} __attribute__((packed));

_Static_assert(sizeof(struct tss) == 104, "TSS size");

struct gdt {
    uint64_t null;
    uint64_t kernel_code;
    uint64_t kernel_data;
    uint64_t user_data;
    uint64_t user_code;
    uint64_t tss_low;
    uint64_t tss_high;
} __attribute__((packed, aligned(16)));

struct gdtr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

/*
 * Separate stacks for the vectors that cannot trust the interrupted
 * context's stack: #DF (a kernel stack overflow), NMI and #MC (may arrive
 * while RSP is still the user's, in the SYSCALL entry/exit windows), #DB
 * (single-stepping through those windows). One set per CPU; the boot
 * CPU's are static so they exist before the heap, the APs' are guarded
 * kernel allocations. The IDT selects them by IST slot (idt.c).
 */
struct x86_cpu_tables {
    struct gdt gdt;
    struct tss tss __attribute__((aligned(16)));
    uintptr_t ist_top[IST_COUNT];   /* index = IST slot - 1 */
};

static struct x86_cpu_tables g_boot_tables __attribute__((aligned(16)));
static uint8_t g_boot_ist_stacks[IST_COUNT][IST_STACK_SIZE] __attribute__((aligned(16)));
static struct x86_cpu_tables *g_tables[CONFIG_MAX_CPUS];

static void set_tss_descriptor(struct gdt *g, const struct tss *t)
{
    uint64_t base = (uint64_t)(uintptr_t)t;
    uint64_t limit = sizeof(*t) - 1;

    /* Type 0x9 = available 64-bit TSS, P, DPL0. */
    g->tss_low = (limit & 0xFFFF) |
                 ((base & 0xFFFFFF) << 16) |
                 (0x89ULL << 40) |
                 (((limit >> 16) & 0xF) << 48) |
                 (((base >> 24) & 0xFF) << 56);
    g->tss_high = (base >> 32) & 0xFFFFFFFF;
}

static void tables_init(struct x86_cpu_tables *t, const uintptr_t ist_top[IST_COUNT])
{
    memset(&t->gdt, 0, sizeof(t->gdt));
    t->gdt.kernel_code = DESC_KERNEL_CODE;
    t->gdt.kernel_data = DESC_KERNEL_DATA;
    t->gdt.user_data = DESC_USER_DATA;
    t->gdt.user_code = DESC_USER_CODE;

    memset(&t->tss, 0, sizeof(t->tss));
    for (unsigned i = 0; i < IST_COUNT; i++) {
        KASSERT((ist_top[i] & 0xF) == 0);
        t->ist_top[i] = ist_top[i];
        t->tss.ist[i] = ist_top[i];
    }
    t->tss.iomap_base = sizeof(t->tss); /* no I/O bitmap */
    set_tss_descriptor(&t->gdt, &t->tss);
}

static void tables_load(struct x86_cpu_tables *t)
{
    struct gdtr gdtr = {
        .limit = sizeof(t->gdt) - 1,
        .base = (uint64_t)(uintptr_t)&t->gdt,
    };

    /* Load GDT, reload CS via a far return, reload data selectors. This
     * resets the GS base: install the per-CPU pointer after this. */
    __asm__ volatile(
        "lgdt %0\n\t"
        "pushq %1\n\t"
        "lea 1f(%%rip), %%rax\n\t"
        "pushq %%rax\n\t"
        "lretq\n\t"
        "1:\n\t"
        "mov %2, %%ax\n\t"
        "mov %%ax, %%ds\n\t"
        "mov %%ax, %%es\n\t"
        "mov %%ax, %%ss\n\t"
        "xor %%eax, %%eax\n\t"
        "mov %%ax, %%fs\n\t"
        "mov %%ax, %%gs\n\t"
        :
        : "m"(gdtr), "i"(GDT_KERNEL_CODE), "i"(GDT_KERNEL_DATA)
        : "rax", "memory");

    __asm__ volatile("ltr %w0" : : "r"(GDT_TSS) : "memory");
}

void gdt_init(void)
{
    uintptr_t tops[IST_COUNT];
    for (unsigned i = 0; i < IST_COUNT; i++)
        tops[i] = (uintptr_t)(g_boot_ist_stacks[i] + IST_STACK_SIZE);
    tables_init(&g_boot_tables, tops);
    g_tables[0] = &g_boot_tables;
    tables_load(&g_boot_tables);
}

int gdt_alloc_cpu(unsigned cpu)
{
    KASSERT(cpu > 0 && cpu < CONFIG_MAX_CPUS);
    if (g_tables[cpu] != NULL)
        return 0;

    struct x86_cpu_tables *t = kmalloc(sizeof(*t), KMEM_ZERO);
    if (t == NULL)
        return -ENOMEM;
    KASSERT(((uintptr_t)t & 0xF) == 0);

    uintptr_t tops[IST_COUNT] = { 0 };
    for (unsigned i = 0; i < IST_COUNT; i++) {
        vaddr_t stack = vm_kernel_alloc(IST_STACK_SIZE, VM_KALLOC_GUARD | VM_KALLOC_POPULATE, VM_PROT_RW);
        if (stack == 0) {
            for (unsigned j = 0; j < i; j++)
                vm_kernel_free(tops[j] - IST_STACK_SIZE);
            kfree(t);
            return -ENOMEM;
        }
        tops[i] = stack + IST_STACK_SIZE;
    }

    tables_init(t, tops);
    g_tables[cpu] = t;
    return 0;
}

uintptr_t gdt_ist_top(unsigned ist)
{
    KASSERT(ist >= 1 && ist <= IST_COUNT);
    return g_tables[arch_cpu_id()]->ist_top[ist - 1];
}

void gdt_init_cpu(unsigned cpu)
{
    KASSERT(cpu < CONFIG_MAX_CPUS && g_tables[cpu] != NULL);
    tables_load(g_tables[cpu]);
}

void gdt_set_kernel_stack(uint64_t rsp0)
{
    struct x86_cpu_tables *t = g_tables[arch_cpu_id()];
    t->tss.rsp[0] = rsp0;
}
