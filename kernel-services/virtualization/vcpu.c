/*
 * vcpu.c - VirtualCPU: lifetime, state, the run loop, CPUID and MSR
 * emulation (docs/kernel-services/virtualization/design.md, "The run loop").
 */

#include <kernel/errno.h>
#include <kernel/ipi.h>
#include <kernel/kmalloc.h>
#include <kernel/log.h>
#include <kernel/process.h>
#include <kernel/string.h>
#include <kernel/wait.h>
#include <arch/timer.h>

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
    int rc = arch_hv_vcpu_create(vm->arch, index, &v->arch);
    if (rc) {
        kfree(v);
        return rc;
    }
    /* The stop handshake's words are this object's; the backend publishes
     * into them around its guest entry (kernel/hvkick.h). Given before any
     * run can happen, which is before this function returns. */
    arch_hv_vcpu_set_kick(v->arch, &v->kick);
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
    if (rc == 0) {
        v->in_completion = false;                /* the owner rewrote rax itself */
        v->mmio_completion.pending = false;      /* and the register an MMIO read was waiting for */
    }
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

void hv_mmio_complete_read(struct vcpu *v, unsigned size, bool sse, bool sf, unsigned reg, uint64_t value)
{
    if (reg >= 31)
        return;   /* XZR: the load had no destination */
    if (size < 8)
        value &= (1ull << (size * 8u)) - 1u;
    if (sse && size < 8) {
        uint64_t sign = 1ull << (size * 8u - 1u);
        if (value & sign)
            value |= ~((sign << 1) - 1u);   /* to 64 bits; a Wt destination is cut below */
    }
    if (!sf)
        value &= 0xFFFFFFFFull;   /* a write to Wt zeroes the upper half */
    arch_hv_vcpu_write_gpr(v->arch, reg, value);
}

static void fill_common(struct vcpu *v, struct cosmo_vm_exit *x, uint32_t kind)
{
    x->kind = kind;
    x->rip = arch_hv_vcpu_rip(v->arch);
    /* Two sources: the owner's injections, and the guest's own controller. */
    x->flags = (vintr_any(v) || arch_hv_vcpu_irq_waiting(v->arch)) ? COSMO_VM_EXIT_F_IRQ_PENDING : 0;
}

static int vcpu_run_bounded(struct vcpu *v, struct cosmo_vm_exit *x, unsigned max_intr, bool preempt);

int vcpu_run(struct vcpu *v, struct cosmo_vm_exit *x)
{
    return vcpu_run_bounded(v, x, 0, false);
}

/* max_intr > 0: give up with -ETIMEDOUT after that many host-interrupt exits (tests). */
int vcpu_run_limited(struct vcpu *v, struct cosmo_vm_exit *x, unsigned max_intr)
{
    return vcpu_run_bounded(v, x, max_intr, false);
}

/* The owner's bounded run: ONE_TICK is "one host interrupt, then a
 * PREEMPTED exit" -- a turn, for an owner that has other vCPUs to run
 * and one thread to run them on. */
/*
 * The kicker's half of the handshake (kernel/hvkick.h): store the stop, then
 * look at whether the vCPU is inside a guest and on which CPU. Both are
 * SEQ_CST and the order matters -- see hvkick.h for why release/acquire is
 * not enough here.
 *
 * Sending the IPI is a latency optimisation and not the mechanism: the stop
 * is sticky, so a vCPU that is not in a guest takes it at its next entry or
 * at the top of its next run. That is what makes a stale `in_guest` read
 * harmless.
 *
 * IPI_RESCHEDULE rather than a vector of its own: what a kick needs from the
 * target CPU is a host interrupt, and that kind's documented effect -- come
 * back to the kernel and look at what changed -- is exactly it. The spurious
 * reschedule costs the vCPU thread a yield it would soon have taken anyway,
 * and a dedicated vector is a later optimisation rather than a correctness
 * matter.
 */
int vcpu_stop(struct vcpu *v)
{
    __atomic_store_n(&v->kick.stop, 1u, __ATOMIC_SEQ_CST);
    unsigned g = __atomic_load_n(&v->kick.in_guest, __ATOMIC_SEQ_CST);
    if (g & HV_IN_GUEST)
        ipi_send(g & ~HV_IN_GUEST, IPI_RESCHEDULE);
    return 0;
}

int vcpu_run_flags(struct vcpu *v, struct cosmo_vm_exit *x, unsigned flags)
{
    if (flags & COSMO_VCPU_RUN_ONE_TICK)
        return vcpu_run_bounded(v, x, 1, true);
    return vcpu_run_bounded(v, x, 0, false);
}

/* The bound: after `max_intr` host-interrupt exits, -ETIMEDOUT (a test's
 * failure) or, with `preempt`, a PREEMPTED exit (an owner's turn over). */
static int vcpu_run_bounded(struct vcpu *v, struct cosmo_vm_exit *x, unsigned max_intr, bool preempt)
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
    if (v->mmio_completion.pending) {
        /* The owner answered an MMIO read: the value it left in the exit
         * lands in the guest's register by the same rules a device's
         * answer does, and the load is stepped over. An owner that set
         * the registers itself instead cancelled this in vcpu_set_regs. */
        uint64_t value = x->kind == COSMO_VM_EXIT_MMIO ? x->mmio.value : 0;
        hv_mmio_complete_read(v, v->mmio_completion.size, v->mmio_completion.sse, v->mmio_completion.sf,
                              v->mmio_completion.reg, value);
        arch_hv_vcpu_advance_rip(v->arch, v->mmio_completion.insn_len);
        v->mmio_completion.pending = false;
    }
    memset(x, 0, sizeof(*x));
    int rc = 0;
    for (;;) {
        if (process_kill_pending()) {
            rc = -EINTR;
            break;
        }
        /*
         * A stop the owner set while this vCPU was between runs, or while it
         * was handling the last exit. Taken by exchange (kernel/hvkick.h) so
         * that one arriving inside this window is not swallowed.
         */
        if (hv_kick_take(&v->kick)) {
            fill_common(v, x, COSMO_VM_EXIT_STOPPED);
            break;
        }
        /* Level lines first: a device whose line is still up after the
         * guest acknowledged raises it again, here, before the offer. */
        vmdev_reassert(vm);
        int offered = vintr_take_lowest(v);
        arch_hv_vcpu_set_irq(v->arch, offered);
        struct hv_exit e;
        v->entries++;
        rc = arch_hv_vcpu_run(v->arch, &e);
        if (rc)
            break;
        v->exits++;
        /* What the guest took, which need not be what this entry
         * offered: a controller that holds an interrupt across entries
         * can deliver one offered several entries ago. */
        int delivered = arch_hv_vcpu_irq_delivered(v->arch);
        if (delivered >= 0)
            vintr_clear(v, delivered);

        /* The backend answered the access itself -- a guest talking to
         * its own interrupt controller. Nothing for the owner; run on. */
        if (e.kind == HV_EXIT_EMULATED)
            continue;
        if (e.kind == HV_EXIT_STOPPED) {
            /* The backend abandoned the entry, or left it at once, because
             * the owner's stop was set. Consume it: this run is over and the
             * next one is not pre-stopped. */
            (void)hv_kick_take(&v->kick);
            fill_common(v, x, COSMO_VM_EXIT_STOPPED);
            break;
        }
        if (e.kind == HV_EXIT_INTR) {
            /* A host interrupt is what a kick's IPI looks like from here.
             * Nothing to do about it *here*, though: `continue` returns to
             * the top of this loop, which takes a pending stop before the
             * next entry. An earlier version checked in this branch too and
             * a bug-proof showed the check was redundant -- removing it
             * changed nothing, because the top of the loop had already
             * covered every path that reaches an entry. */
            if (max_intr && ++intr >= max_intr) {
                if (preempt) {
                    fill_common(v, x, COSMO_VM_EXIT_PREEMPTED);
                    break;
                }
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
            /*
             * The AArch64 form of "the guest has nothing to do until an
             * interrupt": the owner decides whether to inject one and
             * run again, exactly as for HLT.
             *
             * But if the guest's own timer is armed, the guest *has*
             * something to wait for and knows exactly when. Returning at
             * once would hand the owner a WFI it can only answer by
             * spinning on re-entry, so the wait happens here: until the
             * deadline, or until something else becomes pending. The exit
             * is the same WFI it always was; it just arrives when there
             * is a reason to run again, and the timer is then late by
             * the owner's re-entry and nothing more.
             */
            uint64_t deadline;
            if (arch_hv_vcpu_timer_deadline(v->arch, &deadline)) {
                uint64_t hz = arch_clock_hz();
                uint64_t slice_ticks = hz / 1000;      /* one 1 ms slice, in counter ticks */
                for (;;) {
                    uint64_t now = arch_clock_read();
                    if (now >= deadline || vintr_any(v) || arch_hv_vcpu_irq_waiting(v->arch) ||
                        process_kill_pending())
                        break;
                    /*
                     * A slice at a time, so an injection from another
                     * thread is never made to wait for the guest's alarm.
                     * The tick-to-ns multiply is done only once the
                     * remaining interval is inside a slice -- a guest may
                     * arm its timer hours out, and `(deadline - now) * 1e9`
                     * overflows a 64-bit value at a few minutes, which
                     * would collapse a long deadline into a near-zero
                     * sleep and spin here holding run_lock.
                     */
                    uint64_t remaining = deadline - now;
                    if (remaining > slice_ticks) {
                        thread_sleep_ns(1000000ULL);
                    } else {
                        thread_sleep_ns(remaining * 1000000000ULL / hz);
                    }
                }
            }
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
            /* A device in the kernel may complete an access the hardware
             * described. A read's result goes into the guest's register
             * by width and sign, the instruction is stepped over, and the
             * run goes on: no exit. An access no device claims -- or one
             * with size 0, which nothing here can complete -- goes to the
             * owner, now carrying everything it needs to be the device. */
            uint64_t value = e.mmio.value;
            if (e.mmio.size && vmdev_mmio(vm, e.mmio.gpa, e.mmio.write, e.mmio.size, &value) == 0) {
                if (!e.mmio.write)
                    hv_mmio_complete_read(v, e.mmio.size, e.mmio.sse, e.mmio.sf, e.mmio.reg, value);
                arch_hv_vcpu_advance_rip(v->arch, e.mmio.insn_len);
                continue;
            }
            fill_common(v, x, COSMO_VM_EXIT_MMIO);
            x->mmio.gpa = e.mmio.gpa;
            x->mmio.write = e.mmio.write;
            x->mmio.size = e.mmio.size;
            x->mmio.reg = e.mmio.reg;
            x->mmio.sse = e.mmio.sse;
            x->mmio.sf = e.mmio.sf;
            x->mmio.value = e.mmio.value;
            /* An owner that handles this access -- a read it answers in
             * x->mmio.value, or a write it acts on -- must step over the
             * instruction on its next vcpu_run, because a data abort does
             * not advance the PC. The completion does it: for a read it
             * also writes the value into the register (by width and sign);
             * for a write there is no register (reg 31, a no-op), only the
             * advance. An owner that sets the registers itself instead
             * cancels this in vcpu_set_regs. Only when the access was
             * described (size != 0); a size-0 exit is the owner's entirely. */
            if (e.mmio.size) {
                v->mmio_completion.pending = true;
                v->mmio_completion.size = e.mmio.size;
                v->mmio_completion.reg = e.mmio.write ? 31u : e.mmio.reg;
                v->mmio_completion.sse = e.mmio.sse;
                v->mmio_completion.sf = e.mmio.sf;
                v->mmio_completion.insn_len = e.mmio.insn_len;
            }
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
