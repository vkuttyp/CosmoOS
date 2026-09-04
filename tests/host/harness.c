/*
 * harness.c - Host implementations of the kernel services the memory
 * algorithms depend on: panic (as longjmp), logging (to stdout), and a
 * page-frame arena managed by the real buddy allocator.
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#include <kernel/log.h>
#include <kernel/page.h>
#include <kernel/panic.h>
#include <kernel/pmm.h>
#include <kernel/printf.h>

#include "buddy.h"
#include "harness.h"

int harness_failures;
jmp_buf harness_panic_jmp;
bool harness_expect_panic;
char harness_last_panic[256];

/* --- kernel services --- */

void panic(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(harness_last_panic, sizeof(harness_last_panic), fmt, ap);
    va_end(ap);

    if (harness_expect_panic) {
        harness_release_all_locks();
        longjmp(harness_panic_jmp, 1);
    }

    fprintf(stderr, "\nunexpected KERNEL PANIC: %s\n", harness_last_panic);
    abort();
}

void panic_frame(const struct arch_trap_frame *frame, const char *fmt, ...)
{
    (void)frame;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(harness_last_panic, sizeof(harness_last_panic), fmt, ap);
    va_end(ap);
    if (harness_expect_panic) {
        harness_release_all_locks();
        longjmp(harness_panic_jmp, 1);
    }
    fprintf(stderr, "\nunexpected KERNEL PANIC: %s\n", harness_last_panic);
    abort();
}

void backtrace_print(const struct arch_trap_frame *from)
{
    (void)from;
}

void klog(enum klog_level level, const char *fmt, ...)
{
    if (level < KLOG_WARN)
        return;
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
}

void kprintf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
}

int ksnprintf(char *buf, size_t size, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, size, fmt, ap);
    va_end(ap);
    return n;
}

/* --- page-frame arena --- */

struct page *pmm_page_array;
pfn_t pmm_max_pfn;
vaddr_t pmm_hhdm_base;
paddr_t pmm_hhdm_limit;

static void *g_arena;
static size_t g_arena_bytes;
static struct pmm_zone g_zone;

void host_arena_init(size_t bytes)
{
    bytes = (bytes + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    g_arena = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (g_arena == MAP_FAILED) {
        perror("mmap");
        exit(2);
    }
    g_arena_bytes = bytes;

    pmm_max_pfn = bytes / PAGE_SIZE;
    pmm_page_array = calloc(pmm_max_pfn, sizeof(struct page));
    for (pfn_t p = 0; p < pmm_max_pfn; p++) {
        pmm_page_array[p].flags = PG_RESERVED;
        pmm_page_array[p].zone = PMM_ZONE_NORMAL;
    }

    /* Physical address 0 is the arena start; the "direct map" is the
     * identity, so phys_to_virt(pa) == arena + pa. */
    pmm_hhdm_base = (vaddr_t)g_arena;
    pmm_hhdm_limit = bytes;

    buddy_zone_init(&g_zone, "host", 0, pmm_max_pfn);
    arch_irq_state_t s = spin_lock_irqsave(&g_zone.lock);
    buddy_free_range(&g_zone, 0, pmm_max_pfn);
    spin_unlock_irqrestore(&g_zone.lock, s);
}

void host_arena_destroy(void)
{
    free(pmm_page_array);
    pmm_page_array = NULL;
    munmap(g_arena, g_arena_bytes);
    g_arena = NULL;
}

uint64_t host_arena_free_pages(void)
{
    return g_zone.nr_pages_free;
}

struct page *pmm_alloc_pages(unsigned order, unsigned flags)
{
    if (order >= PMM_MAX_ORDER)
        return NULL;
    arch_irq_state_t s = spin_lock_irqsave(&g_zone.lock);
    struct page *page = buddy_alloc_block(&g_zone, order);
    spin_unlock_irqrestore(&g_zone.lock, s);
    if (page && (flags & PMM_FLAGS_ZERO))
        memset(page_to_virt(page), 0, PAGE_SIZE << order);
    return page;
}

void pmm_free_pages(struct page *page, unsigned order)
{
    if (page->flags & (PG_BUDDY | PG_RESERVED))
        panic("host pmm: bad free of pfn %llu", (unsigned long long)page_to_pfn(page));
    if (page->flags & (PG_SLAB | PG_KMALLOC_LARGE | PG_PAGETABLE))
        panic("host pmm: pfn %llu still owned", (unsigned long long)page_to_pfn(page));
    if (page->refcount != 1 || page->order != order)
        panic("host pmm: pfn %llu refcount %u order %u freed as %u",
              (unsigned long long)page_to_pfn(page), page->refcount, page->order, order);
    page->refcount = 0;
    arch_irq_state_t s = spin_lock_irqsave(&g_zone.lock);
    buddy_free_block(&g_zone, page, order);
    spin_unlock_irqrestore(&g_zone.lock, s);
}

/* --- runner --- */

void harness_fail(const char *file, int line, const char *expr)
{
    harness_failures++;
    printf("  FAIL %s:%d: %s\n", file, line, expr);
}

int harness_run(const struct host_test *tests, size_t count)
{
    int total_failures = 0;
    for (size_t i = 0; i < count; i++) {
        harness_failures = 0;
        printf("%-28s ", tests[i].name);
        fflush(stdout);
        tests[i].fn();
        printf("%s\n", harness_failures ? "FAIL" : "ok");
        total_failures += harness_failures;
    }
    printf("%s: %s\n", count ? "host-test" : "host-test (no tests)", total_failures ? "FAIL" : "PASS");
    return total_failures ? 1 : 0;
}
