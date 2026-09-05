# Virtualization: invariants

Each invariant names what upholds it in the code and what checks it.
"Self-test" refers to `kernel-services/virtualization/hvtest.c`
(`SELFTEST: hv-*`), "host test" to `tests/host/test_hv.c`, "rc.test" to
the `vmctl` run in `/etc/rc.test` (`HVTEST: PASS`).

### V1. Generic code never sees the vendor extension

`kernel-services/virtualization/*` includes only `kernel/*`, `uapi/*`
and `arch/hv.h`; no VMCB, ASID, intercept bit, EPT/NPT entry, CPUID or
RDTSC instruction appears above `arch/hv.h` (constitution section 42;
`arch_hv_host_cpuid` and `arch_hv_host_tsc` exist for exactly this
reason). The register file crossing the interface is the UAPI `struct
cosmo_vcpu_regs`. *Upheld by*: the include paths (`x86/` headers are only
reachable from `kernel/arch/x86_64/`); the arch header exposes opaque
`struct arch_hv_vm`/`struct arch_hv_vcpu`. *Checked by*: the build (a
generic file including `x86/svm.h` does not compile); review.

### V2. Guest memory is host memory the VM owns, zeroed, and nothing else

Every nested-table leaf points at an order-0 page allocated with
`PMM_FLAGS_ZERO` by `vm_mem_add` and recorded in a `guest_region`; the
pages are freed only in `vm_release`, after the nested table is
destroyed. No other subsystem maps them; the owner reaches them only
through `vm_mem_rw`, which checks every page of the range before copying
a byte and copies through the direct map under the VM lock. *Checked
by*: `hv-npt` (translation of every page equals `page_to_phys` of the
recorded page; unbacked ranges are `-EFAULT`; fresh memory reads zero;
cross-page round trip), host test (`npt_query` on mapped/unmapped
pages, rollback of a failing map, destroy returns every table page).

### V3. Every port and every MSR is intercepted

The I/O permission map (12 KiB) and the MSR permission map (8 KiB) are
all ones and shared by every VMCB; `IOIO_PROT` and `MSR_PROT` are in the
intercept vector of every vCPU. A guest cannot touch a host port or MSR:
ports go to a device backend or the owner, MSRs are emulated or `#GP`.
*Upheld by*: `arch_hv_probe` (maps) and `vmcb_reset` (intercepts) in
`svm.c`. *Checked by*: `hv-guest-pio` (an OUT to port 0x80 reaches the
owner, an IN is completed by the owner), `hv-guest-cpuid` (EFER reads
and writes are emulated).

### V4. Host state survives every entry

The sequence in `svm_run.S` runs under a cleared GIF, `VMSAVE`s the
host's FS/GS/TR/LDTR bases, KernelGSBase and the SYSCALL/SYSENTER MSRs
to a per-CPU host VMCB before entry and `VMLOAD`s them after; VMRUN
itself saves and restores RSP, RIP, RFLAGS, CR0/CR3/CR4/EFER and the
segment selectors through the per-CPU host save area (`VM_HSAVE_PA`);
the callee-saved GPRs are pushed and popped, DF is cleared. The C caller
disables local interrupts around the call. *Checked by*: every boot test
runs eight guest self-tests and `vmctl` on a 4-CPU host and continues to
run the rest of the suite (SMP=1, release and crash variants included).

### V5. A guest cannot keep a host CPU

`V_INTR_MASKING` is set and the sequence executes `sti` after `clgi`, so
VMRUN records host IF = 1 and a physical interrupt exits the guest
(`INTR` is intercepted) whatever the guest's own IF; the timer tick
therefore bounds every guest run to one host slice, after which the
usual preemption applies to the running thread. *Checked by*:
`hv-guest-spin` (a guest with interrupts disabled in a tight loop; five
INTR exits arrive and `vcpu_run_limited` returns `-ETIMEDOUT`).

### V6. The guest is never a hypervisor

`EFER.SVME` is forced on in the VMCB (VMRUN requires it) but never
reported: `get_state` and `rdmsr EFER` return the remembered guest view;
`set_state` and `wrmsr EFER` refuse SVME (`-EINVAL`, `#GP`). CPUID
leaf 0x80000001 ECX has the SVM bit cleared and leaf 0x8000000A is
zero; VMRUN, VMLOAD, VMSAVE, STGI, CLGI, SKINIT and INVLPGA are
intercepted and answered with `#UD`. *Checked by*: `hv-guest-cpuid`
(`rdmsr EFER` shows SVME clear, the guest's SCE write is visible as
`efer == 1`), `hv-guest-pm` (`set_regs` with SVME is `-EINVAL`).

### V7. Creating a VM requires `/dev/vmm` open for writing

`sys_vm_create` accepts only a handle whose object is a `struct file`
on the manager's vnode with an access mode other than read-only;
anything else is `-EPERM` (or `-EBADF` without WRITE on the handle).
The node is created with mode 0600, so the VFS permission check is the
whole policy. *Checked by*: rc.test (`vmctl` as uid 0 succeeds); the
self-tests call `vm_create` directly, below the check. (Negative test:
none automated yet.)

### V8. The UAPI layouts are fixed

`sizeof(struct cosmo_vcpu_regs) == 448`, `sizeof(struct cosmo_vm_exit)
== 64`, `sizeof(struct cosmo_vcpu_seg) == 16`; fields are added only in
`reserved[]`. *Checked by*: the host test.

### V9. Skipping an instruction ends the interrupt shadow

Every exit that completes an instruction on the guest's behalf (HLT,
CPUID, MSR, IN/OUT, VMMCALL, INVD, and the owner's `set_rip`) goes
through `skip_instruction`, which writes RIP and clears the VMCB's
interrupt-shadow field. Without it an STI shadow recorded at an
intercept would be restored on every re-entry and a pending vector could
never be delivered. *Checked by*: `hv-guest-irq` (`sti; hlt` exits with
the vector pending; the next run delivers it and the handler's output
appears).

### V10. No VM outlives its last handle and its last vCPU

A vCPU holds a reference to its VM; handles hold references to both.
`vm_release` runs only when both counts reach zero, and only then are
the regions unmapped and freed and the arch context destroyed, so guest
memory cannot vanish under a running vCPU. A running vCPU cannot be
released either: the system call holds the lookup reference for the
run. *Checked by*: `hv-guest-spin` (dropping a second vCPU decrements
`nr_vcpus`; dropping the last vCPU and the VM reference brings
`hv_vm_count` back), `hv-npt` (`hv_vm_count` returns to its old value).

### V11. The device table is frozen once a vCPU has run

`vm_device_register` returns `-EBUSY` after `vm->started` is set (on the
first `vcpu_run`), and the run loop walks the list without the VM lock.
Overlapping ranges are refused with `-EEXIST` at registration.
*Checked by*: review (no test registers a device beyond the built-in
console yet).

### V12. VirtIO drivers know nothing about virtualization

`drivers/virtio/` is untouched by this phase; no symbol of
`kernel-services/virtualization/` is referenced from `drivers/` and
none of `drivers/virtio/` from the VMM (constitution invariant 9).
*Checked by*: `git grep` at review; the module boundary (virtio is a
boot module with its own export set).

### V13. Limits hold and are reported honestly

At most 8 VMs, 4 vCPUs per VM, 16 regions and 64 MiB of guest memory
per VM, a 4 GiB guest-physical window; exceeding one is `-ENOSPC`,
`-ENOMEM` or `-EINVAL` and leaves the VM unchanged. `hv.vms`, `hv.vcpus`
and `hv.exits` reflect live objects only. *Checked by*: `hv-npt`
(overlap, alignment, window edge, the per-VM memory limit),
`hv-guest-spin` (vCPU index reuse `-EEXIST`, index ≥ 4 `-EINVAL`).

### V14. Failures are exits, never faults

An entry the hardware refuses, an unknown exit code or a triple fault
becomes a `FAIL` or `SHUTDOWN` exit that marks the vCPU dead (`-EIO` on
the next run) while the state stays readable; guest-physical addresses
from exits are only looked up and reported; unmodelled MSRs are `#GP` in
the guest. The host kernel never panics because of what a guest does.
*Checked by*: `hv-guest-shutdown` (`int $3` with an empty IDT → `SHUTDOWN`,
then `-EIO`, then `vcpu_get_regs` still works), `hv-guest-pm` (an NPF
becomes an `MMIO` exit the owner completes).
