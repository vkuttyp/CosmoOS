# Virtualization: API

Four layers, top down: the user ABI (`kernel/include/uapi/cosmo/syscall.h`,
wrapped by `libc/include/cosmo/hv.h`), the control node `/dev/vmm` and the
`hv.*` sysctl names, the kernel API (`kernel/include/kernel/hv.h`) shared
by the system calls and the self-tests, and the backend interface
(`kernel/include/arch/hv.h`) that the generic layer programs against.
Errors are negative errno values; the C library wrappers return them raw.

## UAPI: structures

### `struct cosmo_vcpu_seg` (16 bytes)

`uint16_t selector; uint16_t attrib; uint32_t limit; uint64_t base;`.
`attrib` is the descriptor's access byte in bits 0–7 (type 4 bits, S,
DPL 2 bits, P) and the flags nibble (AVL, L, D/B, G) in bits 12–15;
bits 8–11 are zero. Real-mode code is `0x009B`, real-mode data `0x0093`,
a flat 32-bit code segment `0x0C9B`, flat 32-bit data `0x0C93`.

### `struct cosmo_vcpu_regs` (448 bytes, the VMState)

| field | meaning |
|---|---|
| `rax` … `r15` | the sixteen general registers, in the order `rax rbx rcx rdx rsi rdi rbp rsp r8 … r15` |
| `rip`, `rflags` | on set, bit 1 is forced on and bits 3, 5, 15 off |
| `cs ds es fs gs ss ldtr tr` | segments; `ldtr`/`tr` attributes are the system-descriptor types |
| `gdtr idtr` | `base` and `limit` only |
| `cr0 cr2 cr3 cr4 cr8` | `cr8` is the task priority (4 bits) |
| `efer` | as the guest sees it: `SVME` never appears and may not be set |
| `dr6 dr7` | |
| `pending_irq` | get: the lowest pending vector, `~0` when none; set: ignored |
| `reserved[9]` | must be zero (ignored today) |

Reset state of a new vCPU: real mode, every segment base 0 limit
0xFFFF, `cs` and data attributes as above, `tr` `0x008B`, `ldtr` `0x0082`,
`rip` 0, `rflags` 2, `cr0` 0x60000010, `efer` 0, `dr6` 0xFFFF0FF0, `dr7`
0x400, all other fields 0.

### `struct cosmo_vm_exit` (64 bytes, the VMExit)

```c
struct cosmo_vm_exit {
    uint32_t kind;      /* COSMO_VM_EXIT_* */
    uint32_t flags;     /* COSMO_VM_EXIT_F_IRQ_PENDING */
    uint64_t rip;       /* the guest rip after the exit was processed */
    union {
        struct { uint16_t port; uint8_t size; uint8_t write; uint8_t string; uint8_t rep; uint16_t pad;
                 uint32_t value; uint32_t pad2; } io;
        struct { uint64_t gpa; uint32_t write; uint32_t pad; } mmio;
        struct { uint64_t nr, a0, a1, a2, a3; } hypercall;
        struct { uint32_t code; uint32_t pad; uint64_t info1, info2; } fail;
        uint64_t raw[6];
    };
};
```

| kind | value | `rip` | payload |
|---|---|---|---|
| `COSMO_VM_EXIT_HLT` | 1 | the instruction after `hlt` | none; inject a vector and run again, or stop |
| `COSMO_VM_EXIT_IO` | 2 | the instruction after `in`/`out` | `io`: `port`, `size` 1/2/4, `write`, `string`, `rep`; `value` holds the bytes written for an OUT; for an IN the caller fills `value` and calls `vcpu_run` again |
| `COSMO_VM_EXIT_MMIO` | 3 | the faulting instruction (not advanced) | `mmio.gpa`, `mmio.write`; the owner completes the access itself (typically `vcpu_regs` to skip the instruction) |
| `COSMO_VM_EXIT_HYPERCALL` | 4 | after `vmmcall` | `hypercall.nr` = rax, `a0..a3` = rbx rcx rdx rsi |
| `COSMO_VM_EXIT_SHUTDOWN` | 5 | where the triple fault happened | none; the vCPU is dead |
| `COSMO_VM_EXIT_FAIL` | 6 | current | `fail.code`, `info1`, `info2` from the backend; the vCPU is dead |

`COSMO_VM_EXIT_F_IRQ_PENDING` (1): at least one injected vector has not
been delivered yet.

CPUID, MSR accesses, host interrupts, interrupt windows and the
intercepted virtualization instructions never produce an exit; the
kernel resolves them and the guest continues.

### Limits

`COSMO_HV_VMS_MAX` 8 VMs, `COSMO_HV_VCPUS_MAX` 4 vCPUs per VM,
`COSMO_HV_VM_MEM_MAX` 64 MiB of guest memory per VM. Not exported but
enforced: 16 memory regions per VM, a 4 GiB guest-physical window, a 4 KiB
console ring.

## UAPI: system calls (native personality, numbers 43–49; `SYS_COUNT` 50)

| nr | name | arguments | result | errors |
|---|---|---|---|---|
| 43 | `vm_create` | `int vmm_h` | a VM handle (READ, WRITE) | `EBADF` (not a handle with WRITE), `EPERM` (not `/dev/vmm` open for writing), `ENOTSUP` (no backend), `ENOSPC` (8 VMs, or no ASID), `ENOMEM`, `EMFILE` |
| 44 | `vm_mem` | `int vm, uint64_t gpa, uint64_t len` | 0 | `EBADF` (not a VM, no WRITE), `EINVAL` (zero, unaligned, beyond 4 GiB, overlapping), `ENOSPC` (16 regions), `ENOMEM` (per-VM limit or pages) |
| 45 | `vm_mem_rw` | `int vm, uint64_t gpa, void *buf, size_t len, int write` | bytes copied (= `len`) | `EINVAL` (`len` > 64 MiB), `EFAULT` (bad user range, or any byte of the guest range unbacked; nothing is copied then), `EBADF` (rights: READ to read, WRITE to write), `ENOMEM` |
| 46 | `vcpu_create` | `int vm, unsigned index` | a vCPU handle (READ, WRITE) | `EBADF`, `ENOTSUP`, `EINVAL` (`index` ≥ 4), `EEXIST` (index in use), `ENOMEM`, `EMFILE` |
| 47 | `vcpu_regs` | `int vcpu, struct cosmo_vcpu_regs *regs, int set` | 0 | `EFAULT`, `EBADF` (READ to get, WRITE to set), `EINVAL` (set: a state the hardware would refuse, see design.md "Register file"; the state is left unchanged) |
| 48 | `vcpu_run` | `int vcpu, struct cosmo_vm_exit *exit` | 0, `*exit` filled | `EFAULT`, `EBADF` (WRITE), `ENOTSUP`, `EIO` (the vCPU is dead), `EINTR` (the calling process is being killed), `ENOMEM` (per-CPU backend state) |
| 49 | `vcpu_irq` | `int vcpu, unsigned vector` | 0 | `EBADF` (WRITE), `EINVAL` (vector < 32 or > 255) |

`vcpu_run` reads `*exit` before running: when the previous exit was an
IN, `exit->kind == COSMO_VM_EXIT_IO` supplies `io.value` for the guest's
`rax` (low `size` bytes, zero-extended); any other `kind` completes the
IN with 0xFFFFFFFF. A `vcpu_regs` set in between cancels the completion.
The structure is zeroed before the exit is written.

A VM handle is an I/O object: `read` drains the debug console ring
(bytes the guest wrote to port 0xE9; returns 0 when empty, never
blocks), `write` is `ENOTSUP`, `fstat` reports `COSMO_DT_CHR`, mode
0600, `ino` = the VM id, `uid` = the creator, `size` = bytes waiting in
the ring. A vCPU handle supports only `close`. Closing the last handle
to a VM while vCPU handles exist keeps the VM alive until they close too.
The Linux personality does not expose these calls.

## libc: `cosmo/hv.h` (native)

Raw wrappers returning the kernel result: `int cosmo_vm_create(int
vmm_handle)`, `int cosmo_vm_mem(int vm, uint64_t gpa, uint64_t len)`,
`long cosmo_vm_mem_read(int vm, uint64_t gpa, void *buf, size_t len)`,
`long cosmo_vm_mem_write(int vm, uint64_t gpa, const void *buf, size_t
len)`, `int cosmo_vcpu_create(int vm, unsigned index)`, `int
cosmo_vcpu_get_regs(int vcpu, struct cosmo_vcpu_regs *)`, `int
cosmo_vcpu_set_regs(int vcpu, const struct cosmo_vcpu_regs *)`, `int
cosmo_vcpu_run(int vcpu, struct cosmo_vm_exit *)`, `int cosmo_vcpu_irq(int
vcpu, unsigned vector)`. The header includes `cosmo/syscall.h`, which
carries the UAPI structures. There are no `errno`-translating
convenience functions yet; `vmctl` uses `strerror(-rc)`.

## `/dev/vmm` and sysctl

`/dev/vmm` is a character node created by `hv_init()` with mode 0600
(`ramfs_mkchr`, `docs/kernel-services/vfs/api.md`). `read` at offset 0
returns one line, `<backend>[ npt] asids=<n> vms=<m>\n`, for example
`svm npt asids=16 vms=0` or `none asids=0 vms=0`; further offsets read
the rest of that line, then 0. `write` is `ENOTSUP`. Holding it open with
`O_WRONLY` or `O_RDWR` is the capability `vm_create` demands; the node's
mode is therefore the whole access policy.

`sysctl` names (`docs/kernel/syscall/api.md`): `hv.backend` (`svm` or
`none`), `hv.vms` (live VMs), `hv.vcpus` (live vCPUs across VMs),
`hv.exits` (sum of exits over live vCPUs). All read-only.

## `vmctl` (`/sbin/vmctl`, `userland/system/vmctl.c`)

| command | does | exit status |
|---|---|---|
| `vmctl probe` | prints the `/dev/vmm` line | 0 backend present, 2 `none` or the node cannot be read |
| `vmctl info` | prints `hv.backend`, `hv.vms`, `hv.vcpus`, `hv.exits` as `name = value` | 0, 1 on a sysctl error |
| `vmctl run [-m KIB] [-a GPA] [-e ENTRY] IMAGE` | creates a VM with `KIB` KiB (default 1024) at guest-physical 0, loads the flat `IMAGE` at `GPA` (default 0x1000), starts one real-mode vCPU at `ENTRY` (default `GPA`, `cs` 0), runs until `HLT`; drains the console after every exit; prints `IO` exits (an IN is completed with 0), `HYPERCALL` exits and continues | 0 on `HLT`; 1 on `MMIO`, `SHUTDOWN`, `FAIL`, an error, or an unreadable image; 2 usage |

## Kernel API (`kernel/include/kernel/hv.h`)

Lifecycle and manager:

- `void hv_init(void)`: probe the backend, create `/dev/vmm`. Called
  from `kernel_main` after `ramfs_populate_boot()`.
- `const struct hv_caps *hv_caps(void)`: `present`, `name`, `max_asids`,
  `nested_paging`.
- `unsigned hv_vm_count(void)`; `void hv_stats(uint64_t *exits, uint64_t
  *entries, unsigned *vcpus)` (sums over live vCPUs; any pointer may be
  NULL); `int hv_sysctl(const char *name, char *out, size_t n)` answers
  the `hv.` names (`name` without the prefix), `-ENOENT` otherwise.
- `int vm_create(uint32_t owner_uid, struct vm **out)`: a referenced VM
  registered with the manager; `-ENOTSUP`, `-ENOMEM`, `-ENOSPC`.
- `struct vm *vm_from_kobject(struct kobject *)`, `struct vcpu
  *vcpu_from_kobject(struct kobject *)`: type checks, NULL otherwise.
- `bool hv_is_vmm_vnode(const struct vnode *)`: the `/dev/vmm` vnode.

GuestMemory:

- `int vm_mem_add(struct vm *, uint64_t gpa, uint64_t len)`: allocates
  `len / 4096` zeroed order-0 pages, maps them, records the region;
  errors as `vm_mem` above. Regions are never removed before release.
- `int vm_mem_read/write(struct vm *, uint64_t gpa, void *buf, size_t
  len)`: copy through the direct map under the VM lock; `-EFAULT` when
  any page of the range is unbacked (checked before copying).
- `bool vm_mem_lookup(struct vm *, uint64_t gpa, struct page **page,
  size_t *offset)`: the backing page of one address (no lock: regions only
  grow while the VM lives).

Device backends and the console:

- `int vm_device_register(struct vm *, struct vm_device *)`: appends a
  backend before the first run; `-EINVAL` (no range, or ports without a
  `pio` handler), `-EBUSY` (a vCPU has run), `-EEXIST` (a port or memory
  range overlaps a registered one). The device's storage belongs to the
  caller for the VM's life. `pio` returns 0 when handled (an IN writes
  `*value`) or `-ENODEV` to pass the exit to the owner; `mmio` is a
  notification only.
- `size_t vm_console_read(struct vm *, void *buf, size_t len)` drains the
  ring; `size_t vm_console_pending(struct vm *)`.

VirtualCPU:

- `int vcpu_create(struct vm *, unsigned index, struct vcpu **out)`: a
  referenced vCPU at the reset state; it holds a reference to the VM.
  `-ENOTSUP`, `-EINVAL`, `-ENOMEM`, `-EEXIST`.
- `int vcpu_get_regs(struct vcpu *, struct cosmo_vcpu_regs *)` (fills
  `pending_irq`), `int vcpu_set_regs(struct vcpu *, const struct
  cosmo_vcpu_regs *)` (`-EINVAL` from the backend; cancels an IN
  completion). Both take the run lock.
- `int vcpu_run(struct vcpu *, struct cosmo_vm_exit *)`: the loop of
  design.md; `-ENOTSUP`, `-EIO` (dead), `-EINTR` (caller being killed),
  backend errors. `int vcpu_run_limited(struct vcpu *, struct
  cosmo_vm_exit *, unsigned max_intr)`: the same, giving up with
  `-ETIMEDOUT` after `max_intr` host-interrupt exits (0 = unlimited; for
  tests of guests that never exit).
- `int vcpu_inject(struct vcpu *, unsigned vector)`: make 32..255
  pending (`-EINVAL` otherwise); callable from any thread while the
  guest runs. `int vcpu_lowest_pending(struct vcpu *)`: the vector the
  next entry will offer, -1 none.

Kobject types: `vm` (a `kobject_io_type`: `read`, `write`, `stat`) and
`vcpu` (plain). `kobject_put` on the last reference releases them:
`vcpu_release` clears the VM's slot and drops the VM reference;
`vm_release` unregisters, frees every region and the arch context.

## The arch interface (`kernel/include/arch/hv.h`)

Implemented by `kernel/arch/x86_64/svm.c` (+ `svm_npt.c`, `svm_run.S`).
Everything is opaque above it: `struct arch_hv_vm` and `struct
arch_hv_vcpu` are incomplete types to generic code.

| function | contract |
|---|---|
| `int arch_hv_probe(struct hv_caps *out)` | Once at boot. Detects the extension and prepares shared state; `-ENOTSUP` (with `present = false`, `name = "none"`) when the CPU lacks it, when firmware disabled it, or when nested paging is missing; `-ENOMEM`. Does not enable it on any CPU: that happens on first use inside `arch_hv_vcpu_run`. |
| `int arch_hv_vm_create(struct arch_hv_vm **out)` / `void arch_hv_vm_destroy(struct arch_hv_vm *)` | A nested page table root and an address-space tag (ASID); `-ENOTSUP`, `-ENOMEM`, `-ENOSPC` (tags exhausted). Destroy frees every table page. |
| `int arch_hv_vm_map(vm, uint64_t gpa, paddr_t hpa, size_t len)` | Maps 4 KiB pages RWX; `-EINVAL` (alignment), `-EEXIST` (a page already mapped: the call is rolled back), `-ENOMEM`. |
| `int arch_hv_vm_unmap(vm, uint64_t gpa, size_t len)` | Clears leaves; unmapped pages are ignored; `-EINVAL` (alignment). Intermediate tables stay until destroy. |
| `bool arch_hv_vm_query(vm, uint64_t gpa, paddr_t *hpa)` | The host-physical address behind a guest-physical one (offset preserved). |
| `int arch_hv_vcpu_create(vm, struct arch_hv_vcpu **out)` / `void arch_hv_vcpu_destroy(v)` | A control block at the reset state; `-ENOTSUP`, `-ENOMEM`. |
| `void arch_hv_vcpu_get_state(v, struct cosmo_vcpu_regs *)` | The whole register file; `pending_irq` is `~0` (generic code fills it). |
| `int arch_hv_vcpu_set_state(v, const struct cosmo_vcpu_regs *)` | Validates first (`-EINVAL`, nothing changed), then writes everything; the guest's EFER view is remembered without SVME and with LMA recomputed. |
| `int arch_hv_vcpu_run(v, struct hv_exit *out)` | Enters the guest on the calling CPU and returns at the first exit with `out` decoded (`HV_EXIT_HLT`, `IO` with `next_rip`, `MMIO`, `CPUID`, `MSR`, `HYPERCALL`, `SHUTDOWN`, `INTR`, `FAIL`). Caller: interrupts enabled, may not hold spinlocks; the function disables them only around the entry sequence and never sleeps. `HLT`, `HYPERCALL` and `INVD` have already advanced RIP; `IO` reports the next RIP but does not advance; `CPUID` and `MSR` leave RIP for generic code to advance; the virtualization instructions and MONITOR/MWAIT were answered with `#UD` and reported as `INTR`. `-ENOMEM` if the per-CPU state cannot be allocated on first use. |
| `void arch_hv_vcpu_set_irq(v, int vector)` | Offers `vector` (−1: none) for delivery when the guest is interruptible. |
| `bool arch_hv_vcpu_irq_taken(v)` | After a run: the offered vector was delivered. |
| `void arch_hv_vcpu_inject_exception(v, uint8_t vector, bool has_error, uint32_t error)` | Queues an exception for the next entry. |
| `void arch_hv_vcpu_advance_rip(v, unsigned bytes)` / `void arch_hv_vcpu_set_rip(v, uint64_t rip)` / `uint64_t arch_hv_vcpu_rip(v)` | Move RIP on the guest's behalf; both writers also end an interrupt shadow recorded at the exit (invariant V9). |
| `void arch_hv_vcpu_write_rax(v, uint64_t value, unsigned size)` | The low 1, 2 or 4 bytes of `rax` (4 zero-extends), as an IN does. |
| `uint64_t arch_hv_vcpu_read_gpr(v, unsigned index)` / `void arch_hv_vcpu_write_gpr(v, unsigned index, uint64_t value)` | Registers by the x86 encoding index: `HV_GPR_RAX` 0, `RCX` 1, `RDX` 2, `RBX` 3, `RSP` 4, `RBP` 5, `RSI` 6, `RDI` 7, then r8–r15. |
| `uint64_t arch_hv_vcpu_guest_efer(v)` / `int arch_hv_vcpu_set_guest_efer(v, uint64_t)` | The guest's view of EFER; the setter rejects reserved bits and SVME (`-EINVAL`) and recomputes LMA. |
| `int arch_hv_vcpu_msr(v, uint32_t index, bool write, uint64_t *value)` | The MSRs the control block holds (STAR, LSTAR, CSTAR, SFMASK, FS/GS/KernelGS base, SYSENTER_CS/ESP/EIP, PAT): 0 handled, `-ENOENT` for any other index. |
| `void arch_hv_host_cpuid(leaf, subleaf, *eax, *ebx, *ecx, *edx)` / `uint64_t arch_hv_host_tsc(void)` | The host's values, for the emulation to filter; generic code never executes the instructions itself. |

`struct hv_caps { bool present; const char *name; unsigned max_asids;
bool nested_paging; }`; `struct hv_exit { enum hv_exit_kind kind; union
{ io { port, size, write, string, rep, next_rip }; mmio { gpa, write };
msr { index, write }; fail { code, info1, info2 }; }; }`.

## Build and test entry points

- `make hv-guests` (`tests/hv/hv.mk`): `out/<arch>-<build>/tests/hv/*.bin`,
  the six flat guest images (`--image-base=0 -Ttext=0x1000
  --oformat=binary`), carried in the boot archive as `tests/hv/*.bin` →
  `/boot/tests/hv/`. Part of `all`.
- `make host-test` includes `test_hv` (`tests/host/test_hv.c`).
- `QEMU_CPU` (default `qemu64,+nx,+svm,+npt`) selects the CPU model
  `scripts/qemu-run.sh` passes to QEMU; `host` for `QEMU_ACCEL=kvm`/`hvf`.
