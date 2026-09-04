/*
 * gdt.c - GDT and TSS for the boot CPU.
 *
 * Memory: the GDT, TSS, and double-fault stack are static. When SMP
 * arrives each CPU gets its own TSS (the GDT can be shared, the TSS
 * descriptor in it cannot), so this file becomes per-CPU data.
 */

#include <kernel/string.h>

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

static struct gdt g_gdt;
static struct tss g_tss __attribute__((aligned(16)));

/* Separate stack for double faults so a kernel stack overflow still gets
 * a readable report instead of a triple fault. */
#define DF_STACK_SIZE 8192
static uint8_t g_df_stack[DF_STACK_SIZE] __attribute__((aligned(16)));

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

void gdt_init(void)
{
    memset(&g_gdt, 0, sizeof(g_gdt));
    g_gdt.kernel_code = DESC_KERNEL_CODE;
    g_gdt.kernel_data = DESC_KERNEL_DATA;
    g_gdt.user_data = DESC_USER_DATA;
    g_gdt.user_code = DESC_USER_CODE;

    memset(&g_tss, 0, sizeof(g_tss));
    g_tss.ist[IST_DOUBLE_FAULT - 1] = (uint64_t)(uintptr_t)(g_df_stack + DF_STACK_SIZE);
    g_tss.iomap_base = sizeof(g_tss); /* no I/O bitmap */
    set_tss_descriptor(&g_gdt, &g_tss);

    struct gdtr gdtr = {
        .limit = sizeof(g_gdt) - 1,
        .base = (uint64_t)(uintptr_t)&g_gdt,
    };

    /* Load GDT, reload CS via a far return, reload data selectors. */
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

void gdt_set_kernel_stack(uint64_t rsp0)
{
    g_tss.rsp[0] = rsp0;
}
