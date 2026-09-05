# CosmoOS documentation

The governing document for this project is the master prompt in
[`prompts/`](../prompts/). It is the constitution: vision, kernel
architecture, fifteen architectural invariants, coding rules, the
development workflow, and the phased roadmap. Nothing in `docs/` overrides
it; when a document here and the constitution disagree, the constitution
wins and the document is wrong.

## Layout

| Path | Content |
|---|---|
| `development.md` | Setting up a development host, building, running, testing, CI |
| `audit/` | Whole-tree architecture audits; `2026-09-post-roadmap-audit.md` is the post-Phase-13 correctness, security, scalability and roadmap audit (Prompt #2); `2026-09-lifetime-quiesce-report.md` is the Prompt #3 final report (critical fix pass, the lifetime and quiescence subsystem, ordering, tests, performance, risks, debt, next subsystem) |
| `first-task.md` | Constitution section 72 deliverables for the section 70 first engineering task |
| `build/` | Build system subsystem documentation |
| `boot/` | Boot protocol and UEFI loader subsystem documentation |
| `kernel/arch/` | Architecture abstraction and its x86-64 implementation; `kernel/arch/aarch64/` covers the AArch64 port (Phase 13: QEMU `virt`, GICv2, stage-1 MMU, PSCI, the loader split) |
| `kernel/interrupt/` | Vector-to-handler dispatch; `controllers.md` covers the Phase 3 IRQ layer (GSI routing, LAPIC/IOAPIC, vector map, IPIs) |
| `kernel/diagnostics/` | Console, logging, printf, panic, self-tests, crash test |
| `kernel/memory/` | Physical memory (bootmem, zones, buddy), virtual memory (page-table takeover, arena, faults), slab heap and kmalloc; host unit tests in `tests/host/` |
| `kernel/scheduler/` | Threads, per-CPU run queues, round-robin policy, preemption, wait queues, mutex, semaphore, completion, per-CPU data |
| `kernel/timer/` | Monotonic TSC clock, LAPIC tick, one-shot timers, sleep, PIT calibration |
| `kernel/smp/` | AP bring-up (trampoline, per-CPU tables), IPIs, cross-CPU calls, TLB shootdown, stopping CPUs, the hang watchdog |
| `drivers/acpi/` | Static ACPI tables: RSDP/XSDT walk and the decoded MADT (CPUs, LAPIC, IOAPICs, overrides; GIC CPU interfaces, distributor and MSI frame on AArch64) |
| `kernel/object/` | Reference-counted kernel objects (with owner-module tracking since the lifetime pass), the per-process handle table with rights, the console object |
| `kernel/lockdep/` | Lock discipline: the debug-build lock-order checker (classes, held-lock stacks, the dependency graph, interrupt-safety, `might_sleep`), the nesting annotations, and the fixes it drove (VFS rename order and vnode cache, `vfs_sync`, the futex copy, the AArch64 IPI path under the run-queue lock) |
| `kernel/quiesce/` | Kernel object lifetime and quiescence: epoch-based grace periods (`synchronize_quiesce`, `call_quiesce`), `synchronize_irq`, `timer_cancel_sync`, the mandatory-release object rule, the module unload protocol, the converted device, block, interface, TCP and UDP lifetimes, with the memory-ordering argument for every barrier |
| `kernel/process/` | Processes, user address spaces, the static ELF loader, ring-3 entry and return, user-memory access, fatal user faults, the `init` boot module; since Phase 9 `spawn` from a file with a handle map, zombies and `wait`, `kill` delivery, the working directory, `procinfo`; since Phase 11 personality selection by ELF note and the two-page initial stack with the Linux auxiliary vector |
| `kernel/syscall/` | SYSCALL/SYSRET entry, the generic dispatcher and personalities (native and, since Phase 11, Linux), the native system-call ABI (`uapi/cosmo/syscall.h`, 56 calls) and user-side wrappers |
| `kernel/module/` | The boot archive, the module ABI (`COSMO_MODULE`, `EXPORT_SYMBOL`, ABI version), the signed `ET_REL` module loader (validation, relocation, symbol resolution, dependencies, W^X, unload), signing tools and keys |
| `kernel/security/` | SHA-512, Ed25519 verification, the compiled-in key ring, kernel taint; credentials and capabilities arrive in a later phase |
| `kernel/device/` | The bus/device/driver model with resources and probing, the DMA API, the block layer (`kernel/block/`), the entropy pool, console sinks, MSI in the interrupt layer, and the QEMU device configuration the tests rely on |
| `drivers/pci/` | PCI core: ECAM or legacy configuration access, enumeration, BAR sizing, capabilities, MSI/MSI-X, the `pci` bus and `struct pci_driver` |
| `drivers/virtio/` | The `virtio` module (bus, device initialisation, split virtqueues, virtio-pci modern transport) and the `virtio_blk`, `virtio_rng`, `virtio_console` driver modules |
| `kernel-services/vfs/` | The VFS (mounts, path walk, vnodes, files, the `fs_type` registry), the page cache, ramfs and the boot namespace, the storage pool (`kernel-services/storage/`), and the twelve filesystem system calls |
| `kernel-services/filesystem/cosmofs/` | The copy-on-write filesystem: on-disk layout, transactions and commit, crash behaviour |
| `kernel-services/network/` | The network stack: mbufs, interfaces and the `netrx` worker, Ethernet/ARP, IPv4/ICMP, IPv6/ICMPv6/ND, UDP, TCP, sockets and system calls 23–31, the `virtio_net` driver's contract, fw_cfg boot parameters, the QEMU network harness |
| `kernel/tty/` | The line discipline: the console tty fed by the serial receive interrupt, canonical editing and echo, killable reads through the console kobject |
| `kernel/ipc/` | IPC primitives: anonymous pipes as two kobject ends (`pipe`, system call 35); since Phase 11 the futex (`futex_wait`/`futex_wake`) |
| `libc/` | The native C library: headers, `errno`, string, allocator, stdio, `spawn`/`wait`/`kill`, files, directories, sockets, the native introspection wrappers; host test |
| `userland/` | init, the shell (`cosmo$ `), the coreutils and system tools, `/etc/rc`, the shell test script and the interactive serial harness |
| `kernel-services/virtualization/` | The VM manager (Phase 12): `/dev/vmm`, VMs and vCPUs as kobjects, guest memory behind nested paging, the run loop with CPUID/MSR emulation and virtual interrupts, device backends (the debug console), the `arch/hv.h` backend interface and its AMD-V (SVM + NPT) implementation, system calls 43–49, `vmctl`, the guest-image tests |
| `compat/linux/` | The Linux personality (Phase 11): selection by the CosmoOS ELF note, the Linux x86-64 system-call table translating onto the native services (files, memory, `brk`, futex, signal tables, time, sockets), the Linux initial stack and thread pointer, the raw-ABI and musl test programs |
| `pkg/` | The package system: recipes (`ports/`), the host builder `tools/pkgbuild.py`, the `.cpk` and `INDEX` formats, the `pkg` manager (`pkg/`), signing and checksums, the `/var/db/pkg` database, the package tests |

Further subsystem directories are added as subsystems come into existence
(`drivers/nvme/`, `drivers/network/`, `compat/<other>/`, and so on).

## Per-subsystem convention

Constitution section 64 requires five files for every major subsystem:

| File | Answers |
|---|---|
| `architecture.md` | Where the subsystem sits, purpose, responsibilities, non-responsibilities, interfaces at a glance |
| `design.md` | Data structures, ownership and lifetime, concurrency model, memory, error handling, performance and security considerations, future extensibility |
| `api.md` | Every public interface with purpose, inputs, outputs, ownership, lifetime, concurrency, blocking behaviour, interrupt-context restrictions, failure modes, ABI stability (section 52) |
| `invariants.md` | Rules that must never be violated without revising the document and the code together |
| `testing.md` | How the subsystem is tested and how to run those tests |

The twelve explanation points of section 64 (purpose, responsibilities,
non-responsibilities, interfaces, data structures, concurrency model,
memory ownership, error handling, performance, security, testing strategy,
future extensibility) are split between `architecture.md` (points 1 to 4)
and `design.md` (points 5 to 12).

## Writing rules

- Reference real file paths, functions, and constants. A statement that
  cannot be checked against the tree does not belong here.
- Say what is implemented. A planned feature is labelled as planned with the
  roadmap phase that delivers it.
- Every public function documents whether it may sleep, allocate, take a
  lock, trigger I/O, or run in interrupt context.
