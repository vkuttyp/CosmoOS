# COSMOOS — PROMPT #2
## Post-Roadmap Architecture Audit, Correctness Review, Performance Engineering & Next-Generation Roadmap

---

## ROLE

Act as a Principal Operating Systems Architect, Kernel Engineer, Computer Architecture Engineer, Networking Engineer, Storage Engineer, Hypervisor Engineer, Security Engineer, and OS Performance Engineer.

You have extensive practical experience with:

- FreeBSD
- Linux
- NetBSD
- OpenBSD
- illumos
- Unix/POSIX
- microkernels
- hybrid kernels
- SMP
- NUMA
- x86-64
- AArch64
- UEFI
- ELF
- virtual memory
- memory allocators
- RCU
- Epoch-Based Reclamation
- lock-free algorithms
- high-performance networking
- FreeBSD mbuf
- Linux SKB
- io_uring
- DPDK
- VFS
- ZFS
- HAMMER2
- Btrfs
- storage pools
- NVMe
- PCIe
- VirtIO
- Intel VT-x
- AMD-V/SVM
- EPT/NPT
- KVM
- bhyve
- capability security
- sandboxing
- Linux ABI compatibility
- kernel fuzzing
- formal verification
- crash consistency
- filesystem testing
- performance profiling

You are now working on an existing operating-system project:

> **CosmoOS**

Repository:

> https://github.com/vkuttyp/CosmoOS

---

# 1. MOST IMPORTANT INSTRUCTION

This is an EXISTING project.

Do NOT start over.

Do NOT redesign the project from scratch.

Do NOT regenerate subsystems that already exist.

Do NOT replace working implementations merely because you personally prefer a different architecture.

First inspect the repository thoroughly.

Treat the current source code as authoritative implementation reality.

Treat the existing architecture constitution as the governing design document unless you identify a concrete contradiction, correctness problem, security flaw, or scalability limitation.

---

# 2. FIRST ACTION — REPOSITORY FORENSICS

Before writing or modifying code, inspect the repository.

You MUST inspect at minimum:

```text
README.md

prompts/

docs/

boot/

kernel/

kernel/arch/

kernel/memory/

kernel/scheduler/

kernel/process/

kernel/syscall/

kernel/ipc/

kernel/security/

kernel/module/

drivers/

kernel-services/

compat/

libc/

userland/

pkg/

ports/

tests/

tools/

scripts/

build/

.github/workflows/
```

Also inspect:

```text
Makefile
build configuration
CI configuration
QEMU scripts
test harness
linker scripts
architecture-specific source
public kernel headers
ABI definitions
module ABI definitions
filesystem on-disk structures
networking structures
virtualization interfaces
```

Do not assume that README descriptions perfectly match the implementation.

Determine what actually exists.

---

# 3. ESTABLISH THE CURRENT BASELINE

Produce a factual implementation matrix.

For every subsystem determine:

```text
Subsystem
Status
Implementation location
Public API
Major data structures
Concurrency model
Memory ownership model
Architecture dependencies
Tests
Known limitations
Potential correctness issues
Potential scalability issues
```

Use this structure:

```text
Subsystem | Status | Implementation | Tests | Limitations
```

Do not invent missing functionality.

If README claims a feature exists but source code does not support the claim, explicitly report the discrepancy.

---

# 4. CURRENT COSMOOS ARCHITECTURE

The current architecture is approximately:

```text
                         USER SPACE

Native applications          Linux applications
        │                           │
        ▼                           ▼
   Native ABI                 Linux personality
        │                           │
        └─────────────┬─────────────┘
                      ▼
                 Syscall layer
                      │
===================================================
                    KERNEL
===================================================
                      │
          ┌───────────┴───────────┐
          │                       │
      Kernel Core          Kernel Services
          │                       │
          │        ┌──────────────┼──────────────┐
          │        │              │              │
          ▼        ▼              ▼              ▼
         VM       VFS           Network        Storage
          │        │              │              │
          └────────┴──────────────┴──────────────┘
                           │
                     Driver framework
                           │
                ┌──────────┼──────────┐
                ▼          ▼          ▼
               PCI       VirtIO      NVMe
                           │
                        Hardware
```

Preserve this general architecture unless the audit demonstrates a compelling reason to change it.

---

# 5. DO NOT CONFUSE "HYBRID" WITH "MONOLITHIC"

CosmoOS intentionally allows high-performance services to execute in privileged mode.

However, maintain strict boundaries between:

```text
kernel core
kernel services
drivers
filesystems
networking
compatibility layers
userland
```

A component running in Ring 0 does NOT automatically become part of the kernel core.

Preserve explicit interfaces.

---

# 6. PRIMARY OBJECTIVE OF THIS PROMPT

The initial development roadmap is complete.

The next phase is:

> **Turn the functional prototype into a robust, scalable, high-performance operating system.**

Focus on:

1. correctness
2. memory safety
3. concurrency correctness
4. security
5. crash consistency
6. SMP scalability
7. I/O scalability
8. networking performance
9. storage performance
10. virtualization correctness
11. ABI completeness
12. observability
13. hardware support
14. maintainability
15. performance optimization

Do not simply add more features.

First make the existing architecture stronger.

---

# 7. PHASE A — DEEP ARCHITECTURAL AUDIT

Audit the following subsystems.

## Memory

Inspect:

```text
PMM
Buddy allocator
Page descriptors
VMM
Kernel address-space layout
Page tables
Page faults
Slab
kmalloc
DMA
MMIO
TLB management
COW
```

Look for:

- races
- fragmentation
- unnecessary locking
- incorrect alignment
- integer overflow
- refcount bugs
- use-after-free
- double free
- stale mappings
- TLB invalidation errors
- SMP issues
- cache coherency problems
- DMA lifetime problems
- incorrect page ownership
- memory leaks
- impossible allocation paths
- deadlocks
- interrupt-context allocation problems

Determine whether the current allocator architecture can scale to:

```text
1 CPU
4 CPUs
16 CPUs
64 CPUs
256 CPUs
```

without fundamental redesign.

---

# 8. VMM AUDIT

Inspect:

```text
AddressSpace
VMRegion
Page tables
Page faults
COW
mmap
unmap
protection changes
user/kernel split
```

Determine whether the design supports future:

```text
ASLR
shared libraries
file-backed mappings
huge pages
NUMA
shared memory
memory pressure
swap
KSM-like deduplication
```

Do not implement those yet.

Determine whether the current abstractions can support them without breaking the ABI.

---

# 9. SMP AND SCHEDULER AUDIT

Inspect:

```text
run queues
per-CPU state
scheduler policy
IPI
TLB shootdown
cross-CPU scheduling
timers
load balancing
CPU affinity
preemption
interrupt interaction
```

Determine whether the scheduler has:

```text
global locks
global queues
cache-line contention
false sharing
unnecessary IPIs
poor CPU migration behavior
priority inversion
starvation
timer scalability problems
```

Measure rather than speculate.

Design benchmarks for:

```text
1 CPU
2 CPU
4 CPU
8 CPU
16 CPU
```

---

# 10. LOCKING AUDIT

Create a kernel lock inventory.

For every lock record:

```text
lock name
subsystem
type
owner
protected state
may sleep?
interrupt context?
IRQ-safe?
CPU-local?
contention risk
lock ordering
```

Detect potential cycles.

Produce a lock-order graph.

Any potential deadlock must be explicitly reported.

---

# 11. EPOCH / RCU AUDIT

Determine where Epoch-Based Reclamation would materially improve scalability.

Candidates may include:

```text
routing tables
network configuration
device lookup
filesystem namespace
VFS caches
process lookup
read-mostly kernel objects
```

Do NOT convert everything to Epoch/RCU.

For every proposed use explain:

```text
read path
write path
retirement
grace period
memory reclamation
latency implications
```

---

# 12. KERNEL OBJECT / HANDLE AUDIT

Inspect:

```text
kobject
handle table
reference counting
object destruction
file descriptors
process handles
socket handles
VM handles
device handles
```

Look for:

- ABA problems
- stale handles
- generation-counter issues
- refcount overflow
- destruction races
- close/use races
- double destruction
- leaked references

Determine whether handles are safe under concurrent operations.

---

# 13. SYSCALL ABI AUDIT

Inventory every native syscall.

For each:

```text
number
name
arguments
return value
error semantics
blocking behavior
memory ownership
concurrency behavior
ABI stability
```

Check:

- 32/64-bit assumptions
- structure packing
- pointer validation
- integer truncation
- time types
- size types
- alignment
- extensibility

Do not change existing syscall numbers without an explicit ABI migration strategy.

---

# 14. USER POINTER SAFETY

Audit every kernel path receiving userspace pointers.

Look for:

```text
TOCTOU
invalid pointer access
length overflow
integer overflow
partial copy
fault handling
kernel pointer exposure
```

Create common primitives such as:

```text
copy_from_user()
copy_to_user()
copy_string_from_user()
validate_user_range()
```

if they do not already exist.

Do not duplicate unsafe pointer-validation code across subsystems.

---

# 15. VFS AUDIT

Inspect:

```text
VNode
File
Dentry
Mount
Path resolution
Page cache
Open
Read
Write
Rename
Unlink
Mount
Unmount
```

Look for:

- namespace races
- rename races
- unlink/open races
- reference lifetime bugs
- page-cache coherency
- locking hierarchy
- deadlocks
- mount lifetime bugs

Determine whether the VFS can support:

```text
multiple filesystems
network filesystems
overlay filesystem
pseudo filesystems
procfs
sysfs
devfs
```

without redesign.

---

# 16. COSMOFS AUDIT

Treat the filesystem as a serious storage engine.

Inspect:

```text
superblocks
root selection
inode map
extent allocation
metadata blocks
transactions
CoW
CRC32C
free-space management
crash recovery
```

Verify the invariant:

> A committed root always represents a structurally valid filesystem state.

Test:

```text
power loss
partial writes
torn superblocks
metadata corruption
out-of-space
concurrent writers
repeated snapshots
large files
large directories
fragmentation
```

Do not claim "corruption immunity."

Use precise terminology.

---

# 17. STORAGE POOL ROADMAP

The current implementation uses a single-member storage pool.

Design the next-generation storage pool architecture.

Do NOT immediately implement RAID.

First define:

```text
Pool
Device
Vdev
Allocation Group
Extent
Transaction Group
Metadata Tree
Checksum
Snapshot Root
```

Design for future:

```text
mirror
RAID-Z-like parity
striping
device replacement
scrubbing
resilvering
hot spares
compression
deduplication
encryption
snapshots
clones
send/receive
```

Do not implement all of them in this phase.

Create the correct abstractions first.

---

# 18. STORAGE TRANSACTION ENGINE

Evaluate whether the current transaction model can evolve into a transaction-group architecture.

Target:

```text
Application writes
       ↓
Dirty state
       ↓
Transaction group
       ↓
CoW allocation
       ↓
Metadata updates
       ↓
Checksum
       ↓
Flush/FUA
       ↓
New root
       ↓
Atomic root commit
```

Explicitly account for:

```text
volatile device caches
flush ordering
FUA
barriers
power-loss protection
```

Never assume that a successful write means data is safely persistent.

---

# 19. NETWORKING AUDIT

Inspect:

```text
mbuf
clusters
ownership
RX queue
TX path
Ethernet
ARP
IPv4
IPv6
TCP
UDP
sockets
```

Measure:

```text
packets/sec
bytes/sec
latency
CPU usage
copy count
allocation rate
lock contention
cache misses
```

---

# 20. MBUF NEXT GENERATION

Evaluate whether the mbuf design can support:

```text
jumbo frames
scatter/gather
checksum offload
TSO
LRO
RSS
multiqueue NICs
zero-copy receive
zero-copy transmit
DMA recycling
XDP-like fast path
```

Do not copy Linux SKB or FreeBSD mbuf literally.

Design a CosmoOS-specific abstraction optimized for:

```text
ownership clarity
zero-copy
DMA
SMP
cache locality
safe lifetime management
```

---

# 21. NETWORK MULTI-QUEUE ARCHITECTURE

The current networking path uses a receive worker architecture.

Determine how it should evolve toward:

```text
NIC
 │
 ├── RX queue 0 → CPU 0
 ├── RX queue 1 → CPU 1
 ├── RX queue 2 → CPU 2
 └── RX queue N → CPU N
```

Evaluate:

```text
RSS
RPS
XPS
per-CPU queues
NAPI-like polling
interrupt moderation
busy polling
```

Do not introduce complexity unless benchmarks demonstrate benefit.

---

# 22. TCP AUDIT

Review TCP against modern requirements.

Check:

```text
RFC correctness
retransmission
RTO
fast retransmit
congestion control
window scaling
SACK
timestamps
ECN
PMTUD
keepalive
TIME_WAIT
listen backlog
socket buffers
zero-copy
```

Prioritize correctness before performance.

---

# 23. ASYNC I/O

Design a unified asynchronous I/O model.

Evaluate whether CosmoOS should eventually have an abstraction inspired by:

```text
io_uring
kqueue
epoll
AIO
```

Do not copy any one API.

Design a native model based on:

```text
submission
completion
buffer ownership
registered resources
polling
event notification
```

The design must work for:

```text
files
sockets
devices
timers
IPC
VM operations
```

---

# 24. DRIVER ARCHITECTURE AUDIT

Review:

```text
PCI
DMA
interrupts
VirtIO
NVMe
device lifecycle
driver probing
device removal
resource ownership
```

Determine whether drivers can eventually support:

```text
MSI-X
multiple queues
NUMA locality
IOMMU
hotplug
power management
reset/recovery
```

---

# 25. IOMMU

Design an IOMMU abstraction.

Do not implement immediately unless needed.

Support future:

```text
Intel VT-d
AMD IOMMU
ARM SMMU
```

The abstraction should support:

```text
domain
device assignment
I/O virtual address
mapping
unmapping
DMA isolation
```

This is important for:

- device security
- virtualization
- DMA protection
- future userspace drivers

---

# 26. NVMe

The next storage driver should eventually be NVMe.

Before implementation design:

```text
controller
admin queue
submission queue
completion queue
namespace
PRP
SGL
MSI-X
queue affinity
DMA
reset
timeout
error recovery
```

Target:

```text
multi-queue
per-CPU/per-NUMA queue ownership
minimal locking
batched I/O
```

---

# 27. VIRTUALIZATION AUDIT

The current hypervisor is AMD-V/NPT based.

Do not replace it.

Instead audit:

```text
VM lifecycle
vCPU lifecycle
guest memory
nested paging
VM exits
CPUID
MSR handling
I/O
interrupt injection
shutdown
device backends
```

Identify missing correctness requirements.

---

# 28. VIRTUALIZATION BACKEND ABSTRACTION

The abstraction must support:

```text
AMD SVM
Intel VMX
AArch64 EL2
```

Conceptually:

```text
Generic VMM
      │
      ├── AMD SVM backend
      ├── Intel VMX backend
      └── ARM EL2 backend
```

Never expose VMX/SVM-specific structures to generic VMM code.

---

# 29. EPT/NPT CORRECTNESS

Audit:

```text
guest physical memory
host physical memory
nested page tables
permissions
dirty tracking
TLB invalidation
large pages
MMIO
```

Determine whether the design can support:

```text
ballooning
snapshot
live migration
dirty page tracking
device passthrough
IOMMU
```

in the future.

---

# 30. LINUX ABI AUDIT

The current Linux compatibility layer is stage 1.

Do NOT claim full Linux compatibility.

Create a compatibility matrix:

```text
Linux subsystem | supported | partial | missing | priority
```

Include:

```text
syscalls
signals
threads
futex
clone
execve
dynamic linker
TLS
/proc
/sys
epoll
poll
select
ioctl
mmap
shared memory
pipes
eventfd
timerfd
signalfd
sockets
netlink
filesystem semantics
```

---

# 31. LINUX COMPATIBILITY PRINCIPLE

Maintain:

```text
Linux ABI
     ↓
Linux personality
     ↓
CosmoOS native kernel services
```

Do NOT fork the native VFS/networking/memory implementations merely for Linux compatibility.

Linux-specific behavior belongs inside:

```text
compat/linux/
```

where practical.

---

# 32. ELF / DYNAMIC LINKING

The current system has static Linux compatibility.

The next major milestone should be:

```text
dynamic ELF loading
```

Design:

```text
ELF interpreter
PT_INTERP
dynamic linker
PLT
GOT
relocations
TLS
shared libraries
```

Native and Linux dynamic linking must remain distinct where their ABI requirements differ.

---

# 33. SIGNALS

The current Linux compatibility implementation stores signal state but does not fully deliver signals.

Design a complete signal architecture.

Support:

```text
signal
sigaction
sigprocmask
sigpending
sigsuspend
sigwait
```

Correctly handle:

```text
signal frame
user/kernel transition
register restoration
alternate stack
restartable syscalls
thread-directed signals
process-directed signals
```

---

# 34. FUTEX

Audit the existing futex primitive.

It must support correct:

```text
wait
wake
requeue
timeout
memory ordering
priority interaction
spurious wakeups
```

Evaluate whether it is sufficient for:

```text
pthreads
glibc-like runtimes
musl
modern Linux applications
```

---

# 35. AARCH64 SECOND-STAGE PLAN

The AArch64 stage 1 implementation already exists.

Do not redesign the x86-64 kernel.

Implement missing AArch64 functionality behind existing architecture interfaces.

Prioritize:

```text
Linux AArch64 ABI
GICv3
ASIDs
TLBI
FP/SIMD user context
EL2 virtualization
```

Only add architectural interfaces when genuinely required.

---

# 36. CPU FEATURES

Create an architecture capability framework.

Expose generic capabilities such as:

```text
has_nx
has_smep
has_smap
has_pcid
has_1g_pages
has_invpcid
has_rdrand
has_rdseed
has_tsc_deadline
has_iommu
has_virtualization
```

Do not scatter CPU feature checks throughout the generic kernel.

---

# 37. CACHE AND NUMA DESIGN

Design for future NUMA.

Do not implement full NUMA immediately.

However, avoid architectural assumptions such as:

```text
all CPUs share identical memory latency
all memory is local
one global allocator is always sufficient
```

Introduce abstractions for:

```text
CPU topology
memory node
NUMA distance
CPU locality
allocation policy
```

---

# 38. SECURITY HARDENING

Perform a full security audit.

Review:

```text
kernel modules
ELF loader
syscalls
user pointers
handles
filesystem
network stack
VM
hypervisor
DMA
device access
capabilities
credentials
```

Look specifically for:

```text
integer overflow
out-of-bounds access
use-after-free
double free
TOCTOU
refcount overflow
race conditions
privilege escalation
kernel pointer leaks
information disclosure
uninitialized memory
DMA attacks
```

---

# 39. MODULE SECURITY

The existing signed-module architecture must remain.

Audit:

```text
signature verification
key management
ELF parsing
relocation
symbol resolution
dependency loading
module unloading
reference counting
W^X
```

A malformed module must never cause:

```text
memory corruption
arbitrary code execution before verification
kernel panic
use-after-free
```

---

# 40. KERNEL MODULE UNLOAD

Pay special attention to unload.

A module cannot be unloaded while:

```text
function pointers
interrupt handlers
work queues
timers
objects
drivers
network callbacks
filesystem operations
```

still reference it.

Design a robust module lifetime model.

Consider:

```text
module reference
call guard
RCU/epoch
quiescence
dependency references
```

---

# 41. DEVICE HOTPLUG

Design future support for:

```text
device add
device remove
driver bind
driver unbind
device reset
```

Do not implement all hotplug features immediately.

But ensure current device lifetimes don't make future hotplug impossible.

---

# 42. OBSERVABILITY 2.0

The kernel now needs production-grade observability.

Design:

```text
structured kernel tracing
per-CPU trace buffers
event IDs
timestamps
CPU IDs
thread IDs
process IDs
```

Potential future tools:

```text
ktrace
kstat
ktraceview
cosmo-top
cosmo-prof
```

---

# 43. PERFORMANCE COUNTERS

Create a generic performance-counter abstraction.

Eventually support:

```text
CPU cycles
instructions
cache misses
branch misses
TLB misses
context switches
interrupts
page faults
scheduler latency
network packets
I/O latency
```

Use architecture-specific implementations behind a generic API.

---

# 44. BENCHMARK SUITE

Create a benchmark suite.

### Memory

```text
kmalloc throughput
page allocation
page fault latency
mmap
COW
```

### Scheduler

```text
context switch
thread wakeup
IPC
CPU migration
```

### Networking

```text
UDP packets/sec
TCP throughput
TCP latency
loopback
multi-CPU scaling
```

### Storage

```text
sequential read
sequential write
random read
random write
fsync latency
metadata operations
```

### Virtualization

```text
VM exit latency
vCPU throughput
guest memory throughput
VirtIO throughput
```

Every optimization must be justified using these benchmarks.

---

# 45. PERFORMANCE REGRESSION SYSTEM

Every major kernel change should be able to compare:

```text
baseline
vs
new implementation
```

Record:

```text
throughput
latency
CPU usage
memory usage
lock contention
```

Do not optimize based purely on intuition.

---

# 46. FUZZING

Expand fuzzing to:

```text
ELF parser
module loader
filesystem metadata
filesystem transactions
network packet parsers
syscall arguments
VFS paths
Linux ABI structures
VirtIO descriptors
PCI configuration
ACPI tables
```

A fuzzing input must never be trusted.

---

# 47. FAULT INJECTION

Build fault injection facilities.

Simulate:

```text
allocation failure
I/O timeout
I/O error
packet loss
packet duplication
packet reordering
corrupted metadata
CPU starvation
interrupt storms
device reset
VM exit storms
```

This is especially important for:

```text
filesystem
network
drivers
hypervisor
```

---

# 48. CRASH CONSISTENCY TESTING

Build an automated filesystem crash harness:

```text
create image
    ↓
run workload
    ↓
inject crash at controlled point
    ↓
restart
    ↓
mount
    ↓
verify invariants
```

Run thousands/millions of iterations where practical.

Never rely solely on manually tested crash scenarios.

---

# 49. STORAGE CORRUPTION MODEL

Distinguish:

```text
power loss
software crash
kernel panic
torn write
bad sector
silent data corruption
malicious modification
device firmware failure
```

For each specify:

```text
detect?
recover?
repair?
report?
```

Do not promise guarantees the implementation cannot provide.

---

# 50. FILESYSTEM CHECKSUM POLICY

Determine whether CRC32C remains appropriate for:

```text
metadata
```

and whether stronger checksums should eventually be available for:

```text
data
```

Possible architecture:

```text
ChecksumAlgorithm
```

with implementations such as:

```text
CRC32C
BLAKE3
SHA-256
```

Do not replace CRC32C merely for marketing reasons.

Evaluate CPU cost and error-detection requirements.

---

# 51. COMPRESSION

Design future compression support.

Potential:

```text
LZ4
Zstandard
```

Compression must integrate with:

```text
extent
checksum
CoW
snapshot
read path
write path
```

Do not implement until the extent/storage abstractions are ready.

---

# 52. ENCRYPTION

Design filesystem encryption architecture.

Possible layers:

```text
device encryption
pool encryption
dataset encryption
file encryption
```

Do not blindly copy ZFS/Btrfs semantics.

Explicitly define:

```text
key hierarchy
key storage
key rotation
metadata encryption
data encryption
integrity
boot-time unlock
```

---

# 53. CONTAINERS

Containers are intentionally deferred.

Before implementing them, determine whether the kernel primitives are sufficient:

```text
namespaces
resource limits
capabilities
process isolation
filesystem isolation
network isolation
cgroups-like accounting
```

Design the primitives independently of container tooling.

---

# 54. CAPABILITY SECURITY

Strengthen the existing capability architecture.

Eventually allow:

```text
process
 ├── file capability
 ├── network capability
 ├── device capability
 ├── IPC capability
 └── VM capability
```

Avoid making all privileged operations dependent on UID 0.

---

# 55. SERVICE MANAGER

Design a minimal Unix-style service supervisor.

Requirements:

```text
start
stop
restart
dependency
logging
restart policy
resource limits
supervision
```

Do not reproduce systemd blindly.

Follow the Unix philosophy.

---

# 56. /proc AND /sys EQUIVALENTS

Design pseudo-filesystems for observability.

Possible:

```text
/proc
/sys
/dev
```

But define clear ownership.

Do not turn pseudo-filesystems into an uncontrolled dumping ground for kernel internals.

---

# 57. USERLAND STRATEGY

Keep the base system small.

Prefer:

```text
small utilities
simple interfaces
pipes
streams
files
sockets
```

Avoid unnecessary monolithic userland components.

---

# 58. PORTS AND PACKAGE SYSTEM

The existing package system should evolve toward:

```text
reproducible builds
binary repositories
package signatures
SBOM
dependency locking
build sandboxing
cross compilation
multiple architectures
```

Package management remains entirely outside the kernel.

---

# 59. SOURCE COMPATIBILITY

The long-term goal should be:

```text
POSIX applications
      ↓
native libc
      ↓
CosmoOS
```

and:

```text
Linux ELF applications
      ↓
Linux compatibility
      ↓
CosmoOS
```

Do not expose Linux-specific APIs in the native ABI merely to make compatibility easier.

---

# 60. HARDWARE ROADMAP

Prioritize real hardware in this order:

```text
NVMe
Intel/AMD modern NIC
USB
AHCI
IOMMU
GPU later
Wi-Fi later
Bluetooth later
```

Do not implement every historical hardware interface.

---

# 61. REAL HARDWARE TESTING

The QEMU test environment remains mandatory.

But eventually create a hardware test matrix:

```text
QEMU x86-64
QEMU AArch64
AMD real hardware
Intel real hardware
Apple Silicon AArch64 where feasible
```

Never assume QEMU behavior exactly matches physical hardware.

---

# 62. BUILD SYSTEM

Audit:

```text
cross compilation
reproducibility
debug builds
release builds
LTO
sanitizers
static analysis
dependency tracking
```

The build must work from a clean checkout.

---

# 63. MAC DEVELOPMENT ENVIRONMENT

The developer's primary machine is:

```text
MacBook Pro
Apple Silicon
16 GB RAM
macOS
Parallels Desktop
```

The supported development architecture is:

```text
macOS
 ↓
Parallels
 ↓
ARM64 Linux
 ↓
LLVM/Clang/LLD
 ↓
CosmoOS cross build
 ↓
QEMU
```

Do not require x86 hardware for development.

Do not make Parallels hardware-specific behavior part of CosmoOS.

QEMU remains the deterministic test environment.

---

# 64. RESOURCE CONSTRAINT

The development machine has 16 GB RAM.

Therefore:

- avoid unnecessarily large VMs
- avoid huge build parallelism
- avoid memory-heavy fuzzing defaults
- avoid running multiple heavyweight VMs simultaneously
- provide configurable worker counts
- provide lightweight QEMU configurations
- provide memory-conscious CI configurations

The kernel itself must not be architecturally constrained by the developer machine's 16 GB RAM.

---

# 65. QEMU MATRIX

Maintain at least:

```text
x86-64 / 1 CPU
x86-64 / 4 CPU
x86-64 / 8 CPU

AArch64 / 1 CPU
AArch64 / 4 CPU
```

Add larger CPU counts when useful.

Test:

```text
boot
memory
SMP
filesystem
network
Linux ABI
virtualization
```

---

# 66. QEMU AS A HARDWARE SIMULATOR

Use QEMU to deliberately exercise:

```text
low memory
multiple CPUs
interrupts
PCI
VirtIO
NVMe emulation where appropriate
network loss
disk errors
device reset
```

Do not optimize only for the happy path.

---

# 67. ARCHITECTURE REVIEW OUTPUT

After inspecting the repository, produce:

## A. Current Architecture Diagram

Show actual architecture.

## B. Subsystem Inventory

Show all major subsystems.

## C. Correctness Findings

Rank:

```text
CRITICAL
HIGH
MEDIUM
LOW
```

## D. Security Findings

Rank similarly.

## E. Performance Findings

Rank similarly.

## F. Architectural Debt

List abstractions that will become problematic later.

## G. Recommended Roadmap

Prioritize concrete next steps.

---

# 68. DO NOT IMPLEMENT EVERYTHING IN ONE PASS

This is extremely important.

After completing the audit:

STOP.

Do not immediately modify the entire repository.

Instead produce:

```text
NEXT SUBSYSTEM
```

with:

```text
problem
current implementation
why it matters
proposed design
affected files
new APIs
migration plan
tests
benchmarks
risks
```

Then wait for the next implementation instruction.

---

# 69. NEXT-GENERATION DEVELOPMENT ORDER

Unless repository inspection identifies a more urgent correctness problem, prioritize approximately:

```text
1. Correctness/security audit
2. Kernel object lifetime hardening
3. SMP/locking scalability
4. Async I/O architecture
5. Advanced filesystem transaction engine
6. NVMe
7. Multi-queue networking
8. zero-copy networking
9. Linux dynamic linking
10. Linux signals/futex completeness
11. IOMMU
12. Intel VMX
13. AArch64 Linux ABI
14. AArch64 EL2 virtualization
15. snapshots/clones
16. storage redundancy
17. filesystem compression
18. encryption
19. containers
20. advanced hardware
```

This order may change after source inspection.

---

# 70. ARCHITECTURAL QUALITY GATE

Before implementing any new subsystem, answer:

### Correctness

Can this design be proven internally consistent?

### Concurrency

What can run concurrently?

### Ownership

Who owns every resource?

### Lifetime

When can it be destroyed?

### Failure

What happens when allocation/I/O/device operations fail?

### Security

What is the trust boundary?

### Performance

Where are the hot paths?

### Scalability

What happens at 1, 4, 16, 64, 256 CPUs?

### Portability

Does this work on x86-64 and AArch64?

### Testing

How can it be tested deterministically?

If any answer is unclear, stop before implementation.

---

# 71. NEVER USE THESE ARGUMENTS

Do not justify a design merely with:

```text
Linux does it
FreeBSD does it
ZFS does it
bhyve does it
```

Use existing systems as references, not authorities.

The correct question is:

> Why is this design appropriate for CosmoOS?

---

# 72. BORROW IDEAS, NOT ACCIDENTAL COMPLEXITY

CosmoOS may borrow:

### From FreeBSD

```text
mbuf
VFS
jails concepts
Capsicum concepts
ZFS concepts
bhyve concepts
ports philosophy
```

### From Linux

```text
SMP techniques
RCU
scheduler ideas
io_uring concepts
KVM concepts
driver ecosystem concepts
networking techniques
```

### From microkernels

```text
small trusted core
capabilities
explicit interfaces
fault isolation concepts
```

But do not blindly reproduce historical implementation complexity.

---

# 73. PERFORMANCE PHILOSOPHY

The target is not:

> fastest benchmark at any cost.

The target is:

> predictable high performance with strong correctness and clean architecture.

Prefer:

```text
cache locality
per-CPU data
batching
zero-copy
lock avoidance
asynchronous I/O
NUMA locality
efficient allocation
```

before:

```text
clever lock-free algorithms
excessive atomics
global optimization
premature assembly
```

---

# 74. CODE REVIEW STANDARD

Every change must be reviewed for:

```text
correctness
race conditions
memory ownership
integer overflow
ABI compatibility
security
performance
architecture boundaries
test coverage
documentation
```

A feature is incomplete until all nine have been addressed.

---

# 75. NO TOY IMPLEMENTATIONS

Never use:

```text
TODO
FIXME
dummy return
fake success
placeholder syscall
silent failure
unimplemented stub
```

in production paths unless explicitly marked and intentionally accepted.

If functionality cannot yet be safely implemented:

```text
return a documented error
```

rather than pretending success.

---

# 76. NO MASSIVE AI REWRITES

Do not rewrite entire subsystems unless:

1. the current implementation is fundamentally incorrect
2. the current abstraction prevents required functionality
3. a migration plan exists
4. tests cover the old behavior
5. the replacement is demonstrably superior

Prefer incremental refactoring.

---

# 77. REQUIRED SOURCE-LEVEL EVIDENCE

When reporting an architectural problem, cite:

```text
file
function
structure
line range where available
```

Do not make vague statements such as:

> "The scheduler might have contention."

Instead:

> "scheduler X protects run queue Y with lock Z; enqueue/dequeue occurs on CPUs A/B; this creates a shared synchronization point under workload W."

---

# 78. FINAL DELIVERABLE OF THIS PROMPT

After inspecting the repository, provide exactly these sections:

```text
1. Executive Summary

2. Current Architecture

3. Implemented Subsystems

4. Correctness Audit

5. Memory Audit

6. SMP/Scheduler Audit

7. Synchronization Audit

8. VFS/Filesystem Audit

9. Networking Audit

10. Driver/DMA Audit

11. Virtualization Audit

12. Linux ABI Audit

13. AArch64 Audit

14. Security Audit

15. Performance Audit

16. Testing/Fuzzing Audit

17. Architectural Debt

18. Critical Findings

19. Recommended Next 10 Milestones

20. NEXT SUBSYSTEM
```

For `NEXT SUBSYSTEM`, provide exactly one recommended subsystem.

---

# 79. IMPLEMENTATION RULE

Do NOT implement the next subsystem during this audit.

The first response from this prompt must be an architecture/code audit and roadmap only.

After the audit, wait for the next instruction.

---

# 80. FINAL PRINCIPLE

CosmoOS is no longer a bootloader experiment.

It is an operating-system engineering project.

The objective from this point forward is not:

```text
"add more features"
```

but:

```text
working prototype
       ↓
correct kernel
       ↓
scalable kernel
       ↓
secure kernel
       ↓
high-performance kernel
       ↓
production-quality operating system
```

Every future change must move CosmoOS toward that goal.

Never trade architectural integrity for rapid AI-generated code.