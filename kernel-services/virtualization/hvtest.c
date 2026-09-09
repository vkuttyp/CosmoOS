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
#include <kernel/page.h>
#include <kernel/pmm.h>
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
    CHECK(make_guest("tests/hv/guest_timer.bin", &vm, &v) == 0);
    struct cosmo_vm_exit x;
    memset(&x, 0, sizeof(x));
    uint64_t before = arch_test_host_vtimer_ctl();

    CHECK(vcpu_run(v, &x) == 0);                                    /* ready */
    CHECK(x.kind == COSMO_VM_EXIT_HYPERCALL && x.hypercall.nr == 1);
    CHECK(vcpu_run(v, &x) == 0);                                    /* armed, then the heartbeat */
    CHECK(x.kind == COSMO_VM_EXIT_HYPERCALL && x.hypercall.nr == 2);

    /* The guest's ENABLE is in the guest's saved state and nowhere else. */
    uint64_t ctl = 0, off = 0;
    CHECK(arch_hv_vcpu_timer_state(v->arch, &ctl, &off));
    CHECK((ctl & 1u) != 0);                                         /* the guest armed it */
    CHECK(arch_test_host_vtimer_ctl() == before);                   /* and the host did not notice */
    drop_guest(vm, v);
    CHECK(arch_test_host_vtimer_ctl() == before);
    kinfo("selftest: el2-guest-timer-isolated: guest CNTV_CTL 0x%llx, host's stayed 0x%llx",
          (unsigned long long)ctl, (unsigned long long)before);
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
    CHECK(make_guest("tests/hv/guest_timer.bin", &vm, &v0) == 0);
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
    CHECK(make_guest("tests/hv/guest_timer.bin", &vm_b, &vb) == 0);
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
    /* The owner steps over the store; the load that follows is a read. */
    struct cosmo_vcpu_regs regs;
    CHECK(vcpu_get_regs(v, &regs) == 0);
    regs.pc += 4;
    CHECK(vcpu_set_regs(v, &regs) == 0);
    CHECK(vcpu_run(v, &x) == 0);
    CHECK(x.kind == COSMO_VM_EXIT_MMIO && x.mmio.gpa == 0x40000000ull && !x.mmio.write);
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
    /* HCR_EL2.TID3 traps the ID-register read: the manager is told which
     * register the guest wanted it in, and that it was a read. */
    CHECK(vcpu_run(v, &x) == 0);
    CHECK(x.kind == COSMO_VM_EXIT_SYSREG);
    CHECK(x.sysreg.reg == 5);                   /* mrs x5, ... */
    CHECK(!x.sysreg.write);
    /* Answer as a model would and step over the instruction. */
    struct cosmo_vcpu_regs regs;
    CHECK(vcpu_get_regs(v, &regs) == 0);
    regs.x[5] = 0;
    regs.pc += 4;
    CHECK(vcpu_set_regs(v, &regs) == 0);
    CHECK(vcpu_run(v, &x) == 0);
    CHECK(x.kind == COSMO_VM_EXIT_WFI);
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
bool selftest_el2_guest_hvc(const char **reason) { (void)reason; return true; }
bool selftest_el2_guest_mmio(const char **reason) { (void)reason; return true; }
bool selftest_el2_guest_sysreg(const char **reason) { (void)reason; return true; }
bool selftest_el2_guest_spin(const char **reason) { (void)reason; return true; }
#endif
