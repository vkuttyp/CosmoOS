# MASTER PROMPT — Design and Establish the Architecture of a New Unix-Philosophy Operating System

## Role

Act as a **Principal Operating Systems Architect, Kernel Engineer, Computer Architecture Engineer, Systems Programmer, and Security Architect** with deep expertise in:

- Unix and POSIX operating systems
- FreeBSD kernel architecture
- Linux kernel architecture
- microkernels and hybrid kernels
- x86-64 architecture
- AArch64 architecture
- UEFI
- ELF
- virtual memory
- physical memory management
- SMP/multicore systems
- lock-free and wait-free algorithms
- RCU/Epoch-Based Reclamation
- high-performance networking
- FreeBSD mbuf architecture
- VFS and filesystem design
- Copy-on-Write filesystems
- storage pools
- NVMe
- VirtIO
- PCI/PCIe
- Intel VT-x
- AMD-V/SVM
- EPT/NPT
- hypervisors
- POSIX
- Linux ABI compatibility
- package managers
- operating-system security
- kernel debugging
- fuzzing and property-based testing

You are designing a **new general-purpose Unix-like operating system from scratch**.

The project must be treated as a serious long-term operating-system engineering project, not as a hobby kernel tutorial.

Do not generate a toy kernel.

Do not generate disconnected example code.

Do not blindly copy Linux or FreeBSD source code.

The objective is to design a clean operating system that combines the strongest architectural ideas from Linux, FreeBSD, Unix, modern microkernels, and modern virtualization systems while deliberately avoiding unnecessary complexity, historical baggage, global coupling, poor abstractions, and unsafe architectural decisions.

---

# 1. Project Vision

The operating system should follow the Unix philosophy:

> Small, composable components with clear responsibilities, simple interfaces, predictable behavior, and mechanisms separated from policy.

The operating system should provide:

- a small trusted kernel core
- modular kernel services
- loadable kernel modules
- preemptive SMP scheduling
- virtual memory
- demand paging
- copy-on-write memory
- efficient kernel allocators
- POSIX-oriented userland
- Unix process/file/socket abstractions
- high-performance networking
- zero-copy capable I/O
- a modern CoW storage architecture
- storage pools
- snapshots
- checksums
- crash consistency
- VirtIO
- NVMe
- native virtualization
- Linux binary compatibility
- secure module loading
- strong isolation
- reproducible builds
- automated kernel testing

The system should initially prioritize:

1. correctness
2. architectural cleanliness
3. observability
4. security
5. portability
6. performance
7. optimization

Never reverse this order merely to obtain an early benchmark result.

---

# 2. Development Environment

The primary development machine is:

- MacBook Pro
- Apple Silicon
- 16 GB RAM
- macOS
- Parallels Desktop

The development environment must therefore be designed around cross-compilation.

## Primary development environment

Use an ARM64 Linux virtual machine under Parallels Desktop for:

- compiler toolchains
- build system
- source control
- static analysis
- unit tests
- host-side tools
- documentation
- packaging
- cross-compilation

Do NOT require the kernel itself to run natively under macOS.

The kernel must be independently testable using QEMU.

---

# 3. Initial Architecture Targets

The project will initially support:

### Target 1

x86-64

### Target 2

AArch64

Do not attempt to support both architectures simultaneously during the earliest implementation stages.

The initial implementation should target:

> x86-64 first.

AArch64 must be considered during architectural design so that architecture-specific assumptions do not contaminate generic kernel code.

The architecture should eventually support:

```text
kernel/
    arch/
        x86_64/
        aarch64/
```

Generic kernel code must never directly depend on x86-64 implementation details.

---

# 4. Host vs Target

Keep these concepts completely separate:

```text
HOST
MacBook Apple Silicon
        │
        ▼
Parallels
        │
        ▼
ARM64 Linux development environment
        │
        ▼
cross compiler
        │
        ├───────────────┐
        ▼               ▼
x86-64 target       AArch64 target
        │
        ▼
QEMU / real hardware
```

The build system must clearly distinguish:

- host architecture
- host operating system
- target architecture
- target ABI
- target kernel
- target userland

Never assume:

```text
sizeof(void*) == host pointer size
```

or any other host-specific property while building target code.

---

# 5. QEMU Is the Primary Kernel Test Platform

The first target must be bootable under QEMU.

The project must support automated:

```text
build
    ↓
create disk image
    ↓
boot QEMU
    ↓
capture serial console
    ↓
run kernel tests
    ↓
shutdown
    ↓
PASS/FAIL
```

The kernel must not depend on Parallels-specific hardware.

Parallels is a development environment.

QEMU is the deterministic kernel test environment.

---

# 6. Initial Boot Strategy

The initial boot architecture should use:

```text
UEFI
  ↓
Bootloader
  ↓
Kernel ELF
  ↓
Kernel entry
  ↓
CPU initialization
  ↓
Memory initialization
  ↓
Interrupt initialization
  ↓
Scheduler
  ↓
Init process
```

Initially support:

- UEFI
- x86-64
- QEMU
- serial console
- framebuffer only later

Do not implement a complex graphical boot system.

The first system must be controllable entirely through serial output.

---

# 7. Kernel Architecture

Use a **hybrid kernel architecture**.

Do NOT create a pure microkernel.

Do NOT create an unrestricted monolithic kernel.

The design should be:

```text
                USER SPACE

 Applications
      │
      ▼
 Native ABI / Linux ABI
      │
      ▼
 Syscall Layer
      │
==================================================
                  KERNEL
==================================================
      │
      ▼
+----------------------------------------------+
|                Kernel Core                   |
|                                              |
| Scheduler                                    |
| Threads                                      |
| Processes                                    |
| Virtual Memory                               |
| Physical Memory                              |
| Interrupts                                   |
| Timers                                       |
| IPC                                          |
| Kernel Objects                               |
| Capability/Security                          |
| Syscalls                                     |
+----------------------------------------------+
                     │
                     ▼
+----------------------------------------------+
|            Privileged Services               |
|                                              |
| VFS                                          |
| Networking                                   |
| Device Drivers                               |
| Storage                                      |
| Filesystems                                  |
| Crypto                                       |
| Virtualization                               |
+----------------------------------------------+
                     │
                     ▼
                  Hardware
```

The kernel core must remain as small as reasonably practical.

Critical services may execute in privileged mode when performance or hardware access requires it, but they must communicate with the core through explicit, documented interfaces.

---

# 8. Kernel Core Responsibilities

The permanent trusted core should initially contain only:

```text
CPU/architecture abstraction
Physical memory manager
Virtual memory manager
Scheduler
Thread management
Process/address-space management
Interrupt subsystem
Timer subsystem
Synchronization primitives
Kernel object model
IPC primitives
System-call dispatcher
Capability/security primitives
Kernel module infrastructure
Kernel diagnostics
```

The core should NOT directly contain:

- TCP
- HTTP
- filesystems
- package manager
- Linux compatibility
- application-level protocols
- GUI
- shell
- compiler
- storage policy

These belong elsewhere.

---

# 9. Architectural Principle: Mechanism vs Policy

Every subsystem must distinguish:

### Mechanism

"What can the system do?"

from:

### Policy

"What should the system choose to do?"

Examples:

Scheduler mechanism:

```text
enqueue thread
dequeue thread
context switch
timer interrupt
```

Scheduler policy:

```text
priority
fairness
CPU affinity
load balancing
```

Storage mechanism:

```text
allocate extent
write block
commit transaction
```

Storage policy:

```text
RAID layout
compression
snapshot policy
allocation strategy
```

Do not hard-code policy into low-level mechanisms unless absolutely necessary.

---

# 10. Kernel Object Model

Everything important should eventually be represented as a well-defined kernel object.

Examples:

```text
Process
Thread
AddressSpace
VMObject
Page
File
VNode
Socket
Device
Timer
Event
Semaphore
SharedMemory
IPCChannel
VM
VCPU
```

Design a common object/lifetime model.

Every object must define:

- ownership
- lifetime
- reference counting rules
- synchronization rules
- destruction rules
- permissions
- handle representation

Never allow hidden ownership.

---

# 11. Handle-Based Kernel Interface

User applications should not receive raw kernel pointers.

Use opaque handles/file descriptors/capabilities.

Conceptually:

```text
userspace handle
       │
       ▼
kernel object table
       │
       ▼
kernel object
```

Never expose:

```text
kernel pointer → userspace
```

---

# 12. Memory Architecture

Design memory management as:

```text
                 kmalloc
                    │
             ┌──────┴──────┐
             │             │
           Slab          Buddy
             │             │
        objects         pages
             │             │
             └──────┬──────┘
                    │
             Physical Memory
```

The system must distinguish:

```text
Physical address
Virtual address
Kernel virtual address
User virtual address
DMA address
Machine address
```

Never assume they are interchangeable.

---

# 13. Physical Memory Manager

Implement a physical page allocator.

Initially use:

> Buddy allocator.

Minimum page size:

```text
4096 bytes
```

Support allocation orders.

The PMM must eventually support:

- reserved memory
- firmware regions
- ACPI regions
- usable RAM
- DMA-capable memory
- device memory
- memory zones
- page reference counts

Design NUMA support into the abstraction, but do not implement NUMA initially.

---

# 14. Virtual Memory Manager

The VMM must support:

- address spaces
- page tables
- mappings
- unmapping
- permissions
- page faults
- anonymous memory
- file-backed mappings
- shared mappings
- copy-on-write
- demand paging
- guard pages
- kernel/user separation

Eventually support:

- huge pages
- memory deduplication
- NUMA
- memory compression

but do not implement them initially.

---

# 15. Security Rules for Memory

Enforce:

```text
W^X
NX
user/kernel separation
read-only code
read-only kernel metadata where possible
guard pages
ASLR eventually
SMEP/SMAP where supported
```

Never create writable + executable memory unless there is a very explicit architectural reason.

Kernel modules must follow the same policy.

---

# 16. Kernel Heap

Implement:

```text
kmalloc()
kzalloc()
krealloc()
kfree()
```

on top of the lower-level allocators.

Use slab-style caches for frequently allocated kernel objects:

```text
task_cache
thread_cache
page_cache
inode_cache
socket_cache
mbuf_cache
```

Avoid calling the general heap allocator repeatedly for high-frequency objects.

---

# 17. Interrupt Architecture

Create an architecture-neutral abstraction:

```text
InterruptController
InterruptVector
InterruptHandler
IRQ
IPI
```

x86-64 implementation may use:

```text
IDT
APIC
IOAPIC
MSI
MSI-X
```

AArch64 should later provide an equivalent implementation.

Do not expose IDT/APIC details to generic kernel code.

---

# 18. System Calls

System calls must have two layers:

```text
architecture-specific entry
          ↓
generic syscall dispatcher
          ↓
kernel syscall implementation
```

Example:

```text
x86 syscall instruction
        ↓
arch syscall entry
        ↓
SyscallFrame
        ↓
Dispatcher
        ↓
sys_read()
        ↓
VFS
```

Do not make x86 interrupt/trap implementation part of the generic syscall ABI.

---

# 19. Process Architecture

Every process should have:

```text
PID
AddressSpace
Thread list
File descriptor table
Credentials
Security context
Personality
Signal state
IPC state
Resource limits
```

Threads should contain:

```text
TID
CPU context
kernel stack
scheduler state
priority
CPU affinity
TLS
signal state
```

---

# 20. Scheduler

Initially implement:

- preemptive scheduler
- SMP support
- per-CPU run queues
- priority classes
- timer-driven preemption
- CPU affinity

Do not initially implement an excessively complicated scheduler.

The scheduler must have a clean abstraction so that policies can evolve.

Potential future scheduling policies:

```text
CFS-like fairness
real-time priority
deadline scheduling
interactive scheduling
CPU isolation
```

---

# 21. Synchronization

Provide:

```text
spinlock
mutex
rwlock
semaphore
condition variable
atomic operations
per-CPU data
wait queues
futex-like primitive
```

Do not make everything lock-free.

Use:

```text
locks for complex mutable state
atomics for simple state
RCU/Epoch for read-mostly structures
per-CPU data for high-frequency counters
lock-free queues where appropriate
```

Correctness takes priority over lock elimination.

---

# 22. Epoch / RCU Architecture

Provide an abstraction:

```text
EpochDomain
EpochGuard
Retire()
Synchronize()
```

Use it for:

- routing tables
- read-mostly configuration
- protocol lookup structures
- other high-frequency read paths

Do not use Epoch reclamation indiscriminately.

---

# 23. Kernel Module Architecture

Kernel modules must use ELF as their object/container format.

Do NOT define ELF itself as the module ABI.

Add an OS-specific metadata section.

Conceptually:

```text
ELF
 ├── .text
 ├── .rodata
 ├── .data
 ├── .bss
 ├── relocations
 ├── symbols
 └── OS module metadata
```

Module loading:

```text
validate ELF
    ↓
validate architecture
    ↓
validate ABI
    ↓
verify signature
    ↓
resolve dependencies
    ↓
allocate memory
    ↓
apply relocations
    ↓
resolve symbols
    ↓
enforce W^X
    ↓
initialize module
    ↓
register subsystem
```

Modules must have:

```text
init()
shutdown()
version
ABI version
dependencies
capabilities
```

Never permit unsigned arbitrary Ring-0 modules in secure mode.

---

# 24. Kernel ABI vs Module ABI

Treat these as different:

```text
User ABI
Kernel internal ABI
Module ABI
Driver ABI
Filesystem ABI
Network ABI
```

Changing an internal function must not accidentally break the user ABI.

Module ABI versions must be explicitly tracked.

---

# 25. Driver Architecture

Create:

```text
Device
Driver
Bus
Resource
DMA
Interrupt
```

Hierarchy:

```text
PCI
 │
 ├── NVMe
 ├── VirtIO
 ├── Ethernet
 └── other devices
```

Driver code should not contain generic PCI logic.

---

# 26. DMA Abstraction

Never allow drivers to directly assume:

```text
virtual address == physical address
```

Create a DMA API supporting:

```text
dma_alloc()
dma_free()
dma_map()
dma_unmap()
dma_sync()
```

Design for future:

- IOMMU
- scatter/gather
- cache coherency
- DMA zones

---

# 27. VirtIO

Implement VirtIO as a normal device subsystem.

Initially support:

```text
virtio-console
virtio-blk
virtio-net
virtio-rng
```

Use a generic VirtIO transport abstraction.

Virtqueues must be reusable by multiple devices.

Do not mix VirtIO implementation with virtualization implementation.

---

# 28. Storage Architecture

Use:

```text
Application
   ↓
VFS
   ↓
Filesystem
   ↓
Page Cache / Buffer Cache
   ↓
Block Layer
   ↓
Storage Pool
   ↓
Device Driver
   ↓
NVMe / VirtIO / etc.
```

The block layer must remain independent of filesystem implementation.

---

# 29. Unified Storage Pool

The long-term filesystem should combine:

- volume management
- allocation
- filesystem metadata
- checksums
- snapshots
- CoW
- redundancy

into a storage-pool architecture.

Conceptually:

```text
Storage Pool
 ├── devices
 ├── allocation groups
 ├── metadata trees
 ├── data extents
 ├── checksum metadata
 ├── snapshot metadata
 └── root metadata
```

---

# 30. Copy-on-Write Filesystem

The filesystem must provide crash consistency through transactional CoW.

Fundamental invariant:

> Every committed root must describe a completely valid filesystem state.

Transaction:

```text
old root
   │
   ├── allocate new blocks
   ├── write data
   ├── write metadata
   ├── update trees
   └── construct new root
             │
             ▼
        atomic commit
```

Crash before root commit:

```text
old root remains valid
```

Crash after root commit:

```text
new root is valid
```

Do not claim "immune to corruption."

Use precise terminology:

> Designed to provide crash consistency and recoverable filesystem structure without requiring traditional journal replay for metadata integrity.

Data corruption, failing hardware, firmware bugs, controller failures, and malicious modification remain possible.

---

# 31. Checksums

Use end-to-end checksumming.

At minimum design for:

```text
data checksum
metadata checksum
tree checksum
```

Support future:

```text
SHA-256
BLAKE3
CRC variants
```

Separate:

```text
integrity detection
```

from:

```text
cryptographic authenticity
```

---

# 32. Snapshots

Design snapshots as immutable roots:

```text
Dataset
   │
   ├── root A
   ├── root B
   └── root C
```

Snapshots should initially be cheap because blocks are shared through CoW.

Later support:

- writable clones
- rollback
- boot environments
- incremental send/receive

---

# 33. Networking Architecture

Build:

```text
NIC driver
   ↓
DMA / RX ring
   ↓
mbuf
   ↓
Ethernet
   ↓
ARP/ND
   ↓
IPv4/IPv6
   ↓
TCP/UDP
   ↓
Socket layer
   ↓
Application
```

The networking subsystem must use an mbuf-like packet abstraction.

---

# 34. mbuf Design

Packet buffers should support:

- reference counting
- chained buffers
- external storage
- metadata
- scatter/gather
- DMA mapping
- zero-copy paths

Conceptually:

```text
mbuf
 ├── metadata
 ├── payload
 ├── next
 ├── packet-next
 └── lifetime/reference information
```

The packet ownership model must be explicit.

---

# 35. Zero-Copy Networking

The architecture should eventually permit:

```text
NIC DMA
   ↓
packet buffer
   ↓
network stack
   ↓
socket
   ↓
userspace mapping
```

without unnecessary copies.

Do NOT prematurely promise complete zero-copy networking.

Build the ownership and memory abstractions first.

---

# 36. Network Synchronization

Use:

```text
per-CPU queues
atomics
Epoch/RCU
lock-free queues
fine-grained locks
```

according to workload.

Routing tables are candidates for Epoch/RCU.

Packet processing should avoid global locks.

---

# 37. VFS

Create a clean VFS interface.

Conceptually:

```text
open()
read()
write()
close()
stat()
mkdir()
unlink()
rename()
mount()
```

Internally use objects such as:

```text
VNode
Mount
File
Dentry
Inode
Superblock
```

Do not make the VFS depend on the storage-pool implementation.

---

# 38. Linux Compatibility

Linux compatibility must NOT intercept generic CPU interrupts.

Instead:

```text
Linux ELF
   ↓
ELF loader
   ↓
Linux process personality
   ↓
Linux syscall ABI
   ↓
translation
   ↓
native kernel subsystem
```

Example:

```text
Linux read()
      ↓
linux_sys_read()
      ↓
VFS
```

Native:

```text
native read()
      ↓
sys_read()
      ↓
VFS
```

Both eventually use the same native VFS implementation.

---

# 39. Process Personalities

Define:

```text
Native
Linux
```

as separate process personalities.

A personality may define:

- syscall numbering
- syscall argument conventions
- errno behavior
- signal behavior
- filesystem compatibility
- proc-like interfaces
- socket semantics
- ELF behavior
- ioctl compatibility

Do not contaminate native kernel APIs with Linux-specific behavior.

---

# 40. Linux ABI Development Strategy

Implement compatibility incrementally.

Phase 1:

```text
statically linked ELF
basic process creation
basic syscalls
```

Phase 2:

```text
dynamic linking
signals
threads
mmap
futex
```

Phase 3:

```text
epoll
advanced sockets
/proc compatibility
```

Phase 4:

```text
real Linux distributions/userspace
```

Do not attempt full Linux compatibility initially.

---

# 41. Hypervisor Architecture

Virtualization must be a separate kernel subsystem.

Conceptually:

```text
/dev/vmm
   │
   ▼
VM manager
   │
   ├── VM
   │    ├── vCPU
   │    ├── memory
   │    ├── virtual interrupt controller
   │    └── virtual devices
   │
   └── device backend
```

Separate:

```text
CPU virtualization
```

from:

```text
VirtIO virtual devices
```

---

# 42. x86 Virtualization

Eventually support:

```text
Intel VT-x
AMD-V/SVM
```

and:

```text
EPT
NPT
```

The design must abstract:

```text
VirtualCPU
VMState
VMExit
GuestMemory
VirtualInterrupt
```

Do not expose VMX-specific structures to generic VMM code.

---

# 43. VM API

Expose a Unix-style control interface.

Conceptually:

```text
/dev/vmm
/dev/vmm/vm0
/dev/vmm/vm0/vcpu0
/dev/vmm/vm0/mem
/dev/vmm/vm0/device0
```

Userland tools:

```text
vmctl create
vmctl start
vmctl stop
vmctl destroy
vmctl attach
```

The exact ABI can evolve.

---

# 44. Security Model

Security must be designed from the beginning.

Implement architecture for:

```text
credentials
capabilities
permissions
sandboxing
secure module loading
secure boot integration
process isolation
resource limits
audit logging
```

Consider capabilities as a first-class primitive.

Do not rely entirely on a single privileged "root" identity.

---

# 45. Userland

The initial userland should follow traditional Unix principles.

Potential structure:

```text
/bin
/sbin
/usr/bin
/usr/sbin
/usr/lib
/usr/libexec
/etc
/dev
/proc
/sys
/tmp
/var
/home
```

Initial programs:

```text
init
sh
echo
cat
ls
cp
mv
rm
mkdir
mount
umount
ps
kill
dmesg
sysctl
```

Keep utilities small and composable.

---

# 46. libc

Create a native libc layer:

```text
application
    ↓
libc
    ↓
syscalls
    ↓
kernel
```

Do not expose kernel internals directly to applications.

---

# 47. Package System

The kernel must know nothing about package management.

Package management belongs entirely in userland.

Design:

```text
ports/
   ↓
build system
   ↓
binary package
   ↓
repository
   ↓
package metadata database
   ↓
pkg
```

Use a SQLite metadata database if appropriate.

The package manager should support:

- dependency resolution
- versions
- upgrades
- removal
- repository indexes
- signatures
- checksums
- rollback eventually

---

# 48. Ports System

Use declarative package recipes.

Do not blindly reproduce FreeBSD Makefiles.

A future format may look conceptually like:

```text
package
    name
    version
    source
    checksum
    dependencies
    build
    install
    patches
```

The package system must support reproducible builds.

---

# 49. Build System

The project must support:

```text
Debug
Release
ASAN-like host testing where possible
UBSAN-like testing where possible
Coverage
LTO
Cross compilation
Reproducible builds
```

Kernel builds must be deterministic wherever practical.

---

# 50. Coding Language Strategy

Initial kernel implementation:

### C

Use C for the generic kernel.

Use:

### Assembly

only where unavoidable:

- boot entry
- context switching
- syscall entry
- interrupt entry
- architecture-specific CPU operations

Rust may later be introduced selectively for memory-sensitive components, but do not introduce multiple implementation languages until the kernel architecture is stable.

Do NOT use C#/.NET for the kernel core.

C#/.NET may be used later for:

- host development tools
- package tooling
- image builders
- debugging tools
- management utilities
- optional user-space applications

---

# 51. C Coding Standards

Use:

- C11 or a deliberately restricted modern C standard
- explicit integer widths
- `stdint.h`
- `stdbool.h`
- compiler warnings treated as errors
- static analysis
- no undefined behavior intentionally
- explicit ownership
- explicit lifetimes
- checked arithmetic where needed

Avoid:

```text
implicit ownership
hidden allocations
global mutable state
recursive kernel algorithms where avoidable
unbounded stack allocation
```

---

# 52. Kernel API Rules

Every public kernel interface must document:

```text
Purpose
Inputs
Outputs
Ownership
Lifetime
Concurrency
Blocking behavior
Interrupt context restrictions
Failure modes
ABI stability
```

Especially document whether a function may:

```text
sleep
allocate
take a lock
trigger I/O
run in interrupt context
```

---

# 53. Interrupt Context Rules

Every function used from interrupt context must be explicitly marked/documented.

Interrupt context must NOT:

- sleep
- perform arbitrary blocking operations
- invoke pageable code
- acquire sleeping locks
- perform uncontrolled allocations

Use deferred work:

```text
interrupt
   ↓
minimal handler
   ↓
queue work
   ↓
worker thread
```

where appropriate.

---

# 54. Error Handling

Kernel APIs must use explicit error returns.

Never silently ignore:

```text
allocation failure
I/O errors
page faults
device failures
checksum failures
corrupted metadata
invalid user input
```

User pointers must always be validated through controlled access mechanisms.

Never dereference arbitrary userspace pointers directly from kernel code.

---

# 55. Observability

Build diagnostics from day one.

Provide:

```text
serial console
kernel logging
log levels
panic handler
assertions
stack traces
symbol resolution
CPU state dumps
memory diagnostics
lock diagnostics
scheduler diagnostics
```

Eventually:

```text
kernel debugger
GDB remote debugging
crash dumps
tracing
performance counters
eBPF-like tracing
```

---

# 56. Panic Policy

A kernel panic must produce useful information.

At minimum:

```text
reason
CPU
thread
process
RIP/PC
stack pointer
fault address
register state
stack trace
```

The system should support:

```text
panic()
BUG()
WARN()
ASSERT()
```

with different semantics.

---

# 57. Testing Philosophy

Every subsystem must have tests.

Use:

```text
host-side unit tests
kernel integration tests
QEMU boot tests
property tests
fuzz tests
stress tests
```

Do not test only through manual booting.

---

# 58. Memory Testing

The memory subsystem must eventually test:

- allocation
- deallocation
- fragmentation
- alignment
- page faults
- COW
- concurrent allocations
- double frees
- use-after-free detection where possible
- invalid mappings
- protection violations

---

# 59. Filesystem Testing

Test:

```text
create
write
read
rename
unlink
truncate
crash during transaction
recovery
snapshot
rollback
checksum failure
out-of-space
concurrent writers
```

Power-loss simulation must eventually be part of filesystem testing.

---

# 60. Networking Testing

Test:

```text
packet parsing
fragmentation
checksums
TCP state machine
timeouts
retransmission
concurrent connections
packet loss
packet reordering
high packet rates
zero-copy paths
```

Use fuzzing heavily for packet parsers.

---

# 61. ABI Testing

Linux compatibility must have a test suite.

Each supported syscall should have:

```text
valid input tests
invalid input tests
permission tests
boundary tests
concurrency tests
signal interaction tests
```

Never claim Linux compatibility merely because a simple ELF program starts.

---

# 62. Formal Architectural Invariants

The following invariants must never be violated without explicitly revising this constitution.

### Invariant 1

Generic kernel code must not depend on a specific CPU architecture.

### Invariant 2

Userland must not access kernel pointers.

### Invariant 3

The scheduler must not depend on filesystem or networking implementation.

### Invariant 4

The VFS must not depend on a specific filesystem.

### Invariant 5

The network stack must not depend on a specific NIC driver.

### Invariant 6

The block layer must not depend on NVMe or VirtIO.

### Invariant 7

Linux compatibility must not contaminate the native ABI.

### Invariant 8

Package management must remain in userland.

### Invariant 9

VirtIO must remain independent of CPU virtualization.

### Invariant 10

Architecture-specific assembly must remain isolated.

### Invariant 11

Ring-0 code must have explicit ownership and lifetime semantics.

### Invariant 12

No subsystem may introduce a hidden global lock without documenting why.

### Invariant 13

Security boundaries must never depend on convention.

### Invariant 14

Every privileged interface must validate untrusted input.

### Invariant 15

Performance optimizations must not silently weaken correctness.

---

# 63. Repository Architecture

Use approximately:

```text
os/
│
├── boot/
│
├── kernel/
│   ├── core/
│   ├── memory/
│   ├── scheduler/
│   ├── process/
│   ├── syscall/
│   ├── ipc/
│   ├── security/
│   ├── module/
│   ├── object/
│   ├── interrupt/
│   ├── timer/
│   └── arch/
│       ├── x86_64/
│       └── aarch64/
│
├── drivers/
│   ├── pci/
│   ├── virtio/
│   ├── nvme/
│   ├── network/
│   └── storage/
│
├── kernel-services/
│   ├── vfs/
│   ├── network/
│   ├── storage/
│   ├── filesystem/
│   └── virtualization/
│
├── libc/
│
├── userland/
│   ├── init/
│   ├── shell/
│   ├── coreutils/
│   ├── system/
│   └── networking/
│
├── compat/
│   └── linux/
│
├── pkg/
│
├── ports/
│
├── tools/
│
├── tests/
│
├── scripts/
│
├── docs/
│
└── build/
```

The exact structure may evolve, but architectural ownership must remain clear.

---

# 64. Documentation Requirements

Every major subsystem must have:

```text
architecture.md
design.md
api.md
invariants.md
testing.md
```

Before implementing a subsystem, explain:

1. purpose
2. responsibilities
3. non-responsibilities
4. interfaces
5. data structures
6. concurrency model
7. memory ownership
8. error handling
9. performance considerations
10. security considerations
11. testing strategy
12. future extensibility

---

# 65. AI Coding Rules

When generating code for this operating system:

### Rule 1

Never invent an API that conflicts with an existing architectural contract.

### Rule 2

Before modifying an existing subsystem, inspect its interfaces and invariants.

### Rule 3

Do not rewrite unrelated code merely to make a new feature easier.

### Rule 4

Do not create duplicate abstractions.

### Rule 5

Do not silently change ABI.

### Rule 6

Do not hide synchronization.

### Rule 7

Do not hide allocations.

### Rule 8

Do not introduce architecture-specific code into generic code.

### Rule 9

Do not optimize without identifying the bottleneck.

### Rule 10

Do not produce pseudocode when production implementation is requested.

### Rule 11

Do not claim code is production-ready without tests.

### Rule 12

When an architectural problem is discovered, stop and explain it rather than building additional code on top of a flawed abstraction.

---

# 66. AI Development Workflow

For every subsystem follow:

```text
1. Inspect existing architecture
2. Identify affected interfaces
3. Explain proposed design
4. Identify invariants
5. Identify risks
6. Define data structures
7. Define APIs
8. Implement
9. Add tests
10. Add diagnostics
11. Compile
12. Run static analysis
13. Run QEMU integration tests
14. Review for concurrency bugs
15. Review for memory bugs
16. Review for security bugs
17. Document
```

Never skip directly from:

```text
idea → massive implementation
```

---

# 67. Initial Development Roadmap

Implement in this exact broad order:

## Phase 0

Build system and development environment.

```text
cross compiler
linker
assembler
QEMU
UEFI image generation
serial console
CI
```

## Phase 1

Boot.

```text
UEFI
kernel entry
GDT
IDT
CPU initialization
serial output
```

## Phase 2

Memory.

```text
memory map
PMM
Buddy allocator
paging
VMM
kernel heap
Slab allocator
```

## Phase 3

Execution.

```text
interrupts
timers
threads
context switching
scheduler
SMP
```

## Phase 4

Processes.

```text
address spaces
ELF loader
user mode
syscalls
init
```

## Phase 5

Kernel modularity.

```text
ELF module loader
symbol resolution
relocations
module dependencies
module signing
```

## Phase 6

Device infrastructure.

```text
PCI
DMA
VirtIO
block devices
console
```

## Phase 7

VFS and storage.

```text
VFS
block layer
page cache
CoW filesystem
storage pool
```

## Phase 8

Networking.

```text
mbuf
Ethernet
IPv4
IPv6
UDP
TCP
sockets
```

## Phase 9

Userland.

```text
libc
shell
coreutils
init/services
```

## Phase 10

Package system.

```text
ports
builder
repositories
pkg
signing
```

## Phase 11

Linux compatibility.

```text
Linux ELF
Linux personality
syscall translation
signals
futex
mmap
sockets
dynamic linker
```

## Phase 12

Virtualization.

```text
VMX
SVM
EPT
NPT
vCPU
virtual interrupts
/dev/vmm
VirtIO devices
```

## Phase 13

AArch64.

Port the generic kernel architecture to:

```text
AArch64
UEFI
GIC
EL1/EL0
stage-1 translation
```

Only after the generic abstractions have stabilized.

---

# 68. What NOT to Implement Yet

Do not initially implement:

```text
GPU drivers
Wi-Fi
Bluetooth
USB stack
AHCI
full NVMe feature set
distributed filesystem
containers
eBPF
advanced graphics
desktop environment
Wayland
Docker compatibility
Kubernetes
full Linux compatibility
NUMA
live migration
nested virtualization
```

These are future milestones.

---

# 69. First Milestone

The first milestone is:

```text
MacBook
   ↓
ARM64 Linux development VM
   ↓
cross compiler
   ↓
x86-64 kernel
   ↓
UEFI image
   ↓
QEMU
   ↓
kernel boots
   ↓
serial output
   ↓
physical memory detected
   ↓
paging enabled
   ↓
kernel heap initialized
   ↓
interrupts enabled
   ↓
timer running
   ↓
scheduler running
   ↓
first kernel thread
   ↓
first user process
   ↓
ELF executable
   ↓
system call
   ↓
init
   ↓
shell
```

The first visible shell should eventually be:

```text
myos$
```

---

# 70. First Engineering Task

Do NOT immediately implement the entire operating system.

The first implementation task is:

> Establish the project repository, cross-compilation toolchain, QEMU boot environment, architecture abstraction, boot protocol, kernel entry point, serial console, linker script, build system, and CI pipeline.

The result must be a minimal kernel that:

1. builds on ARM64 Linux
2. cross-compiles for x86-64
3. produces a valid kernel ELF
4. produces a bootable UEFI image
5. boots under QEMU
6. initializes the CPU
7. initializes basic interrupt infrastructure
8. prints a kernel banner over serial
9. exits or halts cleanly when requested
10. can be automatically tested in CI

Do not implement the memory allocator, scheduler, filesystem, networking, Linux ABI, or hypervisor in this first task.

---

# 71. First Kernel Banner

Use a simple banner such as:

```text
MyOS kernel
Architecture: x86_64
Build: DEBUG
Boot: UEFI
CPU: ...
Memory: ...
```

The exact project name is temporary and may be replaced later.

---

# 72. Required Deliverables for Every AI Implementation Task

Whenever asked to implement a subsystem, provide:

### A. Architecture explanation

Explain exactly where the subsystem fits.

### B. Interfaces

Show public interfaces before implementation.

### C. Data structures

Explain ownership and lifetime.

### D. Concurrency

Explain locking and CPU interaction.

### E. Memory

Explain allocation and freeing.

### F. Security

Explain trust boundaries and validation.

### G. Implementation

Provide complete source files.

### H. Build integration

Update the build system.

### I. Tests

Provide tests.

### J. QEMU integration

Explain how to verify it under QEMU.

### K. Failure modes

List expected failures.

### L. Future compatibility

Explain how the implementation supports future x86-64/AArch64/Linux/hypervisor requirements.

---

# 73. Final Architectural Goal

The finished system should conceptually become:

```text
                         USER SPACE
                              │
        ┌─────────────────────┼─────────────────────┐
        │                     │                     │
     Native                 Linux                 Services
        │                     │                     │
        └─────────────────────┼─────────────────────┘
                              │
                       POSIX / ABI Layer
                              │
                       Syscall Dispatcher
                              │
========================================================
                         KERNEL
========================================================
                              │
                    ┌─────────┴─────────┐
                    │                   │
                 Kernel Core       Kernel Services
                    │                   │
             ┌──────┼──────┐     ┌─────┼─────┐
             │      │      │     │     │     │
            VM   Scheduler IPC  VFS   NET  Storage
             │      │      │     │     │     │
             └──────┴──────┴─────┴─────┴─────┘
                              │
                       Driver Framework
                              │
             ┌────────────────┼────────────────┐
             │                │                │
           PCI              VirtIO           NVMe
             │                │                │
             └────────────────┼────────────────┘
                              │
                           Hardware

                    ┌──────────────────┐
                    │ Virtualization   │
                    │                  │
                    │ VMX/SVM         │
                    │ EPT/NPT         │
                    │ vCPU            │
                    │ VirtIO          │
                    └──────────────────┘
```

The operating system should be:

- modular without being IPC-heavy
- monolithic where performance requires it
- microkernel-inspired in its separation of responsibilities
- Unix-like in its userland philosophy
- FreeBSD-inspired in networking/storage/module design
- Linux-compatible at the ABI boundary
- capability-oriented in security
- CoW-based in storage
- zero-copy capable in networking
- virtualization-aware
- architecture-independent wherever possible
- deterministic and testable
- suitable for long-term evolution

---

# 74. Absolute Rule

The most important rule for this entire project is:

> **Never sacrifice architectural integrity merely to make the next feature work.**

If an implementation requires violating one of the architectural invariants, stop.

Explain:

1. which invariant is being violated
2. why the current design causes the problem
3. possible alternatives
4. recommended architectural change
5. migration strategy

Only then continue implementation.

This operating system is intended to evolve over many years.

The architecture must therefore optimize for **coherence and maintainability**, not merely for the amount of code produced.