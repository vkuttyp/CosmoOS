# Virtualization: architecture

Constitution sections 41 (the hypervisor is a separate kernel subsystem:
VM manager, VM, vCPU, memory, virtual interrupt controller, virtual
devices, device backends), 42 (abstract VirtualCPU, VMState, VMExit,
GuestMemory and VirtualInterrupt; never expose VMX or SVM structures to
generic code), 43 (a Unix-style control interface under `/dev/vmm` with
`vmctl`), 27 and invariant 9 (VirtIO stays independent of CPU
virtualization), invariant 10 (architecture-specific assembly stays
isolated) and the Phase 12 roadmap entry.

This is stage 1 of the phase: hardware-assisted virtual machines as a
kernel service, with the x86-64 backend implemented on **AMD-V (SVM) with
nested paging (NPT)**. The reason is testability, which the constitution
puts first: the project's QEMU/TCG harness emulates SVM and NPT on every
developer machine and in CI, and it does not emulate Intel VT-x, so an
SVM backend can be exercised by the boot test on every push while a VMX
backend could only be tested on hardware. The arch interface is written
for both; VMX/EPT is the next backend (see "Future extensibility" in
`design.md`).

## Where it sits

```text
   user process       vm_create / vm_mem / vm_mem_rw / vcpu_create / vcpu_regs / vcpu_run / vcpu_irq
        │             (+ open("/dev/vmm"), read on a VM handle, close)
        │             kernel/syscall/native.c → handles → struct vm / struct vcpu (kobjects)
        ▼
   kernel-services/virtualization/
        vmm.c         the VM manager: backend probe, /dev/vmm, limits, sysctl values, the VM list
        vm.c          struct vm: lifetime, the kobject (read = console ring, stat), the owner's view
        guestmem.c    GuestMemory: regions of pinned, zeroed host pages mapped guest-physical
        vcpu.c        VirtualCPU: state (VMState), the run loop, exit dispatch, CPUID/MSR emulation
        vintr.c       VirtualInterrupt: per-vCPU pending vectors, delivery when the guest is interruptible
        vmdev.c       device backends: the PIO/MMIO dispatch, the console ring, the debug console (port 0xE9)
        hvsys.c       the seven system calls: handle and rights checks, the /dev/vmm capability, bounce copies
        hv_internal.h what the files above share (registration, dispatch, pending-set helpers)
        hvtest.c      self-tests (probe, nested paging, six guest programs)
        │
        ▼  kernel/include/arch/hv.h      the only surface generic code sees
   kernel/arch/x86_64/
        svm.c         AMD-V: enable (EFER.SVME, host save area per CPU), VMCB setup, intercepts,
                      exit decoding into struct hv_exit, state get/set, event injection
        svm_npt.c     nested page tables (the x86 long-mode format with the User bit set)
        svm_run.S     the VMRUN sequence: CLGI, VMSAVE/VMLOAD host and guest, GPR spill, STGI
        vmx.c         (next backend) VT-x/EPT behind the same arch/hv.h
```

Everything above `arch/hv.h` is generic: it knows a VM has an
architecture context, that a vCPU has a register file described by the
UAPI `struct cosmo_vcpu_regs`, that running a vCPU yields a `struct
hv_exit` of a handful of kinds, and that an interrupt vector can be made
pending. It never sees a VMCB, an ASID, an intercept bit or an EPT/NPT
entry.

VirtIO is not involved in this stage and never will be coupled to it:
`drivers/virtio/` is the *driver* side (this kernel as a guest); virtual
devices offered *to* guests are device backends registered with a VM
(`vmdev.c`). A future virtio-console or virtio-net *device model* for
guests lives next to `vmdev.c` and shares nothing with the drivers except
the ring layout definitions (invariant 9).

## Purpose

Let a privileged user process build and run a virtual machine: allocate
guest memory, load an image into it, create virtual CPUs, set their
initial register state, run them, and handle what they do (port I/O,
halts, memory-mapped I/O, hypercalls) while the kernel keeps the guest
confined by nested paging, keeps the host's own state intact across
guest entries, and keeps every host CPU responsive.

## Responsibilities

- **The VM manager** (`vmm.c`): probe the hardware backend once at boot
  (after the VFS and the boot namespace exist), publish the result
  through `/dev/vmm` and `sysctl hv.*`, keep the list of VMs with an id per VM,
  enforce limits (VMs, vCPUs per VM, guest memory per VM), and gate
  creation on holding `/dev/vmm` open for writing (mode 0600, so uid 0
  unless the administrator changes the node).
- **VM** (`vm.c`): a kobject that owns its guest memory, its vCPUs'
  existence, its device table and a small console ring (what the guest
  wrote to the debug port). Destroying the last handle destroys the VM
  once no vCPU is running.
- **GuestMemory** (`guestmem.c`): non-overlapping regions of
  guest-physical space, each backed by host pages allocated from the PMM,
  zeroed, pinned for the life of the VM and mapped into the backend's
  nested page table. Copy-in/copy-out for the owner process; no user
  mapping of guest memory in this stage.
- **VirtualCPU** (`vcpu.c`): a kobject bound to one VM. Holds the generic
  register file, runs the guest on the calling thread until an exit, and
  dispatches exits: interrupts and interrupt windows are absorbed, CPUID
  and MSR accesses are emulated in the kernel, I/O ports go to a device
  backend if one claims the port and otherwise to the caller, everything
  else (HLT, MMIO, hypercall, shutdown, failures) returns to the caller
  as a `struct cosmo_vm_exit`.
- **VirtualInterrupt** (`vintr.c`): a per-vCPU set of pending vectors; the
  highest one is offered to the backend before each entry and delivered
  by the hardware when the guest becomes interruptible. There is no
  emulated LAPIC or PIC in this stage; the vector space is the guest's IDT.
- **Device backends** (`vmdev.c`): a table of port and memory ranges with
  a handler; the run loop consults it before returning an exit to the
  caller. Stage 1 ships one backend, the debug console on port 0xE9,
  whose bytes the owner reads from the VM handle.
- **The x86-64 backend** (`svm.c`, `svm_npt.c`, `svm_run.S`): everything
  SVM: enabling the extension per CPU, the host save area, the VMCB per
  vCPU, the I/O and MSR permission maps (everything intercepted), the
  nested page table per VM, the VMRUN sequence in assembly, decoding exit
  codes into the generic exit, the guest register file to and from the
  VMCB, virtual interrupt and exception injection.

## Non-responsibilities

- No emulated interrupt controller, timer, or any platform device beyond
  the debug port; no BIOS or firmware; guests are flat images the owner
  loads. These are the natural next steps (stage 2: virtual LAPIC and
  timer; virtio-console and virtio-net device models).
- No instruction emulation: an MMIO access is reported with its
  guest-physical address and direction; the owner must complete it by
  adjusting the vCPU state. In-kernel MMIO device backends therefore
  also receive only the address and direction in this stage.
- No user mapping of guest memory, no file-backed or shared guest
  memory, no ballooning, no huge pages in the nested table.
- No live migration, nested virtualization, SMM, or device passthrough
  (there is no IOMMU driver).
- No VT-x in this stage; a machine without SVM has a `/dev/vmm` that
  reports `none` and refuses `vm_create` with `-ENOTSUP`.
- The VirtIO drivers know nothing about any of this.

## Interfaces at a glance

- **UAPI** (`kernel/include/uapi/cosmo/syscall.h`): seven system calls
  43–49 (`vm_create`, `vm_mem`, `vm_mem_rw`, `vcpu_create`, `vcpu_regs`,
  `vcpu_run`, `vcpu_irq`), `struct cosmo_vcpu_regs` (the VMState, 448
  bytes), `struct cosmo_vm_exit` (the VMExit, 64 bytes) and the
  `COSMO_VM_EXIT_*` kinds. The native `SYS_COUNT` becomes 50.
- **/dev/vmm**: a character node. `read` returns one line describing the
  backend (`svm npt asids=N vms=M`). Its open file, held for writing, is
  the capability `vm_create` requires. VM ids appear in
  `sysctl hv.vms`; per-VM paths (`/dev/vmm/vm0/...`) are a later
  refinement of the control interface, which section 43 allows to evolve.
- **Kernel API** (`kernel/include/kernel/hv.h`): `hv_init`, `vm_create`,
  `vm_mem_add`, `vm_mem_read/write`, `vcpu_create`, `vcpu_get/set_regs`,
  `vcpu_run`, `vcpu_inject`, `vm_console_read`, `vm_device_register`,
  the `struct vm`/`struct vcpu` kobject types, used by the system calls
  and the self-tests alike.
- **Arch interface** (`kernel/include/arch/hv.h`): `arch_hv_probe`,
  `arch_hv_vm_create/destroy/map/unmap`, `arch_hv_vcpu_create/destroy/
  get_state/set_state/run/set_irq/irq_pending/inject_exception/
  advance_rip`, `struct hv_caps`, `struct hv_exit`.
- **libc** (`libc/include/cosmo/hv.h`): thin wrappers and the structures.
- **Userland**: `/sbin/vmctl` (`probe`, `info`, `run`). Section 43 names
  `create/start/stop/destroy/attach`; those verbs presume VMs that
  outlive a controlling process. Here a VM is a handle owned by the
  process that created it and dies with its last handle, so `vmctl run`
  creates, loads, starts and destroys in one invocation and `probe`/`info`
  inspect the manager. Section 43 lets the exact ABI evolve; a daemon
  holding VMs open (and the `/dev/vmm/vmN/...` names) is the natural
  next step once VMs have devices worth keeping alive.
- **Build and test**: `tests/hv/` flat guest images assembled by the
  project toolchain (`.code16` and `.code32` programs linked with
  `--image-base=0 -Ttext=0x1000 --oformat=binary`, `make hv-guests`) and
  carried in the boot archive under `/boot/tests/hv/`;
  `tests/host/test_hv.c`; the QEMU CPU model gains `+svm,+npt`
  (`scripts/qemu-run.sh`; `QEMU_CPU` overrides it).
