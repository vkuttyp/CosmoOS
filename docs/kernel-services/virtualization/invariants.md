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

### V15. Nothing vendor-specific crosses `arch/hv.h`

Segment attributes travel in the architectural descriptor layout
(`COSMO_SEG_*`), not in a control block's packing: SVM moves AVL/L/DB/G
into VMCB bits 8–11 and writes an unusable segment as not present, VMX
uses the layout with unusable at bit 16, and `x86/hvseg.h` is the only
place either encoding exists. A backend refuses reserved attribute bits
and an unusable-and-present combination before it writes anything.
*Checked by*: `test_vmx`/`test_hv` (both translations round-trip every
field, including the DPL and the unusable case), `hv-guest-pm` (a
protected-mode guest built from neutral attributes runs), and the
long-mode `L && DB` refusal in `check_state`, which the old packing made
unreachable. *Gap*: only two backends have ever exercised the layout.

### V16. A capability the backend reports is one it honours

`hv_caps` says what a guest may be, not only what the extension is
called: `real_mode_guest` (the architectural reset state can run at
all — SVM always, VMX only with EPT and unrestricted guest), `map_prot`,
`large_pages`, `max_vcpus`. Generic code asks rather than assumes, and
`/dev/vmm` reports the whole set so a user-space VMM can decide before
it builds anything. *Checked by*: `hv-caps` (the mapping call refuses
`prot` 0 and unknown bits; a read-only mapping is accepted and reads
back when `map_prot`), `hv-probe` (a backend that cannot run the reset
state may not claim to). *Gap*: the false cases are unreachable on the
machines the tests run on, so what is checked is the true branch.

### V17. A guest's translations die with the mapping, on every CPU

The VMX backend records the CPUs a VM has entered and sends `INVEPT`
(and `INVVPID` where VPIDs are used) to each of them when guest-physical
pages are unmapped, and again before a destroyed VM's EPT root and VPID
can be handed to another VM — the rule the IOMMU layer states as IOM6.
The EL2 backend has the same obligation and a cheaper instrument:
`TLBI VMALLS12E1IS` is inner-shareable, so it reaches every CPU by
itself, but its VMID comes from `VTTBR_EL2`, which only EL2 can write —
so the backend asks EL2 to do it through a call of its own
(`HV_EL2_CALL_TLBI`), at the moment the tables change and again before a
destroyed VM's VMID can be reassigned. When that call cannot be made —
EL2 unreachable on this CPU — **nothing of the VM is reused**: `unmap`
returns `-EIO` and `region_free` leaks the guest's frames rather than
returning them to the allocator, and `vm_destroy` keeps both the
stage-2 tables and the VMID for the life of the boot. Leaking is the
cheap half of that trade, as it is for the IOMMU. The descriptors are
cleared with ordinary stores, so a `DSB ISHST` orders them against the
inner-shareable domain **before** the TLBI: a table walk is a memory
access, and without that barrier a walker on another CPU can refill the
entry the invalidation was meant to remove. The `DSB ISH` after the TLBI
then waits for the invalidation itself. Deferring it to the next entry --
the first shape this took -- would have been wrong twice over: a vCPU
already inside the guest keeps its stale translations while the caller
frees the pages, and a VMID handed to the next VM carries the previous
one's entries.
A CPU whose `IA32_VMX_EPT_VPID_CAP` offers no `INVEPT` is refused at
probe. Adding a mapping needs no invalidation because an EPT violation
caches nothing. SVM has the same property by a blunter route: every
`VMRUN` flushes the guest's ASID. *Checked by*: review only — see V18.
*Gap*: like everything else in the backend, this has never run.

### V18. A guest runs at EL1 with EL2 in between, and nothing of the host goes with it

On AArch64 the kernel and the guest both run at EL1, so the switch lives
at EL2 (`hv_el2_switch.S`, installed through the loader's stub). Every
entry saves the host's EL1 system registers, its callee-saved registers,
its reserved `x18` and where its `HVC` came from, and every exit puts
them all back before returning; the guest's own state is saved into the
same context page. Stage 2 is on only while the guest runs
(`HCR_EL2.VM`), which is also how the switch tells the host's call from
a guest's exception. The EL2 code runs with its MMU off, so it never
depends on tables the kernel may reclaim. *Checked by*: the five
`el2-guest-*` tests — a guest's PSTATE and registers survive a round
trip, `x0` keeps the value the guest computed after its `HVC`, and the
host boots on normally afterwards, which it would not if a system
register came back wrong. *Gap*: FP/SIMD state is not switched (guests
run with `CPACR_EL1` as they set it, and the host's registers are
saved by its own context switch), and nothing tests two vCPUs of one VM
on different CPUs at once.

### V19. The VMX backend is inert until it is on Intel hardware

Both backends are compiled into every x86-64 kernel and `arch_hv_probe`
takes the one the CPU has; on every machine these tests run on that is
SVM (QEMU's TCG emulates AMD-V and reports `vmx: false`), so no VMX
instruction is executed by any test in this repository. What is verified
here is the pure logic — control fixing against capability MSR values,
the I/O qualification decoder, the EPT pointer and page-table builder,
the segment translation — and the fact that adding the backend changed
nothing for SVM. *Checked by*: `test_vmx`, and the whole `hv-*` suite
still passing. *Gap*: VMXON, the VMCS field writes, `VMLAUNCH` and the
exit path have never run. That is stated in `testing.md` too, so a green
chain is not misread as evidence that VMX works.
