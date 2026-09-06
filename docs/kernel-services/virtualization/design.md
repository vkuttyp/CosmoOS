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

## Vendor-neutral vCPU state, capabilities, and the Intel VMX backend

The unit the audit names after the IOMMU (§11.4 "Intel VMX readiness",
§11.3's `attrib` finding, §19 "After these"). Written before the code.

### 1. What was wrong

`arch/hv.h` claims that nothing vendor-specific crosses it, and the
UAPI documents `cosmo_vcpu_seg.attrib` as `type(4) S DPL(2) P | AVL L DB
G in bits 12-15` — the layout of an x86 descriptor, which is what both
vendors derive their control-block fields from. The SVM backend then
copies `attrib` into the VMCB verbatim, where the same fields are packed
into bits 0-11, so what actually crosses the interface is the VMCB's
packing. Two consequences the audit found: `check_state`'s long-mode
test reads bits 13 and 14 (`L`, `DB`) of a value whose `L` and `DB` sit
at bits 9 and 10, so the check is dead and only the hardware's
`VMEXIT_INVALID` catches the combination; and the tests encode `0xC9B`,
teaching the VMCB packing to every reader of the UAPI.

A second seam problem blocks more than VMX: `arch_hv_vm_map` maps RWX at
4 KiB and takes no permission or size argument, so ballooning, dirty
tracking, snapshots, device passthrough and stage-2 on EL2 all need the
signature changed before they can start.

And `struct hv_caps` describes the backend in four fields, none of which
say what a guest may *be*: Intel needs EPT plus "unrestricted guest" to
run the real-mode reset state at all, which is a capability the manager
must be able to ask about rather than assume.

### 2. Neutral segment attributes

`attrib` keeps its 16 bits and its documented meaning, now stated as the
UAPI's own constants rather than prose:

```c
#define COSMO_SEG_TYPE   0x00Fu   /* bits 0-3: descriptor type */
#define COSMO_SEG_S      0x010u   /* code/data (0 = system) */
#define COSMO_SEG_DPL    0x060u   /* bits 5-6 */
#define COSMO_SEG_P      0x080u   /* present */
#define COSMO_SEG_UNUSABLE 0x100u /* bit 8: no segment loaded (VMX's unusable) */
#define COSMO_SEG_AVL    0x1000u  /* bit 12 */
#define COSMO_SEG_L      0x2000u  /* bit 13: 64-bit code */
#define COSMO_SEG_DB     0x4000u  /* bit 14 */
#define COSMO_SEG_G      0x8000u  /* bit 15 */
```

Bits 9-11 stay reserved (zero, refused otherwise). The unusable bit is
the one field neither the descriptor format nor SVM has: VMX needs it,
so the neutral form carries it and the SVM backend renders it as
"not present", which is how a VMCB says the same thing.

- **SVM** packs on the way in (`bits 12-15 >> 4`) and unpacks on the way
  out, so the VMCB's 12-bit form never leaves the backend. A segment
  with `COSMO_SEG_UNUSABLE` is written with `P = 0`; on the way out a
  segment with `P = 0` and a null selector reads back as unusable.
- **VMX** uses the value nearly directly: its access-rights word is this
  layout with unusable at bit 16, so the translation is
  `(attrib & 0xF0FF) | ((attrib & COSMO_SEG_UNUSABLE) << 8)`.

`check_state` moves onto the neutral bits, which makes the long-mode
`L && DB` refusal real, and gains the checks VMX will need anyway
(reserved `attrib` bits, `S` and type consistency for CS/SS/TR).

### 3. Capabilities

```c
struct hv_caps {
    bool present;
    const char *name;          /* "svm", "vmx", "none" */
    unsigned max_asids;        /* address-space tags (VMX: VPIDs) */
    bool nested_paging;        /* NPT / EPT */
    bool real_mode_guest;      /* the architectural reset state can run */
    bool map_prot;             /* arch_hv_vm_map honours per-page permissions */
    bool large_pages;          /* 2 MiB mappings when aligned */
    unsigned max_vcpus;        /* per VM, backend limit */
};
```

`real_mode_guest` is the bit the audit asked for: true on SVM always,
true on VMX only with EPT **and** unrestricted guest. The manager
refuses `vcpu_create` with `-ENOTSUP` when it is false rather than
letting the guest fail its first entry, and `/dev/vmm` reports the whole
set so a user-space VMM can decide before it builds anything. The
`hv-caps` self-test asserts the reported set matches what the running
backend does (a mapping refused for permissions it says it does not
support, a real-mode vCPU refused when it says it cannot).

### 4. Mapping permissions

```c
#define HV_MAP_READ  (1u << 0)
#define HV_MAP_WRITE (1u << 1)
#define HV_MAP_EXEC  (1u << 2)
int arch_hv_vm_map(struct arch_hv_vm *vm, uint64_t gpa, paddr_t hpa, size_t len, unsigned prot);
```

`prot == 0` is `-EINVAL`; a backend that reports `map_prot == false`
refuses anything but RWX. NPT gets the permission bits it always had
(present/write/user, NX from EFER.NXE), EPT gets its own three, and both
gain 2 MiB leaves when the guest-physical address, the host-physical
address and the length are all 2 MiB aligned — which is what makes a
64 MiB guest cost 33 tables instead of 16 385. Guest memory regions pass
`HV_MAP_READ | HV_MAP_WRITE | HV_MAP_EXEC` today; the argument exists so
that read-only regions (a ROM, a snapshot's clean pages) do not need
another API change.

### 5. The VMX backend

`kernel/arch/x86_64/vmx.c`, `vmx_ept.c`, `vmx_run.S`, mirroring the SVM
backend's structure so the two can be read side by side.

- **Probe.** `CPUID.1:ECX.VMX`, then `IA32_FEATURE_CONTROL`: locked
  without the VMX-outside-SMX bit is `-ENOTSUP` (firmware disabled it);
  unlocked means the kernel sets `VMXON | LOCK` itself. `IA32_VMX_BASIC`
  gives the VMCS revision id, the region size and whether the controls
  are "true" (the `IA32_VMX_TRUE_*` MSRs). EPT and unrestricted guest
  come from `IA32_VMX_EPT_VPID_CAP` and secondary controls; without EPT
  the backend refuses to be used at all, as SVM does without NPT.
- **Control fixing.** Every control word is filtered through its
  capability MSR: `allowed-0` bits must be set, `allowed-1` bits may be.
  `vmx_fix_ctls(uint64_t cap_msr, uint32_t want)` is a pure function, so
  the host test can check it against the encodings the manuals give.
- **VMXON per CPU**, lazily on the first run as SVM enables EFER.SVME,
  with the VMXON region allocated from the PMM and `CR4.VMXE` set.
- **VMCS layout** is not a structure: fields are read and written with
  `vmread`/`vmwrite` by encoding. The backend keeps its own shadow of
  what generic code asks for (the register file, segments, control
  registers) and writes it into the VMCS at entry, so `get_state` and
  `set_state` never need the vCPU to be loaded on this CPU.
- **Entry and exit.** `vmx_run.S` saves the host GPRs, loads the guest's,
  issues `VMLAUNCH` the first time and `VMRESUME` afterwards, and on exit
  saves the guest GPRs and restores the host's. RSP/RIP for both sides
  are VMCS fields, so there is no host VMCB analogue; what SVM does with
  `VMSAVE`/`VMLOAD` the VMCS host-state area does declaratively.
- **Interrupts.** SVM's `CLGI` + `sti` trick has no VMX form. Instead
  pin-based "external-interrupt exiting" is set: a host interrupt causes
  an exit with reason 1, the backend re-enables interrupts so the host
  handler runs, and reports `HV_EXIT_INTR` — the same shape the manager
  already handles.
- **Exits.** Reason codes map to `hv_exit`: 10 CPUID, 12 HLT, 30 IO
  (with the exit qualification decoded into port/size/direction/string/
  rep), 31/32 MSR, 48 EPT violation → `HV_EXIT_MMIO`, 18 VMCALL →
  `HV_EXIT_HYPERCALL`, 1 external interrupt → `HV_EXIT_INTR`, 2 triple
  fault → `HV_EXIT_SHUTDOWN`, everything else → `HV_EXIT_FAIL` with the
  reason and both qualification fields, as SVM reports unknown exits.
- **Invalidation.** `INVEPT` and `INVVPID` reach only the CPU that runs
  them, and a VM's translations can be cached on every CPU it has
  entered, so the VM records that mask and both instructions are sent
  there with `smp_call_function_single`. This happens when a range is
  unmapped and before a destroyed VM's EPT root and VPID become
  available to the next one — the same rule the IOMMU layer keeps
  (`docs/kernel/iommu/invariants.md` IOM6): an address is reusable only
  once the hardware has stopped translating it. Adding a mapping needs
  nothing, because an EPT violation leaves no cached entry. A CPU
  without `INVEPT` is refused at probe rather than run with memory it
  cannot revoke.
- **MSR bitmaps** all-ones (every MSR exits), as SVM's MSRPM. The
  backend-owned set differs: on VMX, FS/GS/TR bases and `IA32_EFER`,
  `IA32_PAT`, `IA32_SYSENTER_*` are VMCS fields, while `STAR`, `LSTAR`,
  `CSTAR`, `SFMASK` and `KERNEL_GS_BASE` need the VMCS MSR load/store
  lists. `arch_hv_vcpu_msr` keeps its contract; only the storage differs.
- **The guest cannot become a hypervisor**: `CR4.VMXE` is masked out of
  what the guest may set (the CR4 guest/host mask), VMX instructions
  exit and are reflected as `#UD`, and CPUID keeps hiding both VMX and
  SVM, as it does today.
- **XSAVE** handling is identical to SVM's and shared: the owner's state
  is saved, the guest's restored under its own XCR0, and `XSETBV` is
  intercepted (exit reason 55) and validated by the same code.

### 6. What this environment can and cannot verify

QEMU's TCG emulates AMD SVM and reports `vmx: false` for every CPU model
(`query-cpu-model-expansion` on `max`), and the development host is
AArch64, so **no VMX instruction in this unit is ever executed by any
test in this repository.** That is stated here rather than discovered
later, and it decides how the backend is verified:

- Everything that is pure logic is factored so that it can run on the
  host and is covered by `tests/host/test_vmx.c`: control fixing against
  capability MSR values, segment translation both ways (round-tripping
  every field, unusable included), the exit-reason table, the EPT
  builder (the same treatment `svm_npt.c` already gets, over the
  harness's arena), and the EPT entry encoding (memory type, permission
  bits, large-page bit).
- The rest — VMXON, VMCS field writes, `VMLAUNCH`, the exit path — is
  compiled for the target and reviewed against the SDM, and runs the
  first time on Intel hardware. `hv_caps.present` is false on every
  machine the tests run on, so the backend is inert here: the `hv`
  self-tests keep exercising SVM exactly as before, and a machine with
  neither extension keeps reporting `none`.
- `docs/kernel-services/virtualization/testing.md` says the same thing
  in the gap list, so nobody reads a green chain as evidence that VMX
  works.

### 7. Not done

- Nested virtualization on either vendor, APICv/AVIC, posted
  interrupts, VPID beyond a single tag per VM, TSC offsetting and
  scaling, dirty tracking (the EPT dirty bit is present but unused),
  large pages above 2 MiB, MSR pass-through for performance.
- The VMX backend's own instruction-level verification, for want of
  hardware.

## The AArch64 EL2 backend

The second half of the audit's AArch64 unit (§11.5), on the EL2 the
loader now keeps (`docs/kernel/arch/aarch64/design.md`, "Exception level
2"). Written before the code.

### 1. What has to change above the backend

The manager is vendor-neutral but not architecture-neutral: the register
file is x86's, and three of the exit kinds describe x86 instructions.

- **`struct cosmo_vcpu_regs` becomes per-architecture.** The UAPI
  already splits by architecture where it must (`nr_x86_64.h` /
  `nr_aarch64.h`), and a vCPU's register file is the clearest such case:
  `rax`/`cs`/`cr0` have no AArch64 meaning and `x0`–`x30`/`sctlr_el1`
  have no x86 one. Both blocks stay 448 bytes so the system-call shape,
  the copy sizes and the host-test assertions do not vary by
  architecture. The AArch64 block is
  `x[31]`, `sp_el1`, `sp_el0`, `pc`, `pstate`, then the EL1 system state
  a guest owns — `sctlr_el1`, `ttbr0_el1`, `ttbr1_el1`, `tcr_el1`,
  `mair_el1`, `vbar_el1`, `esr_el1`, `far_el1`, `elr_el1`, `spsr_el1`,
  `tpidr_el0`, `tpidr_el1`, `cpacr_el1` — and `pending_irq` where x86
  has it.
- **Two new exit kinds**, and one retired for this architecture:
  `HV_EXIT_WFI` (the guest asked to wait; the manager treats it like
  `HLT`) and `HV_EXIT_SYSREG` (a trapped system-register access, which
  is what CPUID is for x86: the backend reports the encoding and the
  register index, generic code answers). `HV_EXIT_IO` cannot occur —
  AArch64 has no port space — and the manager's device model reaches
  guests through `HV_EXIT_MMIO`, which already exists.
- `HV_EXIT_HYPERCALL` keeps its meaning: `HVC` from the guest.

### 2. Stage 2

The guest's physical memory is a stage-2 translation, which is the same
shape as the NPT/EPT builders and gets the same treatment: a four-level,
4 KiB-granule tree of 512-entry tables, permissions per leaf, 2 MiB
leaves where everything is aligned, and one builder
(`kernel/arch/aarch64/hv_s2.c`) tested on the host next to the other
two. The entries are LPAE stage-2 descriptors: `S2AP` (bits 6–7) for
read and write, `XN` (bits 53–54) for execute, `MemAttr` (bits 2–5)
normal write-back, `AF` set, `SH` inner-shareable.

`VTCR_EL2` is derived from `ID_AA64MMFR0_EL1.PARange` exactly as the
SMMU driver derives its stage-2 configuration from `IDR5.OAS` — the same
lesson, in the same shape: `T0SZ = 64 - PARange bits`, `SL0` for a
four-level walk, `TG0` 4 KiB, inner-shareable, write-back. `VTTBR_EL2`
carries the root and the VMID; a VM allocates a VMID from a bitmap as
the SVM backend allocates an ASID.

Invalidation follows the rule the IOMMU unit wrote down (IOM6) and the
VMX backend then had to learn: an unmapped page or a reused VMID must be
invalidated on every CPU that ran the VM, with
`TLBI VMALLS12E1IS` (which is inner-shareable, so unlike `INVEPT` it
reaches the other CPUs by itself) followed by `DSB ISH`.

### 3. The world switch

The kernel runs at EL1 and the guest runs at EL1 too, so the switch goes
through EL2: `el2_set_vectors` (this unit's use for the stub the loader
left) installs the backend's own EL2 vector table, and a `HVC` with a
private selector enters the world switch.

```text
  host EL1                     EL2                        guest EL1
  vcpu_run ----HVC(RUN)----> save host EL1 state
                             load guest EL1 state
                             VTTBR_EL2 = root|VMID
                             HCR_EL2 = VM|RW|IMO|FMO|TWI|TSC...
                             ERET -------------------> guest instructions
                             <----- exception ---------
                             save guest state
                             restore host state
  exit reason <--ERET------- decode ESR_EL2
```

`HCR_EL2` bits: `VM` (stage 2 on), `RW` (EL1 is AArch64), `TWI` (WFI
exits), `TSC` (SMC exits), `TID3` (ID-register reads trap, which is how
the manager hides what the model does not implement), `IMO`/`FMO` so
physical interrupts are taken to EL2 and the host handler runs after the
exit — the analogue of SVM's `V_INTR_MASKING` and VMX's
external-interrupt exiting.

The EL2 code runs with the MMU off, as the stub does, so it addresses
everything physically: the per-vCPU state it saves and restores lives in
a page whose physical address the world switch is handed.

### 4. Exits

| `ESR_EL2.EC` | meaning | `hv_exit` |
|---|---|---|
| 0x16 | `HVC` from EL1 | `HYPERCALL` |
| 0x17 | `SMC` | `HYPERCALL` (reported, not forwarded) |
| 0x18 | trapped `MSR`/`MRS` | `SYSREG` |
| 0x01 | `WFI`/`WFE` | `WFI` |
| 0x24 | data abort from a lower EL | `MMIO` (the faulting IPA from `HPFAR_EL2`, direction from `ESR_EL2.WnR`) |
| 0x20 | instruction abort from a lower EL | `MMIO`, or `FAIL` when the address is not a device |
| anything else | — | `FAIL`, with `ESR_EL2` and `FAR_EL2` |

A physical interrupt arriving while the guest runs is an EL2 exception
too, and is reported as `INTR` — the manager's existing "nothing to do
but run again" case.

### 5. What this unit does not do

- **Virtual interrupts.** GICv2 virtualization (the GICH list registers,
  the GICV alias mapped into the guest, the maintenance interrupt) is
  not built: `arch_hv_vcpu_set_irq` records the offer and never delivers
  it, and the capability set says so, so the manager's `hv-guest-irq`
  test skips on this architecture rather than lying. The x86 backends
  are unaffected.
- **Timers.** `CNTVOFF_EL2` stays 0 and the guest sees the host's
  virtual counter; nothing traps `CNTV_*`.
- Nested virtualization, SVE/SME state, debug-register virtualization,
  PMU virtualization, VHE.

### 6. Tests

The guest images are per-architecture: `tests/hv/x86_64/*.S` and
`tests/hv/aarch64/*.S`, each built for its own target as a flat binary
at guest-physical 0x1000. The AArch64 set is one guest per exit the
switch decodes — `guest_wfi`, `guest_hvc`, `guest_mmio`, `guest_sysreg`,
`guest_spin` — and the five `el2-guest-*` self-tests run them:

- **wfi**: the exit names WFI and leaves the PC past it, so a second run
  reaches the second WFI; the guest's PSTATE still says EL1h.
- **hvc**: the hypercall carries this architecture's calling convention
  (`x0` the number, `x1`–`x4` the arguments — the generic loop reads
  them per architecture), the PC is already past the `HVC` as the
  architecture defines, and the guest runs on afterwards.
- **mmio**: a store to guest-physical memory the VM does not have is a
  stage-2 fault reported with the faulting address and the direction,
  and the load that follows is reported as a read — the direction is
  decoded, not guessed.
- **sysreg**: `HCR_EL2.TID3` traps an ID-register read and the exit
  names the destination register, which is what lets a model answer.
- **spin**: a guest that never yields is ended by the host's timer tick
  (`HCR_EL2.IMO`), which is what keeps it from owning a CPU.

The x86 guest tests keep their images and skip on AArch64, and vice
versa. `vmctl` runs whichever guest this build carries, so `HVTEST: PASS`
means a guest really ran from userland on both machines; with
`QEMU_EL2=0` there is no backend and the sections report skipped, which
the harness checks too. The host test gains the stage-2 builder next to
NPT and EPT.

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
