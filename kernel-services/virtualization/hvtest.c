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
#include <kernel/selftest.h>
#include <kernel/string.h>
#include <kernel/thread.h>
#include <arch/fpu.h>
#include <arch/testhooks.h>

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

/* A VM with 1 MiB at 0 and the named image at 0x1000; one vCPU with rip 0x1000. */
static int make_guest(const char *image, struct vm **vm_out, struct vcpu **vcpu_out)
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
        regs.rip = LOAD_GPA;
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

static void drop_guest(struct vm *vm, struct vcpu *v)
{
    kobject_put(&v->obj);
    kobject_put(&vm->obj);
}

/* --- the guest rule: no vector register crosses the guest boundary ------ */

bool selftest_hv_guest_fpu(const char **reason)
{
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
}

static bool console_is(struct vm *vm, const char *expect)
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
    CHECK(strcmp(c->name, "svm") == 0 || strcmp(c->name, "vmx") == 0);
    CHECK(c->nested_paging);
    CHECK(c->max_asids >= 2);
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
}

bool selftest_hv_guest_irq(const char **reason)
{
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
}

bool selftest_hv_guest_cpuid(const char **reason)
{
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
}

bool selftest_hv_guest_pm(const char **reason)
{
    if (skip_without_backend(reason))
        return true;
    struct vm *vm;
    struct vcpu *v;
    CHECK(make_guest("tests/hv/guest_pm.bin", &vm, &v) == 0);
    struct cosmo_vcpu_regs regs;
    CHECK(vcpu_get_regs(v, &regs) == 0);
    regs.cr0 = 0x11;                                    /* PE | ET */
    regs.cs.selector = 0x8;  regs.cs.attrib = 0xC9B; regs.cs.limit = 0xFFFFFFFF; regs.cs.base = 0;
    regs.ds.selector = 0x10; regs.ds.attrib = 0xC93; regs.ds.limit = 0xFFFFFFFF; regs.ds.base = 0;
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
}

bool selftest_hv_guest_shutdown(const char **reason)
{
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
}

bool selftest_hv_guest_spin(const char **reason)
{
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
}
