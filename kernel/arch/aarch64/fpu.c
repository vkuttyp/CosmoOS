/*
 * fpu.c - FP/SIMD state on AArch64 (docs/kernel/arch/aarch64/design.md,
 * "FP/SIMD state").
 *
 * `CPACR_EL1.FPEN` is set to 0b11 at every CPU's bring-up: FP and SIMD
 * instructions are allowed at EL0 and at EL1. The field has no encoding
 * that allows EL0 and traps EL1 -- 0b00 and 0b10 trap both, 0b01 traps
 * EL0 only -- and none could be useful, because EL1 is where the vector
 * registers are saved and restored. So the kernel rule (kernel code
 * never touches these registers) is what it is on x86-64: the build flag
 * `-mgeneral-regs-only`, review, and the disassembly check in
 * `scripts/check-fpregs.sh`, which allows exactly the functions below.
 *
 * Ownership is explicit and eager, as arch/fpu.h states: a thread owns
 * state iff `t->fpu != NULL`, a user thread is given state before its
 * first instruction, and the switch hook saves the outgoing owner's
 * registers and restores the incoming owner's. Kernel threads own
 * nothing and are neither saved nor restored, so the registers they run
 * with hold whatever the last user thread left there -- which is exactly
 * why the kernel may not read or write them.
 */

#include <kernel/errno.h>
#include <kernel/kmalloc.h>
#include <kernel/percpu.h>
#include <kernel/log.h>
#include <kernel/panic.h>
#include <kernel/sched.h>
#include <kernel/string.h>
#include <kernel/thread.h>

#include <arch/cpu.h>
#include <arch/fpu.h>
#include <arch/testhooks.h>

#include <aarch64/fpu.h>
#include <aarch64/sysreg.h>

#define CPACR_FPEN_SHIFT 20
#define CPACR_FPEN_ALL   (3ull << CPACR_FPEN_SHIFT)   /* 0b11: no trapping at EL0 or EL1 */

struct arch_fpu_state {
    struct aarch64_fpu_area area;
};

void aarch64_fpu_init_cpu(void)
{
    uint64_t cpacr = READ_SYSREG(cpacr_el1);
    WRITE_SYSREG(cpacr_el1, cpacr | CPACR_FPEN_ALL);
    __asm__ volatile("isb" ::: "memory");
    if (arch_cpu_id() == 0)
        kdebug("fpu: FP/SIMD enabled at EL0 and EL1, %zu-byte state", sizeof(struct aarch64_fpu_area));
}

/* --- arch/fpu.h --- */

int arch_fpu_alloc(struct thread *t)
{
    if (t->fpu != NULL)
        return 0;
    struct arch_fpu_state *st = kmalloc(sizeof(*st), KMEM_ZERO);
    if (st == NULL)
        return -ENOMEM;
    /* The architectural reset values: registers zero, FPSR and FPCR zero
     * (round to nearest, all exceptions untrapped, which is what the
     * procedure call standard expects a program to start with). */
    st->area.fpsr = 0;
    st->area.fpcr = 0;
    t->fpu = st;
    return 0;
}

void arch_fpu_free(struct thread *t)
{
    struct arch_fpu_state *st = t->fpu;
    if (st == NULL)
        return;
    t->fpu = NULL;
    kfree(st);
}

size_t arch_fpu_state_size(void)
{
    return sizeof(struct aarch64_fpu_area);
}

/* --- switching --- */

/* Called from arch_thread_switch_prepare with interrupts off: the
 * registers hold `prev`'s values and `next` runs next. Two owners: save
 * then restore. One: only its half. None: nothing. */
void aarch64_fpu_switch(struct thread *prev, struct thread *next)
{
    if (prev != NULL && prev->fpu != NULL)
        aarch64_fpu_area_save(&prev->fpu->area);
    if (next->fpu != NULL)
        aarch64_fpu_area_restore(&next->fpu->area);
}

/* The running thread's registers into its own area, and back out of it.
 * False when the caller owns no state (a kernel thread). */
bool aarch64_fpu_save_current(void)
{
    struct thread *t = thread_current();
    if (t == NULL || t->fpu == NULL)
        return false;
    aarch64_fpu_area_save(&t->fpu->area);
    return true;
}

bool aarch64_fpu_restore_current(void)
{
    struct thread *t = thread_current();
    if (t == NULL || t->fpu == NULL)
        return false;
    aarch64_fpu_area_restore(&t->fpu->area);
    return true;
}

bool aarch64_fpu_get_current(struct aarch64_fpu_area *out)
{
    struct thread *t = thread_current();
    if (t == NULL || t->fpu == NULL)
        return false;
    arch_irq_state_t s = arch_irq_save();
    aarch64_fpu_area_save(&t->fpu->area);   /* the live registers are the truth */
    *out = t->fpu->area;
    arch_irq_restore(s);
    return true;
}

bool aarch64_fpu_set_current(const struct aarch64_fpu_area *in)
{
    struct thread *t = thread_current();
    if (t == NULL || t->fpu == NULL)
        return false;
    struct aarch64_fpu_area image = *in;
    /* FPCR's reserved bits are RES0 and a program has no business
     * setting them through a signal frame. */
    image.fpcr &= AARCH64_FPCR_MASK;
    arch_irq_state_t s = arch_irq_save();
    t->fpu->area = image;
    aarch64_fpu_area_restore(&t->fpu->area);
    arch_irq_restore(s);
    return true;
}

/* --- test hooks (arch/testhooks.h) --- */

#if CONFIG_SELFTEST

#define FPU_PROBE_ROUNDS 64

struct fpu_probe {
    uint8_t seed;
    int rc;
    unsigned corrupt;
};

static void fill_pattern(uint8_t seed, uint8_t out[16][16])
{
    for (unsigned i = 0; i < 16; i++)
        for (unsigned j = 0; j < 16; j++)
            out[i][j] = (uint8_t)(seed + i * 16u + j);
}

static void fpu_probe_thread(void *arg)
{
    struct fpu_probe *p = arg;
    if (arch_fpu_alloc(thread_current()) != 0) {
        p->rc = -ENOMEM;
        thread_exit(0);
    }
    uint8_t want[16][16], got[16][16];
    fill_pattern(p->seed, want);
    aarch64_fpu_probe_load(want);
    for (unsigned i = 0; i < FPU_PROBE_ROUNDS; i++) {
        sched_yield();
        aarch64_fpu_probe_store(got);
        if (memcmp(got, want, sizeof(got)) != 0) {
            p->corrupt++;
            aarch64_fpu_probe_load(want);
        }
    }
    p->rc = 0;
    thread_exit(0);
}

bool arch_test_fpu_switch(const char **why)
{
    struct fpu_probe a = { .seed = 0x11, .rc = -1, .corrupt = 0 };
    struct fpu_probe b = { .seed = 0xA5, .rc = -1, .corrupt = 0 };
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
    aarch64_fpu_probe_set_q0(pattern);
    return true;
}

bool arch_test_fpu_get(uint8_t out[16])
{
    if (thread_current()->fpu == NULL)
        return false;
    aarch64_fpu_probe_get_q0(out);
    return true;
}

#else

bool arch_test_fpu_switch(const char **why)
{
    *why = "self-tests are not built";
    return false;
}

bool arch_test_fpu_set(const uint8_t pattern[16])
{
    (void)pattern;
    return false;
}

bool arch_test_fpu_get(uint8_t out[16])
{
    (void)out;
    return false;
}

#endif /* CONFIG_SELFTEST */
