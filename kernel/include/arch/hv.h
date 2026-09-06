/*
 * arch/hv.h - Hardware virtualization backend interface
 * (docs/kernel-services/virtualization/design.md).
 *
 * The only surface the generic VM manager sees. A backend (x86-64: SVM
 * today, VMX next) implements these over its own control structures;
 * nothing VMX- or SVM-specific crosses this header (constitution
 * section 42). The register file is the UAPI struct cosmo_vcpu_regs so no
 * translation layer exists between user space, the manager and the
 * backend.
 */

#ifndef ARCH_HV_H
#define ARCH_HV_H

#include <kernel/types.h>
#include <uapi/cosmo/syscall.h>

struct arch_hv_vm;     /* nested page table + address-space tag; opaque */
struct arch_hv_vcpu;   /* control block + register spill; opaque */

/* Permissions a guest-physical mapping grants. */
#define HV_MAP_READ  (1u << 0)
#define HV_MAP_WRITE (1u << 1)
#define HV_MAP_EXEC  (1u << 2)
#define HV_MAP_RWX   (HV_MAP_READ | HV_MAP_WRITE | HV_MAP_EXEC)

struct hv_caps {
    bool present;
    const char *name;        /* "svm", "vmx", "none" */
    unsigned max_asids;
    bool nested_paging;
    bool real_mode_guest;    /* the architectural reset state can run (VMX: unrestricted guest) */
    bool map_prot;           /* arch_hv_vm_map honours prot; false means RWX only */
    bool large_pages;        /* 2 MiB leaves for aligned mappings */
    unsigned max_vcpus;      /* per VM, 0 = the manager's own limit */
};

enum hv_exit_kind {
    HV_EXIT_HLT,
    HV_EXIT_IO,          /* x86 only: there is no port space on AArch64 */
    HV_EXIT_MMIO,
    HV_EXIT_CPUID,       /* x86 only */
    HV_EXIT_MSR,         /* x86 only */
    HV_EXIT_HYPERCALL,
    HV_EXIT_SHUTDOWN,
    HV_EXIT_INTR,        /* a host interrupt arrived; nothing to do but run again */
    HV_EXIT_WFI,         /* AArch64: WFI/WFE, the HLT of this architecture */
    HV_EXIT_SYSREG,      /* AArch64: a trapped system-register access */
    HV_EXIT_FAIL,
};

struct hv_exit {
    enum hv_exit_kind kind;
    union {
        struct {
            uint16_t port;
            uint8_t size;        /* 1, 2, 4 */
            uint8_t write;       /* OUT */
            uint8_t string;      /* INS/OUTS */
            uint8_t rep;
            uint64_t next_rip;   /* the instruction after the I/O */
        } io;
        struct {
            uint64_t gpa;
            bool write;
        } mmio;
        struct {
            uint32_t index;
            bool write;
        } msr;
        struct {
            uint32_t iss;      /* ESR_EL2.ISS: the register encoding and direction */
            uint8_t reg;       /* the guest GPR the value comes from or goes to */
            bool write;
        } sysreg;
        struct {
            uint64_t code, info1, info2;
        } fail;
    };
};

/* Detect and prepare the backend; fills caps (present = false with a
 * clean -ENOTSUP when the CPU lacks it). Called once at boot. */
int arch_hv_probe(struct hv_caps *out);

int arch_hv_vm_create(struct arch_hv_vm **out);
void arch_hv_vm_destroy(struct arch_hv_vm *vm);
/* Map guest-physical [gpa, gpa+len) to host-physical pages with `prot`
 * (HV_MAP_*, nonzero). 4 KiB granular; a backend reporting large_pages
 * uses 2 MiB leaves where everything is aligned. -EINVAL for prot 0 or a
 * prot a backend without `map_prot` cannot express. */
int arch_hv_vm_map(struct arch_hv_vm *vm, uint64_t gpa, paddr_t hpa, size_t len, unsigned prot);
int arch_hv_vm_unmap(struct arch_hv_vm *vm, uint64_t gpa, size_t len);
/* Host-physical page behind a guest-physical address, or false. */
bool arch_hv_vm_query(struct arch_hv_vm *vm, uint64_t gpa, paddr_t *hpa);

/* A vCPU starts at the architectural reset state (real mode, rip 0). */
int arch_hv_vcpu_create(struct arch_hv_vm *vm, struct arch_hv_vcpu **out);
void arch_hv_vcpu_destroy(struct arch_hv_vcpu *v);
void arch_hv_vcpu_get_state(struct arch_hv_vcpu *v, struct cosmo_vcpu_regs *out);
/* -EINVAL for a combination the hardware would refuse. */
int arch_hv_vcpu_set_state(struct arch_hv_vcpu *v, const struct cosmo_vcpu_regs *in);

/* Run until an exit. Interrupts must be enabled on entry; they are
 * enabled again on return. Never sleeps. */
int arch_hv_vcpu_run(struct arch_hv_vcpu *v, struct hv_exit *out);

/* VirtualInterrupt: offer one vector (-1: none) for delivery when the
 * guest is interruptible; irq_taken tells, after a run, whether it went. */
void arch_hv_vcpu_set_irq(struct arch_hv_vcpu *v, int vector);
bool arch_hv_vcpu_irq_taken(struct arch_hv_vcpu *v);
/* Queue an exception for the next entry (vector < 32). */
void arch_hv_vcpu_inject_exception(struct arch_hv_vcpu *v, uint8_t vector, bool has_error, uint32_t error);

void arch_hv_vcpu_advance_rip(struct arch_hv_vcpu *v, unsigned bytes);
void arch_hv_vcpu_set_rip(struct arch_hv_vcpu *v, uint64_t rip);
uint64_t arch_hv_vcpu_rip(struct arch_hv_vcpu *v);

/* Host values the CPUID/MSR emulation filters (generic code never executes CPUID itself). */
void arch_hv_host_cpuid(uint32_t leaf, uint32_t subleaf, uint32_t *eax, uint32_t *ebx, uint32_t *ecx, uint32_t *edx);
uint64_t arch_hv_host_tsc(void);
/* The extended-state components the host enables for guests (x86-64: the
 * kernel's XCR0; 0 when the CPU has no XSAVE). A guest may enable a
 * subset; the CPUID emulation advertises no more than this. */
uint64_t arch_hv_host_xstate(void);
/* Whether the guest has enabled the extended-state instructions for
 * itself (x86-64: its CR4.OSXSAVE), mirrored into CPUID.1:ECX.OSXSAVE. */
bool arch_hv_vcpu_xstate_enabled(struct arch_hv_vcpu *v);
/* Deposit a value into the low `size` bytes of rax (IN completion). */
void arch_hv_vcpu_write_rax(struct arch_hv_vcpu *v, uint64_t value, unsigned size);
/* GPR indices in the x86 encoding order. */
#define HV_GPR_RAX 0u
#define HV_GPR_RCX 1u
#define HV_GPR_RDX 2u
#define HV_GPR_RBX 3u
#define HV_GPR_RSP 4u
#define HV_GPR_RBP 5u
#define HV_GPR_RSI 6u
#define HV_GPR_RDI 7u
uint64_t arch_hv_vcpu_read_gpr(struct arch_hv_vcpu *v, unsigned index);
void arch_hv_vcpu_write_gpr(struct arch_hv_vcpu *v, unsigned index, uint64_t value);

/* Guest EFER as the guest sees it (the control block may differ). */
uint64_t arch_hv_vcpu_guest_efer(struct arch_hv_vcpu *v);
int arch_hv_vcpu_set_guest_efer(struct arch_hv_vcpu *v, uint64_t efer);
/* A backend-owned MSR (segment bases, SYSCALL MSRs, PAT): 0 handled, -ENOENT not one of them. */
int arch_hv_vcpu_msr(struct arch_hv_vcpu *v, uint32_t index, bool write, uint64_t *value);

#endif /* ARCH_HV_H */
