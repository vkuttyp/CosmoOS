/*
 * fpu.c - x87/SSE/AVX state: CPU policy, per-thread areas, eager switching
 * (docs/kernel/arch/design.md, "FPU and SIMD state").
 *
 * Policy, asserted on every CPU (the boot CPU inherits whatever the
 * firmware left, the APs start with CR4 = PAE only): CR0.EM = 0, CR0.MP =
 * 1, CR0.NE = 1, CR0.TS = 0; CR4.OSFXSR = 1 and CR4.OSXMMEXCPT = 1 (SSE is
 * the x86-64 baseline); with XSAVE, CR4.OSXSAVE = 1 and XCR0 = the
 * components this kernel saves, identical on every CPU so a thread's area
 * has one layout wherever it runs.
 *
 * Ownership is explicit: a thread owns state iff t->fpu != NULL. The
 * switch hook (context.c) saves the outgoing owner's registers into its
 * area and restores the incoming owner's; kernel threads own nothing and
 * are neither saved nor restored, so the registers they run with are
 * stale and must never be read or written by kernel code
 * (-mgeneral-regs-only). A guest entry (svm.c) saves the owner, runs the
 * guest from its own area, then restores the owner.
 */

#include <kernel/errno.h>
#include <kernel/kmalloc.h>
#include <kernel/log.h>
#include <kernel/panic.h>
#include <kernel/percpu.h>
#include <kernel/sched.h>
#include <kernel/string.h>
#include <kernel/thread.h>

#include <arch/cpu.h>
#include <arch/fpu.h>
#include <arch/testhooks.h>

#include <x86/cpu.h>
#include <x86/fpu.h>

#define CR0_MP (1ull << 1)
#define CR0_EM (1ull << 2)
#define CR0_TS (1ull << 3)
#define CR0_NE (1ull << 5)
#define CR4_OSFXSR     (1ull << 9)
#define CR4_OSXMMEXCPT (1ull << 10)
#define CR4_OSXSAVE    (1ull << 18)

struct arch_fpu_state {
    void *raw;          /* the kmalloc block */
    uint8_t *area;      /* 64-byte aligned, g_fpu.area_size bytes */
};

static struct x86_fpu_info g_fpu;
static bool g_probed;
static uint8_t g_reset_image[X86_FPU_AREA_MAX] __attribute__((aligned(64)));

static void probe(void)
{
    struct cpuid_regs r;
    cpuid(1, 0, &r);
    g_fpu.xsave = (r.ecx & (1u << 26)) != 0;
    g_fpu.area_size = X86_FXSAVE_SIZE;
    if (g_fpu.xsave) {
        cpuid(0xD, 0, &r);
        g_fpu.xcr0_supported = ((uint64_t)r.edx << 32) | r.eax;
        g_fpu.xcr0 = g_fpu.xcr0_supported & XCR0_SUPPORTED_BY_KERNEL;
        /* AVX-512 is three components that are enabled together or not at all. */
        uint64_t avx512 = XCR0_OPMASK | XCR0_ZMM_HI256 | XCR0_HI16_ZMM;
        if ((g_fpu.xcr0 & avx512) != avx512)
            g_fpu.xcr0 &= ~avx512;
        if ((g_fpu.xcr0 & (XCR0_X87 | XCR0_SSE)) != (XCR0_X87 | XCR0_SSE))
            panic("fpu: XSAVE without x87/SSE state components (XCR0 support 0x%llx)",
                  (unsigned long long)g_fpu.xcr0_supported);
        cpuid(0xD, 1, &r);
        g_fpu.xsaveopt = (r.eax & 1u) != 0;
    }
    g_probed = true;
}

void x86_fpu_init_cpu(void)
{
    if (!g_probed)
        probe();   /* the boot CPU, alone, before any AP exists */

    uint64_t cr0 = read_cr0();
    cr0 &= ~(CR0_EM | CR0_TS);
    cr0 |= CR0_MP | CR0_NE;
    write_cr0(cr0);

    uint64_t cr4 = read_cr4() | CR4_OSFXSR | CR4_OSXMMEXCPT;
    if (g_fpu.xsave)
        cr4 |= CR4_OSXSAVE;
    write_cr4(cr4);

    if (g_fpu.xsave) {
        xsetbv(0, g_fpu.xcr0);
        /* The area size depends on the enabled XCR0: read it now, on the
         * boot CPU, and require every CPU to agree. */
        struct cpuid_regs r;
        cpuid(0xD, 0, &r);
        unsigned size = (r.ebx + 63u) & ~63u;
        if (size > X86_FPU_AREA_MAX)
            panic("fpu: XSAVE area of %u bytes exceeds the %u the kernel provides", size, X86_FPU_AREA_MAX);
        if (this_cpu()->cpu_id == 0) {
            g_fpu.area_size = size;
        } else if (size != g_fpu.area_size) {
            panic("fpu: CPU %u XSAVE area is %u bytes, CPU 0's is %u: heterogeneous CPUs are not supported",
                  this_cpu()->cpu_id, size, g_fpu.area_size);
        }
    }

    if (this_cpu()->cpu_id == 0) {
        x86_fpu_area_init(g_reset_image);
        kdebug("fpu: %s, %u-byte state%s, xcr0 0x%llx", g_fpu.xsave ? "xsave" : "fxsave", g_fpu.area_size,
               g_fpu.xsaveopt ? " (xsaveopt)" : "", (unsigned long long)g_fpu.xcr0);
    }
}

const struct x86_fpu_info *x86_fpu_info(void)
{
    return &g_fpu;
}

const void *x86_fpu_reset_image(void)
{
    return g_reset_image;
}

void x86_fpu_area_save(void *area)
{
    KASSERT(((uintptr_t)area & 63) == 0);
    if (g_fpu.xsave) {
        uint32_t lo = (uint32_t)g_fpu.xcr0, hi = (uint32_t)(g_fpu.xcr0 >> 32);
        /* Plain XSAVE, not XSAVEOPT: the area is restored on another CPU
         * or after another thread's state was live in between, which is
         * exactly what XSAVEOPT's "unchanged since the last XRSTOR"
         * optimisation cannot see. */
        __asm__ volatile("xsave64 (%0)" : : "r"(area), "a"(lo), "d"(hi) : "memory");
    } else {
        __asm__ volatile("fxsave64 (%0)" : : "r"(area) : "memory");
    }
}

void x86_fpu_area_restore(const void *area)
{
    KASSERT(((uintptr_t)area & 63) == 0);
    if (g_fpu.xsave) {
        uint32_t lo = (uint32_t)g_fpu.xcr0, hi = (uint32_t)(g_fpu.xcr0 >> 32);
        __asm__ volatile("xrstor64 (%0)" : : "r"(area), "a"(lo), "d"(hi) : "memory");
    } else {
        __asm__ volatile("fxrstor64 (%0)" : : "r"(area) : "memory");
    }
}

void x86_fpu_area_init(void *area)
{
    /* Legacy region: FCW 0x37F (all exceptions masked, 64-bit precision),
     * FTW 0 (empty tags in the abridged FXSAVE encoding), MXCSR 0x1F80
     * (exceptions masked), everything else zero. With XSAVE the header's
     * XSTATE_BV = 0 tells XRSTOR every component is in its initial state,
     * which agrees with the legacy bytes. */
    uint8_t *a = area;
    memset(a, 0, g_fpu.area_size);
    uint16_t fcw = 0x37F;
    uint32_t mxcsr = 0x1F80;
    memcpy(a + 0, &fcw, sizeof(fcw));
    memcpy(a + 24, &mxcsr, sizeof(mxcsr));
}

/* --- arch/fpu.h --- */

int arch_fpu_alloc(struct thread *t)
{
    if (t->fpu != NULL)
        return 0;
    struct arch_fpu_state *st = kmalloc(sizeof(*st), KMEM_ZERO);
    if (st == NULL)
        return -ENOMEM;
    st->raw = kmalloc(g_fpu.area_size + 64, 0);
    if (st->raw == NULL) {
        kfree(st);
        return -ENOMEM;
    }
    st->area = (uint8_t *)(((uintptr_t)st->raw + 63) & ~(uintptr_t)63);
    memcpy(st->area, g_reset_image, g_fpu.area_size);
    t->fpu = st;
    return 0;
}

void arch_fpu_free(struct thread *t)
{
    struct arch_fpu_state *st = t->fpu;
    if (st == NULL)
        return;
    t->fpu = NULL;
    kfree(st->raw);
    kfree(st);
}

size_t arch_fpu_state_size(void)
{
    return g_fpu.area_size;
}

/* --- switching --- */

/* Called by arch_thread_switch_prepare with the run-queue lock held and
 * interrupts disabled: the registers hold `prev`'s values, `next` runs
 * next. Two owners: save then restore. One owner: only its half. None:
 * nothing (kernel threads never touch the registers). */
void x86_fpu_switch(struct thread *prev, struct thread *next)
{
    if (prev != NULL && prev->fpu != NULL)
        x86_fpu_area_save(prev->fpu->area);
    if (next->fpu != NULL)
        x86_fpu_area_restore(next->fpu->area);
}

bool x86_fpu_save_current(void)
{
    struct thread *t = thread_current();
    if (t == NULL || t->fpu == NULL)
        return false;
    x86_fpu_area_save(t->fpu->area);
    return true;
}

bool x86_fpu_restore_current(void)
{
    struct thread *t = thread_current();
    if (t == NULL || t->fpu == NULL)
        return false;
    x86_fpu_area_restore(t->fpu->area);
    return true;
}

bool x86_fpu_legacy_get(void *out)
{
    struct thread *t = thread_current();
    if (t == NULL || t->fpu == NULL)
        return false;
    arch_irq_state_t s = arch_irq_save();
    x86_fpu_area_save(t->fpu->area);
    memcpy(out, t->fpu->area, X86_FXSAVE_SIZE);
    arch_irq_restore(s);
    return true;
}

bool x86_fpu_legacy_set(const void *in)
{
    struct thread *t = thread_current();
    if (t == NULL || t->fpu == NULL)
        return false;
    uint8_t image[X86_FXSAVE_SIZE];
    memcpy(image, in, sizeof(image));
    uint32_t mxcsr;
    memcpy(&mxcsr, image + 24, 4);
    mxcsr &= 0xFFFFu;   /* bits 16-31 are reserved; a set one makes FXRSTOR/XRSTOR fault */
    memcpy(image + 24, &mxcsr, 4);
    arch_irq_state_t s = arch_irq_save();
    x86_fpu_area_save(t->fpu->area);   /* the other components' live values */
    memcpy(t->fpu->area, image, X86_FXSAVE_SIZE);
    if (g_fpu.xsave) {
        uint64_t bv;
        memcpy(&bv, t->fpu->area + X86_FXSAVE_SIZE, 8);
        bv |= 0x3;   /* x87 and SSE are in the legacy region now */
        memcpy(t->fpu->area + X86_FXSAVE_SIZE, &bv, 8);
    }
    x86_fpu_area_restore(t->fpu->area);
    arch_irq_restore(s);
    return true;
}

/* --- arch/testhooks.h: register-state isolation under test --- */

#define FPU_PROBE_ROUNDS 400

struct fpu_probe {
    uint8_t seed;
    int rc;
    unsigned corrupt;
};

static void xmm_load(const uint8_t r[16][16])
{
    __asm__ volatile("movdqu 0(%0), %%xmm0\n\t"
                     "movdqu 16(%0), %%xmm1\n\t"
                     "movdqu 32(%0), %%xmm2\n\t"
                     "movdqu 48(%0), %%xmm3\n\t"
                     "movdqu 64(%0), %%xmm4\n\t"
                     "movdqu 80(%0), %%xmm5\n\t"
                     "movdqu 96(%0), %%xmm6\n\t"
                     "movdqu 112(%0), %%xmm7\n\t"
                     "movdqu 128(%0), %%xmm8\n\t"
                     "movdqu 144(%0), %%xmm9\n\t"
                     "movdqu 160(%0), %%xmm10\n\t"
                     "movdqu 176(%0), %%xmm11\n\t"
                     "movdqu 192(%0), %%xmm12\n\t"
                     "movdqu 208(%0), %%xmm13\n\t"
                     "movdqu 224(%0), %%xmm14\n\t"
                     "movdqu 240(%0), %%xmm15"
                     : : "r"(r) : "memory");
}

static void xmm_store(uint8_t r[16][16])
{
    __asm__ volatile("movdqu %%xmm0, 0(%0)\n\t"
                     "movdqu %%xmm1, 16(%0)\n\t"
                     "movdqu %%xmm2, 32(%0)\n\t"
                     "movdqu %%xmm3, 48(%0)\n\t"
                     "movdqu %%xmm4, 64(%0)\n\t"
                     "movdqu %%xmm5, 80(%0)\n\t"
                     "movdqu %%xmm6, 96(%0)\n\t"
                     "movdqu %%xmm7, 112(%0)\n\t"
                     "movdqu %%xmm8, 128(%0)\n\t"
                     "movdqu %%xmm9, 144(%0)\n\t"
                     "movdqu %%xmm10, 160(%0)\n\t"
                     "movdqu %%xmm11, 176(%0)\n\t"
                     "movdqu %%xmm12, 192(%0)\n\t"
                     "movdqu %%xmm13, 208(%0)\n\t"
                     "movdqu %%xmm14, 224(%0)\n\t"
                     "movdqu %%xmm15, 240(%0)"
                     : : "r"(r) : "memory");
}

static void fill_pattern(uint8_t seed, uint8_t r[16][16])
{
    for (unsigned i = 0; i < 16; i++)
        for (unsigned j = 0; j < 16; j++)
            r[i][j] = (uint8_t)(seed ^ (i * 17u) ^ (j * 3u));
}

/* A kernel thread that deliberately owns register state (the one
 * sanctioned exception to the kernel rule, for this test only): load a
 * pattern, then keep yielding to its partner on the same CPU and check
 * that its registers come back intact every time. */
static void fpu_probe_thread(void *arg)
{
    struct fpu_probe *p = arg;
    if (arch_fpu_alloc(thread_current()) != 0) {
        p->rc = -ENOMEM;
        return;
    }
    uint8_t want[16][16], got[16][16];
    fill_pattern(p->seed, want);
    xmm_load(want);
    for (unsigned i = 0; i < FPU_PROBE_ROUNDS; i++) {
        sched_yield();
        xmm_store(got);
        if (memcmp(got, want, sizeof(got)) != 0) {
            p->corrupt++;
            xmm_load(want);
        }
    }
    p->rc = 0;
}

bool arch_test_fpu_switch(const char **why)
{
    struct fpu_probe a = { .seed = 0x11, .rc = -1 }, b = { .seed = 0xA5, .rc = -1 };
    cpumask_t here = CPUMASK_OF(arch_cpu_id());
    struct thread *ta = thread_create_on(fpu_probe_thread, &a, "fpu-a", SCHED_PRIO_DEFAULT, here);
    struct thread *tb = thread_create_on(fpu_probe_thread, &b, "fpu-b", SCHED_PRIO_DEFAULT, here);
    if (ta == NULL || tb == NULL) {
        *why = "cannot create the probe threads";
        if (ta)
            thread_join(ta);
        if (tb)
            thread_join(tb);
        return false;
    }
    thread_join(ta);
    thread_join(tb);
    if (a.rc != 0 || b.rc != 0) {
        *why = "a probe thread could not allocate its state";
        return false;
    }
    if (a.corrupt != 0 || b.corrupt != 0) {
        *why = "a thread observed another thread's vector registers";
        return false;
    }
    *why = NULL;
    return true;
}

bool arch_test_fpu_set(const uint8_t pattern[16])
{
    if (thread_current()->fpu == NULL)
        return false;
    __asm__ volatile("movdqu (%0), %%xmm0" : : "r"(pattern) : "memory");
    return true;
}

bool arch_test_fpu_get(uint8_t out[16])
{
    if (thread_current()->fpu == NULL)
        return false;
    __asm__ volatile("movdqu %%xmm0, (%0)" : : "r"(out) : "memory");
    return true;
}
