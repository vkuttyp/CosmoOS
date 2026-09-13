/*
 * hvtest.c - Virtualization self-tests
 * (docs/kernel-services/virtualization/testing.md).
 *
 * The guest images come from the boot archive (tests/hv/, flat
 * binaries for guest-physical 0x1000). Without a backend the guest tests
 * log "skipped" and pass; the harness treats that as a failure in the CI
 * configuration, where SVM is present.
 */

#include <kernel/bootarchive.h>
#include <kernel/errno.h>
#include <kernel/hv.h>
#include <kernel/log.h>
#include <kernel/net/tap.h>
#include <kernel/page.h>
#include <kernel/pmm.h>
#include <kernel/printf.h>
#include <kernel/selftest.h>
#include <kernel/string.h>
#include <kernel/thread.h>
#include <kernel/timer.h>
#include <arch/fpu.h>
#include <arch/testhooks.h>
#include <arch/el2.h>
#include <arch/timer.h>

#define CHECK(c)                                                    \
    do {                                                            \
        if (!(c)) {                                                 \
            *reason = "check failed: " #c " at line " STR(__LINE__); \
            return false;                                           \
        }                                                           \
    } while (0)
#define STR_(x) #x
#define STR(x) STR_(x)

#define LOAD_GPA 0x1000ull
#define MEM_LEN  (1ull << 20)

static bool skip_without_backend(const char **reason)
{
    if (!hv_caps()->present) {
        kinfo("selftest: hv: skipped: no backend");
        *reason = NULL;
        return true;
    }
    return false;
}

/* A VM with 1 MiB at 0 and the named image at 0x1000; one vCPU entering
 * there. The guests are per-architecture, so on a build whose guests are
 * all compiled out these helpers go unused. */
static __maybe_unused int make_guest(const char *image, struct vm **vm_out, struct vcpu **vcpu_out)
{
    const void *data;
    size_t size;
    if (!bootarchive_find(image, &data, &size))
        return -ENOENT;
    struct vm *vm;
    int rc = vm_create(0, HV_VM_MEM_MAX, &vm);
    if (rc)
        return rc;
    rc = vm_mem_add(vm, 0, MEM_LEN);
    if (rc == 0)
        rc = vm_mem_write(vm, LOAD_GPA, data, size);
    struct vcpu *v = NULL;
    if (rc == 0)
        rc = vcpu_create(vm, 0, &v);
    if (rc == 0) {
        struct cosmo_vcpu_regs regs;
        vcpu_get_regs(v, &regs);
#if defined(ARCH_AARCH64)
        regs.pc = LOAD_GPA;
#else
        regs.rip = LOAD_GPA;
#endif
        rc = vcpu_set_regs(v, &regs);
    }
    if (rc) {
        if (v)
            kobject_put(&v->obj);
        kobject_put(&vm->obj);
        return rc;
    }
    *vm_out = vm;
    *vcpu_out = v;
    return 0;
}

static __maybe_unused void drop_guest(struct vm *vm, struct vcpu *v)
{
    kobject_put(&v->obj);
    kobject_put(&vm->obj);
}

/* --- the guest rule: no vector register crosses the guest boundary ------ */

bool selftest_hv_guest_fpu(const char **reason)
{
#if !defined(ARCH_X86_64)
    /* This guest image and these expectations are x86's; the AArch64
     * guests are covered by the el2-* tests. */
    (void)reason;
    kinfo("selftest: hv-fpu: an x86 guest; skipping");
    return true;
#else
    if (skip_without_backend(reason))
        return true;
    /* This (kernel) thread takes ownership of register state for the
     * duration, as a user thread would have it. */
    struct thread *me = thread_current();
    bool owned = me->fpu != NULL;
    CHECK(arch_fpu_alloc(me) == 0);

    struct vm *vm;
    struct vcpu *v;
    CHECK(make_guest("tests/hv/guest_fpu.bin", &vm, &v) == 0);
    uint8_t guest_pattern[16], host_pattern[16], seen[16];
    for (unsigned i = 0; i < 16; i++) {
        guest_pattern[i] = (uint8_t)(0xB0 + i);
        host_pattern[i] = (uint8_t)(0x40 + 3 * i);
    }
    CHECK(vm_mem_write(vm, 0x3010, guest_pattern, sizeof(guest_pattern)) == 0);
    CHECK(arch_test_fpu_set(host_pattern));

    struct cosmo_vm_exit x;
    CHECK(vcpu_run(v, &x) == 0);
    CHECK(x.kind == COSMO_VM_EXIT_HLT);

    /* What the guest saw in xmm0 on its first instruction: the reset
     * state, not the owner's pattern. */
    static const uint8_t zero[16];
    CHECK(vm_mem_read(vm, 0x3000, seen, sizeof(seen)) == 0);
    CHECK(memcmp(seen, zero, sizeof(seen)) == 0);
    /* What the owner has after the run: its own pattern, not the guest's. */
    CHECK(arch_test_fpu_get(seen));
    CHECK(memcmp(seen, host_pattern, sizeof(seen)) == 0);
    /* The guest keeps running (its trailing hlt loop): its state survives an exit and re-entry. */
    CHECK(vcpu_run(v, &x) == 0 && x.kind == COSMO_VM_EXIT_HLT);
    drop_guest(vm, v);
    if (!owned)
        arch_fpu_free(me);
    return true;
#endif
}

/* A list register is back on offer when it holds nothing and the
 * controller agrees it is free. Both, because either alone would pass
 * on a switch that read one of them and not the other. */
static __maybe_unused bool lr_free(uint64_t lr0, uint64_t elrsr)
{
    return (lr0 >> 62) == 0 && (elrsr & 1u) != 0;
}

static __maybe_unused bool console_is(struct vm *vm, const char *expect)
{
    char buf[64];
    size_t n = vm_console_read(vm, buf, sizeof(buf) - 1);
    buf[n] = '\0';
    if (strcmp(buf, expect) != 0) {
        kwarn("selftest: hv: console \"%s\", expected \"%s\"", buf, expect);
        return false;
    }
    return true;
}

bool selftest_hv_probe(const char **reason)
{
    const struct hv_caps *c = hv_caps();
    if (!c->present) {
        kinfo("selftest: hv-probe: no backend (%s)", c->name);
        struct vm *vm;
        CHECK(vm_create(0, HV_VM_MEM_MAX, &vm) == -ENOTSUP);
        return true;
    }
    CHECK(strcmp(c->name, "svm") == 0 || strcmp(c->name, "vmx") == 0 || strcmp(c->name, "el2") == 0);
    CHECK(c->nested_paging);
    CHECK(c->max_asids >= 2);
    /* A backend that cannot run the architectural reset state has no
     * business creating vCPUs with it (nothing here reports that today;
     * VMX without unrestricted guest would). */
    CHECK(c->real_mode_guest);
    return true;
}

/* The capabilities a backend reports are the ones it honours. */
bool selftest_hv_caps(const char **reason)
{
    if (skip_without_backend(reason))
        return true;
    const struct hv_caps *c = hv_caps();
    struct vm *vm;
    CHECK(vm_create(0, HV_VM_MEM_MAX, &vm) == 0);

    /* Guest memory is mapped RWX; the mapping call refuses a prot the
     * interface does not define, and 0. */
    CHECK(arch_hv_vm_map(vm->arch, 0x10000000ull, 0x1000, PAGE_SIZE, 0) == -EINVAL);
    CHECK(arch_hv_vm_map(vm->arch, 0x10000000ull, 0x1000, PAGE_SIZE, 0x10u) == -EINVAL);
    if (c->map_prot) {
        /* A read-only mapping is accepted and readable back. */
        struct page *pg = pmm_alloc_page(PMM_FLAGS_ZERO);
        CHECK(pg != NULL);
        paddr_t pa = page_to_phys(pg), got = 0;
        CHECK(arch_hv_vm_map(vm->arch, 0x10000000ull, pa, PAGE_SIZE, HV_MAP_READ) == 0);
        CHECK(arch_hv_vm_query(vm->arch, 0x10000000ull, &got) && got == pa);
        CHECK(arch_hv_vm_unmap(vm->arch, 0x10000000ull, PAGE_SIZE) == 0);
        CHECK(!arch_hv_vm_query(vm->arch, 0x10000000ull, &got));
        pmm_free_page(pg);
    }
    kobject_put(&vm->obj);

    /* `inject_irq` is not decoration: it is what `vcpu_inject` consults
     * before it promises anything, so the two must agree. Whichever way
     * this machine answers, the answer is checked rather than logged. */
    struct vm *ivm;
    struct vcpu *iv;
    CHECK(vm_create(0, HV_VM_MEM_MAX, &ivm) == 0);
    if (vcpu_create(ivm, 0, &iv) == 0) {
        int rc = vcpu_inject(iv, 64);
        if (c->inject_irq)
            CHECK(rc == 0);
        else
            CHECK(rc == -ENOTSUP);
        kobject_put(&iv->obj);
    }
    kobject_put(&ivm->obj);

    kinfo("selftest: hv-caps: %s%s%s%s%s, %u asids", c->name, c->nested_paging ? " npt" : "",
          c->real_mode_guest ? " realmode" : "", c->large_pages ? " largepages" : "",
          c->inject_irq ? " inject-irq" : "", c->max_asids);
    return true;
}

bool selftest_hv_npt(const char **reason)
{
    if (skip_without_backend(reason))
        return true;
    unsigned before = hv_vm_count();
    struct vm *vm;
    CHECK(vm_create(0, HV_VM_MEM_MAX, &vm) == 0);
    CHECK(hv_vm_count() == before + 1);
    CHECK(vm_mem_add(vm, 0, 0x10000) == 0);
    CHECK(vm_mem_add(vm, 0x200000, 0x3000) == 0);
    CHECK(vm_mem_add(vm, 0x8000, 0x1000) == -EINVAL);          /* overlap */
    CHECK(vm_mem_add(vm, 0x1001, 0x1000) == -EINVAL);          /* alignment */
    CHECK(vm_mem_add(vm, 0x300000, HV_VM_MEM_MAX) == -ENOMEM); /* the per-VM limit */
    struct vm *small;
    CHECK(vm_create(0, 0x2000, &small) == 0);                    /* the creator's COSMO_RLIMIT_VMEM */
    CHECK(vm_mem_add(small, 0, 0x3000) == -ENOMEM);
    CHECK(vm_mem_add(small, 0, 0x2000) == 0);
    kobject_put(&small->obj);
    CHECK(vm_mem_add(vm, HV_GPA_LIMIT - 0x1000, 0x2000) == -EINVAL);
    for (uint64_t gpa = 0; gpa < 0x10000; gpa += 0x1000) {
        paddr_t hpa;
        CHECK(arch_hv_vm_query(vm->arch, gpa, &hpa));
        struct page *pg;
        size_t off;
        CHECK(vm_mem_lookup(vm, gpa + 0x10, &pg, &off) && off == 0x10);
        CHECK(page_to_phys(pg) == hpa);
    }
    CHECK(!arch_hv_vm_query(vm->arch, 0x10000, NULL));
    CHECK(arch_hv_vm_query(vm->arch, 0x202000, NULL));
    CHECK(!arch_hv_vm_query(vm->arch, 0x203000, NULL));
    /* Copies: whole range must be backed; zeroed memory; round trip. */
    uint8_t buf[16];
    CHECK(vm_mem_read(vm, 0xFFF8, buf, 16) == -EFAULT);
    CHECK(vm_mem_read(vm, 0x10000, buf, 1) == -EFAULT);
    CHECK(vm_mem_read(vm, 0x0FF8, buf, 16) == 0);
    for (unsigned i = 0; i < 16; i++)
        CHECK(buf[i] == 0);
    memcpy(buf, "cross-page-copy!", 16);
    CHECK(vm_mem_write(vm, 0x0FF8, buf, 16) == 0);
    memset(buf, 0, sizeof(buf));
    CHECK(vm_mem_read(vm, 0x0FF8, buf, 16) == 0 && memcmp(buf, "cross-page-copy!", 16) == 0);
    /* The host page really holds the bytes (the guest will see them). */
    struct page *pg;
    size_t off;
    CHECK(vm_mem_lookup(vm, 0x1000, &pg, &off));
    CHECK(memcmp(phys_to_virt(page_to_phys(pg)), "ge-copy!", 8) == 0);   /* the second page of the copy */
    kobject_put(&vm->obj);
    CHECK(hv_vm_count() == before);
    return true;
}

bool selftest_hv_guest_pio(const char **reason)
{
#if !defined(ARCH_X86_64)
    /* This guest image and these expectations are x86's; the AArch64
     * guests are covered by the el2-* tests. */
    (void)reason;
    kinfo("selftest: hv-pio: an x86 guest; skipping");
    return true;
#else
    if (skip_without_backend(reason))
        return true;
    struct vm *vm;
    struct vcpu *v;
    CHECK(make_guest("tests/hv/guest_pio.bin", &vm, &v) == 0);
    struct cosmo_vm_exit x;
    memset(&x, 0, sizeof(x));
    CHECK(vcpu_run(v, &x) == 0);
    CHECK(x.kind == COSMO_VM_EXIT_IO && x.io.write && x.io.port == 0x80 && x.io.size == 2 && x.io.value == 0x1234);
    CHECK(x.rip == LOAD_GPA + 13);           /* after B0 48 E6 E9 B0 56 E6 E9 B8 34 12 E7 80 */
    CHECK(console_is(vm, "HV"));
    CHECK(vcpu_run(v, &x) == 0);
    CHECK(x.kind == COSMO_VM_EXIT_HLT && x.rip == LOAD_GPA + 14);
    CHECK(vcpu_run(v, &x) == 0);
    CHECK(x.kind == COSMO_VM_EXIT_IO && !x.io.write && x.io.port == 0x81 && x.io.size == 1);
    x.io.value = 'Q';                        /* the IN completion */
    CHECK(vcpu_run(v, &x) == 0);
    CHECK(x.kind == COSMO_VM_EXIT_HLT);
    CHECK(console_is(vm, "Q"));
    struct cosmo_vcpu_regs regs;
    CHECK(vcpu_get_regs(v, &regs) == 0);
    CHECK((regs.rax & 0xFF) == 'Q');
    CHECK(regs.cs.selector == 0 && regs.cr0 == (0x10 | (1ull << 29) | (1ull << 30)));
    CHECK(v->exits >= 4 && v->entries >= 4);
    drop_guest(vm, v);
    return true;
#endif
}

bool selftest_hv_guest_irq(const char **reason)
{
#if !defined(ARCH_X86_64)
    /* This guest image and these expectations are x86's; the AArch64
     * guests are covered by the el2-* tests. */
    (void)reason;
    kinfo("selftest: hv-irq: an x86 guest; skipping");
    return true;
#else
    if (skip_without_backend(reason))
        return true;
    struct vm *vm;
    struct vcpu *v;
    CHECK(make_guest("tests/hv/guest_irq.bin", &vm, &v) == 0);
    struct cosmo_vm_exit x;
    memset(&x, 0, sizeof(x));
    CHECK(vcpu_run(v, &x) == 0);
    CHECK(x.kind == COSMO_VM_EXIT_HLT && x.flags == 0);          /* cli; hlt */
    CHECK(vcpu_inject(v, 3) == -EINVAL && vcpu_inject(v, 256) == -EINVAL);
    CHECK(vcpu_inject(v, 0x20) == 0);
    struct cosmo_vcpu_regs regs;
    CHECK(vcpu_get_regs(v, &regs) == 0 && regs.pending_irq == 0x20);
    CHECK(vcpu_run(v, &x) == 0);
    CHECK(x.kind == COSMO_VM_EXIT_HLT);                           /* sti; hlt: the sti shadow covers the hlt */
    CHECK(x.flags & COSMO_VM_EXIT_F_IRQ_PENDING);
    CHECK(console_is(vm, ""));
    /* Skipping the intercepted hlt ends the shadow: the vector is delivered on re-entry. */
    CHECK(vcpu_run(v, &x) == 0);
    CHECK(x.kind == COSMO_VM_EXIT_HLT && x.flags == 0 && x.rip == LOAD_GPA + 0x1A);
    CHECK(console_is(vm, "I"));
    CHECK(vcpu_get_regs(v, &regs) == 0 && regs.pending_irq == ~0ull && (regs.rflags & (1u << 9)));
    /* A second vector while nothing blocks it is delivered on the next run. */
    CHECK(vcpu_inject(v, 0x20) == 0);
    CHECK(vcpu_run(v, &x) == 0);
    CHECK(x.kind == COSMO_VM_EXIT_HLT && x.flags == 0);
    CHECK(console_is(vm, "I"));
    drop_guest(vm, v);
    return true;
#endif
}

bool selftest_hv_guest_cpuid(const char **reason)
{
#if !defined(ARCH_X86_64)
    /* This guest image and these expectations are x86's; the AArch64
     * guests are covered by the el2-* tests. */
    (void)reason;
    kinfo("selftest: hv-cpuid: an x86 guest; skipping");
    return true;
#else
    if (skip_without_backend(reason))
        return true;
    struct vm *vm;
    struct vcpu *v;
    CHECK(make_guest("tests/hv/guest_cpuid.bin", &vm, &v) == 0);
    struct cosmo_vm_exit x;
    memset(&x, 0, sizeof(x));
    CHECK(vcpu_run(v, &x) == 0);
    CHECK(x.kind == COSMO_VM_EXIT_HYPERCALL);
    CHECK(x.hypercall.nr == 7 && x.hypercall.a0 == 0x11 && x.hypercall.a1 == 0x22 && x.hypercall.a2 == 0x33 &&
          x.hypercall.a3 == 0x44);
    CHECK(console_is(vm, "CosmoOSCosmo101"));
    struct cosmo_vcpu_regs regs;
    CHECK(vcpu_get_regs(v, &regs) == 0 && regs.efer == 1);      /* SCE set by the guest, SVME hidden */
    CHECK(vcpu_run(v, &x) == 0 && x.kind == COSMO_VM_EXIT_HLT);
    drop_guest(vm, v);
    return true;
#endif
}

bool selftest_hv_guest_pm(const char **reason)
{
#if !defined(ARCH_X86_64)
    /* This guest image and these expectations are x86's; the AArch64
     * guests are covered by the el2-* tests. */
    (void)reason;
    kinfo("selftest: hv-pm: an x86 guest; skipping");
    return true;
#else
    if (skip_without_backend(reason))
        return true;
    struct vm *vm;
    struct vcpu *v;
    CHECK(make_guest("tests/hv/guest_pm.bin", &vm, &v) == 0);
    struct cosmo_vcpu_regs regs;
    CHECK(vcpu_get_regs(v, &regs) == 0);
    regs.cr0 = 0x11;                                    /* PE | ET */
    regs.cs.selector = 0x8;  regs.cs.attrib = 0xC09B; regs.cs.limit = 0xFFFFFFFF; regs.cs.base = 0;
    regs.ds.selector = 0x10; regs.ds.attrib = 0xC093; regs.ds.limit = 0xFFFFFFFF; regs.ds.base = 0;
    regs.es = regs.ss = regs.fs = regs.gs = regs.ds;
    regs.rsp = 0x8000;
    CHECK(vcpu_set_regs(v, &regs) == 0);
    struct cosmo_vm_exit x;
    memset(&x, 0, sizeof(x));
    CHECK(vcpu_run(v, &x) == 0);
    CHECK(x.kind == COSMO_VM_EXIT_MMIO && x.mmio.gpa == 0x10000000 && x.mmio.write);
    CHECK(console_is(vm, "P"));
    CHECK(vcpu_get_regs(v, &regs) == 0 && regs.rax == 0x5A5A5A5A);
    regs.rip += 5;                                      /* skip "mov %eax, 0x10000000" */
    CHECK(vcpu_set_regs(v, &regs) == 0);
    CHECK(vcpu_run(v, &x) == 0);
    CHECK(x.kind == COSMO_VM_EXIT_HLT);
    CHECK(console_is(vm, "Q"));
    /* Unusable states are refused before the hardware sees them. */
    regs.cr0 = 0x80000000ull;                           /* PG without PE */
    CHECK(vcpu_set_regs(v, &regs) == -EINVAL);
    regs.cr0 = 0x11;
    regs.efer = 1ull << 12;                             /* SVME */
    CHECK(vcpu_set_regs(v, &regs) == -EINVAL);
    drop_guest(vm, v);
    return true;
#endif
}

bool selftest_hv_guest_shutdown(const char **reason)
{
#if !defined(ARCH_X86_64)
    /* This guest image and these expectations are x86's; the AArch64
     * guests are covered by the el2-* tests. */
    (void)reason;
    kinfo("selftest: hv-shutdown: an x86 guest; skipping");
    return true;
#else
    if (skip_without_backend(reason))
        return true;
    struct vm *vm;
    struct vcpu *v;
    CHECK(make_guest("tests/hv/guest_shutdown.bin", &vm, &v) == 0);
    struct cosmo_vcpu_regs regs;
    CHECK(vcpu_get_regs(v, &regs) == 0);
    regs.idtr.limit = 0;
    CHECK(vcpu_set_regs(v, &regs) == 0);
    struct cosmo_vm_exit x;
    memset(&x, 0, sizeof(x));
    CHECK(vcpu_run(v, &x) == 0);
    CHECK(x.kind == COSMO_VM_EXIT_SHUTDOWN);
    CHECK(console_is(vm, "S"));
    CHECK(vcpu_run(v, &x) == -EIO);                     /* dead */
    CHECK(vcpu_get_regs(v, &regs) == 0);                /* state still readable */
    drop_guest(vm, v);
    return true;
#endif
}

bool selftest_hv_guest_spin(const char **reason)
{
#if !defined(ARCH_X86_64)
    /* This guest image and these expectations are x86's; the AArch64
     * guests are covered by the el2-* tests. */
    (void)reason;
    kinfo("selftest: hv-spin: an x86 guest; skipping");
    return true;
#else
    if (skip_without_backend(reason))
        return true;
    struct vm *vm;
    struct vcpu *v;
    CHECK(make_guest("tests/hv/guest_spin.bin", &vm, &v) == 0);
    struct cosmo_vm_exit x;
    memset(&x, 0, sizeof(x));
    /* The guest never exits voluntarily; the host tick does it for us. */
    CHECK(vcpu_run_limited(v, &x, 5) == -ETIMEDOUT);
    CHECK(console_is(vm, "."));
    CHECK(v->exits >= 5);
    /* Two vCPUs of one VM; the second index in use is refused; the limit holds. */
    struct vcpu *v1, *dup;
    CHECK(vcpu_create(vm, 1, &v1) == 0);
    CHECK(vcpu_create(vm, 1, &dup) == -EEXIST);
    CHECK(vcpu_create(vm, HV_VCPUS_MAX, &dup) == -EINVAL);
    CHECK(vm->nr_vcpus == 2);
    kobject_put(&v1->obj);
    CHECK(vm->nr_vcpus == 1);
    unsigned before = hv_vm_count();
    drop_guest(vm, v);
    CHECK(hv_vm_count() == before - 1);
    return true;
#endif
}

/*
 * The kick (`SYS_vcpu_stop`, kernel/hvkick.h). A kernel self-test rather
 * than only a userland one, because the property is about what happens
 * *inside* `vcpu_run` and the owner cannot see that from outside.
 *
 * Both guests here are `guest_spin`, which never exits of its own accord:
 * on both architectures it is the image that makes "did the kick work?" the
 * only question the test can be answering, since nothing else would ever
 * end the run.
 */
struct kicker {
    struct vcpu *v;
    uint64_t after_ns;
    uint64_t sent_ns;      /* when the stop was actually sent */
    unsigned in_guest;     /* what the kicker saw: did it have a CPU to IPI? */
    bool sent;
};

static void kicker_main(void *arg)
{
    struct kicker *k = arg;
    thread_sleep_ns(k->after_ns);
    /*
     * A short delay before the stop, so that the kick has a chance to land
     * while the guest is *running* rather than between two of its runs. A
     * sleeping thread wakes on a timer tick and the host timer tick is also
     * what exits a spinning guest (`guest_spin.S` says so), so a kicker
     * that sends the moment it wakes tends to find `in_guest` already
     * clear, send no IPI, and exercise only the sticky flag.
     *
     * It is a *tendency* and not a guarantee, which is why what the kicker
     * saw is recorded and printed rather than asserted: whether the kick
     * lands inside a guest depends on how the two threads are placed, and
     * an assertion about that is an assertion about the host's scheduler.
     * Both outcomes are correct -- the flag is sticky -- and the gate below
     * is on the property that holds either way.
     */
    udelay(500);
    k->in_guest = __atomic_load_n(&k->v->kick.in_guest, __ATOMIC_SEQ_CST);
    k->sent_ns = clock_now_ns();
    (void)vcpu_stop(k->v);
    __atomic_store_n(&k->sent, true, __ATOMIC_RELEASE);
    thread_exit(0);
}

bool selftest_hv_vcpu_stop(const char **reason)
{
    if (skip_without_backend(reason))
        return true;
    struct vm *vm;
    struct vcpu *v;
    CHECK(make_guest("tests/hv/guest_spin.bin", &vm, &v) == 0);
    struct cosmo_vm_exit x;

    /*
     * A stop set while the vCPU is between runs is taken by the next run,
     * *without entering the guest* -- the entry counter is what says so,
     * and it is the difference between a sticky stop and one that needs the
     * IPI to be the mechanism.
     */
    uint64_t entries = v->entries;
    CHECK(vcpu_stop(v) == 0);
    memset(&x, 0, sizeof(x));
    CHECK(vcpu_run(v, &x) == 0);
    CHECK(x.kind == COSMO_VM_EXIT_STOPPED);
    CHECK(v->entries == entries);          /* the guest never ran */

    /* And it was consumed: the next run enters and the guest spins until a
     * bound ends it, which is the un-stopped behaviour. */
    memset(&x, 0, sizeof(x));
    CHECK(vcpu_run_limited(v, &x, 3) == -ETIMEDOUT);
    CHECK(v->entries > entries);

    /* Stopping a vCPU that is not running is not an error; it is the same
     * sticky store, and the run after it says so. */
    CHECK(vcpu_stop(v) == 0);
    memset(&x, 0, sizeof(x));
    CHECK(vcpu_run(v, &x) == 0);
    CHECK(x.kind == COSMO_VM_EXIT_STOPPED);

    /*
     * The case the whole mechanism exists for: a stop that arrives while
     * the guest is *running*, from another thread, ending a run that is
     * bounded by nothing. Without the kick this run never returns and the
     * harness's per-test budget fails the test -- which is the right
     * failure, because a kick that does not arrive is exactly a hang.
     */
    struct kicker k;
    memset(&k, 0, sizeof(k));
    k.v = v;
    k.after_ns = 20000000ull;   /* 20 ms: several scheduler ticks into the run */
    struct thread *th = thread_create(kicker_main, &k, "vcpu-kicker", SCHED_PRIO_DEFAULT);
    CHECK(th != NULL);
    memset(&x, 0, sizeof(x));
    uint64_t t0 = clock_now_ns();
    CHECK(vcpu_run(v, &x) == 0);
    uint64_t t1 = clock_now_ns();
    CHECK(x.kind == COSMO_VM_EXIT_STOPPED);
    CHECK(__atomic_load_n(&k.sent, __ATOMIC_ACQUIRE));   /* it was the kicker, not a bound */
    CHECK(v->exits > 0);
    /*
     * How long the kick took and whether it found the guest entered, both
     * **reported and not asserted**. Two things a bug-proof taught:
     *
     *  - Removing the IPI entirely does *not* fail this test.
     *    `guest_spin.S` says why: every host timer tick is taken to EL2, so
     *    the run loop sees INTR exits anyway and the sticky flag is noticed
     *    at the next one. **Liveness comes from the tick plus the sticky
     *    flag; what the IPI buys is promptness** -- microseconds instead of
     *    up to a tick -- and independence from the host's tick policy. A
     *    tickless host, or one that ever suppressed ticks while a guest
     *    runs, would need the IPI for liveness too.
     *  - Whether the kick lands inside a guest is a matter of thread
     *    placement, so it is printed rather than gated. Runs have been
     *    seen both ways on the same build.
     *
     * A latency assertion would be a flake waiting to happen -- this tree
     * has three of that family already -- so the gate stays on what is
     * guaranteed however the threads are placed: the run returns STOPPED,
     * and it was the kicker that ended it.
     */
    kinfo("selftest: hv-vcpu-stop: kick to return %llu us, kicker saw in_guest=%s",
          (unsigned long long)((t1 - k.sent_ns) / 1000),
          (k.in_guest & HV_IN_GUEST) ? "yes" : "no");
    (void)t0;

    drop_guest(vm, v);
    return true;
}

/* AArch64: the EL2 the loader kept (docs/kernel/arch/aarch64/design.md,
 * "Exception level 2"). The stub answers HVC, hands EL2 over when asked,
 * and takes it back; a machine booted at EL1 (QEMU_EL2=0) skips. */
bool selftest_el2_stub(const char **reason)
{
#if defined(ARCH_AARCH64)
    if (!el2_available()) {
        kinfo("selftest: el2: firmware handed over at EL1; skipping");
        CHECK(el2_stub_phys() == 0);
        CHECK(el2_set_vectors(0x1000) == -1);   /* nothing to talk to */
        return true;
    }
    uint64_t stub = el2_stub_phys();
    CHECK(stub != 0 && (stub & (PAGE_SIZE - 1)) == 0);
    CHECK(el2_call_raw(0x1234, 0) == -1);            /* a selector nobody knows */
    if (hv_caps()->present) {
        /* The hypervisor backend owns EL2 on this CPU by now: its own
         * vectors answer, and the stub's ABI is gone until they hand it
         * back. That the switch answers at all is the check here. */
        CHECK(el2_call_raw(HV_EL2_CALL_VERSION, 0) == HV_EL2_VERSION);
        kinfo("selftest: el2: the EL2 backend owns the vectors (switch v%u), stub page 0x%llx",
              HV_EL2_VERSION, (unsigned long long)stub);
        return true;
    }
    CHECK(el2_call_raw(EL2_STUB_VERSION_CALL, 0) == EL2_STUB_VERSION);
    /* Hand EL2 to another vector table and take it back. The stub's own
     * base is a vector table, so this is the swap without the risk. */
    CHECK(el2_set_vectors(stub) == 0);
    CHECK(el2_call_raw(EL2_STUB_VERSION_CALL, 0) == EL2_STUB_VERSION);
    CHECK(el2_restore_stub_vectors() == 0);
    CHECK(el2_call_raw(EL2_STUB_VERSION_CALL, 0) == EL2_STUB_VERSION);
    kinfo("selftest: el2: stub v%u at 0x%llx, vectors handed over and back", EL2_STUB_VERSION,
          (unsigned long long)stub);
    return true;
#else
    (void)reason;
    kinfo("selftest: el2: x86-64 has no EL2; skipping");
    return true;
#endif
}

/* --- AArch64 guests (docs/kernel-services/virtualization/testing.md) --- */

#if defined(ARCH_AARCH64)
/* Reading a host ID register by its encoding, for the feature-model
 * test. kernel-services are built without the arch include path, so this
 * is inline rather than <aarch64/sysreg.h>. */
#define VIRTIO_BLK_S_OK 0u
#define HOST_IDREG(sname) ({ uint64_t v_; __asm__ volatile("mrs %0, " sname : "=r"(v_)); v_; })
/* The EL2 backend's own guests: one per exit the world switch decodes.
 * Each image is loaded at guest-physical 0x1000 and entered at EL1 with
 * the MMU off, which is this architecture's reset state. */

bool selftest_el2_guest_wfi(const char **reason)
{
    if (skip_without_backend(reason))
        return true;
    struct vm *vm;
    struct vcpu *v;
    CHECK(make_guest("tests/hv/guest_wfi.bin", &vm, &v) == 0);
    struct cosmo_vm_exit x;
    memset(&x, 0, sizeof(x));
    /* The guest's first instruction is WFI: the exit names it, and the
     * PC is past it so a second run reaches the second WFI. */
    CHECK(vcpu_run(v, &x) == 0);
    CHECK(x.kind == COSMO_VM_EXIT_WFI && x.rip == LOAD_GPA + 4);
    CHECK(vcpu_run(v, &x) == 0);
    CHECK(x.kind == COSMO_VM_EXIT_WFI && x.rip == LOAD_GPA + 8);
    /* The guest's own registers survive a round trip through EL2. */
    struct cosmo_vcpu_regs regs;
    CHECK(vcpu_get_regs(v, &regs) == 0);
    CHECK(regs.pc == LOAD_GPA + 8);
    CHECK((regs.pstate & 0xF) == 0x5);          /* still EL1h */
    drop_guest(vm, v);
    return true;
}

/* --- the guest's interrupt state crosses EL2 intact ---
 *
 * `ICH_*_EL2` cannot be touched from EL1, so the switch moves them
 * through `struct hv_ctx`: written before the guest runs, read back
 * after. Nothing is injected here -- what is checked is that the
 * journey happens at all, which is the half of the delivery path that
 * can be wrong without any interrupt being involved.
 */
bool selftest_el2_vgic_roundtrip(const char **reason)
{
    if (skip_without_backend(reason))
        return true;
    struct vm *vm;
    struct vcpu *v;
    CHECK(make_guest("tests/hv/guest_wfi.bin", &vm, &v) == 0);
    uint64_t lr0 = ~0ull, elrsr = 0;

    if (!hv_caps()->inject_irq) {
        /* No virtual interface: the switch must not touch those
         * registers at all, and says so by having no state to report. */
        CHECK(!arch_hv_vcpu_vgic_state(v->arch, &lr0, &elrsr));
        drop_guest(vm, v);
        kinfo("selftest: el2-vgic-roundtrip: no virtual GIC here; the switch leaves it alone");
        return true;
    }

    struct cosmo_vm_exit x;
    memset(&x, 0, sizeof(x));
    CHECK(vcpu_run(v, &x) == 0);
    CHECK(x.kind == COSMO_VM_EXIT_WFI);
    CHECK(arch_hv_vcpu_vgic_state(v->arch, &lr0, &elrsr));
    /* An empty list register comes back empty, and the controller says
     * it is free. Both are read from the hardware after the run, so a
     * switch that wrote nothing or read nothing back fails here. */
    CHECK(lr0 == 0);
    CHECK((elrsr & 1u) != 0);
    drop_guest(vm, v);
    kinfo("selftest: el2-vgic-roundtrip: LR0 %llu, ELRSR 0x%llx after a run",
          (unsigned long long)lr0, (unsigned long long)elrsr);
    return true;
}

/* --- a guest takes an interrupt ---
 *
 * The whole point of the unit. The guest enables its own CPU interface
 * and says "ready"; the host injects; the guest's handler acknowledges
 * and reports the interrupt number it was given. Nothing here emulates
 * a distributor: a virtual interrupt placed in a list register bypasses
 * one, and the guest's `ICC_*_EL1` accesses reach the virtual interface
 * because `HCR_EL2.IMO` is set.
 */
bool selftest_el2_guest_irq(const char **reason)
{
    if (skip_without_backend(reason))
        return true;
    if (!hv_caps()->inject_irq) {
        kinfo("selftest: el2-guest-irq: no virtual GIC on this machine; skipping");
        return true;
    }
    struct vm *vm;
    struct vcpu *v;
    CHECK(make_guest("tests/hv/guest_irq.bin", &vm, &v) == 0);
    struct cosmo_vm_exit x;
    memset(&x, 0, sizeof(x));

    /* The guest sets up its interface and says it is ready. */
    CHECK(vcpu_run(v, &x) == 0);
    CHECK(x.kind == COSMO_VM_EXIT_HYPERCALL && x.hypercall.nr == 1);

    /* Nothing has been offered yet, so nothing is pending. */
    struct cosmo_vcpu_regs regs;
    CHECK(vcpu_get_regs(v, &regs) == 0 && regs.pending_irq == ~0ull);

    CHECK(vcpu_inject(v, 42) == 0);
    CHECK(vcpu_get_regs(v, &regs) == 0 && regs.pending_irq == 42);

    /* The guest takes it, acknowledges it, and calls out from inside
     * its handler with the number it was given. */
    CHECK(vcpu_run(v, &x) == 0);
    CHECK(x.kind == COSMO_VM_EXIT_HYPERCALL);
    CHECK(x.hypercall.nr == 42);

    /* --- the Active window ---
     * The guest has acknowledged and not completed, so the list
     * register is Active. That is delivery: the pending bit must be
     * clear, or the same interrupt is given to a guest already handling
     * it. */
    uint64_t lr0 = 0, elrsr = 0;
    CHECK(arch_hv_vcpu_vgic_state(v->arch, &lr0, &elrsr));
    CHECK((lr0 >> 62) == 2);                 /* Active */
    CHECK((lr0 & 0xFFFFFFFFu) == 42);
    CHECK((elrsr & 1u) == 0);                /* and so not free */
    CHECK(vcpu_get_regs(v, &regs) == 0 && regs.pending_irq == ~0ull);

    /* The handler completes and the guest returns to its heartbeat. The
     * list register is free again and the interrupt is not redelivered:
     * one injection, one delivery. */
    CHECK(vcpu_run(v, &x) == 0);
    CHECK(x.kind == COSMO_VM_EXIT_HYPERCALL && x.hypercall.nr == 2);
    CHECK(arch_hv_vcpu_vgic_state(v->arch, &lr0, &elrsr));
    CHECK(lr_free(lr0, elrsr));
    CHECK(vcpu_get_regs(v, &regs) == 0 && regs.pending_irq == ~0ull);

    /* A second injection is delivered too: the register was released,
     * not merely emptied once. */
    CHECK(vcpu_inject(v, 43) == 0);
    CHECK(vcpu_run(v, &x) == 0);
    CHECK(x.kind == COSMO_VM_EXIT_HYPERCALL && x.hypercall.nr == 43);
    drop_guest(vm, v);
    kinfo("selftest: el2-guest-irq: a guest took INTID 42 and then 43, acknowledged and completed");
    return true;
}

/* --- an interrupt the guest has masked is not lost ---
 *
 * Injected while the guest has PSTATE.I set, it stays Pending in the
 * list register and stays pending in the owner's set; the guest takes it
 * when it unmasks. This is the AArch64 shape of the x86 test's `sti`
 * shadow case, and it is what tells "delivered" apart from "the guest
 * happened to call out for another reason".
 */
bool selftest_el2_guest_irq_masked(const char **reason)
{
    if (skip_without_backend(reason))
        return true;
    if (!hv_caps()->inject_irq) {
        kinfo("selftest: el2-guest-irq-masked: no virtual GIC on this machine; skipping");
        return true;
    }
    struct vm *vm;
    struct vcpu *v;
    CHECK(make_guest("tests/hv/guest_irq.bin", &vm, &v) == 0);
    struct cosmo_vm_exit x;
    memset(&x, 0, sizeof(x));
    CHECK(vcpu_run(v, &x) == 0);
    CHECK(x.kind == COSMO_VM_EXIT_HYPERCALL && x.hypercall.nr == 1);

    /* Mask IRQ in the guest, behind its back. */
    struct cosmo_vcpu_regs regs;
    CHECK(vcpu_get_regs(v, &regs) == 0);
    regs.pstate |= (1u << 7);                /* PSTATE.I */
    CHECK(vcpu_set_regs(v, &regs) == 0);

    CHECK(vcpu_inject(v, 42) == 0);
    CHECK(vcpu_run(v, &x) == 0);
    /* It did not take it: the guest reached its heartbeat instead of its
     * handler, and the list register is still Pending. */
    CHECK(x.kind == COSMO_VM_EXIT_HYPERCALL && x.hypercall.nr == 2);
    uint64_t lr0 = 0, elrsr = 0;
    CHECK(arch_hv_vcpu_vgic_state(v->arch, &lr0, &elrsr));
    CHECK((lr0 >> 62) == 1);                 /* Pending */
    CHECK(vcpu_get_regs(v, &regs) == 0 && regs.pending_irq == 42);

    /* Unmask, and it arrives. */
    regs.pstate &= ~(uint64_t)(1u << 7);
    CHECK(vcpu_set_regs(v, &regs) == 0);
    CHECK(vcpu_run(v, &x) == 0);
    CHECK(x.kind == COSMO_VM_EXIT_HYPERCALL && x.hypercall.nr == 42);

    /*
     * And it is delivered *once*. An interrupt that waited in the list
     * register across an earlier entry was placed by that entry, not by
     * this one, and a hypervisor that decides "was it taken?" from
     * "did I place it just now?" answers no here -- leaving the pending
     * bit set and handing the guest the same INTID again the moment the
     * EOI frees the register. The guest acknowledged it above, so the
     * bit must already be clear, and the runs after this must be
     * heartbeats.
     */
    CHECK(vcpu_get_regs(v, &regs) == 0 && regs.pending_irq == ~0ull);
    CHECK(vcpu_run(v, &x) == 0);                 /* the EOI, then the heartbeat */
    CHECK(x.kind == COSMO_VM_EXIT_HYPERCALL && x.hypercall.nr == 2);
    CHECK(vcpu_run(v, &x) == 0);                 /* and not INTID 42 a second time */
    CHECK(x.kind == COSMO_VM_EXIT_HYPERCALL && x.hypercall.nr == 2);
    drop_guest(vm, v);
    kinfo("selftest: el2-guest-irq-masked: held while PSTATE.I was set, delivered once when it cleared");
    return true;
}

/* --- the private interrupts, which the old range refused ---
 *
 * SGIs (0..15) and PPIs (16..31) are how a guest's own software
 * interrupts itself and how its timer will reach it; `vcpu_inject`
 * used to refuse everything below 32 because on x86 those numbers are
 * exceptions. The range is the architecture's now, and this is the
 * test that AArch64's is right and x86's is unchanged.
 */
bool selftest_el2_guest_irq_private(const char **reason)
{
    if (skip_without_backend(reason))
        return true;
    if (!hv_caps()->inject_irq) {
        kinfo("selftest: el2-guest-irq-private: no virtual GIC on this machine; skipping");
        return true;
    }
    unsigned lo, hi;
    arch_hv_vintr_range(&lo, &hi);
    CHECK(lo == 0 && hi == 1019);

    struct vm *vm;
    struct vcpu *v;
    CHECK(make_guest("tests/hv/guest_irq.bin", &vm, &v) == 0);
    struct cosmo_vm_exit x;
    memset(&x, 0, sizeof(x));
    CHECK(vcpu_run(v, &x) == 0);
    CHECK(x.kind == COSMO_VM_EXIT_HYPERCALL && x.hypercall.nr == 1);

    /* A PPI: the number the virtual timer will use when it exists. */
    CHECK(vcpu_inject(v, 27) == 0);
    CHECK(vcpu_run(v, &x) == 0);
    CHECK(x.kind == COSMO_VM_EXIT_HYPERCALL && x.hypercall.nr == 27);
    CHECK(vcpu_run(v, &x) == 0);                         /* the EOI, then the heartbeat */
    CHECK(x.kind == COSMO_VM_EXIT_HYPERCALL && x.hypercall.nr == 2);

    /* And an SGI, the lowest number there is. */
    CHECK(vcpu_inject(v, 0) == 0);
    CHECK(vcpu_run(v, &x) == 0);
    CHECK(x.kind == COSMO_VM_EXIT_HYPERCALL && x.hypercall.nr == 0);

    /* Past the end is still refused. */
    CHECK(vcpu_inject(v, 1020) == -EINVAL);
    CHECK(vcpu_inject(v, 8192) == -EINVAL);              /* an LPI: nothing maps one */
    drop_guest(vm, v);
    kinfo("selftest: el2-guest-irq-private: PPI 27 and SGI 0 delivered, 1020 refused");
    return true;
}

/* --- what the list register holds is not always what was offered ---
 *
 * With one list register, an interrupt the guest has masked stays in it
 * across every entry until the guest unmasks. Two things follow, and
 * both were wrong before this test existed:
 *
 *   - a *second* injection of the same INTID, while the first is still
 *     Active in the register, must not be swallowed by the first one's
 *     completion; and
 *   - a resident interrupt the guest finally takes must be cleared from
 *     the pending set even when a *lower-numbered* vector is the offer
 *     for that entry, or it is delivered twice.
 *
 * Both are the same mistake: asking "was the offered vector taken?"
 * when the question is "which interrupt did the guest take?".
 */
bool selftest_el2_guest_irq_queue(const char **reason)
{
    if (skip_without_backend(reason))
        return true;
    if (!hv_caps()->inject_irq) {
        kinfo("selftest: el2-guest-irq-queue: no virtual GIC on this machine; skipping");
        return true;
    }
    struct vm *vm;
    struct vcpu *v;
    struct cosmo_vcpu_regs regs;
    CHECK(make_guest("tests/hv/guest_irq.bin", &vm, &v) == 0);
    struct cosmo_vm_exit x;
    memset(&x, 0, sizeof(x));
    CHECK(vcpu_run(v, &x) == 0);
    CHECK(x.kind == COSMO_VM_EXIT_HYPERCALL && x.hypercall.nr == 1);

    /* --- the same INTID twice, the second while the first is Active --- */
    CHECK(vcpu_inject(v, 42) == 0);
    CHECK(vcpu_run(v, &x) == 0);
    CHECK(x.kind == COSMO_VM_EXIT_HYPERCALL && x.hypercall.nr == 42);   /* acknowledged */
    CHECK(vcpu_inject(v, 42) == 0);                                     /* again, while Active */
    CHECK(vcpu_run(v, &x) == 0);
    /* The guest completed the first and went back to its heartbeat. The
     * second injection must still be pending: the completion of one
     * instance is not the delivery of the next. */
    CHECK(x.kind == COSMO_VM_EXIT_HYPERCALL && x.hypercall.nr == 2);
    CHECK(vcpu_get_regs(v, &regs) == 0 && regs.pending_irq == 42);
    CHECK(vcpu_run(v, &x) == 0);
    CHECK(x.kind == COSMO_VM_EXIT_HYPERCALL && x.hypercall.nr == 42);   /* and now it arrives */
    CHECK(vcpu_run(v, &x) == 0);
    CHECK(x.kind == COSMO_VM_EXIT_HYPERCALL && x.hypercall.nr == 2);
    CHECK(vcpu_get_regs(v, &regs) == 0 && regs.pending_irq == ~0ull);

    /* --- a resident interrupt taken while a lower number is offered --- */
    CHECK(vcpu_get_regs(v, &regs) == 0);
    regs.pstate |= (1u << 7);                       /* mask */
    CHECK(vcpu_set_regs(v, &regs) == 0);
    CHECK(vcpu_inject(v, 42) == 0);
    CHECK(vcpu_run(v, &x) == 0);                    /* 42 goes into the register, unheeded */
    CHECK(x.kind == COSMO_VM_EXIT_HYPERCALL && x.hypercall.nr == 2);
    CHECK(vcpu_inject(v, 5) == 0);                  /* now a lower number is the offer */
    CHECK(vcpu_get_regs(v, &regs) == 0 && regs.pending_irq == 5);
    regs.pstate &= ~(uint64_t)(1u << 7);            /* unmask */
    CHECK(vcpu_set_regs(v, &regs) == 0);
    CHECK(vcpu_run(v, &x) == 0);
    /* The guest takes the resident 42, not the offered 5, and 42 is what
     * must be cleared. */
    CHECK(x.kind == COSMO_VM_EXIT_HYPERCALL && x.hypercall.nr == 42);
    CHECK(vcpu_get_regs(v, &regs) == 0 && regs.pending_irq == 5);
    CHECK(vcpu_run(v, &x) == 0);                    /* the EOI frees the register */
    CHECK(x.kind == COSMO_VM_EXIT_HYPERCALL && x.hypercall.nr == 2);
    CHECK(vcpu_run(v, &x) == 0);
    CHECK(x.kind == COSMO_VM_EXIT_HYPERCALL && x.hypercall.nr == 5);    /* then 5, once */
    CHECK(vcpu_get_regs(v, &regs) == 0 && regs.pending_irq == ~0ull);
    drop_guest(vm, v);
    kinfo("selftest: el2-guest-irq-queue: a second instance is not swallowed, and a resident "
          "interrupt is cleared when it is taken");
    return true;
}

/* --- a guest's timer does not outlive the guest ---
 *
 * The measurement that opened the virtual-timer report, as a test. The
 * host's tick is the physical timer and the virtual one is the guest's,
 * but they are one set of registers: before the switch saved and
 * disarmed them, a guest that armed CNTV left ENABLE live in the host
 * (CNTV_CTL_EL0 read 0x1 where the host's own IMASK, 0x2, had been).
 */
bool selftest_el2_guest_timer_isolated(const char **reason)
{
    if (skip_without_backend(reason))
        return true;
    struct vm *vm;
    struct vcpu *v;
    CHECK(make_guest("tests/hv/guest_ctimer.bin", &vm, &v) == 0);
    struct cosmo_vm_exit x;
    memset(&x, 0, sizeof(x));
    CHECK(vcpu_run(v, &x) == 0);                                    /* ready */
    CHECK(x.kind == COSMO_VM_EXIT_HYPERCALL && x.hypercall.nr == 1);
    CHECK(vcpu_run(v, &x) == 0);                                    /* armed, then the heartbeat */
    CHECK(x.kind == COSMO_VM_EXIT_HYPERCALL && x.hypercall.nr == 2);

    /* The guest armed its timer -- its saved state says so -- and the
     * host's CNTV_CTL as that run's *exit* left it is disarmed. Read
     * from the value the switch captured with interrupts off, so a
     * missing disarm is seen here and not papered over by the host's
     * PPI handler cleaning up a moment later. */
    uint64_t ctl = 0, off = 0;
    CHECK(arch_hv_vcpu_timer_state(v->arch, &ctl, &off));
    CHECK((ctl & 1u) != 0);                                         /* the guest armed it */
    uint64_t host_after = arch_hv_vcpu_host_vtimer_after(v->arch);
    CHECK((host_after & 1u) == 0);                                  /* the exit disarmed it */
    drop_guest(vm, v);
    kinfo("selftest: el2-guest-timer-isolated: guest saved CNTV_CTL 0x%llx, host left 0x%llx",
          (unsigned long long)ctl, (unsigned long long)host_after);
    return true;
}

/* --- a guest's clock is its VM's, not the host's and not its vCPU's ---
 *
 * CNTVOFF_EL2 is one value per VM. Two VMs created at different times
 * must see different clocks, neither of them the host's uptime; two
 * vCPUs of one VM created at different times must see the same one.
 * The second half is what a per-vCPU offset would fail, and a guest
 * that compares time across its CPUs would find them disagreeing by
 * however long apart the vCPUs were made.
 */
bool selftest_el2_guest_timer_offset(const char **reason)
{
    if (skip_without_backend(reason))
        return true;
    struct vm *vm;
    struct vcpu *v0;
    CHECK(make_guest("tests/hv/guest_ctimer.bin", &vm, &v0) == 0);
    struct cosmo_vm_exit x;
    memset(&x, 0, sizeof(x));
    CHECK(vcpu_run(v0, &x) == 0);
    CHECK(x.kind == COSMO_VM_EXIT_HYPERCALL && x.hypercall.nr == 1);
    uint64_t t_vm_a = x.hypercall.a0;                               /* the guest's CNTVCT */

    /* A second vCPU of the same VM, made later: same clock. */
    thread_sleep_ms(20);
    struct vcpu *v1;
    CHECK(vcpu_create(vm, 1, &v1) == 0);
    struct cosmo_vcpu_regs regs;
    CHECK(vcpu_get_regs(v1, &regs) == 0);
    regs.pc = LOAD_GPA;
    CHECK(vcpu_set_regs(v1, &regs) == 0);
    CHECK(vcpu_run(v1, &x) == 0);
    CHECK(x.kind == COSMO_VM_EXIT_HYPERCALL && x.hypercall.nr == 1);
    uint64_t t_vm_a_cpu1 = x.hypercall.a0;
    uint64_t off0 = 0, off1 = 0, c = 0;
    CHECK(arch_hv_vcpu_timer_state(v0->arch, &c, &off0));
    CHECK(arch_hv_vcpu_timer_state(v1->arch, &c, &off1));
    CHECK(off0 == off1);                                            /* the VM's, copied */
    /* Both read a clock that started at the VM's creation: small, and
     * the later vCPU's later -- by the 20 ms plus the run, not by the
     * host's uptime. */
    CHECK(t_vm_a_cpu1 > t_vm_a);

    /* A second VM, made later still: a different, also-small clock. */
    struct vm *vm_b;
    struct vcpu *vb;
    CHECK(make_guest("tests/hv/guest_ctimer.bin", &vm_b, &vb) == 0);
    CHECK(vcpu_run(vb, &x) == 0);
    CHECK(x.kind == COSMO_VM_EXIT_HYPERCALL && x.hypercall.nr == 1);
    uint64_t t_vm_b = x.hypercall.a0;
    uint64_t offb = 0;
    CHECK(arch_hv_vcpu_timer_state(vb->arch, &c, &offb));
    CHECK(offb != off0);                                            /* its own */
    /* Neither VM sees the host's counter: the offset is subtracted, so a
     * guest's first read is well below the offset itself. */
    CHECK(t_vm_a < off0);
    CHECK(t_vm_b < offb);
    kobject_put(&v1->obj);
    drop_guest(vm_b, vb);
    drop_guest(vm, v0);
    kinfo("selftest: el2-guest-timer-offset: VM A read %llu then %llu on its second vCPU; VM B read %llu",
          (unsigned long long)t_vm_a, (unsigned long long)t_vm_a_cpu1, (unsigned long long)t_vm_b);
    return true;
}

/* --- a guest may not read the host's clock or arm the host's tick ---
 *
 * CNTPCT_EL0 is the host's uptime and CNTP_* is the host's tick timer;
 * both were open to a guest at EL1 because the loader's CNTHCTL_EL2
 * (0x3) permits EL1 to use them and a guest is at EL1. The switch clears
 * it for a guest now, so each access is a trap the owner sees -- and the
 * second half is the one worth having: a guest that armed the host's
 * tick was a fault the host would have felt.
 */
bool selftest_el2_guest_phys_timer(const char **reason)
{
    if (skip_without_backend(reason))
        return true;
    struct vm *vm;
    struct vcpu *v;
    CHECK(make_guest("tests/hv/guest_ptimer.bin", &vm, &v) == 0);
    struct cosmo_vm_exit x;
    memset(&x, 0, sizeof(x));
    struct cosmo_vcpu_regs regs;

    /* mrs x3, CNTPCT_EL0: a read of a CRn 14 register, trapped. The
     * ISS names the register: Op0[21:20]=3 Op1[16:14]=3 CRn[13:10]=14
     * CRm[4:1]=0 Op2[19:17]=1 for the counter. */
    CHECK(vcpu_run(v, &x) == 0);
    CHECK(x.kind == COSMO_VM_EXIT_SYSREG);
    CHECK(!x.sysreg.write && x.sysreg.reg == 3);
    CHECK(((x.sysreg.iss >> 10) & 0xF) == 14);                     /* CRn: the timers */
    CHECK(((x.sysreg.iss >> 1) & 0xF) == 0 && ((x.sysreg.iss >> 17) & 0x7) == 1);  /* CNTPCT */
    CHECK(vcpu_get_regs(v, &regs) == 0);
    regs.x[3] = 0x1234;                                             /* the owner's answer */
    regs.pc += 4;
    CHECK(vcpu_set_regs(v, &regs) == 0);

    /* msr CNTP_CTL_EL0, x4: a write to CRm 2 Op2 1, trapped -- and the
     * host's own timer control must not have moved. */
    CHECK(vcpu_run(v, &x) == 0);
    CHECK(x.kind == COSMO_VM_EXIT_SYSREG);
    CHECK(x.sysreg.write && x.sysreg.reg == 4);
    CHECK(((x.sysreg.iss >> 10) & 0xF) == 14);
    CHECK(((x.sysreg.iss >> 1) & 0xF) == 2 && ((x.sysreg.iss >> 17) & 0x7) == 1);  /* CNTP_CTL */
    CHECK(vcpu_get_regs(v, &regs) == 0);
    regs.pc += 4;
    CHECK(vcpu_set_regs(v, &regs) == 0);

    CHECK(vcpu_run(v, &x) == 0);
    CHECK(x.kind == COSMO_VM_EXIT_HYPERCALL && x.hypercall.nr == 9);
    CHECK(vcpu_get_regs(v, &regs) == 0 && regs.x[3] == 0x1234);    /* the guest got the answer, not the clock */
    drop_guest(vm, v);
    kinfo("selftest: el2-guest-phys-timer: CNTPCT_EL0 read and CNTP_CTL_EL0 write both trapped");
    return true;
}

/* --- a guest is woken by its own timer ---
 *
 * The unit's point. The guest arms CNTV for ~15 ms and heartbeats; the
 * owner runs it until the guest's handler calls out, and requires that
 * what arrived was the timer's INTID -- not one the owner injected,
 * since the owner injects nothing here -- and that the guest's own
 * CNTV_CTL, read in the handler, shows the timer had fired. A backend
 * that injected on every host interrupt would deliver on the host's
 * tick instead, and the handler's CNTV_CTL would not read ISTATUS.
 */
bool selftest_el2_guest_timer(const char **reason)
{
    if (skip_without_backend(reason))
        return true;
    if (!hv_caps()->inject_irq || arch_hv_guest_timer_intid() == 0) {
        kinfo("selftest: el2-guest-timer: no virtual GIC or no guest timer here; skipping");
        return true;
    }
    unsigned intid = arch_hv_guest_timer_intid();
    struct vm *vm;
    struct vcpu *v;
    CHECK(make_guest("tests/hv/guest_timer.bin", &vm, &v) == 0);
    struct cosmo_vm_exit x;
    memset(&x, 0, sizeof(x));
    CHECK(vcpu_run(v, &x) == 0);                                    /* ready */
    CHECK(x.kind == COSMO_VM_EXIT_HYPERCALL && x.hypercall.nr == 1);
    uint64_t armed_at = x.hypercall.a0;

    /* Heartbeats until the handler speaks; bounded, because a timer that
     * never fires is the failure this test exists to catch. */
    unsigned beats = 0;
    for (;;) {
        CHECK(vcpu_run(v, &x) == 0);
        CHECK(x.kind == COSMO_VM_EXIT_HYPERCALL);
        if (x.hypercall.nr == intid)
            break;
        CHECK(x.hypercall.nr == 2);
        CHECK(++beats < 20000);
    }
    /* The handler's x1 is CNTV_CTL as the guest read it: ENABLE and
     * ISTATUS, before it masked. And a real expiry is at or after the
     * deadline the guest asked for. */
    CHECK((x.hypercall.a0 & 0x5u) == 0x5u);
    uint64_t ctl = 0, off = 0;
    CHECK(arch_hv_vcpu_timer_state(v->arch, &ctl, &off));
    CHECK((ctl & 0x2u) != 0);                                       /* the handler masked it */
    struct cosmo_vcpu_regs regs;
    CHECK(vcpu_get_regs(v, &regs) == 0 && regs.pending_irq == ~0ull);   /* delivered, cleared */

    /* Completing the handler returns the guest to its heartbeat, whose
     * clock is past the deadline; and the timer, masked, does not fire
     * again. */
    CHECK(vcpu_run(v, &x) == 0);
    CHECK(x.kind == COSMO_VM_EXIT_HYPERCALL && x.hypercall.nr == 2);
    CHECK(x.hypercall.a0 > armed_at);
    CHECK(vcpu_run(v, &x) == 0);
    CHECK(x.kind == COSMO_VM_EXIT_HYPERCALL && x.hypercall.nr == 2);
    drop_guest(vm, v);
    kinfo("selftest: el2-guest-timer: INTID %u after %u heartbeat(s), handler saw CNTV_CTL 0x%llx", intid,
          beats, (unsigned long long)ctl);
    return true;
}

/* --- and woken on time ---
 *
 * A guest that arms its timer and waits in WFI is the shape of every
 * idle loop. Before this, the WFI exit came back at once and the owner
 * could only spin on re-entry; now `vcpu_run` waits until the guest's
 * deadline (or an injection) before returning it. Two things are
 * measured, both in units the guest controls: the WFI run must have
 * taken most of the 15 ms the guest asked for, and the handler's own
 * CNTVCT minus its CVAL -- the lateness -- must be a small number of
 * guest ticks, not the "whenever the owner next ran it" of before.
 */
bool selftest_el2_guest_timer_ontime(const char **reason)
{
    if (skip_without_backend(reason))
        return true;
    if (!hv_caps()->inject_irq || arch_hv_guest_timer_intid() == 0) {
        kinfo("selftest: el2-guest-timer-ontime: no virtual GIC or no guest timer here; skipping");
        return true;
    }
    unsigned intid = arch_hv_guest_timer_intid();
    struct vm *vm;
    struct vcpu *v;
    CHECK(make_guest("tests/hv/guest_timer_wfi.bin", &vm, &v) == 0);
    struct cosmo_vm_exit x;
    memset(&x, 0, sizeof(x));
    CHECK(vcpu_run(v, &x) == 0);                                    /* armed */
    CHECK(x.kind == COSMO_VM_EXIT_HYPERCALL && x.hypercall.nr == 1);
    uint64_t armed_at = x.hypercall.a0, cval = x.hypercall.a1;
    uint64_t asked_ticks = cval - armed_at;                         /* ~15 ms of guest ticks */

    /* The WFI: the guest has nothing to do until its timer, and the run
     * must not come back until then. Measured on the host's clock. */
    uint64_t t0 = clock_now_ns();
    CHECK(vcpu_run(v, &x) == 0);
    uint64_t waited = clock_now_ns() - t0;
    CHECK(x.kind == COSMO_VM_EXIT_WFI);
    uint64_t asked_ns = asked_ticks * 1000000000ULL / arch_clock_hz();
    CHECK(waited >= asked_ns / 2);                                  /* it waited, rather than returning at once */

    /* Re-entered, the guest takes its timer straight away. */
    CHECK(vcpu_run(v, &x) == 0);
    CHECK(x.kind == COSMO_VM_EXIT_HYPERCALL && x.hypercall.nr == intid);
    uint64_t fired_at = x.hypercall.a0, cval_seen = x.hypercall.a1;
    CHECK(cval_seen == cval);
    CHECK(fired_at >= cval);                                        /* never early */
    uint64_t late_ticks = fired_at - cval;
    /* Late by less than the time it asked for: the owner's re-entry and
     * a tick's granularity, not a scheduling accident. */
    CHECK(late_ticks < asked_ticks);
    drop_guest(vm, v);
    kinfo("selftest: el2-guest-timer-ontime: asked %llu ticks, WFI held the run %llu ms, "
          "fired %llu ticks late",
          (unsigned long long)asked_ticks, (unsigned long long)(waited / 1000000ULL),
          (unsigned long long)late_ticks);
    return true;
}

/* --- the guest's distributor ------------------------------------------- */

static __maybe_unused bool skip_without_vdist(const char *name, const char **reason)
{
    if (skip_without_backend(reason))
        return true;
    if (!hv_caps()->inject_irq) {
        kinfo("selftest: %s: no virtual GIC, so no distributor here; skipping", name);
        return true;
    }
    return false;
}

/*
 * A guest that reads GICD_TYPER gets an answer and not a fault to its
 * owner; the answer describes this distributor; and each of two vCPUs
 * finds a redistributor frame that is its own, carrying its affinity,
 * with Last on the frame of the higher one and not the lower. The MPIDR
 * a vCPU reads is its index, on whatever host CPU it happened to run.
 */
bool selftest_el2_guest_gicd_probe(const char **reason)
{
    if (skip_without_vdist("el2-guest-gicd-probe", reason))
        return true;
    struct vm *vm;
    struct vcpu *v0, *v1;
    CHECK(make_guest("tests/hv/guest_gicd.bin", &vm, &v0) == 0);
    CHECK(vcpu_create(vm, 1, &v1) == 0);          /* before either runs: both frames exist */
    struct cosmo_vcpu_regs regs;
    CHECK(vcpu_get_regs(v1, &regs) == 0);
    regs.pc = LOAD_GPA;
    CHECK(vcpu_set_regs(v1, &regs) == 0);

    struct cosmo_vm_exit x;
    memset(&x, 0, sizeof(x));
    CHECK(vcpu_run(v0, &x) == 0);
    CHECK(x.kind == COSMO_VM_EXIT_HYPERCALL);      /* not MMIO: something answered */
    CHECK(x.hypercall.nr == 7);
    uint64_t typer = x.hypercall.a0;
    unsigned lines = 32u * ((unsigned)(typer & 0x1Fu) + 1u);
    CHECK(lines == 288);
    CHECK(x.hypercall.a1 == 0x43Bu);              /* IIDR */
    CHECK((x.hypercall.a2 & 0xF0u) == 0x30u);      /* PIDR2: a GICv3 */
    uint64_t rtyper0 = x.hypercall.a3;
    CHECK((rtyper0 >> 32) == 0);                   /* frame 0 carries Aff0 = 0 */
    CHECK(((rtyper0 >> 8) & 0xFFFFu) == 0);        /* processor number 0 */
    CHECK((rtyper0 & (1u << 4)) == 0);             /* not Last: vCPU 1's frame follows */
    CHECK(vcpu_get_regs(v0, &regs) == 0);
    CHECK((regs.x[5] & 0xFFu) == 0 && (regs.x[5] & (1ull << 31)) != 0);   /* MPIDR: vCPU 0 */

    CHECK(vcpu_run(v1, &x) == 0);
    CHECK(x.kind == COSMO_VM_EXIT_HYPERCALL && x.hypercall.nr == 7);
    uint64_t rtyper1 = x.hypercall.a3;
    CHECK((rtyper1 >> 32) == 1);                   /* frame 1 carries Aff0 = 1 */
    CHECK(((rtyper1 >> 8) & 0xFFFFu) == 1);
    CHECK((rtyper1 & (1u << 4)) != 0);             /* and it is the last */
    CHECK(vcpu_get_regs(v1, &regs) == 0);
    CHECK((regs.x[5] & 0xFFu) == 1);

    kobject_put(&v1->obj);
    drop_guest(vm, v0);
    kinfo("selftest: el2-guest-gicd-probe: GICD_TYPER 0x%llx (%u lines), two vCPUs each found their frame",
          (unsigned long long)typer, lines);
    return true;
}

/*
 * The register file returns what was written, through each access size
 * a driver uses (a 64-bit route, 32-bit words, a single priority byte),
 * a set/clear pair acts on one state, and the state is where the
 * architecture puts it: an SPI's is the VM's and any vCPU reads it, a
 * PPI's is one redistributor's and a sibling's frame does not show it.
 * vCPU 1 configures, so the route it writes -- its own affinity -- reads
 * back as 1 and not as the zero an unimplemented register would give.
 */
bool selftest_el2_guest_gic_config(const char **reason)
{
    if (skip_without_vdist("el2-guest-gic-config", reason))
        return true;
    const void *probe;
    size_t probe_len;
    CHECK(bootarchive_find("tests/hv/guest_gicd.bin", &probe, &probe_len));
    struct vm *vm;
    struct vcpu *v0, *v1;
    CHECK(make_guest("tests/hv/guest_gicc.bin", &vm, &v0) == 0);
    CHECK(vm_mem_write(vm, 0x20000, probe, probe_len) == 0);   /* the prober, for vCPU 0 */
    struct cosmo_vcpu_regs regs;
    CHECK(vcpu_get_regs(v0, &regs) == 0);
    regs.pc = 0x20000;
    regs.x[8] = 40;
    CHECK(vcpu_set_regs(v0, &regs) == 0);
    CHECK(vcpu_create(vm, 1, &v1) == 0);
    CHECK(vcpu_get_regs(v1, &regs) == 0);
    regs.pc = LOAD_GPA;                                        /* the configurer */
    regs.x[8] = 40;
    CHECK(vcpu_set_regs(v1, &regs) == 0);

    struct cosmo_vm_exit x;
    memset(&x, 0, sizeof(x));
    CHECK(vcpu_run(v1, &x) == 0);
    CHECK(x.kind == COSMO_VM_EXIT_HYPERCALL && x.hypercall.nr == 3);
    CHECK(x.hypercall.a0 == (1ull << 8));                       /* SPI 40 enabled: word 1, bit 8 */
    CHECK(x.hypercall.a1 == 0xA0u);                            /* its priority, lane 0 of its word */
    CHECK(x.hypercall.a2 == (1ull << 27));                      /* PPI 27 enabled in vCPU 1's frame */
    CHECK(x.hypercall.a3 == 0x90332211u);                      /* its priority, written as one byte, its three neighbours untouched */
    CHECK(vcpu_get_regs(v1, &regs) == 0);
    CHECK(regs.x[6] == 1);                                     /* routed to Aff0 = 1: the writer */
    CHECK(regs.x[7] == (1ull << 8));                           /* in group 1 */
    CHECK(regs.x[9] == 0);                                     /* awake, and its children with it */
    CHECK(regs.x[15] == 0x53u);                                /* CTLR as written, plus DS */

    /* vCPU 0 looks: the SPI's enable is the VM's and it sees it; the
     * PPI's is vCPU 1's frame's and its own frame shows none. */
    CHECK(vcpu_run(v0, &x) == 0);
    CHECK(x.kind == COSMO_VM_EXIT_HYPERCALL && x.hypercall.nr == 7);
    CHECK(vcpu_get_regs(v0, &regs) == 0);
    CHECK(regs.x[6] == (1ull << 8));
    CHECK(regs.x[7] == 0);

    /* Clearing through ICENABLER clears what ISENABLER set, and only that. */
    CHECK(vcpu_run(v1, &x) == 0);
    CHECK(x.kind == COSMO_VM_EXIT_HYPERCALL && x.hypercall.nr == 4);
    CHECK(x.hypercall.a0 == 0);
    CHECK(x.hypercall.a2 == (1ull << 27));

    kobject_put(&v1->obj);
    drop_guest(vm, v0);
    kinfo("selftest: el2-guest-gic-config: SPI 40 and PPI 27 configured and read back through 64-, 32- and 8-bit accesses");
    return true;
}

/* Run until the guest's hypercall `want`, allowing only the numbers in
 * `allowed` (a bitmask over 0..63) on the way; each stop is one guest
 * exit. Fails the bound, or an unexpected number, by returning false. */
static __maybe_unused bool run_until(struct vcpu *v, struct cosmo_vm_exit *x, unsigned want, uint64_t allowed,
                                     unsigned bound, unsigned *steps)
{
    for (unsigned i = 0; i < bound; i++) {
        if (vcpu_run(v, x) != 0 || x->kind != COSMO_VM_EXIT_HYPERCALL)
            return false;
        if (steps)
            (*steps)++;
        if (x->hypercall.nr == want)
            return true;
        if (x->hypercall.nr >= 64 || !(allowed & (1ull << x->hypercall.nr)))
            return false;
    }
    return false;
}

/*
 * The whole thing, the ordinary way: a guest switches on its distributor,
 * wakes its redistributor, groups, prioritises and enables its timer PPI
 * there, routes and enables an SPI in the distributor, then turns on its
 * CPU interface -- and its timer arrives through the controller it
 * configured. Then the part only a distributor can show: with the PPI
 * disabled in the redistributor the next expiry is *held*, not delivered,
 * past its deadline; re-enabling releases it. Last, an SPI the guest
 * makes pending by hand through ISPENDR arrives at the vCPU its route
 * names. Direct injection could pass the first phase; it cannot pass the
 * second, which is the phase that proves the enable bit gates delivery.
 */
bool selftest_el2_guest_gic_timer(const char **reason)
{
    if (skip_without_vdist("el2-guest-gic-timer", reason))
        return true;
    if (arch_hv_guest_timer_intid() == 0) {
        kinfo("selftest: el2-guest-gic-timer: no guest timer here; skipping");
        return true;
    }
    unsigned intid = arch_hv_guest_timer_intid();
    struct vm *vm;
    struct vcpu *v;
    CHECK(make_guest("tests/hv/guest_gic.bin", &vm, &v) == 0);
    struct cosmo_vcpu_regs regs;
    CHECK(vcpu_get_regs(v, &regs) == 0);
    regs.x[8] = 40;
    CHECK(vcpu_set_regs(v, &regs) == 0);
    struct cosmo_vm_exit x;
    memset(&x, 0, sizeof(x));
    CHECK(vcpu_run(v, &x) == 0);
    CHECK(x.kind == COSMO_VM_EXIT_HYPERCALL && x.hypercall.nr == 1);     /* configured, ready */

    /* Phase A: the timer, through the redistributor the guest enabled it in. */
    unsigned beats = 0;
    CHECK(run_until(v, &x, intid, (1ull << 2), 20000, &beats));
    CHECK((x.hypercall.a0 & 0x5u) == 0x5u);                         /* ENABLE and ISTATUS, as the handler saw it */
    CHECK(run_until(v, &x, 3, (1ull << 2), 4, NULL));               /* re-armed, PPI disabled */
    uint64_t rearmed_at = x.hypercall.a0, cval = x.hypercall.a1;
    CHECK(cval > rearmed_at);

    /* Phase B: past the deadline with the PPI disabled -- heartbeats only,
     * no handler. The guest's own count of handler runs must be zero. */
    unsigned held = 0;
    CHECK(run_until(v, &x, 5, (1ull << 4), 200000, &held));
    CHECK(x.hypercall.a0 == 0);

    /* Phase C: re-enabled; the held expiry arrives, and it is the timer. */
    unsigned released = 0;
    CHECK(run_until(v, &x, intid, (1ull << 6), 2000, &released));
    CHECK((x.hypercall.a0 & 0x5u) == 0x5u);

    /* Phase D: an SPI made pending by the guest, routed to itself. */
    CHECK(run_until(v, &x, 40, (1ull << 6) | (1ull << 7), 2000, NULL));
    CHECK(run_until(v, &x, 8, (1ull << 7), 4, NULL));
    drop_guest(vm, v);
    kinfo("selftest: el2-guest-gic-timer: PPI %u after %u beat(s); held through %u beat(s) while disabled, released after %u; SPI 40 by ISPENDR",
          intid, beats, held, released);
    return true;
}

/*
 * A guest can be SMP: vCPU 0 writes ICC_SGI1R_EL1 naming SGI 3 for the
 * CPU whose Aff0 is 1, and vCPU 1's handler runs with INTID 3 on a CPU
 * whose MPIDR says 1. Untrapped, the write would go to the host's GIC or
 * nowhere and vCPU 1 would wait forever -- the hang an SMP kernel would
 * hit bringing up its second CPU. And it is routed, not broadcast: vCPU
 * 0, which was not in the target list, does not take it.
 */
bool selftest_el2_guest_sgi(const char **reason)
{
    if (skip_without_vdist("el2-guest-sgi", reason))
        return true;
    struct vm *vm;
    struct vcpu *v0, *v1;
    CHECK(make_guest("tests/hv/guest_sgi.bin", &vm, &v0) == 0);
    CHECK(vcpu_create(vm, 1, &v1) == 0);
    struct cosmo_vcpu_regs regs;
    CHECK(vcpu_get_regs(v1, &regs) == 0);
    regs.pc = LOAD_GPA;
    CHECK(vcpu_set_regs(v1, &regs) == 0);
    struct cosmo_vm_exit x;
    memset(&x, 0, sizeof(x));

    /* Both ready, each knowing who it is. */
    CHECK(vcpu_run(v0, &x) == 0);
    CHECK(x.kind == COSMO_VM_EXIT_HYPERCALL && x.hypercall.nr == 1 && (x.hypercall.a0 & 0xFFu) == 0);
    CHECK(vcpu_run(v1, &x) == 0);
    CHECK(x.kind == COSMO_VM_EXIT_HYPERCALL && x.hypercall.nr == 1 && (x.hypercall.a0 & 0xFFu) == 1);

    /* vCPU 0 sends and says so; the write did not reach its owner. */
    CHECK(vcpu_run(v0, &x) == 0);
    CHECK(x.kind == COSMO_VM_EXIT_HYPERCALL && x.hypercall.nr == 9);

    /* vCPU 1 takes it: INTID 3, in a handler running as Aff0 = 1. */
    unsigned beats = 0;
    CHECK(run_until(v1, &x, 3, (1ull << 2), 100, &beats));
    CHECK((x.hypercall.a0 & 0xFFu) == 1);
    CHECK(vcpu_run(v1, &x) == 0);                                   /* completes, back to its heartbeat */
    CHECK(x.kind == COSMO_VM_EXIT_HYPERCALL && x.hypercall.nr == 2);

    /* vCPU 0 was not a target and never sees it. */
    for (unsigned i = 0; i < 20; i++) {
        CHECK(vcpu_run(v0, &x) == 0);
        CHECK(x.kind == COSMO_VM_EXIT_HYPERCALL && x.hypercall.nr == 2);
    }
    kobject_put(&v1->obj);
    drop_guest(vm, v0);
    kinfo("selftest: el2-guest-sgi: vCPU 0 sent SGI 3 to Aff0 1; vCPU 1 took it after %u beat(s), vCPU 0 never did", beats);
    return true;
}

/*
 * A guest's distributor writes land in its VM's distributor and nowhere
 * else. Nowhere else has two readers: the host, whose physical GICD sits
 * at the very address the guest wrote to and whose enable bit for that
 * line must not move (a model that "helpfully" wrote through to hardware
 * so a device could fire would fail here); and a second VM, whose own
 * fresh distributor must show none of what the first configured (a model
 * with one static register file for all VMs would fail here). The line is
 * the host's spare SPI, so what the host's bit reads is not an accident
 * of some device having enabled it already.
 */
bool selftest_el2_guest_gicd_isolated(const char **reason)
{
    if (skip_without_vdist("el2-guest-gicd-isolated", reason))
        return true;
    int spare = arch_test_irq_spare_gsi();
    if (spare < 32 || spare >= 288) {
        kinfo("selftest: el2-guest-gicd-isolated: no spare SPI a guest's distributor also has; skipping");
        return true;
    }
    int host_before = arch_test_irq_is_enabled((unsigned)spare);
    CHECK(host_before >= 0);

    /* VM A configures the line in its distributor, and sees it there. */
    struct vm *vm_a, *vm_b;
    struct vcpu *va, *vb;
    CHECK(make_guest("tests/hv/guest_gicc.bin", &vm_a, &va) == 0);
    struct cosmo_vcpu_regs regs;
    CHECK(vcpu_get_regs(va, &regs) == 0);
    regs.x[8] = (uint64_t)spare;
    CHECK(vcpu_set_regs(va, &regs) == 0);
    struct cosmo_vm_exit x;
    memset(&x, 0, sizeof(x));
    CHECK(vcpu_run(va, &x) == 0);
    CHECK(x.kind == COSMO_VM_EXIT_HYPERCALL && x.hypercall.nr == 3);
    CHECK(x.hypercall.a0 == (1ull << (spare % 32)));
    CHECK(regs.x[8] == (uint64_t)spare);

    /* The host's line did not move. */
    CHECK(arch_test_irq_is_enabled((unsigned)spare) == host_before);

    /* VM B looks at the same registers of its own distributor: nothing. */
    CHECK(make_guest("tests/hv/guest_gicd.bin", &vm_b, &vb) == 0);
    CHECK(vcpu_get_regs(vb, &regs) == 0);
    regs.x[8] = (uint64_t)spare;
    CHECK(vcpu_set_regs(vb, &regs) == 0);
    CHECK(vcpu_run(vb, &x) == 0);
    CHECK(x.kind == COSMO_VM_EXIT_HYPERCALL && x.hypercall.nr == 7);
    CHECK(vcpu_get_regs(vb, &regs) == 0);
    CHECK(regs.x[6] == 0);                                  /* the SPI: not enabled here */
    CHECK(regs.x[7] == 0);                                  /* the PPI: not here either */

    drop_guest(vm_b, vb);
    drop_guest(vm_a, va);
    CHECK(arch_test_irq_is_enabled((unsigned)spare) == host_before);
    kinfo("selftest: el2-guest-gicd-isolated: VM A enabled SPI %d in its distributor; the host's bit stayed %d and VM B saw 0",
          spare, host_before);
    return true;
}

/* --- devices ------------------------------------------------------------ */

struct test_word_dev {
    uint64_t word;
    unsigned writes, wsize;
    uint64_t wgpa, wval;
};

static __maybe_unused int test_word_mmio(struct vm_device *d, uint64_t gpa, bool write, unsigned size,
                                         uint64_t *value)
{
    struct test_word_dev *t = d->priv;
    unsigned off = (unsigned)(gpa & 0xFFFu);
    if (write) {
        t->writes++;
        t->wsize = size;
        t->wgpa = gpa;
        t->wval = *value;
        return 0;
    }
    uint64_t v = off < 8 ? t->word >> (8u * off) : 0;
    *value = size >= 8 ? v : (v & ((1ull << (8u * size)) - 1u));
    return 0;
}

/*
 * The seam every device will use, tested before the first device leans
 * on it. A word of memory the test registers at an address answers a
 * guest's loads of every width and extension the architecture has, and
 * each must land in the register as that load would leave it -- the
 * ldrsb into a W register reads 0xFFFFFFF3 and not 0xF3 or
 * 0xFFFFFFFFFFFFFFF3 -- with no exit to the owner; a halfword store
 * reports its size and value; and a load no device claims goes to the
 * owner, who answers it in the exit, and the answer lands the same way.
 */
bool selftest_el2_mmio_device(const char **reason)
{
    if (skip_without_backend(reason))
        return true;
    struct vm *vm;
    struct vcpu *v;
    CHECK(make_guest("tests/hv/guest_mmio_widths.bin", &vm, &v) == 0);
    struct test_word_dev t;
    memset(&t, 0, sizeof(t));
    t.word = 0x80018081F0F1F2F3ull;
    struct vm_device dev;
    memset(&dev, 0, sizeof(dev));
    dev.name = "test-word";
    dev.mmio_base = 0x0A001000ull;
    dev.mmio_len = 0x1000;
    dev.mmio = test_word_mmio;
    dev.priv = &t;
    CHECK(vm_device_register(vm, &dev) == 0);

    struct cosmo_vm_exit x;
    memset(&x, 0, sizeof(x));
    CHECK(vcpu_run(v, &x) == 0);
    CHECK(x.kind == COSMO_VM_EXIT_HYPERCALL && x.hypercall.nr == 1);   /* seven loads, no exit */
    CHECK(x.hypercall.a0 == 0xF3ull);                                   /* ldrb  w */
    CHECK(x.hypercall.a1 == 0xF2F3ull);                                 /* ldrh  w */
    CHECK(x.hypercall.a2 == 0xF0F1F2F3ull);                             /* ldr   w */
    CHECK(x.hypercall.a3 == 0x80018081F0F1F2F3ull);                     /* ldr   x */
    struct cosmo_vcpu_regs regs;
    CHECK(vcpu_get_regs(v, &regs) == 0);
    CHECK(regs.x[5] == 0xFFFFFFF3ull);                                  /* ldrsb w: signed to 32, zero above */
    CHECK(regs.x[6] == 0xFFFFFFFFFFFFF2F3ull);                          /* ldrsh x: signed to 64 */
    CHECK(regs.x[7] == 0xFFFFFFFFF0F1F2F3ull);                          /* ldrsw x */
    CHECK(t.writes == 1 && t.wsize == 2 && t.wgpa == 0x0A001008ull && t.wval == 0xBEEFull);

    /* No device at 0x40000000: the owner is asked, with everything it
     * needs, and answers in the exit; the answer lands in x9. */
    CHECK(vcpu_run(v, &x) == 0);
    CHECK(x.kind == COSMO_VM_EXIT_MMIO && x.mmio.gpa == 0x40000000ull && !x.mmio.write);
    CHECK(x.mmio.size == 8 && x.mmio.reg == 9 && x.mmio.sf);
    x.mmio.value = 0x1122334455667788ull;
    CHECK(vcpu_run(v, &x) == 0);
    CHECK(x.kind == COSMO_VM_EXIT_HYPERCALL && x.hypercall.nr == 2);
    CHECK(vcpu_get_regs(v, &regs) == 0);
    CHECK(regs.x[9] == 0x1122334455667788ull);
    drop_guest(vm, v);
    kinfo("selftest: el2-mmio-device: seven loads completed in the kernel by width and sign, one by the owner");
    return true;
}

/*
 * The measurement that opened the report, as a test: a guest stores
 * "hello\n" to the UART's data register a byte at a time, the way an
 * earlycon does, and the VM's console -- what the owner reads from the
 * VM's descriptor -- holds exactly that. The flag register said the
 * transmitter was ready throughout, and the identification registers
 * say a PL011, so a driver that checks finds one. Before this the first
 * store was an MMIO exit and the ring stayed empty.
 */
bool selftest_el2_guest_uart(const char **reason)
{
    if (skip_without_backend(reason))
        return true;
    struct vm *vm;
    struct vcpu *v;
    CHECK(make_guest("tests/hv/guest_uart.bin", &vm, &v) == 0);
    CHECK(vm_console_pending(vm) == 0);
    struct cosmo_vm_exit x;
    memset(&x, 0, sizeof(x));
    CHECK(vcpu_run(v, &x) == 0);
    CHECK(x.kind == COSMO_VM_EXIT_HYPERCALL && x.hypercall.nr == 1);   /* six stores, no exit */
    CHECK(x.hypercall.a0 == 0x90u);                                     /* FR: TXFE and RXFE, throughout */
    CHECK(x.hypercall.a1 == 0x11u && x.hypercall.a2 == 0x10u);         /* PeriphID0, PeriphID1: a PL011 */
    CHECK(x.hypercall.a3 == 0xB1u);                                    /* PCellID3 */
    CHECK(vm_console_pending(vm) == 6);
    char buf[16];
    memset(buf, 0, sizeof(buf));
    CHECK(vm_console_read(vm, buf, sizeof(buf) - 1) == 6);
    CHECK(strcmp(buf, "hello\n") == 0);
    CHECK(vm_console_pending(vm) == 0);
    drop_guest(vm, v);
    kinfo("selftest: el2-guest-uart: the guest printed \"hello\" through a PL011 and its owner read it back");
    return true;
}

/*
 * The owner types, and the guest is interrupted: the first device
 * interrupt the distributor routes that the guest did not fake through
 * ISPENDR. The guest has SPI 33 routed to itself and RXIM unmasked; the
 * test writes 'x' to the VM as its owner would through the descriptor;
 * the handler runs with INTID 33, reads RXMIS set, reads 'x' from DR,
 * and the flag register then says the FIFO is empty. Then it heartbeats
 * on with no further interrupt: one byte, one interrupt.
 */
bool selftest_el2_guest_uart_rx(const char **reason)
{
    if (skip_without_vdist("el2-guest-uart-rx", reason))
        return true;
    struct vm *vm;
    struct vcpu *v;
    CHECK(make_guest("tests/hv/guest_uart_rx.bin", &vm, &v) == 0);
    struct cosmo_vm_exit x;
    memset(&x, 0, sizeof(x));
    CHECK(vcpu_run(v, &x) == 0);
    CHECK(x.kind == COSMO_VM_EXIT_HYPERCALL && x.hypercall.nr == 1);
    for (unsigned i = 0; i < 5; i++) {                                 /* nothing typed: nothing arrives */
        CHECK(vcpu_run(v, &x) == 0);
        CHECK(x.kind == COSMO_VM_EXIT_HYPERCALL && x.hypercall.nr == 2 && x.hypercall.a0 == 0);
    }
    CHECK(vm_console_write(vm, "x", 1) == 1);
    unsigned beats = 0;
    CHECK(run_until(v, &x, 33, (1ull << 2), 100, &beats));
    CHECK((x.hypercall.a0 & 0x10u) != 0);                              /* MIS: RXMIS */
    CHECK(x.hypercall.a1 == 'x');
    CHECK((x.hypercall.a2 & 0x10u) != 0);                              /* FR: RXFE after the read */
    for (unsigned i = 0; i < 10; i++) {
        CHECK(vcpu_run(v, &x) == 0);
        CHECK(x.kind == COSMO_VM_EXIT_HYPERCALL && x.hypercall.nr == 2 && x.hypercall.a0 == 1);
    }
    drop_guest(vm, v);
    kinfo("selftest: el2-guest-uart-rx: 'x' typed at the guest arrived as SPI 33 after %u beat(s), and only once", beats);
    return true;
}

static struct vm *g_wake_vm;
static uint64_t g_wake_at;

static void wake_by_typing(struct timer *t, void *arg)
{
    (void)t;
    (void)arg;
    g_wake_at = clock_now_ns();
    vm_console_write(g_wake_vm, "w", 1);
}

/*
 * Level, and the wake-up. Two bytes written before the guest runs: the
 * handler drains exactly one and returns, the line is still up, and the
 * handler runs again for the second -- an edge-triggered model would
 * deliver one interrupt for two bytes. After the second the FIFO is empty
 * and no third interrupt comes. Then a guest whose WFI has a two-second
 * deadline is woken by a byte typed 20 ms in: the WFI run returns long
 * before the deadline, with an interrupt pending, and the byte is taken.
 */
bool selftest_el2_guest_uart_level(const char **reason)
{
    if (skip_without_vdist("el2-guest-uart-level", reason))
        return true;
    struct vm *vm;
    struct vcpu *v;
    CHECK(make_guest("tests/hv/guest_uart_rx.bin", &vm, &v) == 0);
    struct cosmo_vm_exit x;
    memset(&x, 0, sizeof(x));
    CHECK(vcpu_run(v, &x) == 0);
    CHECK(x.kind == COSMO_VM_EXIT_HYPERCALL && x.hypercall.nr == 1);
    CHECK(vm_console_write(vm, "ab", 2) == 2);
    CHECK(run_until(v, &x, 33, (1ull << 2), 100, NULL));
    CHECK(x.hypercall.a1 == 'a');
    CHECK((x.hypercall.a2 & 0x10u) == 0);                              /* FR: a byte still waits */
    unsigned between = 0;
    CHECK(run_until(v, &x, 33, (1ull << 2), 100, &between));           /* the line stayed up: again */
    CHECK(x.hypercall.a1 == 'b');
    CHECK((x.hypercall.a2 & 0x10u) != 0);                              /* now empty */
    for (unsigned i = 0; i < 10; i++) {
        CHECK(vcpu_run(v, &x) == 0);
        CHECK(x.kind == COSMO_VM_EXIT_HYPERCALL && x.hypercall.nr == 2 && x.hypercall.a0 == 2);
    }
    drop_guest(vm, v);

    /* The wake-up. */
    CHECK(make_guest("tests/hv/guest_uart_wfi.bin", &vm, &v) == 0);
    CHECK(vcpu_run(v, &x) == 0);
    CHECK(x.kind == COSMO_VM_EXIT_HYPERCALL && x.hypercall.nr == 1);
    struct timer t;
    g_wake_vm = vm;
    g_wake_at = 0;
    timer_setup(&t, wake_by_typing, NULL);
    uint64_t t0 = clock_now_ns();
    timer_start(&t, 20000000ull);                                      /* 20 ms in, a keystroke */
    CHECK(vcpu_run(v, &x) == 0);                                       /* the WFI, with a 2 s deadline */
    uint64_t waited = clock_now_ns() - t0;
    timer_cancel_sync(&t);
    CHECK(x.kind == COSMO_VM_EXIT_WFI);
    CHECK((x.flags & COSMO_VM_EXIT_F_IRQ_PENDING) != 0);               /* woken for a reason */
    CHECK(g_wake_at != 0 && waited < 1000000000ull);                    /* long before two seconds */
    CHECK(run_until(v, &x, 33, (1ull << 2), 100, NULL));
    CHECK(x.hypercall.a1 == 'w');
    drop_guest(vm, v);
    kinfo("selftest: el2-guest-uart-level: two bytes, two interrupts (%u beat(s) between); a WFI with a 2 s deadline woken by a keystroke after %llu ms",
          between, (unsigned long long)(waited / 1000000ull));
    return true;
}

struct typist {
    struct vm *vm;
    unsigned bytes;
    volatile bool done;
};

static void typist_main(void *arg)
{
    struct typist *t = arg;
    for (unsigned i = 0; i < t->bytes; i++) {
        char c = (char)('a' + (i % 26));
        while (vm_console_write(t->vm, &c, 1) != 1)   /* the FIFO is full: the guest has not caught up */
            thread_sleep_ns(10000);
        /* In bursts, with a pause every eighth byte. A stale raise is only
         * visible as a spurious interrupt if no fresh byte arrives before
         * the handler reads MIS; a steady cadence would hide exactly the
         * race this test exists to catch. No sleep between the bytes of a
         * burst: every sleep here is at least one scheduler tick (4 ms),
         * and a sleep per byte made the test 6 s on four host CPUs and
         * 9.7 s on one, past the harness's 8 s budget. */
        if (i % 8 == 7)
            thread_sleep_ns(1000000);
    }
    t->done = true;
    thread_exit(0);
}

/* The second vCPU's owner: runs it until told to stop, counting what it
 * consumed and any interrupt it took with MIS zero. */
struct sibling {
    struct vcpu *v;
    volatile bool stop;
    unsigned consumed, irqs, spurious;
    bool failed;
};

static void sibling_main(void *arg)
{
    struct sibling *s = arg;
    struct cosmo_vm_exit x;
    memset(&x, 0, sizeof(x));
    while (!s->stop) {
        if (vcpu_run(s->v, &x) != 0 || x.kind != COSMO_VM_EXIT_HYPERCALL) {
            s->failed = true;
            break;
        }
        if (x.hypercall.nr == 33) {
            if ((x.hypercall.a0 & 0x10u) == 0)
                s->spurious++;
        } else if (x.hypercall.nr == 2) {
            s->irqs = (unsigned)x.hypercall.a0;
            s->consumed = (unsigned)x.hypercall.a1;
        }
    }
    thread_exit(0);
}

/*
 * A property about two threads needs two threads, and this test has two
 * phases because two different things are being claimed.
 *
 * Phase one, one vCPU: a kernel thread types at a guest that both polls
 * DR (with interrupts masked around the poll) and takes SPI 33 for it, so
 * the owner's raise and the guest's lower race in the UART for every
 * byte. No interrupt may arrive with MIS zero: with one vCPU there is no
 * one else to drain the byte, so an interrupt whose cause is gone can
 * only come from the hypervisor's own ordering -- a raise decided from a
 * state the guest has since changed, which two versions of this UART
 * could produce (a transition applied after dropping the lock; "the line
 * is up" returned for the run loop to act on). Both windows are too
 * narrow to hit on purpose, so their bug-proofs force them; this phase is
 * the regression test that the device decides and raises under one lock.
 *
 * Phase two, two vCPUs each on its own thread, both polling and both able
 * to take the interrupt: every byte typed is consumed by one path on one
 * vCPU and none is lost. MIS-zero interrupts are counted and reported but
 * NOT asserted absent here, because with a sibling they are not the
 * hypervisor's to prevent: a sibling that drains the byte between the
 * GIC forwarding the interrupt and the handler reading MIS makes MIS zero
 * on real hardware too, and a guest driver treats it as no work (Linux's
 * PL011 driver returns). One list register adds a window hardware does
 * not have -- a line that drops after the interrupt was placed is still
 * delivered, where a GIC would return 1023 at IAR -- and that is recorded
 * in the design doc as a deviation a guest handler must tolerate. The
 * first version of this test asserted zero in phase two as well and failed
 * once in ten runs, on exactly that interleaving.
 */
static bool uart_race_phase(const char **reason, struct vm *vm, struct vcpu *v0, struct vcpu *v1, unsigned bytes,
                            unsigned *consumed_out, unsigned *irqs_out, unsigned *spurious_out, struct sibling *sib)
{
    struct cosmo_vm_exit x;
    memset(&x, 0, sizeof(x));
    struct thread *ts = NULL;
    if (v1) {
        memset(sib, 0, sizeof(*sib));
        sib->v = v1;
        ts = thread_create(sibling_main, sib, "uart-sibling", SCHED_PRIO_DEFAULT);
        CHECK(ts != NULL);
    }
    /* The guest's byte counter (x23) carries over from an earlier phase:
     * take its value now, from the registers, before a single byte is
     * typed. Taking it from the first heartbeat instead -- as the first
     * version did -- misses any byte the guest consumed before that
     * heartbeat, and a count that is one short never reaches the total:
     * the loop then spins to its step bound, which is minutes, and two
     * chain steps timed out inside this test. */
    struct cosmo_vcpu_regs regs;
    CHECK(vcpu_get_regs(v0, &regs) == 0);
    unsigned base = (unsigned)regs.x[23];
    struct typist t;
    memset(&t, 0, sizeof(t));
    t.vm = vm;
    t.bytes = bytes;
    struct thread *th = thread_create(typist_main, &t, "uart-typist", SCHED_PRIO_DEFAULT);
    CHECK(th != NULL);
    unsigned irqs = 0, spurious = 0, consumed = 0, steps = 0;
    for (;;) {
        CHECK(vcpu_run(v0, &x) == 0);
        CHECK(x.kind == COSMO_VM_EXIT_HYPERCALL);
        if (x.hypercall.nr == 33) {
            if ((x.hypercall.a0 & 0x10u) == 0)
                spurious++;
        } else {
            CHECK(x.hypercall.nr == 2);
            irqs = (unsigned)x.hypercall.a0;
            consumed = (unsigned)x.hypercall.a1 - base;
            if (t.done && consumed + (v1 ? sib->consumed : 0) >= bytes)
                break;
        }
        CHECK(++steps < 200000);   /* a phase is seconds; this is a failure, not a wait */
    }
    thread_join(th);
    if (v1) {
        sib->stop = true;
        thread_join(ts);
        CHECK(!sib->failed);
    }
    *consumed_out = consumed;
    *irqs_out = irqs;
    *spurious_out = spurious;
    return true;
}

bool selftest_el2_guest_uart_race(const char **reason)
{
    if (skip_without_vdist("el2-guest-uart-race", reason))
        return true;
    struct vm *vm;
    struct vcpu *v0, *v1;
    CHECK(make_guest("tests/hv/guest_uart_poll.bin", &vm, &v0) == 0);
    CHECK(vcpu_create(vm, 1, &v1) == 0);
    struct cosmo_vcpu_regs regs;
    CHECK(vcpu_get_regs(v1, &regs) == 0);
    regs.pc = LOAD_GPA;
    CHECK(vcpu_set_regs(v1, &regs) == 0);
    struct cosmo_vm_exit x;
    memset(&x, 0, sizeof(x));
    CHECK(vcpu_run(v0, &x) == 0);
    CHECK(x.kind == COSMO_VM_EXIT_HYPERCALL && x.hypercall.nr == 1);   /* vCPU 0 routes SPI 33 to itself */

    /* Phase one: one vCPU, the hypervisor's own ordering under test. */
    unsigned c1 = 0, i1 = 0, s1 = 0;
    struct sibling sib;
    CHECK(uart_race_phase(reason, vm, v0, NULL, 200, &c1, &i1, &s1, &sib));
    CHECK(s1 == 0);
    CHECK(c1 == 200);

    /* Phase two: the sibling joins (and routes the SPI to itself, last). */
    CHECK(vcpu_run(v1, &x) == 0);
    CHECK(x.kind == COSMO_VM_EXIT_HYPERCALL && x.hypercall.nr == 1);
    unsigned c2 = 0, i2 = 0, s2 = 0;
    CHECK(uart_race_phase(reason, vm, v0, v1, 300, &c2, &i2, &s2, &sib));
    CHECK(c2 + sib.consumed == 300);
    kobject_put(&v1->obj);
    drop_guest(vm, v0);
    kinfo("selftest: el2-guest-uart-race: one vCPU: %u bytes, %u by interrupt, 0 spurious; two vCPUs on two threads: 300 bytes, vCPU 0 took %u (%u by interrupt), vCPU 1 %u (%u), none lost, %u MIS-zero (a sibling drained first: tolerated)",
          c1, i1, c2, i2, sib.consumed, sib.irqs, s2 + sib.spurious);
    return true;
}

/* --- the machine a guest is handed -------------------------------------- */

#define MACHINE_RAM_BYTES (8ull << 20)   /* what the archive's virt.dtb was built for */

/*
 * A VM laid out as machine mode lays one out: RAM at COSMO_HVM_RAM_BASE,
 * the image where its arm64 Image header's text_offset says, the device
 * tree at the first 2 MiB boundary past the image, vCPU 0 entering at the
 * image with x0 = the tree -- the boot protocol, as an owner would follow
 * it. The header is read, not assumed, so a fixture with the wrong magic
 * or offset fails here and not in a guest that never prints.
 */
static __maybe_unused int make_machine_guest(const char *image, const char *dtb, struct vm **vm_out,
                                             struct vcpu **vcpu_out, uint64_t *dtb_gpa_out)
{
    const void *img, *blob;
    size_t img_len, blob_len;
    if (!bootarchive_find(image, &img, &img_len) || !bootarchive_find(dtb, &blob, &blob_len))
        return -ENOENT;
    const uint8_t *h = img;
    if (img_len < 64 || *(const uint32_t *)(h + COSMO_HVM_IMAGE_MAGIC_OFF) != COSMO_HVM_IMAGE_MAGIC)
        return -EINVAL;
    uint64_t text_off = *(const uint64_t *)(h + COSMO_HVM_IMAGE_TEXT_OFF);
    uint64_t image_size = *(const uint64_t *)(h + COSMO_HVM_IMAGE_SIZE_OFF);
    uint64_t load = COSMO_HVM_RAM_BASE + text_off;
    uint64_t dtb_gpa = (load + image_size + (2ull << 20) - 1) & ~((2ull << 20) - 1);
    if (dtb_gpa + blob_len > COSMO_HVM_RAM_BASE + MACHINE_RAM_BYTES)
        return -ENOSPC;
    struct vm *vm;
    int rc = vm_create(0, HV_VM_MEM_MAX, &vm);
    if (rc)
        return rc;
    rc = vm_mem_add(vm, COSMO_HVM_RAM_BASE, MACHINE_RAM_BYTES);
    if (rc == 0)
        rc = vm_mem_write(vm, load, img, img_len);
    if (rc == 0)
        rc = vm_mem_write(vm, dtb_gpa, blob, blob_len);
    struct vcpu *v = NULL;
    if (rc == 0)
        rc = vcpu_create(vm, 0, &v);
    if (rc == 0) {
        struct cosmo_vcpu_regs regs;
        vcpu_get_regs(v, &regs);
        regs.pc = load;
        regs.x[0] = dtb_gpa;
        rc = vcpu_set_regs(v, &regs);
    }
    if (rc) {
        if (v)
            kobject_put(&v->obj);
        kobject_put(&vm->obj);
        return rc;
    }
    *vm_out = vm;
    *vcpu_out = v;
    *dtb_gpa_out = dtb_gpa;
    return 0;
}

/* Everything the console holds, NUL-terminated, into `buf`. */
static __maybe_unused size_t console_drain(struct vm *vm, char *buf, size_t cap)
{
    size_t n = vm_console_read(vm, buf, cap - 1);
    buf[n] = '\0';
    return n;
}

/*
 * The whole unit in one line: a guest that knows nothing of this
 * hypervisor reads the device tree in x0, finds its UART through
 * stdout-path, and prints what it found through that UART -- so the line
 * arrives only if the tree named the device the kernel implements, at the
 * address it implements it. Every field is compared to the uapi constant
 * it came from, not to a literal: the test is the header's, the blob's
 * and the kernel's agreement.
 */
bool selftest_el2_guest_dtb(const char **reason)
{
    if (skip_without_backend(reason))
        return true;
    struct vm *vm;
    struct vcpu *v;
    uint64_t dtb_gpa = 0;
    int rc = make_machine_guest("tests/hv/guest_dtb.bin", "tests/hv/virt.dtb", &vm, &v, &dtb_gpa);
    if (rc == -ENOENT) {
        kinfo("selftest: el2-guest-dtb: no C guest or device tree in the archive; skipping");
        return true;
    }
    CHECK(rc == 0);
    CHECK(dtb_gpa == COSMO_HVM_RAM_BASE + (2ull << 20));   /* past a 256 KiB image at +0x80000 */
    struct cosmo_vm_exit x;
    memset(&x, 0, sizeof(x));
    CHECK(vcpu_run(v, &x) == 0);
    CHECK(x.kind == COSMO_VM_EXIT_HYPERCALL);
    CHECK(x.hypercall.nr == 0x84000000ull);                   /* PSCI_VERSION: the tree was read and the line printed */
    char line[160], want[160];
    console_drain(vm, line, sizeof(line));
    ksnprintf(want, sizeof(want), "dtb: uart@%llx irq %u cpus %u mem %llx+%llx psci hvc\n",
              (unsigned long long)COSMO_HVM_UART_BASE, COSMO_HVM_UART_INTID, 2u,
              (unsigned long long)COSMO_HVM_RAM_BASE, (unsigned long long)MACHINE_RAM_BYTES);
    if (strcmp(line, want) != 0)
        kwarn("selftest: el2-guest-dtb: guest said \"%s\", wanted \"%s\"", line, want);
    CHECK(strcmp(line, want) == 0);
    drop_guest(vm, v);
    kinfo("selftest: el2-guest-dtb: %s", line);
    return true;
}

/*
 * The owner is the machine's firmware. The kernel gives it the exit,
 * set_regs, and vcpu_create after the VM has started; this test answers
 * PSCI as vmctl will and requires the guest to see the answers: a
 * version, a second CPU that starts at the entry it named with the
 * context it gave -- checked through that vCPU's own first line, not by
 * its existence -- and a power-off.
 */
bool selftest_el2_guest_psci(const char **reason)
{
    if (skip_without_backend(reason))
        return true;
    struct vm *vm;
    struct vcpu *v0, *v1 = NULL;
    uint64_t dtb_gpa = 0;
    int rc = make_machine_guest("tests/hv/guest_dtb.bin", "tests/hv/virt.dtb", &vm, &v0, &dtb_gpa);
    if (rc == -ENOENT) {
        kinfo("selftest: el2-guest-psci: no C guest or device tree in the archive; skipping");
        return true;
    }
    CHECK(rc == 0);
    struct cosmo_vm_exit x;
    memset(&x, 0, sizeof(x));
    struct cosmo_vcpu_regs regs;
    char line[160];

    CHECK(vcpu_run(v0, &x) == 0);
    CHECK(x.kind == COSMO_VM_EXIT_HYPERCALL && x.hypercall.nr == 0x84000000ull);   /* PSCI_VERSION */
    console_drain(vm, line, sizeof(line));
    CHECK(vcpu_get_regs(v0, &regs) == 0);
    regs.x[0] = 0x10000;                                                          /* PSCI 1.0 */
    CHECK(vcpu_set_regs(v0, &regs) == 0);

    CHECK(vcpu_run(v0, &x) == 0);
    CHECK(x.kind == COSMO_VM_EXIT_HYPERCALL && x.hypercall.nr == 0xC4000003ull);   /* CPU_ON */
    console_drain(vm, line, sizeof(line));
    CHECK(strcmp(line, "psci version 0x10000\n") == 0);
    uint64_t target = x.hypercall.a0, entry = x.hypercall.a1, ctx = x.hypercall.a2;
    CHECK(target == 1 && ctx == 0x1234cafeull);
    CHECK(entry > COSMO_HVM_RAM_BASE && entry < COSMO_HVM_RAM_BASE + MACHINE_RAM_BYTES);
    CHECK(vcpu_create(vm, (unsigned)target, &v1) == 0);                            /* after the VM started */
    CHECK(vcpu_get_regs(v1, &regs) == 0);
    regs.pc = entry;
    regs.x[0] = ctx;
    CHECK(vcpu_set_regs(v1, &regs) == 0);
    CHECK(vcpu_get_regs(v0, &regs) == 0);
    regs.x[0] = 0;                                                                /* SUCCESS */
    CHECK(vcpu_set_regs(v0, &regs) == 0);

    /* The second CPU runs from the entry with the context, prints, and
     * powers itself off. */
    CHECK(vcpu_run(v1, &x) == 0);
    CHECK(x.kind == COSMO_VM_EXIT_HYPERCALL && x.hypercall.nr == 0x84000002ull);   /* CPU_OFF */
    console_drain(vm, line, sizeof(line));
    CHECK(strcmp(line, "cpu1: up ctx=1234cafe\n") == 0);

    /* The first sees SUCCESS and powers the machine off. */
    CHECK(vcpu_run(v0, &x) == 0);
    CHECK(x.kind == COSMO_VM_EXIT_HYPERCALL && x.hypercall.nr == 0x84000008ull);   /* SYSTEM_OFF */
    console_drain(vm, line, sizeof(line));
    CHECK(strcmp(line, "cpu_on 1 -> 0\n") == 0);

    kobject_put(&v1->obj);
    drop_guest(vm, v0);
    kinfo("selftest: el2-guest-psci: version answered, vCPU 1 brought up at the guest's entry with its context, power-off requested");
    return true;
}

/*
 * The bounded run an owner with one thread needs: a guest that never
 * exits, run with ONE_TICK, comes back at the first host interrupt as
 * PREEMPTED rather than never; run again it comes back again, having run
 * in between; and the flag is per call -- without it the same guest still
 * needs the tests' own bound to be stopped at all.
 */
bool selftest_el2_vcpu_run_tick(const char **reason)
{
    if (skip_without_backend(reason))
        return true;
    struct vm *vm;
    struct vcpu *v;
    CHECK(make_guest("tests/hv/guest_spin.bin", &vm, &v) == 0);
    struct cosmo_vm_exit x;
    memset(&x, 0, sizeof(x));
    uint64_t t0 = clock_now_ns();
    CHECK(vcpu_run_flags(v, &x, COSMO_VCPU_RUN_ONE_TICK) == 0);
    uint64_t first = clock_now_ns() - t0;
    CHECK(x.kind == COSMO_VM_EXIT_PREEMPTED);
    uint64_t entries = v->entries;
    CHECK(entries >= 1);
    CHECK(first < 100000000ull);                                   /* a tick, not forever: under 100 ms */
    CHECK(vcpu_run_flags(v, &x, COSMO_VCPU_RUN_ONE_TICK) == 0);
    CHECK(x.kind == COSMO_VM_EXIT_PREEMPTED);
    CHECK(v->entries > entries);                                   /* it ran again in between */
    struct cosmo_vcpu_regs regs;
    CHECK(vcpu_get_regs(v, &regs) == 0 && regs.pc == LOAD_GPA);    /* and is still in its loop */
    /* The flag is per call: without it, only the tests' bound stops this guest. */
    CHECK(vcpu_run_limited(v, &x, 3) == -ETIMEDOUT);
    CHECK(vcpu_run_flags(v, &x, 0x80000000u | COSMO_VCPU_RUN_ONE_TICK) == 0);   /* unknown bits are ignored */
    CHECK(x.kind == COSMO_VM_EXIT_PREEMPTED);
    drop_guest(vm, v);
    kinfo("selftest: el2-vcpu-run-tick: a spinning guest gave its turn back after %llu us, twice",
          (unsigned long long)(first / 1000));
    return true;
}

bool selftest_el2_guest_idreg(const char **reason)
{
    if (skip_without_backend(reason))
        return true;
    struct vm *vm;
    struct vcpu *v;
    CHECK(make_guest("tests/hv/guest_idreg.bin", &vm, &v) == 0);
    struct cosmo_vm_exit x;
    memset(&x, 0, sizeof(x));
    uint64_t got[0x80];
    memset(got, 0xFF, sizeof(got));
    for (;;) {
        CHECK(vcpu_run(v, &x) == 0);
        CHECK(x.kind == COSMO_VM_EXIT_HYPERCALL);   /* every read answered in-kernel: no SYSREG exit */
        if (x.hypercall.nr == 0)
            break;
        CHECK(x.hypercall.nr < 0x80);
        got[x.hypercall.nr] = x.hypercall.a0;
    }
    uint64_t host_pfr0 = HOST_IDREG("S3_0_c0_c4_0");
    uint64_t host_isar1 = HOST_IDREG("S3_0_c0_c6_1");
    uint64_t host_mmfr0 = HOST_IDREG("S3_0_c0_c7_0");
    uint64_t host_isar2 = HOST_IDREG("S3_0_c0_c6_2");

    /* ID_AA64PFR0: EL2/EL3 hidden, FP and GIC kept from the host. */
    CHECK(((got[0x40] >> 8) & 0xF) == 0);                       /* EL2 */
    CHECK(((got[0x40] >> 12) & 0xF) == 0);                      /* EL3 */
    CHECK(((got[0x40] >> 16) & 0xF) == ((host_pfr0 >> 16) & 0xF));   /* FP: as the host has it */
    CHECK(((got[0x40] >> 24) & 0xF) == ((host_pfr0 >> 24) & 0xF));   /* GIC */
    CHECK(got[0x40] != host_pfr0);                              /* something was masked (EL2 present on the host) */

    CHECK(got[0x41] == 0);                                     /* ID_AA64PFR1 hidden */
    CHECK((got[0x50] & 0xF) >= 6);                              /* ID_AA64DFR0 DebugVer: the minimum */
    CHECK(((got[0x50] >> 8) & 0xF) == 0);                       /* PMUVer: none */

    /* ID_AA64ISAR1: the host's, minus the pointer-authentication fields
     * (APA, API, GPA, GPI: IDREG_ISAR1_DROP in the model's header). The
     * exact-mask form catches a wrong ISAR1 value; the specific auth bits
     * are only *observably* removed on a host that has them, and QEMU's
     * TCG reports none, so on this host the mask is a no-op there and the
     * check reduces to "the model returned the host's ISAR1". */
    CHECK(got[0x61] == (host_isar1 & ~0xFF000FF0ull));
    CHECK(((got[0x61] >> 24) & 0xF) == 0 && ((got[0x61] >> 28) & 0xF) == 0);   /* GPA, GPI: absent regardless */
    /* ID_AA64ISAR2: the host's, minus its pointer-auth fields APA3[15:12]
     * and GPA3[11:8] (IDREG_ISAR2_DROP). Same host caveat as ISAR1. */
    CHECK(got[0x62] == (host_isar2 & ~0x0000FF00ull));
    CHECK(((got[0x62] >> 8) & 0xF) == 0 && ((got[0x62] >> 12) & 0xF) == 0);   /* GPA3, APA3: absent */

    CHECK((got[0x70] & 0xF) == (host_mmfr0 & 0xF));            /* ID_AA64MMFR0 PARange: the truth */
    CHECK(((got[0x71] >> 8) & 0xF) == 0);                       /* ID_AA64MMFR1 VH: hidden */

    CHECK(got[0x44] == 0);                                     /* ID_AA64ZFR0 (SVE), in the space, unnamed: zero */
    drop_guest(vm, v);
    kinfo("selftest: el2-guest-idreg: feature registers answered in-kernel -- EL2/EL3/SVE/PMU/pointer-auth hidden, FP/GIC/PARange kept");
    return true;
}

/*
 * The interrupt an owner-side device raises goes through the guest's
 * distributor -- routed and gated by the guest's own GIC configuration --
 * not injected past it. The guest enables SPI 50 and routes it to itself;
 * vm_raise_spi (what cosmo_vm_raise_spi reaches from userland, and what a
 * virtio device will call) delivers it, and the handler runs. Lowered
 * before the guest takes it, it is withdrawn. And the distinction from
 * vcpu_inject: an SPI the guest has NOT enabled is not delivered by
 * vm_raise_spi -- the distributor's enable gates it -- where a direct
 * injection would reach the guest regardless.
 */
bool selftest_el2_vm_raise_spi(const char **reason)
{
    if (skip_without_vdist("el2-vm-raise-spi", reason))
        return true;
    struct vm *vm;
    struct vcpu *v;
    CHECK(make_guest("tests/hv/guest_spi.bin", &vm, &v) == 0);
    struct cosmo_vm_exit x;
    memset(&x, 0, sizeof(x));
    CHECK(vcpu_run(v, &x) == 0);
    CHECK(x.kind == COSMO_VM_EXIT_HYPERCALL && x.hypercall.nr == 1);   /* GIC up, SPI 50 enabled */

    /* Raised through the distributor, the guest takes it. */
    CHECK(vm_raise_spi(vm, 50) == 0);
    unsigned beats = 0;
    CHECK(run_until(v, &x, 50, (1ull << 2), 100, &beats));
    /* The handler ran once; the line was level and the guest acknowledged,
     * so lowering it now leaves nothing pending. */
    CHECK(vm_lower_spi(vm, 50) == 0);
    for (unsigned i = 0; i < 10; i++) {
        CHECK(vcpu_run(v, &x) == 0);
        CHECK(x.kind == COSMO_VM_EXIT_HYPERCALL && x.hypercall.nr == 2 && x.hypercall.a0 == 1);
    }

    /* An SPI the guest never enabled: raised through the distributor it is
     * gated (no delivery), so the guest heartbeats on with its handler
     * count unchanged. A direct vcpu_inject would have delivered it. */
    CHECK(vm_raise_spi(vm, 51) == 0);
    for (unsigned i = 0; i < 20; i++) {
        CHECK(vcpu_run(v, &x) == 0);
        CHECK(x.kind == COSMO_VM_EXIT_HYPERCALL && x.hypercall.nr == 2);
        CHECK(x.hypercall.a0 == 1);                                   /* still 1: SPI 51 was gated */
    }
    vm_lower_spi(vm, 51);
    drop_guest(vm, v);
    kinfo("selftest: el2-vm-raise-spi: SPI 50 raised through the distributor was taken after %u beat(s); SPI 51, not enabled, was gated", beats);
    return true;
}

/*
 * A guest drives a virtio-mmio block device the test models. This is the
 * transport handshake and the queue, end to end in the kernel harness: the
 * correctness and hostile-input handling of the device-side ring walk are
 * proved exhaustively on the host (test_vblk_dev); here a real guest driver
 * negotiates features, sets up its queue, reads a sector, and must get the
 * bytes the test's disk holds. The transport window is unclaimed in the
 * kernel, so the guest's register accesses arrive as MMIO exits the test
 * answers -- the test is the owner, as vmctl will be.
 */
#define VIO_BASE 0x0A000000ull
#define VIO_SIZE 0x200ull

struct vio_model {
    uint32_t feat_sel, drv_feat_sel;
    uint64_t desc, avail, used;   /* assembled from the guest's lo/hi writes */
    uint32_t status, q_num;
    int ready;
    uint16_t used_idx;
    unsigned flushes;             /* T_FLUSH requests served */
    uint8_t disk[8 * 512];        /* the backing "disk": known bytes */
};

/* Serve whatever the guest has made available: a read fills its data buffer
 * from the disk, a write drains its data buffer to the disk, a flush is
 * counted. A compact, direction-aware, happy-path device; the robust
 * hostile-input walk is test_vblk_dev's. */
static void vio_notify(struct vm *vm, struct vio_model *m)
{
    uint16_t avail_idx = 0;
    if (vm_mem_read(vm, m->avail + 2, &avail_idx, 2) != 0)
        return;
    while (m->used_idx != avail_idx) {
        uint16_t head = 0;
        vm_mem_read(vm, m->avail + 4 + (m->used_idx % m->q_num) * 2u, &head, 2);
        struct { uint64_t addr; uint32_t len; uint16_t flags; uint16_t next; } d;
        vm_mem_read(vm, m->desc + (uint64_t)head * 16u, &d, sizeof(d));
        struct { uint32_t type, reserved; uint64_t sector; } hdr;
        vm_mem_read(vm, d.addr, &hdr, sizeof(hdr));          /* the request header */
        uint64_t off = hdr.sector * 512u;
        uint8_t status = 0;                                  /* OK */
        uint32_t len = 0;
        /* walk data descriptors to the status byte, moving each per direction */
        while (d.flags & 1u) {                               /* NEXT */
            vm_mem_read(vm, m->desc + (uint64_t)d.next * 16u, &d, sizeof(d));
            if (!(d.flags & 1u))                             /* the last: status */
                break;
            uint32_t sz = d.len < 512u ? d.len : 512u;
            if (off + sz > sizeof(m->disk))
                status = 1;                                  /* IOERR: past the disk */
            else if (hdr.type == 0) {                        /* IN: disk -> guest */
                vm_mem_write(vm, d.addr, m->disk + off, sz);
                len += sz;
            } else if (hdr.type == 1)                        /* OUT: guest -> disk */
                vm_mem_read(vm, d.addr, m->disk + off, sz);
            off += sz;
        }
        if (hdr.type == 4)                                   /* FLUSH */
            m->flushes++;
        else if (hdr.type != 0 && hdr.type != 1)
            status = 2;                                       /* UNSUPP */
        vm_mem_write(vm, d.addr, &status, 1);                /* d is the status descriptor */
        uint32_t elem[2] = { head, len + 1u };
        vm_mem_write(vm, m->used + 4 + (m->used_idx % m->q_num) * 8u, elem, 8);
        m->used_idx++;
        vm_mem_write(vm, m->used + 2, &m->used_idx, 2);
    }
}

/* Answer one transport register access; `*val` is the result on a read. */
static void vio_reg(struct vm *vm, struct vio_model *m, unsigned off, bool write, uint64_t *val)
{
    if (!write) {
        switch (off) {
        case 0x000: *val = 0x74726976u; return;   /* MagicValue "virt" */
        case 0x004: *val = 2; return;              /* Version 2 */
        case 0x008: *val = 2; return;              /* DeviceID: block */
        case 0x00c: *val = 0x554d4551u; return;    /* VendorID */
        case 0x010: *val = m->feat_sel == 1 ? 1u : (1u << 9); return;   /* VERSION_1 (bit 32) ; BLK_F_FLUSH (bit 9): writable */
        case 0x034: *val = 8; return;              /* QueueNumMax */
        case 0x044: *val = (uint32_t)m->ready; return;
        case 0x070: *val = m->status; return;
        case 0x100: *val = 8; return;              /* capacity low: 8 sectors */
        case 0x104: *val = 0; return;              /* capacity high */
        default: *val = 0; return;
        }
    }
    uint32_t v = (uint32_t)*val;
    switch (off) {
    case 0x014: m->feat_sel = v; break;
    case 0x024: m->drv_feat_sel = v; break;
    case 0x038: m->q_num = v; break;
    case 0x044: m->ready = (int)v; break;
    case 0x050: vio_notify(vm, m); break;          /* QueueNotify */
    case 0x070: m->status = v; break;
    case 0x080: m->desc = (m->desc & ~0xFFFFFFFFull) | v; break;
    case 0x084: m->desc = (m->desc & 0xFFFFFFFFull) | ((uint64_t)v << 32); break;
    case 0x090: m->avail = (m->avail & ~0xFFFFFFFFull) | v; break;
    case 0x094: m->avail = (m->avail & 0xFFFFFFFFull) | ((uint64_t)v << 32); break;
    case 0x0a0: m->used = (m->used & ~0xFFFFFFFFull) | v; break;
    case 0x0a4: m->used = (m->used & 0xFFFFFFFFull) | ((uint64_t)v << 32); break;
    default: break;
    }
}

bool selftest_el2_virtq_device(const char **reason)
{
    if (skip_without_backend(reason))
        return true;
    struct vm *vm;
    struct vcpu *v;
    CHECK(make_guest("tests/hv/guest_vblk.bin", &vm, &v) == 0);
    struct vio_model m;
    memset(&m, 0, sizeof(m));
    for (unsigned i = 0; i < sizeof(m.disk); i++)
        m.disk[i] = (uint8_t)(i * 5 + 3);          /* the known pattern the guest must read back */
    struct cosmo_vm_exit x;
    memset(&x, 0, sizeof(x));
    unsigned steps = 0;
    for (;;) {
        CHECK(vcpu_run(v, &x) == 0);
        if (x.kind == COSMO_VM_EXIT_MMIO && x.mmio.gpa >= VIO_BASE && x.mmio.gpa < VIO_BASE + VIO_SIZE) {
            uint64_t val = x.mmio.value;
            vio_reg(vm, &m, (unsigned)(x.mmio.gpa - VIO_BASE), x.mmio.write, &val);
            if (!x.mmio.write) {
                struct cosmo_vcpu_regs r;
                CHECK(vcpu_get_regs(v, &r) == 0);
                /* the read's value goes to the guest register; the kernel's
                 * completion path would also serve it, but answering in the
                 * exit is how vmctl does it, so do that. */
                x.mmio.value = val;
            }
            CHECK(++steps < 100000);
            continue;   /* the kernel completes the read / steps the write on the next run */
        }
        CHECK(x.kind == COSMO_VM_EXIT_HYPERCALL);
        break;
    }
    /* hvc 1: transport identity and the capacity from config space */
    CHECK(x.hypercall.nr == 1);
    CHECK(x.hypercall.a0 == 0x74726976u);          /* MagicValue */
    CHECK(x.hypercall.a1 == 2);                     /* DeviceID block */
    CHECK(x.hypercall.a2 == 8);                     /* capacity: 8 sectors */
    /* run to the read result */
    for (;;) {
        CHECK(vcpu_run(v, &x) == 0);
        if (x.kind == COSMO_VM_EXIT_MMIO && x.mmio.gpa >= VIO_BASE && x.mmio.gpa < VIO_BASE + VIO_SIZE) {
            uint64_t val = x.mmio.value;
            vio_reg(vm, &m, (unsigned)(x.mmio.gpa - VIO_BASE), x.mmio.write, &val);
            if (!x.mmio.write)
                x.mmio.value = val;
            CHECK(++steps < 200000);
            continue;
        }
        CHECK(x.kind == COSMO_VM_EXIT_HYPERCALL);
        break;
    }
    /* hvc 2: status OK and the first three bytes of sector 1 */
    CHECK(x.hypercall.nr == 2);
    CHECK(x.hypercall.a0 == VIRTIO_BLK_S_OK);
    CHECK(x.hypercall.a1 == m.disk[512 + 0]);
    CHECK(x.hypercall.a2 == m.disk[512 + 1]);
    CHECK(x.hypercall.a3 == m.disk[512 + 2]);
    /* the guest now writes sector 2, flushes, and reads it back; run to hvc 3 */
    for (;;) {
        CHECK(vcpu_run(v, &x) == 0);
        if (x.kind == COSMO_VM_EXIT_MMIO && x.mmio.gpa >= VIO_BASE && x.mmio.gpa < VIO_BASE + VIO_SIZE) {
            uint64_t val = x.mmio.value;
            vio_reg(vm, &m, (unsigned)(x.mmio.gpa - VIO_BASE), x.mmio.write, &val);
            if (!x.mmio.write)
                x.mmio.value = val;
            CHECK(++steps < 300000);
            continue;
        }
        CHECK(x.kind == COSMO_VM_EXIT_HYPERCALL);
        break;
    }
    /* hvc 3: the write, flush and read-back all OK, and the bytes read back
     * are the ones the guest wrote (i + 0x40) -- so the write reached the
     * disk and the read returned it, not stale sector-2 bytes. */
    CHECK(x.hypercall.nr == 3);
    CHECK((x.hypercall.a0 & 0xff) == VIRTIO_BLK_S_OK);          /* write */
    CHECK(((x.hypercall.a0 >> 8) & 0xff) == VIRTIO_BLK_S_OK);   /* flush */
    CHECK(((x.hypercall.a0 >> 16) & 0xff) == VIRTIO_BLK_S_OK);  /* read-back */
    CHECK(x.hypercall.a1 == 0x40);
    CHECK(x.hypercall.a2 == 0x41);
    CHECK(x.hypercall.a3 == 0x42);
    CHECK(m.flushes == 1);                                      /* the flush reached the device */
    CHECK(m.disk[2 * 512] == 0x40);                             /* and the write reached the disk */
    drop_guest(vm, v);
    kinfo("selftest: el2-virtq-device: a guest negotiated a virtio-mmio block device, read a sector, "
          "then wrote one, flushed, and read back what it wrote");
    return true;
}

/*
 * el2-virtq-net: a guest drives a virtio-mmio network device at the second
 * transport window, transmits a frame and -- the owner's wire being a
 * loopback -- receives it back. The hostile-input walk is test_vnet_dev's;
 * here a real guest driver negotiates two queues and the frame round-trips.
 */
#define NET_BASE 0x0A000200ull
#define NET_SIZE 0x200ull
#define NET_HDR  12u
#define NET_FRAME_MAX 1514u

struct net_model {
    uint32_t feat_sel, status, queue_sel;
    uint64_t rq_desc, rq_avail, rq_used, tq_desc, tq_avail, tq_used;
    uint32_t rq_num, tq_num;
    int rq_ready, tq_ready;
    uint16_t rq_used_idx, tq_used_idx;
    uint8_t mac[6];
    uint8_t wire[NET_FRAME_MAX];   /* a single-frame loopback (bridge == NULL) */
    uint32_t wire_len;
    int wire_full;
    struct tap *bridge;            /* if set, transmit goes to this real tap instead */
};

/* Drain the transmit queue into the wire, then fill posted receive buffers
 * from it. A compact happy-path loopback; the robust walk is vnet.c's. */
static void net_notify(struct vm *vm, struct net_model *m)
{
    struct { uint64_t addr; uint32_t len; uint16_t flags; uint16_t next; } d;
    uint16_t avail = 0;
    if (m->tq_num && vm_mem_read(vm, m->tq_avail + 2, &avail, 2) == 0) {
        while (m->tq_used_idx != avail) {
            uint16_t head = 0;
            vm_mem_read(vm, m->tq_avail + 4 + (m->tq_used_idx % m->tq_num) * 2u, &head, 2);
            vm_mem_read(vm, m->tq_desc + (uint64_t)head * 16u, &d, sizeof(d));
            uint8_t frame[NET_HDR + NET_FRAME_MAX];
            uint32_t total = d.len <= sizeof(frame) ? d.len : 0;
            if (total)
                vm_mem_read(vm, d.addr, frame, total);
            if (m->bridge) {                            /* to a real tap: strip the virtio header */
                if (total > NET_HDR)
                    tap_inject(m->bridge, frame + NET_HDR, total - NET_HDR);
            } else if (total >= NET_HDR && !m->wire_full) {   /* loopback: keep the frame */
                m->wire_len = total - NET_HDR;
                memcpy(m->wire, frame + NET_HDR, m->wire_len);
                m->wire_full = 1;
            }
            uint32_t elem[2] = { head, 0 };            /* transmit returns nothing */
            vm_mem_write(vm, m->tq_used + 4 + (m->tq_used_idx % m->tq_num) * 8u, elem, 8);
            m->tq_used_idx++;
            vm_mem_write(vm, m->tq_used + 2, &m->tq_used_idx, 2);
        }
    }
    avail = 0;
    if (m->rq_num && vm_mem_read(vm, m->rq_avail + 2, &avail, 2) == 0) {
        while (m->wire_full && m->rq_used_idx != avail) {
            uint16_t head = 0;
            vm_mem_read(vm, m->rq_avail + 4 + (m->rq_used_idx % m->rq_num) * 2u, &head, 2);
            vm_mem_read(vm, m->rq_desc + (uint64_t)head * 16u, &d, sizeof(d));
            uint8_t buf[NET_HDR + NET_FRAME_MAX];
            memset(buf, 0, NET_HDR);
            memcpy(buf + NET_HDR, m->wire, m->wire_len);
            uint32_t total = NET_HDR + m->wire_len;
            if (d.len >= total)
                vm_mem_write(vm, d.addr, buf, total);
            m->wire_full = 0;
            uint32_t elem[2] = { head, total };
            vm_mem_write(vm, m->rq_used + 4 + (m->rq_used_idx % m->rq_num) * 8u, elem, 8);
            m->rq_used_idx++;
            vm_mem_write(vm, m->rq_used + 2, &m->rq_used_idx, 2);
        }
    }
}

static void net_reg(struct vm *vm, struct net_model *m, unsigned off, bool write, uint64_t *val)
{
    /* only queues 0 and 1 exist; a QueueSel past them selects nothing, so
     * the queue-shaped registers do not alias an existing queue's state */
    int sel = m->queue_sel == 0 || m->queue_sel == 1;
    uint64_t *desc = m->queue_sel == 1 ? &m->tq_desc : &m->rq_desc;
    uint64_t *drv  = m->queue_sel == 1 ? &m->tq_avail : &m->rq_avail;
    uint64_t *dev  = m->queue_sel == 1 ? &m->tq_used : &m->rq_used;
    uint32_t *num  = m->queue_sel == 1 ? &m->tq_num : &m->rq_num;
    int *ready     = m->queue_sel == 1 ? &m->tq_ready : &m->rq_ready;
    if (!write) {
        switch (off) {
        case 0x000: *val = 0x74726976u; return;
        case 0x004: *val = 2; return;
        case 0x008: *val = 1; return;                  /* DeviceID: network */
        case 0x00c: *val = 0x554d4551u; return;
        case 0x010: *val = m->feat_sel == 1 ? 1u : (1u << 5); return;  /* VERSION_1 ; NET_F_MAC */
        case 0x034: *val = 8; return;                  /* QueueNumMax */
        case 0x044: *val = sel ? (uint32_t)*ready : 0u; return;
        case 0x070: *val = m->status; return;
        case 0x100: *val = (uint32_t)m->mac[0] | ((uint32_t)m->mac[1] << 8) |
                           ((uint32_t)m->mac[2] << 16) | ((uint32_t)m->mac[3] << 24); return;
        case 0x104: *val = (uint32_t)m->mac[4] | ((uint32_t)m->mac[5] << 8); return;
        default: *val = 0; return;
        }
    }
    uint32_t w = (uint32_t)*val;
    switch (off) {
    case 0x014: m->feat_sel = w; break;
    case 0x030: m->queue_sel = w; break;
    case 0x050: net_notify(vm, m); break;
    case 0x070: m->status = w; break;
    case 0x038: if (sel) *num = w; break;
    case 0x044: if (sel) *ready = (int)w; break;
    case 0x080: if (sel) *desc = (*desc & ~0xFFFFFFFFull) | w; break;
    case 0x084: if (sel) *desc = (*desc & 0xFFFFFFFFull) | ((uint64_t)w << 32); break;
    case 0x090: if (sel) *drv = (*drv & ~0xFFFFFFFFull) | w; break;
    case 0x094: if (sel) *drv = (*drv & 0xFFFFFFFFull) | ((uint64_t)w << 32); break;
    case 0x0a0: if (sel) *dev = (*dev & ~0xFFFFFFFFull) | w; break;
    case 0x0a4: if (sel) *dev = (*dev & 0xFFFFFFFFull) | ((uint64_t)w << 32); break;
    default: break;
    }
}

bool selftest_el2_virtq_net(const char **reason)
{
    if (skip_without_backend(reason))
        return true;
    struct vm *vm;
    struct vcpu *v;
    CHECK(make_guest("tests/hv/guest_vnet.bin", &vm, &v) == 0);
    struct net_model m;
    memset(&m, 0, sizeof(m));
    const uint8_t mac[6] = { 0x52, 0x54, 0x00, 0x00, 0x00, 0x01 };
    memcpy(m.mac, mac, 6);
    struct cosmo_vm_exit x;
    memset(&x, 0, sizeof(x));
    unsigned steps = 0;
    for (;;) {
        CHECK(vcpu_run(v, &x) == 0);
        if (x.kind == COSMO_VM_EXIT_MMIO && x.mmio.gpa >= NET_BASE && x.mmio.gpa < NET_BASE + NET_SIZE) {
            uint64_t val = x.mmio.value;
            net_reg(vm, &m, (unsigned)(x.mmio.gpa - NET_BASE), x.mmio.write, &val);
            if (!x.mmio.write)
                x.mmio.value = val;
            CHECK(++steps < 200000);
            continue;
        }
        CHECK(x.kind == COSMO_VM_EXIT_HYPERCALL);
        break;
    }
    /* hvc 1: transport identity and the MAC's low bytes from config space */
    CHECK(x.hypercall.nr == 1);
    CHECK(x.hypercall.a0 == 0x74726976u);          /* MagicValue */
    CHECK(x.hypercall.a1 == 1);                     /* DeviceID network */
    CHECK((x.hypercall.a2 & 0xff) == 0x52);         /* MAC[0] */
    for (;;) {
        CHECK(vcpu_run(v, &x) == 0);
        if (x.kind == COSMO_VM_EXIT_MMIO && x.mmio.gpa >= NET_BASE && x.mmio.gpa < NET_BASE + NET_SIZE) {
            uint64_t val = x.mmio.value;
            net_reg(vm, &m, (unsigned)(x.mmio.gpa - NET_BASE), x.mmio.write, &val);
            if (!x.mmio.write)
                x.mmio.value = val;
            CHECK(++steps < 400000);
            continue;
        }
        CHECK(x.kind == COSMO_VM_EXIT_HYPERCALL);
        break;
    }
    /* hvc 2: the frame came back -- used length is header+frame, and the
     * bytes are the ones transmitted (i + 0x30), not a stale buffer. */
    CHECK(x.hypercall.nr == 2);
    CHECK(x.hypercall.a0 == NET_HDR + 64u);
    CHECK(x.hypercall.a1 == 0x30);
    CHECK(x.hypercall.a2 == 0x31);
    CHECK(x.hypercall.a3 == 0x32);
    drop_guest(vm, v);
    kinfo("selftest: el2-virtq-net: a guest negotiated a virtio-mmio network device, "
          "transmitted a frame and received it back through the loopback wire");
    return true;
}

/*
 * el2-tap-host: a guest transmits an ARP request over virtio-net, the wire
 * bridged to a real tap in the host stack, and the host stack answers on the
 * tap. So the guest's frame crossed virtio-net, the owner's bridge, and into
 * the real stack -- the outbound guest-to-host path end to end. (The reply
 * reaching the guest's receive queue is el2-virtq-net's loopback and the tap
 * selftest's stack->tap; this is the piece those do not cover.)
 */
bool selftest_el2_tap_host(const char **reason)
{
    if (skip_without_backend(reason))
        return true;
    static const uint8_t tap_mac[6] = { 0x52, 0x54, 0x00, 0xaa, 0xbb, 0xdd };
    /* a subnet of its own, distinct from tap0's 10.0.3.0/24 */
    struct tap *t = tap_create("taphost", 0x0104000au /* 10.0.4.1 */, 0x00ffffffu, tap_mac);
    CHECK(t != NULL);
    struct vm *vm;
    struct vcpu *v;
    if (make_guest("tests/hv/guest_tap.bin", &vm, &v) != 0) {
        tap_destroy(t);
        CHECK(0);
    }
    struct net_model m;
    memset(&m, 0, sizeof(m));
    m.bridge = t;                                   /* transmit -> the real tap */
    struct cosmo_vm_exit x;
    memset(&x, 0, sizeof(x));
    unsigned steps = 0;
    for (;;) {
        if (vcpu_run(v, &x) != 0) { tap_destroy(t); drop_guest(vm, v); CHECK(0); }
        if (x.kind == COSMO_VM_EXIT_MMIO && x.mmio.gpa >= NET_BASE && x.mmio.gpa < NET_BASE + NET_SIZE) {
            uint64_t val = x.mmio.value;
            net_reg(vm, &m, (unsigned)(x.mmio.gpa - NET_BASE), x.mmio.write, &val);
            if (!x.mmio.write)
                x.mmio.value = val;
            if (++steps >= 200000) { tap_destroy(t); drop_guest(vm, v); CHECK(0); }
            continue;
        }
        if (x.kind == COSMO_VM_EXIT_HYPERCALL && x.hypercall.nr == 2)
            break;                                  /* the guest transmitted the ARP */
        if (x.kind == COSMO_VM_EXIT_HYPERCALL)
            continue;                               /* hvc 1 (identity), or the idle 9 */
        tap_destroy(t); drop_guest(vm, v); CHECK(0);
    }
    /* the host stack processes the injected ARP on its worker and answers out
     * the tap; poll the tap for the reply. */
    struct mbuf *reply = NULL;
    for (unsigned i = 0; i < 50 && reply == NULL; i++) {
        reply = tap_recv(t);
        if (reply == NULL)
            thread_sleep_ms(10);
    }
    int ok = reply != NULL;
    if (ok) {
        uint8_t r[42];
        ok = m_copydata(reply, 0, 42, r)
             && r[12] == 0x08 && r[13] == 0x06   /* ARP */
             && r[21] == 2                        /* reply */
             && memcmp(r + 22, tap_mac, 6) == 0   /* from the tap's MAC */
             && r[28] == 10 && r[29] == 0 && r[30] == 4 && r[31] == 1;   /* spa 10.0.4.1 */
        m_freem(reply);
    }
    drop_guest(vm, v);
    tap_destroy(t);
    CHECK(ok);
    kinfo("selftest: el2-tap-host: a guest's ARP crossed virtio-net and the bridge into the host stack, "
          "which answered on the tap");
    return true;
}

bool selftest_el2_guest_hvc(const char **reason)
{
    if (skip_without_backend(reason))
        return true;
    struct vm *vm;
    struct vcpu *v;
    CHECK(make_guest("tests/hv/guest_hvc.bin", &vm, &v) == 0);
    struct cosmo_vm_exit x;
    memset(&x, 0, sizeof(x));
    CHECK(vcpu_run(v, &x) == 0);
    CHECK(x.kind == COSMO_VM_EXIT_HYPERCALL);
    /* The registers the guest set, as the manager reports them. */
    CHECK(x.hypercall.nr == 0x2A);
    CHECK(x.hypercall.a0 == 1 && x.hypercall.a1 == 2 && x.hypercall.a2 == 3 && x.hypercall.a3 == 4);
    /* HVC leaves the PC on the instruction after it: the guest runs on
     * without the owner moving anything. */
    CHECK(x.rip == LOAD_GPA + 6 * 4);
    CHECK(vcpu_run(v, &x) == 0);
    CHECK(x.kind == COSMO_VM_EXIT_WFI);
    struct cosmo_vcpu_regs regs;
    CHECK(vcpu_get_regs(v, &regs) == 0);
    CHECK(regs.x[0] == 0x2B);                   /* the add after the HVC ran */
    CHECK(regs.x[4] == 4);
    drop_guest(vm, v);
    return true;
}

bool selftest_el2_guest_mmio(const char **reason)
{
    if (skip_without_backend(reason))
        return true;
    struct vm *vm;
    struct vcpu *v;
    CHECK(make_guest("tests/hv/guest_mmio.bin", &vm, &v) == 0);
    struct cosmo_vm_exit x;
    memset(&x, 0, sizeof(x));
    /* A store to guest-physical memory the VM does not have: a stage-2
     * fault, reported with the address and the direction. */
    CHECK(vcpu_run(v, &x) == 0);
    CHECK(x.kind == COSMO_VM_EXIT_MMIO);
    CHECK(x.mmio.gpa == 0x40000000ull);
    CHECK(x.mmio.write);
    /* And what the access was: eight bytes, from x1, the value it held. */
    CHECK(x.mmio.size == 8 && x.mmio.reg == 1 && x.mmio.value == 0xABCDull);
    /* The owner steps over the store; the load that follows is a read,
     * into x2. Setting the registers is also how an owner declines to
     * answer a read: the completion is cancelled with them. */
    struct cosmo_vcpu_regs regs;
    CHECK(vcpu_get_regs(v, &regs) == 0);
    regs.pc += 4;
    CHECK(vcpu_set_regs(v, &regs) == 0);
    CHECK(vcpu_run(v, &x) == 0);
    CHECK(x.kind == COSMO_VM_EXIT_MMIO && x.mmio.gpa == 0x40000000ull && !x.mmio.write);
    CHECK(x.mmio.size == 8 && x.mmio.reg == 2 && x.mmio.sf && !x.mmio.sse);
    drop_guest(vm, v);
    return true;
}

bool selftest_el2_guest_sysreg(const char **reason)
{
    if (skip_without_backend(reason))
        return true;
    struct vm *vm;
    struct vcpu *v;
    CHECK(make_guest("tests/hv/guest_sysreg.bin", &vm, &v) == 0);
    struct cosmo_vm_exit x;
    memset(&x, 0, sizeof(x));
    /* HCR_EL2.TID3 traps the ID-register read, and the feature model now
     * answers it in the kernel: the guest's mrs of id_aa64pfr0_el1 does
     * not reach the owner at all -- the run goes straight to the WFI, and
     * x5 holds the sanitized value (its EL2 field cleared, though this
     * host runs at EL2, so it is not the host's raw register). */
    CHECK(vcpu_run(v, &x) == 0);
    CHECK(x.kind == COSMO_VM_EXIT_WFI);
    struct cosmo_vcpu_regs regs;
    CHECK(vcpu_get_regs(v, &regs) == 0);
    CHECK(((regs.x[5] >> 8) & 0xF) == 0);       /* EL2 field: hidden */
    CHECK(regs.x[5] != HOST_IDREG("S3_0_c0_c4_0"));   /* not the host's own (EL2 is present there) */
    drop_guest(vm, v);
    return true;
}

bool selftest_el2_guest_spin(const char **reason)
{
    if (skip_without_backend(reason))
        return true;
    struct vm *vm;
    struct vcpu *v;
    CHECK(make_guest("tests/hv/guest_spin.bin", &vm, &v) == 0);
    struct cosmo_vm_exit x;
    memset(&x, 0, sizeof(x));
    /* The guest never leaves its loop; the host's timer tick is taken to
     * EL2 (HCR_EL2.IMO) and ends the run, which is what keeps a guest
     * from owning a CPU. */
    CHECK(vcpu_run_limited(v, &x, 5) == -ETIMEDOUT);
    CHECK(v->exits >= 5);
    struct cosmo_vcpu_regs regs;
    CHECK(vcpu_get_regs(v, &regs) == 0);
    CHECK(regs.pc == LOAD_GPA);                 /* still in its one-instruction loop */
    drop_guest(vm, v);
    return true;
}
#else
bool selftest_el2_guest_wfi(const char **reason) { (void)reason; return true; }
bool selftest_el2_vgic_roundtrip(const char **reason) { (void)reason; return true; }
bool selftest_el2_guest_irq(const char **reason) { (void)reason; return true; }
bool selftest_el2_guest_irq_masked(const char **reason) { (void)reason; return true; }
bool selftest_el2_guest_irq_private(const char **reason) { (void)reason; return true; }
bool selftest_el2_guest_irq_queue(const char **reason) { (void)reason; return true; }
bool selftest_el2_guest_timer_isolated(const char **reason) { (void)reason; return true; }
bool selftest_el2_guest_timer_offset(const char **reason) { (void)reason; return true; }
bool selftest_el2_guest_phys_timer(const char **reason) { (void)reason; return true; }
bool selftest_el2_guest_timer(const char **reason) { (void)reason; return true; }
bool selftest_el2_guest_timer_ontime(const char **reason) { (void)reason; return true; }
bool selftest_el2_guest_gicd_probe(const char **reason) { (void)reason; return true; }
bool selftest_el2_guest_gic_config(const char **reason) { (void)reason; return true; }
bool selftest_el2_guest_gic_timer(const char **reason) { (void)reason; return true; }
bool selftest_el2_guest_sgi(const char **reason) { (void)reason; return true; }
bool selftest_el2_guest_gicd_isolated(const char **reason) { (void)reason; return true; }
bool selftest_el2_mmio_device(const char **reason) { (void)reason; return true; }
bool selftest_el2_guest_uart(const char **reason) { (void)reason; return true; }
bool selftest_el2_guest_uart_rx(const char **reason) { (void)reason; return true; }
bool selftest_el2_guest_uart_level(const char **reason) { (void)reason; return true; }
bool selftest_el2_guest_uart_race(const char **reason) { (void)reason; return true; }
bool selftest_el2_guest_dtb(const char **reason) { (void)reason; return true; }
bool selftest_el2_guest_psci(const char **reason) { (void)reason; return true; }
bool selftest_el2_vcpu_run_tick(const char **reason) { (void)reason; return true; }
bool selftest_el2_guest_hvc(const char **reason) { (void)reason; return true; }
bool selftest_el2_guest_mmio(const char **reason) { (void)reason; return true; }
bool selftest_el2_guest_sysreg(const char **reason) { (void)reason; return true; }
bool selftest_el2_guest_idreg(const char **reason) { (void)reason; return true; }
bool selftest_el2_vm_raise_spi(const char **reason) { (void)reason; return true; }
bool selftest_el2_virtq_device(const char **reason) { (void)reason; return true; }
bool selftest_el2_virtq_net(const char **reason) { (void)reason; return true; }
bool selftest_el2_tap_host(const char **reason) { (void)reason; return true; }
bool selftest_el2_guest_spin(const char **reason) { (void)reason; return true; }
#endif
