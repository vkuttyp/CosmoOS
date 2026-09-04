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
| `first-task.md` | Constitution section 72 deliverables for the section 70 first engineering task |
| `build/` | Build system subsystem documentation |
| `boot/` | Boot protocol and UEFI loader subsystem documentation |
| `kernel/arch/` | Architecture abstraction and its x86-64 implementation |
| `kernel/interrupt/` | Vector-to-handler dispatch; `controllers.md` covers the Phase 3 IRQ layer (GSI routing, LAPIC/IOAPIC, vector map, IPIs) |
| `kernel/diagnostics/` | Console, logging, printf, panic, self-tests, crash test |
| `kernel/memory/` | Physical memory (bootmem, zones, buddy), virtual memory (page-table takeover, arena, faults), slab heap and kmalloc; host unit tests in `tests/host/` |
| `kernel/scheduler/` | Threads, per-CPU run queues, round-robin policy, preemption, wait queues, mutex, semaphore, completion, per-CPU data |
| `kernel/timer/` | Monotonic TSC clock, LAPIC tick, one-shot timers, sleep, PIT calibration |
| `kernel/smp/` | AP bring-up (trampoline, per-CPU tables), IPIs, cross-CPU calls, TLB shootdown, stopping CPUs, the hang watchdog |
| `drivers/acpi/` | Static ACPI tables: RSDP/XSDT walk and the decoded MADT (CPUs, LAPIC, IOAPICs, overrides) |
| `kernel/object/` | Reference-counted kernel objects, the per-process handle table with rights, the console object |
| `kernel/process/` | Processes, user address spaces, the static ELF loader, ring-3 entry and return, user-memory access, fatal user faults, the `init` boot module |
| `kernel/syscall/` | SYSCALL/SYSRET entry, the generic dispatcher and personalities, the native system-call ABI (`uapi/cosmo/syscall.h`) and user-side wrappers |
| `kernel/module/` | The boot archive, the module ABI (`COSMO_MODULE`, `EXPORT_SYMBOL`, ABI version), the signed `ET_REL` module loader (validation, relocation, symbol resolution, dependencies, W^X, unload), signing tools and keys |
| `kernel/security/` | SHA-512, Ed25519 verification, the compiled-in key ring, kernel taint; credentials and capabilities arrive in a later phase |
| `kernel/device/` | The bus/device/driver model with resources and probing, the DMA API, the block layer (`kernel/block/`), the entropy pool, console sinks, MSI in the interrupt layer, and the QEMU device configuration the tests rely on |
| `drivers/pci/` | PCI core: ECAM or legacy configuration access, enumeration, BAR sizing, capabilities, MSI/MSI-X, the `pci` bus and `struct pci_driver` |
| `drivers/virtio/` | The `virtio` module (bus, device initialisation, split virtqueues, virtio-pci modern transport) and the `virtio_blk`, `virtio_rng`, `virtio_console` driver modules |

Further subsystem directories are added as subsystems come into existence
(`kernel-services/vfs/`, `drivers/nvme/`, `drivers/network/`, and so on).

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
