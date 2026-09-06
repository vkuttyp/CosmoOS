/*
 * el2.c - What the kernel does with the EL2 the loader kept for it
 * (docs/kernel/arch/aarch64/design.md, "Exception level 2").
 *
 * The kernel runs at EL1. A stub the loader installed owns EL2 and
 * answers HVC: it can report its version and it can point VBAR_EL2
 * somewhere else, which is how the hypervisor backend will take EL2 over
 * when it exists. Nothing here enters EL2 itself.
 */

#include <kernel/bootinfo.h>
#include <kernel/log.h>

#include <arch/el2.h>

static uint64_t g_stub_phys;
static bool g_have_el2;

/* HVC #0 with the selector in x0; the stub returns in x0. */
static int64_t el2_hvc(uint64_t selector, uint64_t arg)
{
    register uint64_t x0 __asm__("x0") = selector;
    register uint64_t x1 __asm__("x1") = arg;
    __asm__ volatile("hvc #0" : "+r"(x0) : "r"(x1) : "memory", "x2", "x3");
    return (int64_t)x0;
}

void el2_init(const struct cosmoboot_info *info)
{
    g_stub_phys = info->el2_stub_phys;
    if (g_stub_phys == 0) {
        kinfo("el2: firmware handed over at EL1; no EL2 for guests");
        return;
    }
    int64_t version = el2_hvc(EL2_STUB_VERSION_CALL, 0);
    if (version != EL2_STUB_VERSION) {
        kwarn("el2: stub at 0x%llx answered %lld, expected version %u; not used",
              (unsigned long long)g_stub_phys, (long long)version, EL2_STUB_VERSION);
        g_stub_phys = 0;
        return;
    }
    g_have_el2 = true;
    kinfo("el2: stub v%lld at 0x%llx; EL2 available", (long long)version, (unsigned long long)g_stub_phys);
}

bool el2_available(void)
{
    return g_have_el2;
}

uint64_t el2_stub_phys(void)
{
    return g_stub_phys;
}

int el2_set_vectors(uint64_t vbar_phys)
{
    if (!g_have_el2)
        return -1;
    return (int)el2_hvc(EL2_STUB_SET_VECTORS, vbar_phys);
}

int el2_restore_stub_vectors(void)
{
    if (!g_have_el2)
        return -1;
    return (int)el2_hvc(EL2_STUB_RESTORE, 0);
}

int64_t el2_call_raw(uint64_t selector, uint64_t arg)
{
    return el2_hvc(selector, arg);
}
