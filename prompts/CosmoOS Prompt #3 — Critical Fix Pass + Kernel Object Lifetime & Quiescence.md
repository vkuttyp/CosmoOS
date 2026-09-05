# CosmoOS Prompt #3 — Critical Fix Pass + Kernel Object Lifetime & Quiescence

## Mission

You are now moving from architecture audit into **production-grade kernel engineering**.

The previous architecture audit was intentionally read-only. That audit is now complete and has been merged into the repository.

Your task is to:

1. Re-read the complete post-roadmap architecture audit.
2. Reproduce and verify every CRITICAL finding against the current source tree.
3. Implement a carefully scoped **Critical-Fix Pass**.
4. After all critical fixes are complete and validated, implement the recommended:
   **Kernel Object Lifetime and Quiescence Hardening subsystem**.
5. Add comprehensive regression tests, stress tests, assertions, and documentation.
6. Do not start unrelated roadmap work.

The repository is the authoritative source of truth. Do not assume that the audit is perfectly correct; verify every finding against the current code before changing it.

---

# 1. Repository and Architectural Context

This is CosmoOS, a Unix-philosophy operating system built from scratch.

The architecture is a **hybrid kernel**:

- small trusted kernel core
- modular privileged services
- loadable kernel modules
- native userland
- POSIX-oriented interfaces
- Linux ABI compatibility at the boundary
- mbuf-based networking
- CoW storage
- native virtualization
- x86-64 first
- AArch64 designed into the architecture from the beginning

The architectural priorities are:

1. correctness
2. architectural cleanliness
3. observability
4. security
5. portability
6. performance
7. optimization

Do not violate these priorities for implementation convenience.

Read the governing documents under `prompts/` before modifying code.

Read:

- repository README
- governing architecture prompt
- post-roadmap architecture audit under `docs/audit/`
- relevant subsystem documentation
- current tests
- build scripts
- QEMU harness
- CI configuration

---

# 2. Critical Rule: Do Not Trust the Audit Blindly

For every CRITICAL finding:

1. Locate the exact code.
2. Understand the complete execution path.
3. Confirm whether the vulnerability/bug still exists in the current `main`.
4. Determine whether the audit's proposed fix is architecturally correct.
5. Identify all callers and affected architectures.
6. Implement the smallest architecturally correct fix.
7. Add a regression test where possible.
8. Run the existing test suite.
9. Run relevant QEMU tests.
10. Run sanitizers/host tests where applicable.
11. Document the invariant that prevents recurrence.

Do not blindly patch the line mentioned by the audit.

A local patch that leaves another equivalent race elsewhere is not acceptable.

---

# 3. Phase A — Critical-Fix Pass

Implement the CRITICAL findings first.

The current audit identifies the following major critical areas.

---

## 3.1 TCP: `seg_mss` / Locking Violation

Investigate the TCP path where `seg_mss` requires a mutex while the TCP spinlock is held with interrupts disabled.

Determine:

- exact lock dependency
- interrupt context requirements
- whether the mutex can sleep
- whether the value can be cached
- whether it belongs in immutable connection state
- whether it can be calculated outside the spinlocked section
- whether a lock hierarchy violation exists

The final implementation must guarantee:

- no sleeping lock while holding an IRQ-disabled spinlock
- no deadlock
- no race on MSS-related state
- correct behavior under concurrent TCP activity
- correct behavior on SMP

Add a regression test that exercises the affected path.

Also search the entire networking subsystem for the same anti-pattern:

> sleeping operation while holding a spinlock or IRQ-disabled critical section

Do not limit the fix to the exact reported function.

---

# 3.2 VirtIO Descriptor Validation

Audit the VirtIO queue implementation, especially descriptor-chain traversal.

The audit reports that `virtq_add` trusts device-writable:

`desc[].next`

This must be treated as hostile/device-controlled state.

Never allow a malformed descriptor chain to:

- escape the queue bounds
- loop indefinitely
- reference invalid descriptors
- cause memory corruption
- cause an infinite traversal
- cause an out-of-bounds access

Design a robust descriptor-chain validator.

Requirements:

- validate descriptor index
- validate `next`
- detect cycles
- enforce a bounded traversal
- validate descriptor ownership/state
- validate length/address constraints where required
- preserve zero-copy operation
- avoid unnecessary overhead in the trusted fast path when safe
- distinguish construction-time validation from device-return validation

Do not merely add an arbitrary iteration limit without understanding the VirtIO ownership model.

Add malformed-descriptor tests.

Test:

- self-loop
- two-element loop
- long chain
- out-of-range `next`
- invalid descriptor state
- boundary descriptor
- empty chain
- normal multi-descriptor chain

---

# 3.3 FPU/SIMD State Isolation

This is a critical architectural issue.

The audit reports that FPU/SIMD state is not properly saved/restored across:

- process/process context switches
- guest/host transitions

and that CR4.OSFXSR handling is incomplete.

Do a complete audit of:

- CPU initialization
- CR0
- CR4
- FPU ownership
- XSAVE/FXSAVE/FXRSTOR
- XMM/YMM state
- XSAVE feature discovery
- context switching
- interrupt entry/exit
- kernel use of floating-point/SIMD
- virtualization entry/exit
- guest FPU state
- lazy versus eager FPU switching

Do not implement an unsafe partial FPU design.

The architecture must clearly define:

### Kernel rule

Kernel code must not accidentally use user FPU/SIMD state.

### Process rule

A process must never observe another process's FPU/SIMD registers.

### Guest rule

A guest must never observe host/process FPU/SIMD state.

### Context-switch rule

FPU ownership must be explicitly represented and synchronized with scheduler context switching.

### Architecture rule

x86-64 implementation must be correct now.

AArch64 must have a clean architectural abstraction for its eventual FP/SIMD state handling rather than introducing x86-specific assumptions into generic scheduler/process code.

If full XSAVE support is required by the current CPU configuration, implement it correctly rather than silently assuming legacy FXSAVE is sufficient.

Add tests capable of detecting register-state leakage.

For example:

- process A writes recognizable XMM state
- switch to process B
- process B writes different state
- switch back
- verify A's state

Also test guest/host transitions where the existing virtualization test infrastructure permits.

---

# 3.4 LAPIC ICR Race

Audit the APIC IPI implementation.

The audit reports that the LAPIC ICR write pair is not atomic against local interrupts.

Understand the architecture:

- xAPIC versus x2APIC assumptions
- ICR high/low register ordering
- interrupt masking
- concurrent local interrupt handlers
- concurrent IPI senders
- SMP startup
- TLB shootdown
- scheduler IPIs

Design a proper abstraction for sending IPIs.

The implementation must guarantee that an unrelated interrupt cannot corrupt the ICR write sequence where the hardware model requires atomicity.

Do not simply disable interrupts globally for an unnecessarily large region.

Keep the critical section as small as architecturally possible.

Audit all callers.

Add SMP stress coverage.

---

# 3.5 NMI / Machine Check Exception Entry

Audit exception entry for:

- NMI
- machine check
- double fault
- page fault
- general protection
- debug exceptions
- IRQs

The audit identifies missing IST handling and SWAPGS-related windows.

Treat NMI and #MC as fundamentally different from ordinary interrupts.

Verify:

- IST configuration
- TSS setup
- per-CPU exception stacks
- SWAPGS correctness
- GS-base assumptions
- nested exception behavior
- entry from user mode
- entry from kernel mode
- entry during scheduler activity
- entry during interrupt-disabled sections

The design must be safe against:

- NMI arriving during SWAPGS-sensitive entry/exit windows
- nested faults
- stack corruption
- use of an invalid kernel stack
- recursive exception failure

Do not paper over the issue with arbitrary masking. NMI cannot simply be treated as an ordinary maskable IRQ.

Document the entry/exit invariants.

---

# 3.6 Process Access Control / UID Model

The audit reports that every process currently effectively runs as UID 0.

This is a security-critical architectural issue.

Do not implement a superficial UID field merely to make the audit disappear.

Design the process credential model so it can support:

- real UID
- effective UID
- saved UID if required
- supplementary groups
- capability-style privilege model
- root-equivalent privileges
- future user namespaces if compatible with the architecture

For now, implement only what the current OS actually needs.

The security boundary must be explicit.

Audit:

- process creation
- executable loading
- file access
- device access
- `/dev/vmm`
- module loading
- privileged syscalls
- kill
- ptrace-like functionality if present
- IPC
- filesystem operations

Do not break the native ABI.

Add tests proving an unprivileged process cannot perform privileged operations.

---

# 3.7 Module-Signing Private Key

The audit reports that the module-signing private key is tracked in Git.

This is unacceptable.

Immediately remove the private signing material from the repository.

Do NOT simply rename the file.

Do NOT replace it with another committed private key.

Establish a proper model:

- public verification key may be compiled into the kernel
- private signing key exists only outside the repository
- development signing must use an explicitly generated developer key
- production/release signing must be externally controlled
- CI must never accidentally expose private signing material
- build scripts must clearly distinguish development and release signing

If the repository history contains the private key, determine the appropriate remediation and document it.

Do not claim the problem is fixed merely because the current working tree no longer contains the key.

---

# 4. Critical-Fix Completion Gate

After implementing all critical fixes:

STOP.

Do not begin the lifetime subsystem until the following are true:

- all seven critical findings are addressed
- existing tests pass
- new regression tests pass
- x86-64 QEMU boot works
- SMP QEMU tests pass
- AArch64 build/tests remain healthy
- no new sanitizer failures
- no new compiler warnings
- lock-order documentation remains consistent
- architecture documentation is updated
- security-sensitive changes are documented

Run the complete repository test suite.

Then perform a second targeted audit of the changed areas.

The purpose is to catch:

> "The original bug is gone, but the fix introduced a different race."

Only after this gate should implementation proceed to Section B.

---

# 5. Phase B — Kernel Object Lifetime & Quiescence

Now implement the subsystem recommended by the architecture audit.

The objective is to establish a **general kernel lifetime/reclamation framework** that can safely support:

- kobjects
- handle tables
- process objects
- sockets
- VFS objects
- timers
- IRQ registrations
- network objects
- device objects
- kernel modules
- read-mostly data structures
- future RCU/EBR users

This must become an architectural primitive, not a one-off workaround.

---

# 6. Quiescence Design

Implement:

```c
void quiesce_read_lock(void);
void quiesce_read_unlock(void);

void synchronize_quiesce(void);
```

The exact API may be adjusted to match existing CosmoOS naming conventions.

The key invariant is:

> An object cannot be reclaimed until every CPU that could still have a reference to that object has passed through a provably safe quiescent state.

---

# 7. Read-Side Critical Sections

In the current kernel architecture, read-side sections correspond to preemption-disabled regions where appropriate.

Do NOT blindly equate:

> interrupts disabled == quiescent

That is incorrect.

An interrupt may interrupt a preempt-disabled reader.

Therefore an IRQ return path must not declare a CPU quiescent merely because:

```text
irq_depth == 0
```

The quiescence condition must account for the interrupted context's preemption state and interrupt-enable state.

Use the audit's corrected model as the starting point.

A CPU may report quiescence only when it is provably outside a read-side critical section.

---

# 8. Quiescent-State Conditions

The design must correctly handle:

### IRQ return

A CPU may publish quiescence only when the interrupted context satisfies the required preemption/interrupt predicate.

### `schedule()`

Passing through the scheduler constitutes a quiescent state when the scheduler's invariants guarantee the CPU is outside a protected read-side section.

### Idle CPU

An idle CPU may be considered quiescent only when it is actually in the defined idle state and cannot still be executing a protected reader.

### Nested interrupts

Nested interrupt depth must not cause premature quiescence.

### Preemption-disabled code

A CPU executing preemption-disabled code must remain non-quiescent.

### Spinlocks

A CPU executing inside a protected spinlocked/preemption-disabled region must not prematurely publish quiescence.

---

# 9. Memory Ordering — CRITICAL

This subsystem must work correctly on both:

- x86-64
- AArch64

Do not design it assuming x86 TSO.

The audit already identified a weak-memory ordering problem with relaxed epoch publication.

Use explicit memory-ordering semantics.

The intended model is:

1. unlink object
2. advance global epoch with the required ordering
3. quiescent CPU observes epoch using acquire semantics
4. CPU publishes its observed epoch with release semantics
5. waiter reads publication with acquire semantics
6. only then reclaim

The implementation must provide a real happens-before relationship between:

```text
reader's last object access
        ↓
reader's quiescent publication
        ↓
reclaimer observes publication
        ↓
object reclamation
```

Do not weaken the memory ordering merely for a microbenchmark improvement.

Document exactly why every acquire/release/seq-cst operation exists.

---

# 10. Epoch Design

Design a small and scalable epoch mechanism.

At minimum consider:

```text
global_epoch

per_cpu:
    last_seen_epoch
    reader_depth
    quiescent_state
```

Use appropriate atomic types and cache-line placement.

Avoid a single heavily contended cache line for all CPUs if the design can avoid it.

The mechanism must scale toward the architecture's target of hundreds of CPUs.

---

# 11. `synchronize_quiesce()`

This is a sleeping operation.

It must:

1. establish a new reclamation epoch
2. snapshot/identify the relevant online CPUs
3. wait for every relevant CPU to pass a safe quiescent state
4. tolerate CPUs entering/leaving relevant states according to the SMP lifecycle rules
5. prevent premature reclamation
6. avoid deadlock with scheduler/interrupt paths

Do not busy-spin indefinitely.

Do not hold ordinary kernel locks while sleeping unless explicitly proven safe.

If necessary, use:

- wait queues
- completion objects
- scheduler sleep
- reschedule IPIs
- timeout/watchdog instrumentation

The common case should be efficient.

---

# 12. `synchronize_irq()`

Implement an IRQ quiescence primitive where required:

```c
synchronize_irq(irq);
```

Its purpose is to guarantee that an IRQ handler currently executing has completed before the caller releases associated resources.

This must correctly handle:

- IRQ currently running on another CPU
- nested IRQ behavior
- IRQ disabled state
- IRQ teardown
- CPU hotplug assumptions
- handler deregistration

Do not implement this as an arbitrary delay.

It must establish a real completion guarantee.

---

# 13. `timer_cancel_sync()`

Implement synchronous timer cancellation:

```c
timer_cancel_sync(timer);
```

Required guarantee:

> After `timer_cancel_sync()` returns, the timer callback cannot still be executing or subsequently begin executing.

Handle races between:

- timer expiry
- timer queue removal
- callback dispatch
- cancellation
- callback rescheduling
- CPU migration
- concurrent cancellation

Audit all existing timer users.

Replace unsafe patterns where objects are freed immediately after asynchronous timer cancellation.

Add race tests.

---

# 14. Mandatory Kobject Release Semantics

Audit the entire kernel object system.

Every kobject type must have an explicit destruction/release mechanism.

Establish a clear lifecycle:

```text
allocation
   ↓
initial reference
   ↓
published
   ↓
get/reference
   ↓
use
   ↓
close/remove/unpublish
   ↓
drop reference
   ↓
release
   ↓
final destruction
```

The final release must not occur while an active user can still dereference the object.

Audit:

- reference acquisition
- reference release
- handle lookup
- handle close
- process exit
- socket close
- vnode lifetime
- file objects
- device objects
- timers
- modules
- IRQ registrations
- network objects

---

# 15. Handle Table Lifetime

Audit handle lookup versus close.

Look specifically for:

```text
CPU A: lookup(handle)
CPU B: close(handle)
CPU B: free(object)
CPU A: use(object)
```

Prevent this class of use-after-free.

Consider whether handle tables require:

- generation counters
- stable references
- object pinning
- lock/RCU/quiescence protection

Do not assume an integer handle itself provides lifetime safety.

Audit ABA possibilities.

---

# 16. Module Unload Protocol

Module unloading must become lifetime-safe.

The required high-level sequence should resemble:

```text
stop new users
        ↓
unpublish module
        ↓
prevent new references
        ↓
wait for existing users
        ↓
wait for IRQs
        ↓
cancel timers synchronously
        ↓
wait for quiescence where required
        ↓
release module references
        ↓
unmap/free module
```

Do not free executable module memory while:

- an interrupt handler may still execute
- a timer callback may still execute
- a worker may still execute
- another CPU may still hold a code/data reference
- a kobject may still reference module-owned data

Audit every module callback.

---

# 17. Networking Lifetime

Audit socket/network object teardown specifically for the three close-vs-RX UAF patterns reported by the audit.

Trace:

```text
RX
interrupt
netrx
socket lookup
socket close
socket destruction
timer
TCP callback
```

Use the new lifetime primitives where appropriate.

Do not simply add a giant socket lock.

Preserve the existing high-performance networking architecture.

The goal is safe lifetime with minimal contention.

---

# 18. VFS Lifetime

Audit:

- vnode
- file
- mount
- filesystem
- directory cache
- page cache
- inode metadata
- open file descriptions

Look for:

- close/use races
- unmount/use races
- lookup/free races
- timer/workqueue callbacks
- asynchronous I/O references

Integrate with kobject/reference semantics rather than inventing an unrelated lifetime model.

---

# 19. IRQ and Device Lifetime

Audit device removal/unload paths.

A device cannot be freed while:

- an IRQ handler can execute
- DMA can still target memory
- a work item can still reference it
- a timer can still fire
- a network RX queue can still reference it

Where appropriate establish:

```text
disable new work
→ quiesce IRQ
→ stop DMA
→ drain work
→ cancel timers
→ wait for readers
→ release object
```

The exact ordering must depend on hardware semantics.

---

# 20. RCU/EBR Usage Policy

Do not introduce RCU/EBR everywhere.

Use quiescence only where it provides a clear advantage.

Good candidates include:

- read-mostly routing tables
- device lookup tables
- namespace lookup
- process lookup
- read-mostly VFS caches
- immutable/read-mostly network structures

Do not replace ordinary mutexes or reference counting merely because EBR is faster in theory.

Every quiescence user must document:

- what is protected
- who enters the read-side section
- who unlinks the object
- who calls synchronize
- who frees the object
- why reference counting alone is insufficient

---

# 21. Debugging Instrumentation

In debug builds provide strong diagnostics.

Track:

- reader depth
- quiescent transitions
- epoch transitions
- synchronize callers
- CPUs that delay grace periods
- IRQ synchronization waits
- timer cancellation waits
- object release
- double release
- reference underflow
- reference overflow
- stale handles
- module unload waits

Provide assertions for invalid usage.

For example:

```text
quiesce_read_unlock without lock
blocking inside quiesce read section
free before grace period
release with active references
timer callback after cancel_sync
IRQ callback after synchronize_irq
module unload while active execution exists
```

---

# 22. Stress Testing

Add SMP stress tests.

At minimum test:

### Object lifetime stress

Many CPUs repeatedly:

- get
- use
- put
- close
- reopen

### Handle stress

Concurrent:

- lookup
- close
- reuse

### Quiescence stress

Readers continuously enter/exit while writers repeatedly:

- unlink
- synchronize
- reclaim

### Timer stress

Concurrent:

- arm
- fire
- cancel
- cancel_sync
- reschedule

### IRQ stress

Repeated:

- register
- trigger
- synchronize
- unregister

where the test infrastructure supports it.

### Module stress

Repeated:

- load
- use
- unload

with concurrent worker/timer/IRQ activity.

### Networking stress

Concurrent RX/close/free.

---

# 23. Memory-Ordering Tests

AArch64 is particularly important here.

Create tests that attempt to expose incorrect ordering.

Do not write a test that merely passes on x86.

Where practical use:

- host concurrency tests
- randomized stress
- architecture-specific QEMU testing
- compiler barriers/assertions
- ThreadSanitizer-compatible host models where possible

Document what the test can and cannot prove.

---

# 24. Performance Requirements

This is a kernel lifetime primitive, so performance matters.

Benchmark:

- uncontended read-side entry/exit
- nested read-side sections
- synchronize_quiesce
- 1 CPU
- 2 CPUs
- 4 CPUs
- 16 CPUs
- 64 CPUs
- 256 CPUs where test infrastructure permits

The target should be:

> extremely cheap read-side protection and infrequent writer-side synchronization.

Do not optimize prematurely.

First establish correctness.

Then measure.

Then optimize.

---

# 25. Architecture Requirements

The generic lifetime API must not contain x86-specific logic.

Architecture-specific operations belong under:

```text
kernel/arch/x86_64/
kernel/arch/aarch64/
```

The generic kernel must depend on architecture-neutral abstractions.

The design must remain compatible with:

- x86-64
- AArch64
- SMP
- future NUMA
- future CPU hotplug

---

# 26. No Mass Rewrite

Do NOT:

- rewrite the kernel
- redesign unrelated subsystems
- replace working allocators
- replace the scheduler
- replace the networking stack
- replace cosmofs
- rewrite the VFS
- rewrite the module loader
- introduce an entirely new object system

Modify only what is required.

Preserve existing APIs when they are architecturally sound.

When an API must change, update every caller and document the reason.

---

# 27. Code Quality Requirements

All code must be:

- freestanding-kernel-safe
- deterministic where required
- SMP-safe
- memory-ordering correct
- warning-clean
- sanitizer-clean in host-testable portions
- architecture-aware
- documented
- testable

Avoid:

- arbitrary sleeps
- arbitrary delays
- infinite polling
- unexplained memory barriers
- global locks added as quick fixes
- disabling interrupts around large regions
- reference-count hacks
- "temporary" lifetime workarounds

Every synchronization primitive must have a documented ownership/lifetime invariant.

---

# 28. Required Documentation

Update the appropriate documentation with:

### Critical Fixes

For every critical issue:

- root cause
- affected subsystem
- fix
- invariant
- regression test

### Quiescence

Document:

- read-side definition
- quiescent-state definition
- epoch algorithm
- memory ordering
- CPU lifecycle assumptions
- scheduler interaction
- IRQ interaction
- idle CPU handling

### Kobject Lifetime

Document:

- ownership
- reference counting
- release
- handles
- unload
- asynchronous callbacks

### Module Lifetime

Document the complete unload protocol.

---

# 29. Required Final Verification

Before declaring this task complete:

Run:

```text
make check-tools
make
make image
make test
make host-test
make analyze
make reproducible
```

Run the relevant QEMU tests for:

- boot
- SMP
- memory
- scheduler
- networking
- VirtIO
- modules
- VFS
- Linux compatibility
- virtualization
- AArch64

Use the repository's actual available targets if names differ.

Do not claim a test passed unless it actually ran.

---

# 30. Final Audit After Implementation

Perform a focused post-implementation audit.

Search the entire repository for the classes of bugs fixed.

For example:

- sleeping under spinlock
- unsafe device-controlled descriptor traversal
- FPU state leakage
- unsafe APIC ICR writes
- SWAPGS/NMI hazards
- privileged operations without credential checks
- private signing keys
- object release without ownership
- close/use races
- timer callback/free races
- IRQ/free races
- module unload/use races
- weak-memory lifetime bugs

Do not only inspect changed files.

---

# 31. Required Final Report

At the end provide:

## 1. Critical Fix Summary

For every critical finding:

- status
- root cause
- files/functions changed
- fix
- regression test

## 2. Lifetime Architecture

Explain:

- kobject ownership
- reference model
- quiescence
- epoch mechanism
- IRQ synchronization
- timer synchronization
- module unloading

## 3. Memory Ordering

List every important atomic operation and explain why its ordering is required.

## 4. Concurrency Model

Explain:

- lock interactions
- quiescence interactions
- scheduler interactions
- interrupt interactions

## 5. Tests

List every test executed and its result.

Do not fabricate results.

## 6. Performance

Provide measured results for:

- read-side enter/exit
- synchronization
- object lifetime
- relevant networking/device paths

## 7. Remaining Risks

Explicitly identify anything that remains unresolved.

## 8. Architectural Debt

List anything that should be addressed in a future milestone.

## 9. NEXT SUBSYSTEM

Select exactly one next subsystem after this work.

Do not implement it.

Explain:

- why it should come next
- dependencies
- architectural impact
- expected risks
- proposed design

---

# 32. Most Important Rule

Do not optimize for "making the audit green."

Optimize for:

> **CosmoOS becoming a genuinely correct, scalable, SMP-safe, memory-safe operating system.**

A passing test is not proof of correctness.

A small diff is not necessarily a good diff.

A large diff is not necessarily bad.

Choose the smallest **architecturally correct** implementation.

If the audit's recommendation is itself incorrect, explain why and implement the stronger design.

If you discover a new CRITICAL issue while implementing these fixes:

1. stop the affected implementation path
2. document the discovery
3. fix it if it is required for correctness of the current milestone
4. add a regression test
5. continue only when the architecture remains sound

Do not silently ignore newly discovered correctness or security issues.

**Begin now by inspecting the current repository and the complete audit. Do not ask me which files to inspect. Do not ask me to select the first critical issue. Execute the Critical-Fix Pass in dependency order, validate it, and then proceed to the kernel object lifetime and quiescence subsystem.**