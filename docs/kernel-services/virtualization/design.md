# Virtualization: design

## Data structures

### VMState: `struct cosmo_vcpu_regs` (`kernel/include/uapi/cosmo/syscall.h`)

The generic register file. It is the UAPI, the kernel API and the arch
interface's state type, so no translation layer exists between them; the
backend copies fields to and from its own control block.

```c
struct cosmo_vcpu_seg { uint16_t selector; uint16_t attrib; uint32_t limit; uint64_t base; };   /* 16 bytes */
struct cosmo_vcpu_regs {
    uint64_t rax, rbx, rcx, rdx, rsi, rdi, rbp, rsp, r8, r9, r10, r11, r12, r13, r14, r15;   /* 128 */
    uint64_t rip, rflags;                                                                    /*  16 */
    struct cosmo_vcpu_seg cs, ds, es, fs, gs, ss, ldtr, tr;                                  /* 128 */
    struct cosmo_vcpu_seg gdtr, idtr;          /* selector/attrib unused */                  /*  32 */
    uint64_t cr0, cr2, cr3, cr4, cr8;                                                        /*  40 */
    uint64_t efer;                                                                           /*   8 */
    uint64_t dr6, dr7;                                                                       /*  16 */
    uint64_t pending_irq;    /* read: lowest pending vector or -1; write: ignored */          /*   8 */
    uint64_t reserved[9];                                                                    /*  72 */
};                                                                                           /* 448 */
```

`attrib` is the SVM/VMX-neutral "access rights" word: type (4 bits), S,
DPL (2), P, then AVL, L, D/B, G in bits 12–15, i.e. the descriptor's
byte 5 and the high nibble of byte 6 packed as SVM does; a VMX backend
repacks it. A freshly created vCPU is at the reset state: real mode, `cs`
selector 0 with base 0, `rip` 0, `rflags` 2, `cr0` 0x60000010 (ET, NW,
CD, as after a reset), `efer` 0, all segments base 0 limit 0xFFFF,
`dr6` 0xFFFF0FF0, `dr7` 0x400, `pending_irq` ~0. The owner overwrites
what it needs (typically `rip`, or `cr0/cr3/cr4/efer` plus segments for a
protected- or long-mode guest).

### VMExit: `struct cosmo_vm_exit`

```c
struct cosmo_vm_exit {
    uint32_t kind;          /* COSMO_VM_EXIT_* */
    uint32_t flags;         /* COSMO_VM_EXIT_F_IRQ_PENDING: a vector is still undelivered */
    uint64_t rip;           /* guest rip at the exit (after an IN/OUT it is the next instruction) */
    union {
        struct { uint16_t port; uint8_t size; uint8_t write; uint8_t string; uint8_t rep; uint16_t pad; uint32_t value; } io;
        struct { uint64_t gpa; uint32_t write; uint32_t pad; } mmio;
        struct { uint64_t nr, a0, a1, a2, a3; } hypercall;
        struct { uint32_t code; uint32_t pad; uint64_t info1, info2; } fail;
        uint64_t raw[6];
    };
};                          /* 64 bytes */

#define COSMO_VM_EXIT_HLT       1   /* the guest halted with interrupts enabled or not; inject and run again */
#define COSMO_VM_EXIT_IO        2   /* port I/O no backend claimed; for reads, complete on the next run */
#define COSMO_VM_EXIT_MMIO      3   /* access to guest-physical memory with no region behind it */
#define COSMO_VM_EXIT_HYPERCALL 4   /* VMMCALL (SVM) / VMCALL (VMX): nr in rax, args rbx rcx rdx rsi */
#define COSMO_VM_EXIT_SHUTDOWN  5   /* triple fault: the vCPU is dead; state readable, run refused */
#define COSMO_VM_EXIT_FAIL      6   /* the hardware refused the state (VMEXIT_INVALID) or an unknown exit */
```

CPUID, MSR accesses, interrupts arriving on the host, interrupt windows
and nested-paging fills never reach the caller; they are resolved in the
kernel and the guest continues.

`vcpu_set_regs` cancels a pending IN completion (the owner rewrote
`rax` itself). `vcpu_run` is also how a port **read** completes: the kernel returns
`IO` with `write == 0`; the caller fills `io.value` and calls `vcpu_run`
again with the same structure; the kernel sees the vCPU is waiting for an
IN completion, deposits `value` into the low `size` bytes of `rax`
(zero-extending, as IN does), and enters the guest. A run request that
arrives while an IN completion is due and carries a different `kind` is
treated as value 0xFFFFFFFF (open bus). For an OUT, `io.value` carries
the bytes written and RIP has already been advanced. String and REP
forms are reported with `string`/`rep` set; the kernel does not iterate
them (they exit once per iteration with the current `rcx`/`rsi`/`rdi`, and
the caller is expected to complete each; in practice stage-1 guests use
plain IN/OUT).

### The kernel objects (`kernel/include/kernel/hv.h`)

```c
struct vm {
    struct kobject obj;                 /* the VM handle's object */
    unsigned id;                        /* small integer, unique for the boot */
    struct mutex lock;                  /* regions (add only), devices, vcpus[], mem_bytes, started */
    struct arch_hv_vm *arch;            /* the nested page table and the ASID */
    struct list_node regions;           /* struct guest_region, sorted by gpa */
    unsigned nr_regions;
    uint64_t mem_bytes;                 /* sum of region lengths (limit HV_VM_MEM_MAX) */
    struct vcpu *vcpus[HV_VCPUS_MAX];   /* weak pointers; a vcpu holds a reference to its vm */
    unsigned nr_vcpus;
    bool started;                       /* a vCPU has run: the device table is frozen */
    struct list_node devices;           /* struct vm_device */
    struct { spinlock_t lock; uint8_t buf[4096]; unsigned head, tail; uint64_t dropped; } console;
    struct vm_device debug_console;     /* the built-in port 0xE9 backend */
    uint32_t owner_uid;
    struct list_node link;              /* the manager's list */
};

struct guest_region {
    struct list_node link;
    uint64_t gpa, len;                  /* page aligned */
    struct page **pages;                /* len / PAGE_SIZE entries, each an order-0 page */
};

struct vcpu {
    struct kobject obj;
    struct vm *vm;                      /* referenced */
    unsigned index;
    struct mutex run_lock;              /* one runner at a time; also serialises regs get/set */
    struct arch_hv_vcpu *arch;          /* the VMCB and the backend's GPR block */
    spinlock_t irq_lock;                /* pending[]: inject comes from other threads */
    uint64_t pending[4];                /* VirtualInterrupt: 256-bit pending vector set */
    int offered;                        /* vector offered for the current entry, -1 none */
    bool in_completion;                 /* an IN is waiting for a value */
    uint8_t in_size;
    bool dead;                          /* after SHUTDOWN or FAIL */
    uint64_t exits, entries;            /* statistics for sysctl and tests */
    unsigned msr_gp;                    /* #GP injected for unmodelled MSRs */
};

struct vm_device {
    struct list_node link;
    const char *name;
    uint16_t pio_base, pio_count;       /* 0 count: no ports */
    uint64_t mmio_base, mmio_len;       /* 0 len: no memory range */
    /* return 0 handled (value in/out), -ENODEV to pass the exit to the caller */
    int (*pio)(struct vm_device *d, uint16_t port, bool write, unsigned size, uint32_t *value);
    void (*mmio)(struct vm_device *d, uint64_t gpa, bool write);  /* stage 1: notification only */
    void *priv;
};
```

Handles to a VM or a vCPU are installed with `HANDLE_RIGHT_READ |
HANDLE_RIGHT_WRITE`; all seven system calls require WRITE on the object
they act on, `vm_mem_rw` with `write == 0` and `vcpu_regs` with `set == 0`
require READ. The VM object is a `kobject_io_type`: `read` drains the
console ring (returns 0 when it is empty; it never blocks), `write`
returns `-ENOTSUP`, `stat` reports `COSMO_DT_CHR` with the VM id as the
inode. The vCPU object is a plain kobject.

### The arch interface (`kernel/include/arch/hv.h`)

```c
struct hv_caps { bool present; const char *name; unsigned max_asids; bool nested_paging; };

enum hv_exit_kind { HV_EXIT_HLT, HV_EXIT_IO, HV_EXIT_MMIO, HV_EXIT_CPUID, HV_EXIT_MSR, HV_EXIT_HYPERCALL,
                    HV_EXIT_SHUTDOWN, HV_EXIT_INTR, HV_EXIT_FAIL };
struct hv_exit {
    enum hv_exit_kind kind;
    union {
        struct { uint16_t port; uint8_t size, write, string, rep; uint64_t next_rip; } io;
        struct { uint64_t gpa; bool write; } mmio;
        struct { uint32_t index; bool write; } msr;
        struct { uint64_t code, info1, info2; } fail;
    };
};

int  arch_hv_probe(struct hv_caps *out);                 /* enables the extension on every online CPU */
int  arch_hv_vm_create(struct arch_hv_vm **out);
void arch_hv_vm_destroy(struct arch_hv_vm *vm);
int  arch_hv_vm_map(struct arch_hv_vm *vm, uint64_t gpa, paddr_t hpa, size_t len);   /* RWX, 4 KiB granular */
int  arch_hv_vm_unmap(struct arch_hv_vm *vm, uint64_t gpa, size_t len);
int  arch_hv_vcpu_create(struct arch_hv_vm *vm, struct arch_hv_vcpu **out);           /* reset state */
void arch_hv_vcpu_destroy(struct arch_hv_vcpu *v);
void arch_hv_vcpu_get_state(struct arch_hv_vcpu *v, struct cosmo_vcpu_regs *out);
int  arch_hv_vcpu_set_state(struct arch_hv_vcpu *v, const struct cosmo_vcpu_regs *in);   /* -EINVAL if unusable */
int  arch_hv_vcpu_run(struct arch_hv_vcpu *v, struct hv_exit *out);   /* IRQs enabled on entry and return */
void arch_hv_vcpu_set_irq(struct arch_hv_vcpu *v, int vector);       /* -1: none pending */
bool arch_hv_vcpu_irq_taken(struct arch_hv_vcpu *v);                 /* the offered vector was delivered */
void arch_hv_vcpu_inject_exception(struct arch_hv_vcpu *v, uint8_t vector, bool has_error, uint32_t error);
void arch_hv_vcpu_advance_rip(struct arch_hv_vcpu *v, unsigned bytes);   /* both end an interrupt shadow */
void arch_hv_vcpu_set_rip(struct arch_hv_vcpu *v, uint64_t rip);
uint64_t arch_hv_vcpu_rip(struct arch_hv_vcpu *v);
void arch_hv_vcpu_write_rax(struct arch_hv_vcpu *v, uint64_t value, unsigned size);
uint64_t arch_hv_vcpu_read_gpr(struct arch_hv_vcpu *v, unsigned index);    /* HV_GPR_*: x86 encoding order */
void arch_hv_vcpu_write_gpr(struct arch_hv_vcpu *v, unsigned index, uint64_t value);
uint64_t arch_hv_vcpu_guest_efer(struct arch_hv_vcpu *v);
int  arch_hv_vcpu_set_guest_efer(struct arch_hv_vcpu *v, uint64_t efer);  /* -EINVAL: reserved bits or SVME */
int  arch_hv_vcpu_msr(struct arch_hv_vcpu *v, uint32_t index, bool write, uint64_t *value);  /* backend-owned MSRs */
void arch_hv_host_cpuid(uint32_t leaf, uint32_t sub, uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d);
uint64_t arch_hv_host_tsc(void);
```

`struct arch_hv_vm` and `struct arch_hv_vcpu` are opaque to generic
code, which also never executes CPUID or RDTSC itself: the two `host`
functions keep even that behind the interface (invariant V1).

### The SVM backend (`kernel/arch/x86_64/svm.c`, `svm_npt.c`, `svm_run.S`)

```c
struct arch_hv_vm { paddr_t ncr3; uint32_t asid; };
struct arch_hv_vcpu {
    struct vmcb *vmcb;  paddr_t vmcb_pa;           /* one 4 KiB page: control area + save area */
    struct svm_gprs gprs;                          /* rbx rcx rdx rsi rdi rbp r8-r15; rax/rsp/rip live in the VMCB */
    struct arch_hv_vm *vm;
    uint64_t guest_efer;                           /* what the guest believes (VMCB EFER always has SVME) */
    int offered;                                   /* vector in V_INTR_VECTOR, -1 none */
    unsigned unknown_exits;                        /* the first unknown exit code is logged */
};
```

The VMCB layout is written as a struct with explicit offsets checked by
`STATIC_ASSERT` (control area: intercept vectors at 0x00–0x14, IOPM base
0x40, MSRPM base 0x48, TSC offset 0x50, ASID 0x58, TLB control 0x5C,
V_IRQ/V_INTR_PRIO/V_IGN_TPR/V_INTR_VECTOR at 0x60–0x64, interrupt
shadow 0x68, EXITCODE 0x70, EXITINFO1 0x78, EXITINFO2 0x80, EXITINTINFO
0x88, NP_ENABLE 0x90, EVENTINJ 0xA8, N_CR3 0xB0; save area at 0x400 with
segments ES CS SS DS FS GS GDTR LDTR IDTR TR, then CPL 0x4CB, EFER 0x4D0,
CR4 0x548, CR3 0x550, CR0 0x558, DR7 0x560, DR6 0x568, RFLAGS 0x570, RIP
0x578, RSP 0x5D8, RAX 0x5F8, STAR/LSTAR/CSTAR/SFMASK/KernelGSBase/
SYSENTER_CS/ESP/EIP 0x600–0x638, CR2 0x640, G_PAT 0x668).

Per CPU: a 4 KiB **host save area** whose physical address is written to
`MSR_VM_HSAVE_PA` (0xC0010117) when the extension is enabled on that CPU,
and a 4 KiB **host VMCB** used only as the VMSAVE/VMLOAD target for the
host's FS/GS/TR/LDTR bases, KernelGSBase and the SYSCALL MSRs.

Shared, read-only: the **I/O permission map** (12 KiB, every bit set:
every port intercepts) and the **MSR permission map** (8 KiB, every bit
set: every MSR read and write intercepts).

**Intercepts** in every VMCB: INTR, NMI, SMI, INIT, HLT, CPUID, INVD,
IOIO, MSR, SHUTDOWN, VMRUN (architecturally required), VMMCALL, VMLOAD,
VMSAVE, STGI, CLGI, SKINIT, INVLPGA, MONITOR, MWAIT (armed or not).
XSETBV is intercepted: XCR0 is not part of the VMCB, so the guest's
value is kept in the vCPU, validated with the hardware's rules against
the host's XCR0 (`#UD` without CR4.OSXSAVE, `#GP(0)` for CPL > 0, ECX != 0
or an unacceptable value) and installed around VMRUN; the guest's
x87/SSE/AVX registers live in a per-vCPU area swapped with the owner
thread's around every entry (`docs/kernel/arch/design.md`, "FPU and SIMD
state"). RDTSC and RDTSCP are not intercepted (the guest reads the host
TSC with a zero offset);
exceptions are not intercepted (the guest handles its own faults); nested
paging turns a guest access to unmapped guest-physical memory into an NPF
exit. NP_ENABLE = 1, N_CR3 = the VM's table, ASID = the VM's,
TLB_CONTROL = flush-all on every VMRUN (correct and simple; the
flush-by-ASID optimisation is deferred), V_IGN_TPR = 1 with priority 15
(no APIC to model), **V_INTR_MASKING = 1** (the host's RFLAGS.IF as
recorded by VMRUN, not the guest's, decides whether a physical interrupt
is taken, so a guest with interrupts disabled still exits on the host
tick), G_PAT = the default PAT.

**The entry sequence** (`svm_run.S`, `svm_run(vmcb_pa, host_vmcb_pa,
gprs)`): push callee-saved registers; `clgi`; `sti`; `vmsave host_vmcb`;
load the guest GPRs from `gprs`; `vmload vmcb`; `vmrun vmcb`; `vmsave
vmcb`; spill guest GPRs to `gprs`; `vmload host_vmcb`; `cli`; `stgi`;
`cld`; pop and return. The C caller disables local interrupts around the
whole sequence (`arch_irq_save`) and restores them afterwards. Inside,
`sti` under a cleared GIF does not deliver anything; it makes VMRUN
record host IF = 1, which with V_INTR_MASKING is what lets a physical
interrupt exit the guest (`VMEXIT_INTR`) regardless of the guest's own
IF. The interrupt is then held by GIF until `stgi` and by IF until the
caller's restore, and is taken on the host as usual, where it may set
`need_resched` and preempt the running thread on the way back. `clgi`
also masks NMIs; the sequence is short and does not sleep.

**Exit decoding** (`arch_hv_vcpu_run`): `VMEXIT_INTR`, `NMI`, `SMI`,
`INIT` → `HV_EXIT_INTR`; `HLT` → advance RIP 1, `HV_EXIT_HLT`; `CPUID` →
`HV_EXIT_CPUID` (generic code emulates, then advances RIP by 2 via
`advance_rip`); `INVD` → advance RIP 2 and continue (a no-op: the guest's
caches are coherent with the host's); `IOIO` →
decode EXITINFO1 (bit 0 direction IN, bits 4–6 size, bit 2 string, bit 3
REP, bits 16–31 port) and take EXITINFO2 as the next RIP, which SVM
provides for I/O regardless of NRIP support (the harness's TCG has no
NRIP or decode assists, so nothing else relies on them); `MSR` →
EXITINFO1 bit 0 write, `rcx` index; `VMMCALL` → advance RIP 3,
`HV_EXIT_HYPERCALL`; `NPF` → EXITINFO2 is the faulting guest-physical
address, EXITINFO1 bit 1 the write flag, `HV_EXIT_MMIO`; `SHUTDOWN` →
`HV_EXIT_SHUTDOWN`; `VMEXIT_INVALID` (-1) and anything else →
`HV_EXIT_FAIL` with the code and both infos (the first unknown code per
vCPU is logged at warning level). The intercepted SVM instructions
(VMRUN, VMLOAD, VMSAVE, STGI, CLGI, SKINIT, INVLPGA) and MONITOR/MWAIT
inject `#UD` into the guest and continue, so a guest cannot tell it is
not the outermost hypervisor and cannot use the instructions. An event
that was being delivered when the exit happened (EXITINTINFO valid) is
copied back to EVENTINJ so it is redelivered on the next entry.

**Skipping an instruction** (`advance_rip`, `set_rip`, used by every
exit that completes an instruction on the guest's behalf) also clears
the VMCB's interrupt-shadow field. An intercept is taken *before* the
instruction executes, so an STI shadow that covers a `hlt` is recorded
in the VMCB at the HLT exit; if the shadow were left in place, every
re-entry would inhibit the first instruction (the same `hlt`, now
skipped by the hypervisor, or the next one), the exit would record the
shadow again, and a pending virtual interrupt would never be delivered.
The `hv-guest-irq` self-test found this and pins it (invariant V9).

**Nested page tables** (`svm_npt.c`): the 4-level long-mode format.
Every present entry has P, RW and **US** set (nested walks fault without
the User bit; this is the classic NPT mistake and is a host-test case);
leaf entries carry the host physical page; no large pages; tables are
order-0 pages from the PMM, freed recursively on destroy. `npt_map`,
`npt_unmap`, `npt_query` are pure functions over a page allocator so the
host test can exercise them with the harness arena.

**Register file** conversion: `get_state` reads the VMCB save area and the
GPR block (`cr8` is V_TPR, `efer` the guest's view); `set_state` writes
them, forcing `EFER.SVME` on in the VMCB (VMRUN fails without it) while
remembering the guest's own EFER with `LMA` recomputed from `LME` and
`PG`, and rejects with `-EINVAL` before the hardware sees them: `cr0`
bits above 31, PG without PE, NW without CD, EFER reserved bits, **EFER
with SVME** (the owner may not make the guest a hypervisor either), LME
with PG without PAE, LME with PG and CS.L with CS.D, `dr6`/`dr7` bits
above 31. `rflags` gets bit 1 set and bits 3, 5 and 15 cleared; the CPL
field follows SS.DPL in protected mode.

**Virtual interrupts**: `set_irq(vector)` writes V_IRQ = 1,
V_INTR_VECTOR = vector, V_INTR_PRIO = 0xF; the CPU delivers it when the
guest's RFLAGS.IF is set and no interrupt shadow is active, clearing
V_IRQ. `irq_taken` reads V_IRQ back after the run. `inject_exception`
writes EVENTINJ (type 3, vector, error-code valid bit, error code) for
delivery at the next entry.

### The VM manager (`vmm.c`)

After a successful probe `hv_init` runs a **self-check**: a VM with one
page at guest-physical 0x80000000 holding `hlt`, a flat 32-bit
paging-off vCPU with `idtr.limit` 0 and `rsp` in the same page, run for
at most 50 host-interrupt exits. Nested paging that confines the guest
yields an `HLT` exit at 0x80000001; a nested walk that is bypassed while
guest paging is off (QEMU/TCG before 9.2) fetches from the host's PCI
hole, faults with an empty IDT and shuts down, and the backend is
disabled (`present = false`, name `none`). The address was chosen to be
outside host RAM in the harness so the check itself never touches host
memory when the bug is present. `hv_init` runs after interrupts are
enabled, since the self-check enters a guest.

`hv_init()` runs from `kernel_main` after `vfs_init` and the ramfs boot
population: it calls `arch_hv_probe` (CPUID for SVM and nested paging,
`VM_CR.SVMDIS`, the permission maps; SVM itself is enabled per CPU on
first use from `arch_hv_vcpu_run` behind a per-CPU flag, so no IPI is
needed and CPUs that never run a guest cost nothing) and creates the
`/dev/vmm` character node (`ramfs_mkchr("/dev/vmm", 0600, ...)`, the
first device node in the namespace; `read` returns
`<backend>[ npt] asids=<n> vms=<m>\n`). The sysctl names `hv.backend`,
`hv.vms`, `hv.vcpus` and `hv.exits` are answered by `hv_sysctl()` from
`kernel/syscall/native.c`. When no backend is present the node still
exists and reads `none ...`; `vm_create` fails with `-ENOTSUP`.

Limits: `HV_VMS_MAX` 8, `HV_VCPUS_MAX` 4 per VM, `vm->mem_limit` per
VM (the creator's `COSMO_RLIMIT_VMEM`, `HV_VM_MEM_MAX` 64 MiB by
default; `vm_create(owner_uid, mem_limit, &vm)`), `HV_REGIONS_MAX` 16
per VM. Exceeding one returns `-ENOSPC` (VMs, vCPUs, regions) or
`-ENOMEM` (memory).

### The run loop (`vcpu.c`)

```text
vcpu_run(vcpu, uexit):
    copy in the caller's struct (for an IN completion)
    lock run_lock; refuse if dead (-EIO) or if the vm is being destroyed
    if in_completion: write_rax(value, in_size); in_completion = false
    loop:
        if kill pending for the current thread: unlock, return -EINTR (the caller's process is dying)
        (tests: vcpu_run_limited(max_intr) returns -ETIMEDOUT after that many INTR exits)
        offer the lowest pending vector (or -1) to the backend
        rc = arch_hv_vcpu_run(arch, &x)         (IRQs enabled before and after)
        if the offered vector was taken: clear it from pending
        switch x.kind:
            INTR:      continue                   (the host interrupt has been handled by now)
            CPUID:     emulate (below); advance_rip(2); continue
            MSR:       emulate (below); advance_rip(2) or inject #GP; continue
            IO:        set_rip(next_rip) (also ends an interrupt shadow)
                       if not a string form and a device claims the port: call it; for IN write_rax; continue
                       else fill uexit IO (value = rax bytes for OUT); if IN: in_completion = true; break
            HLT:       fill uexit; break
            MMIO:      if a device claims the range: notify it; then fill uexit; break
            HYPERCALL: fill uexit from rax rbx rcx rdx rsi; break
            SHUTDOWN, FAIL: dead = true; fill uexit; break
    flags |= IRQ_PENDING if any vector remains
    unlock; copy out; return 0
```

A vCPU may be run by any thread of the owning process, one at a time
(`run_lock`, which is also taken by `vcpu_regs`, so state is never read
mid-run). Different vCPUs of one VM may run concurrently on different
host CPUs; the VM lock is not held while a guest runs. Guest memory
regions are never removed while the VM lives, so the run loop reads the
region list without the lock. `process_kill` of the owner interrupts a
run at the next exit (the timer tick guarantees one within a slice
because INTR is intercepted).

**CPUID emulation**: the host's values for the requested leaf/subleaf
(through `arch_hv_host_cpuid`), then: leaf 0 caps the maximum basic leaf
at 0xD; leaf 1 ECX clears MONITOR (bit 3), VMX (5), SMX (6) and sets the
hypervisor bit (31); leaf 1 EBX reports the vCPU index as the initial
APIC id; leaf 7 clears nothing; leaf 0x80000001 ECX clears SVM (bit 2);
leaf 0x8000000A (SVM features) returns zeros; leaf 0x40000000 returns
0x40000000 and the signature `CosmoOSCosmo`; leaves 0x40000001–
0x400000FF return zeros; leaf 6, 0xB, 0xD subleaf > 1 and any
unsupported leaf return zeros. Everything else passes through
(the guest sees the host's vendor and feature set; there is no per-VM
CPU model in stage 1).

**MSR emulation**: `EFER` (0xC0000080) read returns the guest's view,
write stores it (SVME or reserved bits: #GP) and updates the VMCB with
SVME forced on; `STAR`, `LSTAR`, `CSTAR`, `SFMASK`, `FS_BASE`, `GS_BASE`,
`KERNEL_GS_BASE`, `SYSENTER_CS/ESP/EIP`, `PAT` live in the VMCB save area
(`arch_hv_vcpu_msr`); `TSC` (0x10) reads the host TSC and refuses writes;
`APIC_BASE` (0x1B) reads 0xFEE00800 plus the BSP bit for vCPU 0, writes
are ignored; `MISC_ENABLE` (0x1A0), `MTRRcap` (0xFE), `MTRRdefType`
(0x2FF) and the microcode revision (0x8B) read 0 and ignore writes. Any
other MSR injects `#GP(0)` without advancing RIP, like a CPU that lacks
it; the vCPU's `msr_gp` counter grows and the first eight are logged at
debug level.

### VirtualInterrupt (`vintr.c`)

`vcpu_inject(vcpu, vector)` sets bit `vector` in `pending` under the
vCPU's own spinlock (it is called from another thread than the runner,
possibly while the guest is running; the runner picks it up at the next
exit, and the timer tick bounds that wait). Before each entry the runner
offers the lowest pending vector (fixed priority by number, matching the
8259/LAPIC convention that lower vectors are not higher priority — the
choice is documented, not architectural; a virtual LAPIC in stage 2
brings real priorities). Vectors 0–31 are refused (`-EINVAL`): those are
exceptions, which a hypervisor injects as events, and stage 1 exposes no
API for that.

### Device backends (`vmdev.c`)

`vm_device_register(vm, dev)` appends to the VM's list (under the VM
lock, only before the first vCPU runs; afterwards `-EBUSY`, so the run
loop can walk the list unlocked). Ranges may not overlap an existing
device's. The built-in **debug console** claims port 0xE9: a write
appends the low byte(s) to the ring (dropping the oldest when full), a
read returns 0xE9 (the Bochs convention that lets a guest detect the
port). The owner drains the ring with `read` on the VM handle; the VM's
`stat` reports the number of buffered bytes as `size`.

### Guest memory (`guestmem.c`)

`vm_mem_add(vm, gpa, len)`: page-aligned, non-zero, within the 4 GiB
guest-physical window of stage 1 (`gpa + len <= 1 << 32`), non-overlapping,
under the per-VM byte limit; allocates `len / PAGE_SIZE` order-0 pages
with `PMM_ZERO`, records them, and maps each with `arch_hv_vm_map`. A
failure midway unmaps and frees what was done and leaves the VM as it was.
`vm_mem_read/write(vm, gpa, kbuf, len)` copy through the direct map page
by page after locating the region; a range that crosses out of every
region returns `-EFAULT` before any byte is copied. Regions live until
the VM is released; `vm_release` unmaps everything, destroys the arch
context, and returns the pages (`pmm_free_page` each).

## The system calls (`kernel/syscall/native.c`)

| nr | name | arguments | result |
|----|------|-----------|--------|
| 43 | `vm_create` | `(int vmm_h)` — a handle to `/dev/vmm` open for writing | VM handle |
| 44 | `vm_mem` | `(int vm, uint64_t gpa, uint64_t len)` | 0 |
| 45 | `vm_mem_rw` | `(int vm, uint64_t gpa, void *buf, size_t len, int write)` | bytes copied |
| 46 | `vcpu_create` | `(int vm, unsigned index)` | vCPU handle |
| 47 | `vcpu_regs` | `(int vcpu, struct cosmo_vcpu_regs *regs, int set)` | 0 |
| 48 | `vcpu_run` | `(int vcpu, struct cosmo_vm_exit *exit)` | 0; `exit` filled |
| 49 | `vcpu_irq` | `(int vcpu, unsigned vector)` | 0 |

Errors: `-ENOTSUP` no backend; `-EBADF` bad handle or wrong object type
or rights; `-EPERM` `vm_create` with a handle that is not `/dev/vmm` open
for writing; `-EINVAL` alignment, overlap, vector < 32, index ≥ max,
unusable register state; `-ENOSPC`/`-ENOMEM` limits; `-EEXIST` vCPU index
in use; `-EIO` running a dead vCPU; `-EINTR` the caller is being killed;
`-EFAULT` guest-physical range outside every region, or a bad user
pointer. `vm_mem_rw` copies through a 4 KiB kernel bounce buffer per
chunk so no user pointer is dereferenced while the VM lock is held; a
length above 64 MiB is `-EINVAL` before anything is looked up.

The Linux personality does not expose these (Linux has `/dev/kvm`,
whose ioctl model is a later compat stage).

## Ownership and lifetime

- A VM is referenced by: each handle to it, each of its vCPUs, and the
  manager's list (weak: the list entry is removed in `vm_release` under
  the manager lock). `vm_release` (last reference) runs when the last
  handle **and** the last vCPU are gone, so guest memory can never
  disappear under a running vCPU.
- A vCPU is referenced by its handles; `vcpu_release` destroys the arch
  context, clears the VM's slot under the VM lock, and drops the VM
  reference. A vCPU that is running cannot be released: the running
  thread holds a handle (the syscall's `handle_lookup` reference) for the
  duration.
- Guest pages belong to the VM (their `struct page` refcount is the
  allocation's); no other subsystem maps them. The nested page table
  pages belong to the arch context.
- Per-CPU SVM state (host save area, host VMCB) is allocated at first
  use on that CPU and never freed.
- Process exit closes handles like any other; a process that dies while
  a vCPU runs is interrupted at the next exit (`-EINTR`) and then closes.

## Concurrency

- Manager lock (mutex): the VM list and id allocation.
- VM lock (mutex): regions (add only), devices (register only before
  running), vcpus[] slots, the console ring's producer and consumer,
  `mem_bytes`. Never held while a guest runs; never held while taking a
  vCPU run lock (order: run_lock → vm lock, taken by the run loop only
  for the console ring producer through a spinlock-protected fast path:
  the ring has its own spinlock so the run loop never takes the mutex).
- vCPU run lock (mutex): run and regs. `pending` has its own spinlock
  (inject is a cross-thread operation).
- Backend: `arch_hv_vcpu_run` disables local interrupts across the entry
  sequence only; enabling SVM on a CPU (`EFER.SVME`, `VM_HSAVE_PA`) is a
  per-CPU operation done with interrupts disabled on first use, guarded
  by a per-CPU flag, so no cross-CPU synchronisation is needed.
- Preemption: a guest runs with the host's timer interrupt intercepted,
  so a tight guest loop still yields every tick; the host's preemption
  rules apply to the runner thread unchanged. Migration of the runner
  between runs is fine: nothing about a vCPU is bound to a host CPU.
- TLB: the flush-all on every entry makes NPT changes (only additions in
  this stage) visible without shootdown logic across CPUs.

## Memory

- Per VM: the arch context (ASID, N_CR3), up to 64 MiB of guest pages,
  the nested table (about 1 page per 2 MiB of guest memory plus 3
  upper-level pages), the console ring (4 KiB inline).
- Per vCPU: one VMCB page, the GPR block, the pending set.
- Global: the two permission maps (20 KiB), per-CPU host save area and
  host VMCB (8 KiB per CPU that has run a guest).
- Guest pages are allocated one at a time (order 0) so fragmentation
  never blocks a VM and the accounting is exact; `PMM_ZERO` prevents
  leaking host data into a guest.

## Error handling

- Every hardware refusal of a state (`VMEXIT_INVALID`) is a `FAIL` exit
  with the code and both infos, the vCPU is marked dead and the kernel
  continues; `set_state` catches the combinations it can before entry.
- A guest cannot make the host fault: guest memory is only ever touched
  through the direct map of pages the VM owns; guest-physical addresses
  from exits are used only for lookup and for reporting.
- An MSR the kernel does not model is `#GP` in the guest, not an exit, so
  a stray driver probe does not hang a guest on an unhelpful caller.
- Resource exhaustion returns errors; nothing is pre-reserved.
- Unknown exit codes are `FAIL`, logged once per vCPU at warning level.

## Performance

Stage 1 makes no performance claims: full TLB flush per entry, every port
and MSR intercepted, no large pages, a bounce buffer for memory
copies. Each is a documented optimisation for later (flush-by-ASID,
selective permission maps, 2 MiB nested mappings, user mapping of guest
memory). Exits and entries are counted per vCPU and summed into
`hv.exits` (entries are kept per vCPU as well).

## Security

- Creation requires holding `/dev/vmm` open for writing (mode 0600).
- Guests are confined by nested paging to pages the VM owns; the only
  other channels are the intercepted instructions, whose handlers use
  values only after validation.
- Host state integrity: VMSAVE/VMLOAD of the host's segment bases and
  SYSCALL MSRs around every entry; GIF cleared across the sequence; the
  host's CR0/CR3/CR4/EFER are restored by VMRUN itself from the host
  save area.
- The guest sees no SVM (CPUID cleared, SVM instructions #UD, EFER.SVME
  hidden), no host memory, no host MSRs beyond the modelled set.
- Denial of service: limits on VMs, vCPUs and memory per VM; a guest that
  never exits voluntarily still exits on every host tick.
- Zeroed guest memory; the console ring is per VM.

## Testing strategy

Specified in full in `testing.md`; in outline:

- **Host** (`tests/host/test_hv.c`): the nested page table builder over
  the harness arena (mapping, query, refusals, rollback of a partially
  failing map, unmap, the User bit on every level, every table page
  returned by destroy), the IOIO EXITINFO1 decoder, and the layout
  assertions (`sizeof(struct cosmo_vcpu_regs) == 448`,
  `sizeof(struct cosmo_vm_exit) == 64`, VMCB field offsets) under
  ASan/UBSan.
- **Kernel self-tests** (`hvtest.c`, eight of the 70): `hv-probe`,
  `hv-npt`, and six guest programs from `tests/hv/` (`guest_pio`: debug
  console, an owner-visible OUT, an IN completion; `guest_irq`: virtual
  interrupt delivery through a real-mode IVT, the STI shadow, a second
  vector; `guest_cpuid`: the hypervisor CPUID leaf, the hypervisor bit,
  EFER without SVME, a `wrmsr`, a hypercall; `guest_pm`: a 32-bit
  protected-mode guest entered through `set_regs`, an MMIO exit
  completed by the owner, refused states; `guest_shutdown`: a triple
  fault; `guest_spin`: a guest that never exits, bounded by host ticks
  through `vcpu_run_limited`, plus vCPU slot rules). Without a backend
  the guest tests log `selftest: hv: skipped` and pass; the harness
  forbids that line, so the CI configuration (SVM present) fails if the
  backend went missing.
- **Userland**: `/etc/rc.test` runs `vmctl probe`, `vmctl run
  /boot/tests/hv/guest_pio.bin` and `vmctl info` and prints
  `HVTEST: PASS`, which `run_boot_test.py` requires (`HVTEST: skipped`
  is forbidden).
- **QEMU**: `scripts/qemu-run.sh` uses `-cpu qemu64,+nx,+svm,+npt`
  (`QEMU_CPU` overrides; `host` for KVM/HVF where nested virtualization
  exists). The ubuntu-24.04 CI QEMU (8.2) emulates both features.
- **Not covered yet**: the owner-kill path out of a running guest
  (`process_kill_pending` in the loop) has no automated test; a shell
  with job control would let `rc.test` kill a `vmctl run guest_spin.bin`.

## Future extensibility

- **VT-x/EPT backend** (`vmx.c`): same `arch/hv.h`; VMCS fields and EPT
  format behind it; `attrib` repacked; the IN/OUT next-RIP comes from the
  instruction length field; unrestricted guest for real mode.
- **Stage 2 devices**: a virtual LAPIC (with TPR, real priorities, EOI,
  the timer) replacing the flat pending set; an emulated PIT or the
  LAPIC timer; virtio-console and virtio-net device models registered as
  `vm_device`s with MMIO ranges once an instruction emulator or a
  virtio-pci-on-PIO transport exists; the `/dev/vmm/vmN/...` path
  namespace once the VFS grows a synthetic filesystem.
- **Memory**: user mapping of guest memory (a `vm_space` region backed by
  the guest pages), file-backed regions, 2 MiB nested mappings,
  flush-by-ASID.
- **Linux compat**: `/dev/kvm` as a translation of this API.
- **AArch64**: Phase 13 ported the kernel with a stub backend
  (`kernel/arch/aarch64/hv.c`: `arch_hv_probe` returns `-ENOTSUP`, the
  manager reports `none`, `/dev/vmm` is absent and `rc.test` prints
  `HVTEST: skipped`). The real backend is a later phase: the same generic
  layer over EL2 (stage-2 translation is the GuestMemory; the vGIC is
  the stage-2 interrupt controller), which also needs the loader to
  accept EL2 instead of refusing it.
