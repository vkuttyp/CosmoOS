/*
 * vcpu.c - VirtualCPU: lifetime, state, the run loop, CPUID and MSR
 * emulation (docs/kernel-services/virtualization/design.md, "The run loop").
 */

#include <kernel/errno.h>
#include <kernel/kmalloc.h>
#include <kernel/log.h>
#include <kernel/process.h>
#include <kernel/string.h>

#include "hv_internal.h"

static void vcpu_release(struct kobject *obj);

static const struct kobject_type vcpu_type = {
    .name = "vcpu",
    .release = vcpu_release,
};

struct vcpu *vcpu_from_kobject(struct kobject *obj)
{
    return obj != NULL && obj->type == &vcpu_type ? container_of(obj, struct vcpu, obj) : NULL;
}

int vcpu_create(struct vm *vm, unsigned index, struct vcpu **out)
{
    if (!hv_caps()->present)
        return -ENOTSUP;
    if (index >= HV_VCPUS_MAX)
        return -EINVAL;
    struct vcpu *v = kzalloc(sizeof(*v));
    if (v == NULL)
        return -ENOMEM;
    kobject_init(&v->obj, &vcpu_type);
    mutex_init(&v->run_lock, "vcpu");
    vintr_init(v);
    v->index = index;
    int rc = arch_hv_vcpu_create(vm->arch, &v->arch);
    if (rc) {
        kfree(v);
        return rc;
    }
    mutex_lock(&vm->lock);
    if (vm->vcpus[index] != NULL) {
        mutex_unlock(&vm->lock);
        arch_hv_vcpu_destroy(v->arch);
        kfree(v);
        return -EEXIST;
    }
    kobject_get(&vm->obj);
    v->vm = vm;
    vm->vcpus[index] = v;
    vm->nr_vcpus++;
    mutex_unlock(&vm->lock);
    *out = v;
    return 0;
}

static void vcpu_release(struct kobject *obj)
{
    struct vcpu *v = container_of(obj, struct vcpu, obj);
    struct vm *vm = v->vm;
    mutex_lock(&vm->lock);
    vm->vcpus[v->index] = NULL;
    vm->nr_vcpus--;
    mutex_unlock(&vm->lock);
    arch_hv_vcpu_destroy(v->arch);
    kfree(v);
    kobject_put(&vm->obj);
}

int vcpu_get_regs(struct vcpu *v, struct cosmo_vcpu_regs *out)
{
    mutex_lock(&v->run_lock);
    arch_hv_vcpu_get_state(v->arch, out);
    int p = vintr_take_lowest(v);
    out->pending_irq = p < 0 ? ~0ull : (uint64_t)p;
    mutex_unlock(&v->run_lock);
    return 0;
}

int vcpu_set_regs(struct vcpu *v, const struct cosmo_vcpu_regs *in)
{
    mutex_lock(&v->run_lock);
    int rc = arch_hv_vcpu_set_state(v->arch, in);
    if (rc == 0)
        v->in_completion = false;   /* the owner rewrote rax itself */
    mutex_unlock(&v->run_lock);
    return rc;
}

/* --- CPUID --- */

void vcpu_emulate_cpuid(struct vcpu *v)
{
    uint32_t leaf = (uint32_t)arch_hv_vcpu_read_gpr(v->arch, HV_GPR_RAX);
    uint32_t sub = (uint32_t)arch_hv_vcpu_read_gpr(v->arch, HV_GPR_RCX);
    struct { uint32_t eax, ebx, ecx, edx; } r = { 0, 0, 0, 0 };
    if (leaf >= 0x40000000u && leaf <= 0x400000FFu) {
        if (leaf == 0x40000000u) {
            r.eax = 0x40000000u;
            memcpy(&r.ebx, "Cosm", 4);
            memcpy(&r.ecx, "oOSC", 4);
            memcpy(&r.edx, "osmo", 4);
        }
    } else if (leaf == 6 || leaf == 0xB || (leaf == 0xD && sub > 1)) {
        /* thermal/power, extended topology, per-component XSAVE sub-leaves: none */
    } else if (leaf == 0x8000000Au) {
        /* SVM features: the guest is not a hypervisor */
    } else if (leaf == 0xD) {
        /* Extended state: exactly the components the host holds for the
         * guest (arch_hv_host_xstate), the area size for all of them, and
         * of the sub-leaf 1 instruction forms only XSAVEOPT (XSAVEC,
         * XGETBV with ECX=1, XSAVES need state the backend does not keep). */
        uint64_t xs = arch_hv_host_xstate();
        if (xs != 0) {
            arch_hv_host_cpuid(0xD, sub, &r.eax, &r.ebx, &r.ecx, &r.edx);
            if (sub == 0) {
                r.eax = (uint32_t)xs;
                r.edx = (uint32_t)(xs >> 32);
                r.ecx = r.ebx;            /* max size == size for everything we enable */
            } else {
                r.eax &= 1u;              /* XSAVEOPT only */
                r.ebx = r.ecx = r.edx = 0;
            }
        }
    } else {
        arch_hv_host_cpuid(leaf, sub, &r.eax, &r.ebx, &r.ecx, &r.edx);
        if (leaf == 1) {
            r.ecx &= ~((1u << 3) | (1u << 5) | (1u << 6));    /* MONITOR, VMX, SMX */
            r.ecx |= 1u << 31;                                /* hypervisor present */
            r.ebx = (r.ebx & 0x00FFFFFFu) | ((uint32_t)v->index << 24);   /* initial APIC id */
            /* OSXSAVE reflects the guest's own CR4, and XSAVE is offered
             * only when the host keeps extended state for guests. */
            r.ecx &= ~(1u << 27);
            if (arch_hv_host_xstate() == 0)
                r.ecx &= ~(1u << 26);
            else if (arch_hv_vcpu_xstate_enabled(v->arch))
                r.ecx |= 1u << 27;
            if (!(arch_hv_host_xstate() & (1ull << 2)))
                r.ecx &= ~(1u << 28);                             /* AVX needs the AVX state component */
        } else if (leaf == 7 && sub == 0) {
            /* Instruction-set bits whose state the host does not hold for
             * the guest would only lead it to an XSETBV #GP: AVX2 needs
             * AVX state, AVX-512 needs the opmask/ZMM components. */
            uint64_t xs = arch_hv_host_xstate();
            if (!(xs & (1ull << 2)))
                r.ebx &= ~(1u << 5);                              /* AVX2 */
            if (!(xs & (1ull << 5))) {
                r.ebx &= ~((1u << 16) | (1u << 17) | (1u << 21) | (1u << 26) | (1u << 27) | (1u << 28) |
                           (1u << 30) | (1u << 31));              /* AVX512 F/DQ/IFMA/PF/ER/CD/BW/VL */
                r.ecx &= ~((1u << 1) | (1u << 6) | (1u << 11) | (1u << 12) | (1u << 14));   /* VBMI, VBMI2, VNNI, BITALG, VPOPCNTDQ */
                r.edx &= ~((1u << 2) | (1u << 3) | (1u << 8) | (1u << 23));                 /* 4VNNIW, 4FMAPS, VP2INTERSECT, FP16 */
            }
        } else if (leaf == 0x80000001u) {
            r.ecx &= ~(1u << 2);                              /* SVM */
        } else if (leaf == 0) {
            if (r.eax > 0x0D)
                r.eax = 0x0D;
        }
    }
    arch_hv_vcpu_write_gpr(v->arch, HV_GPR_RAX, r.eax);
    arch_hv_vcpu_write_gpr(v->arch, HV_GPR_RBX, r.ebx);
    arch_hv_vcpu_write_gpr(v->arch, HV_GPR_RCX, r.ecx);
    arch_hv_vcpu_write_gpr(v->arch, HV_GPR_RDX, r.edx);
    arch_hv_vcpu_advance_rip(v->arch, 2);
}

/* --- MSRs --- */

#define MSR_EFER_ 0xC0000080u

static void gp(struct vcpu *v)
{
    if (v->msr_gp++ < 8)
        kdebug("hv: vm%u vcpu%u: #GP for msr 0x%x", v->vm->id, v->index,
               (uint32_t)arch_hv_vcpu_read_gpr(v->arch, HV_GPR_RCX));
    arch_hv_vcpu_inject_exception(v->arch, 13, true, 0);
}

int vcpu_emulate_msr(struct vcpu *v, uint32_t index, bool write)
{
    uint64_t value = 0;
    if (write)
        value = (arch_hv_vcpu_read_gpr(v->arch, HV_GPR_RDX) << 32) |
                (arch_hv_vcpu_read_gpr(v->arch, HV_GPR_RAX) & 0xFFFFFFFFull);
    int rc = 0;
    switch (index) {
    case MSR_EFER_:
        if (write)
            rc = arch_hv_vcpu_set_guest_efer(v->arch, value);
        else
            value = arch_hv_vcpu_guest_efer(v->arch);
        break;
    case 0x10:                                /* TSC: the host's */
        if (write)
            rc = -EINVAL;
        else
            value = arch_hv_host_tsc();
        break;
    case 0x1B:                                /* APIC_BASE: enabled, default address, BSP for vcpu 0 */
        if (!write)
            value = 0xFEE00800ull | (v->index == 0 ? 0x100 : 0);
        break;
    case 0x1A0:                               /* MISC_ENABLE */
    case 0xFE:                                /* MTRRcap */
    case 0x2FF:                               /* MTRRdefType */
    case 0x8B:                                /* microcode revision */
        if (!write)
            value = 0;
        break;
    default:
        rc = arch_hv_vcpu_msr(v->arch, index, write, &value);
        break;
    }
    if (rc) {
        gp(v);
        return -ENOENT;
    }
    if (!write) {
        arch_hv_vcpu_write_gpr(v->arch, HV_GPR_RAX, value & 0xFFFFFFFFull);
        arch_hv_vcpu_write_gpr(v->arch, HV_GPR_RDX, value >> 32);
    }
    arch_hv_vcpu_advance_rip(v->arch, 2);
    return 0;
}

/* --- the run loop --- */

static void fill_common(struct vcpu *v, struct cosmo_vm_exit *x, uint32_t kind)
{
    x->kind = kind;
    x->rip = arch_hv_vcpu_rip(v->arch);
    x->flags = vintr_any(v) ? COSMO_VM_EXIT_F_IRQ_PENDING : 0;
}

int vcpu_run(struct vcpu *v, struct cosmo_vm_exit *x)
{
    return vcpu_run_limited(v, x, 0);
}

/* max_intr > 0: give up with -ETIMEDOUT after that many host-interrupt exits (tests). */
int vcpu_run_limited(struct vcpu *v, struct cosmo_vm_exit *x, unsigned max_intr)
{
    struct vm *vm = v->vm;
    unsigned intr = 0;
    if (!hv_caps()->present)
        return -ENOTSUP;
    mutex_lock(&v->run_lock);
    if (v->dead) {
        mutex_unlock(&v->run_lock);
        return -EIO;
    }
    if (!vm->started) {
        mutex_lock(&vm->lock);
        vm->started = true;
        mutex_unlock(&vm->lock);
    }
    if (v->in_completion) {
        uint64_t value = x->kind == COSMO_VM_EXIT_IO ? x->io.value : 0xFFFFFFFFu;
        arch_hv_vcpu_write_rax(v->arch, value, v->in_size);
        v->in_completion = false;
    }
    memset(x, 0, sizeof(*x));
    int rc = 0;
    for (;;) {
        if (process_kill_pending()) {
            rc = -EINTR;
            break;
        }
        int offered = vintr_take_lowest(v);
        arch_hv_vcpu_set_irq(v->arch, offered);
        struct hv_exit e;
        v->entries++;
        rc = arch_hv_vcpu_run(v->arch, &e);
        if (rc)
            break;
        v->exits++;
        if (offered >= 0 && arch_hv_vcpu_irq_taken(v->arch))
            vintr_clear(v, offered);

        if (e.kind == HV_EXIT_INTR) {
            if (max_intr && ++intr >= max_intr) {
                rc = -ETIMEDOUT;
                break;
            }
            continue;
        }
        if (e.kind == HV_EXIT_CPUID) {
            vcpu_emulate_cpuid(v);
            continue;
        }
        if (e.kind == HV_EXIT_MSR) {
            vcpu_emulate_msr(v, e.msr.index, e.msr.write);
            continue;
        }
        if (e.kind == HV_EXIT_IO) {
            uint32_t value = 0;
            if (e.io.write)
                value = (uint32_t)(arch_hv_vcpu_read_gpr(v->arch, HV_GPR_RAX) &
                                   (e.io.size == 4 ? 0xFFFFFFFFu : e.io.size == 2 ? 0xFFFFu : 0xFFu));
            if (!e.io.string && vmdev_pio(vm, e.io.port, e.io.write, e.io.size, &value) == 0) {
                if (!e.io.write)
                    arch_hv_vcpu_write_rax(v->arch, value, e.io.size);
                arch_hv_vcpu_set_rip(v->arch, e.io.next_rip);
                continue;
            }
            /* To the owner. RIP already points past the instruction. */
            arch_hv_vcpu_set_rip(v->arch, e.io.next_rip);
            fill_common(v, x, COSMO_VM_EXIT_IO);
            x->io.port = e.io.port;
            x->io.size = e.io.size;
            x->io.write = e.io.write;
            x->io.string = e.io.string;
            x->io.rep = e.io.rep;
            x->io.value = value;
            if (!e.io.write) {
                v->in_completion = true;
                v->in_size = e.io.size;
            }
            break;
        }
        if (e.kind == HV_EXIT_HLT) {
            fill_common(v, x, COSMO_VM_EXIT_HLT);
            break;
        }
        if (e.kind == HV_EXIT_WFI) {
            /* The AArch64 form of "the guest has nothing to do until an
             * interrupt": the owner decides whether to inject one and
             * run again, exactly as for HLT. */
            fill_common(v, x, COSMO_VM_EXIT_WFI);
            break;
        }
        if (e.kind == HV_EXIT_SYSREG) {
            /* What CPUID is on x86: the model answers for the registers
             * it implements, and everything else goes to the owner. */
            fill_common(v, x, COSMO_VM_EXIT_SYSREG);
            x->sysreg.iss = e.sysreg.iss;
            x->sysreg.reg = e.sysreg.reg;
            x->sysreg.write = e.sysreg.write;
            break;
        }
        if (e.kind == HV_EXIT_MMIO) {
            vmdev_mmio(vm, e.mmio.gpa, e.mmio.write);
            fill_common(v, x, COSMO_VM_EXIT_MMIO);
            x->mmio.gpa = e.mmio.gpa;
            x->mmio.write = e.mmio.write;
            break;
        }
        if (e.kind == HV_EXIT_HYPERCALL) {
            fill_common(v, x, COSMO_VM_EXIT_HYPERCALL);
            /* The calling convention is the architecture's, not this
             * layer's: x86 uses rax and rbx/rcx/rdx/rsi (the VMMCALL
             * habit), AArch64 the first five argument registers. */
#if defined(ARCH_AARCH64)
            x->hypercall.nr = arch_hv_vcpu_read_gpr(v->arch, 0);
            x->hypercall.a0 = arch_hv_vcpu_read_gpr(v->arch, 1);
            x->hypercall.a1 = arch_hv_vcpu_read_gpr(v->arch, 2);
            x->hypercall.a2 = arch_hv_vcpu_read_gpr(v->arch, 3);
            x->hypercall.a3 = arch_hv_vcpu_read_gpr(v->arch, 4);
#else
            x->hypercall.nr = arch_hv_vcpu_read_gpr(v->arch, HV_GPR_RAX);
            x->hypercall.a0 = arch_hv_vcpu_read_gpr(v->arch, HV_GPR_RBX);
            x->hypercall.a1 = arch_hv_vcpu_read_gpr(v->arch, HV_GPR_RCX);
            x->hypercall.a2 = arch_hv_vcpu_read_gpr(v->arch, HV_GPR_RDX);
            x->hypercall.a3 = arch_hv_vcpu_read_gpr(v->arch, HV_GPR_RSI);
#endif
            break;
        }
        if (e.kind == HV_EXIT_SHUTDOWN) {
            v->dead = true;
            fill_common(v, x, COSMO_VM_EXIT_SHUTDOWN);
            break;
        }
        /* HV_EXIT_FAIL */
        v->dead = true;
        fill_common(v, x, COSMO_VM_EXIT_FAIL);
        x->fail.code = (uint32_t)e.fail.code;
        x->fail.info1 = e.fail.info1;
        x->fail.info2 = e.fail.info2;
        break;
    }
    mutex_unlock(&v->run_lock);
    return rc;
}
