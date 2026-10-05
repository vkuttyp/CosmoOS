# CosmoOS — Lock Discipline, Lockdep & Concurrency Hardening

You are working directly on the current CosmoOS repository.

CosmoOS is a from-scratch Unix-philosophy operating system with a hybrid kernel, currently targeting x86-64 and AArch64.

This is an active OS project. The repository is the source of truth.

Do NOT assume that a feature is missing merely because it appears in an older roadmap, audit, issue, TODO, or design document.

Do NOT implement from this prompt blindly.

Your first responsibility is to understand the current tree.

---

# 1. Mission

The previous major correctness milestone established a kernel-wide lifetime and quiescence model.

The next major milestone is:

**Lock Discipline, Lock Dependency Validation, and Concurrency Hardening**

The objective is not merely to add debugging assertions.

The objective is to establish a coherent, kernel-wide locking model that:

1. documents actual locking relationships,
2. detects invalid locking behavior at runtime,
3. detects lock-order cycles,
4. detects sleeping in atomic contexts,
5. understands interrupt-context restrictions,
6. integrates with the existing scheduler and quiescence architecture,
7. produces useful diagnostics,
8. has low enough overhead to remain usable in debug/test kernels,
9. provides the foundation for later scheduler/preemption work,
10. exposes real bugs rather than hiding them.

Correctness is more important than feature count.

Do not weaken existing invariants to make tests pass.

---

# 2. Mandatory repository investigation

Before modifying any source code, read and understand at minimum:

- `README.md`
- repository architecture/design documentation
- the current deferred-work inventory
- the lifetime/quiescence audit report
- lifetime/quiescence design documentation
- lifetime/quiescence API documentation
- lifetime/quiescence invariants
- lifetime/quiescence testing documentation
- scheduler documentation
- synchronization/locking documentation
- SMP documentation
- interrupt/IRQ documentation
- VFS locking documentation
- networking architecture documentation
- module lifecycle documentation

Search the repository rather than assuming exact filenames.

Then inspect the actual implementation.

The implementation always wins over stale documentation.

---

# 3. Treat lifetime/quiescence as established architecture

The lifetime/quiescence subsystem has already been implemented and extensively tested.

Do NOT replace it merely because another RCU/epoch design looks cleaner.

Understand it first.

The established conceptual lifecycle is:

    unlink / unregister
            ↓
    prevent new references
            ↓
    existing readers finish
            ↓
    quiescent state
            ↓
    grace period completes
            ↓
    release final reference
            ↓
    free

Existing concepts include mechanisms equivalent to:

- read-side quiescence sections,
- grace-period synchronization,
- deferred reclamation,
- synchronous IRQ unregister,
- synchronous timer cancellation,
- kobject release ownership,
- module lifetime protection,
- device/block/network lifetime handling.

Find their actual current implementations.

Do not infer APIs from this prompt.

---

# 4. Preserve lifetime invariants

Lockdep must understand the constraints introduced by the lifetime architecture.

At minimum investigate and enforce where appropriate:

- grace-period synchronization must not sleep while holding a spinlock,
- synchronous timer cancellation must not deadlock against callback locks,
- synchronous IRQ unregister must respect interrupt/locking context,
- deferred reclamation callbacks must execute in valid contexts,
- module shutdown must not introduce loader re-entry deadlocks,
- resource unregister paths must not violate lock ordering,
- sleeping primitives must not execute in atomic contexts,
- preemption-disabled regions must remain compatible with quiescence tracking.

Do not change these rules simply to simplify lockdep.

---

# 5. Phase 0 — establish a clean baseline

Before implementation:

1. determine supported host/build environments;
2. determine current x86-64 build/test commands;
3. determine current AArch64 build/test commands;
4. identify host-side tests;
5. identify sanitizers/static-analysis jobs;
6. inspect CI;
7. run the appropriate baseline tests.

Record:

- successful tests,
- failures,
- flaky tests,
- warnings,
- known baseline failures.

Do not attribute an existing failure to your changes.

Do not silently ignore baseline failures either.

---

# 6. Phase 1 — locking architecture audit

Before designing lockdep, audit the current locking architecture.

Produce:

`docs/audit/<current-date>-lock-discipline-audit.md`

Do this BEFORE substantial implementation.

The audit must identify every synchronization primitive actually present in the current tree.

Examples may include:

- spinlocks,
- IRQ-saving spinlocks,
- mutexes,
- semaphores,
- rwlocks,
- wait queues,
- completions,
- atomics,
- scheduler synchronization,
- preemption disabling,
- interrupt disabling,
- quiescence read sections,
- reference counts.

Do not invent primitives that do not exist.

---

# 7. Inventory locks

Build a repository-wide inventory of important locks.

For each important lock or lock family determine where possible:

- subsystem,
- purpose,
- type,
- ownership,
- acquisition sites,
- release sites,
- whether recursive acquisition is legal,
- whether IRQ context can acquire it,
- whether IRQs must be disabled,
- whether preemption is disabled,
- whether sleeping is allowed while held,
- whether callbacks execute while held,
- whether user-memory access occurs while held,
- whether allocation occurs while held,
- ordering relationships,
- lifetime interactions.

Pay special attention to dynamically instantiated locks.

Do not assume one lock object equals one lock class.

---

# 8. Audit known risk areas

Explicitly inspect the current implementation of areas previously identified as potentially relevant, including equivalents of:

- vnode cache lookup locking,
- futex wait/user-copy interactions,
- global VFS mount synchronization,
- rename locking,
- scheduler locking,
- VFS lock ordering,
- network lock ordering.

These references may already be fixed.

Determine their CURRENT status.

For each classify it as:

- confirmed safe,
- confirmed bug,
- partially addressed,
- stale finding,
- needs runtime validation.

Never implement an old recommendation without verifying it against current source.

---

# 9. Search for additional problems

Do not limit the audit to known findings.

Search systematically for:

    lock(A)
    lock(B)

versus:

    lock(B)
    lock(A)

Search callback relationships.

Search error paths.

Search:

- early returns,
- `goto` cleanup,
- nested locking,
- callbacks under locks,
- allocation under spinlocks,
- user copies under spinlocks,
- blocking waits under spinlocks,
- timer callbacks,
- IRQ handlers,
- deferred callbacks,
- module unload,
- device unregister,
- filesystem teardown,
- socket teardown,
- process exit,
- scheduler paths.

Trace cross-function relationships.

A locking bug may span several files.

---

# 10. Produce a lock-order graph

Construct the actual observed lock dependency graph.

Conceptually:

    lock class A
        ↓
    lock class B
        ↓
    lock class C

Record observed edges:

    A → B

when B is acquired while A is held.

Look for:

    A → B → C → A

Do not restrict cycle detection to two-lock inversions.

Support arbitrary dependency cycles.

The audit should identify both:

- statically visible relationships,
- relationships that require runtime observation.

---

# 11. Stop-and-assess gate

After completing the audit, decide whether the proposed lockdep architecture still fits the current kernel.

If the current repository already contains significant lockdep infrastructure, extend it rather than creating a parallel system.

If fundamental assumptions in this prompt are stale, document them and adapt.

Before major implementation, clearly record:

- current locking architecture,
- confirmed bugs,
- suspected risks,
- proposed lockdep architecture,
- integration points,
- testing strategy.

Then proceed unless repository evidence demonstrates that a different milestone is necessary first.

---

# 12. Lockdep architecture

Implement a kernel lock dependency validator suitable for CosmoOS.

Prefer a small understandable design over copying Linux lockdep wholesale.

The system should conceptually contain:

    lock instance
          ↓
      lock class
          ↓
    acquisition
          ↓
    per-CPU/per-thread held-lock state
          ↓
    dependency edge
          ↓
    dependency graph
          ↓
     cycle detection

Keep instrumentation separate from core lock implementation where practical.

---

# 13. Lock classes

A lock class represents logically equivalent locks.

Support dynamic lock instances without treating every instance as an unrelated class.

Determine an appropriate CosmoOS mechanism for identifying classes.

Possible approaches include:

- static class keys,
- explicit initialization metadata,
- call-site metadata,
- lock-type metadata.

Choose based on the existing kernel architecture.

Avoid fragile dependence on raw addresses.

Lock objects can be freed and reused.

---

# 14. Held-lock tracking

Maintain enough execution-context state to determine which locks are currently held.

The design must work correctly with:

- multiple CPUs,
- scheduler context switches,
- interrupt entry/exit,
- preemption-disabled regions.

Determine whether held-lock state belongs primarily to:

- threads,
- CPUs,
- or a carefully defined combination.

Do not blindly choose per-CPU state if migration or context switching makes that incorrect.

Document the reasoning.

---

# 15. Dependency graph

When acquiring B while holding A, record:

    A → B

Before accepting a new edge, determine whether it creates a dependency cycle.

If:

    B → ... → A

already exists, report a potential deadlock.

Diagnostics should include the complete cycle where possible.

Example:

    lockdep: possible circular locking dependency

      fs_inode_lock
        acquired at ...
          ↓
      page_cache_lock
        acquired at ...
          ↓
      fs_inode_lock

Include useful source metadata where practical.

---

# 16. Recursive locking

Detect accidental recursive acquisition of non-recursive locks.

Do not assume recursion is always illegal.

If CosmoOS contains legitimate recursive synchronization, model it explicitly.

Never silently permit recursion merely because a subsystem currently depends on it.

---

# 17. Atomic-context tracking

Establish a coherent definition of atomic context.

Investigate actual CosmoOS behavior involving:

- spinlocks,
- IRQ context,
- IRQ disabling,
- preemption disabling,
- NMI/machine-check context if applicable,
- scheduler internals.

Provide a helper conceptually similar to:

    in_atomic_context()

but use naming consistent with CosmoOS.

The semantics matter more than the name.

---

# 18. Sleeping-in-atomic detection

Instrument blocking/sleeping paths so lockdep can detect invalid sleeps.

Investigate:

- mutex slow paths,
- semaphore waits,
- wait queues,
- scheduler blocking,
- timer waits,
- synchronous cancellation,
- grace-period synchronization,
- memory allocation paths that may sleep,
- user-memory operations if they can fault/block.

The kernel should produce a strong diagnostic when code attempts to sleep from an invalid context.

Do not merely instrument one `sleep()` function.

Find the actual blocking boundary/boundaries.

---

# 19. IRQ safety

Track whether lock classes are acquired from interrupt context.

Detect invalid patterns such as:

    thread context:
        acquire A
        interrupts enabled

    interrupt context:
        acquire A

when that can self-deadlock on the same CPU.

Understand existing `irqsave` semantics before implementing this.

Where useful, classify lock classes as:

- IRQ-safe,
- IRQ-unsafe,
- observed in IRQ context,
- observed with IRQs enabled/disabled.

Avoid false assumptions around NMI/#MC contexts.

---

# 20. Preemption interaction

Understand the exact relationship between spinlocks and preemption.

If spinlock acquisition disables preemption, validate nesting.

Detect:

- preemption count underflow,
- invalid enable,
- mismatched disable/enable,
- sleeping while preemption is disabled where illegal.

Do not break the existing quiescence implementation, which may intentionally use preemption disabling.

---

# 21. Lock release validation

Detect:

- releasing a lock not held,
- releasing a lock owned by another execution context where ownership applies,
- invalid release order where relevant,
- mismatched IRQ state restoration,
- corrupted lock nesting.

Diagnostics should make these failures actionable.

---

# 22. Diagnostics

A lockdep failure must provide enough information to debug the problem.

Include where practical:

- CPU,
- thread/process,
- interrupt state,
- preemption state,
- lock address,
- lock class,
- lock type,
- acquisition location,
- previously held locks,
- dependency path,
- detected cycle,
- relevant callback/IRQ context.

Do not allocate memory in dangerous diagnostic paths unless proven safe.

Avoid introducing a second deadlock while reporting the first one.

---

# 23. Source metadata

Provide acquisition source information where practical.

Consider:

- file,
- line,
- function,
- return address.

Avoid excessive permanent memory cost.

If compile-time macros are appropriate, use them cleanly.

Normal release builds should not pay unnecessary debug metadata cost.

---

# 24. Configuration

Lockdep must be configurable.

Use the existing CosmoOS configuration mechanism.

Conceptually support:

    CONFIG_LOCKDEP

or the repository's appropriate naming convention.

When disabled:

- runtime overhead should be negligible,
- memory overhead should be negligible,
- public synchronization APIs should remain stable.

Do not scatter ugly conditional compilation throughout unrelated code.

---

# 25. Lock statistics

If architecturally clean, add lightweight lock statistics.

Potential measurements:

- acquisitions,
- contention,
- failed try-locks,
- maximum hold duration,
- maximum wait duration.

Do not let lockstat complexity delay correctness-critical lockdep functionality.

Lock statistics are secondary to dependency validation.

---

# 26. Scheduler audit

Lockdep touches scheduler semantics.

Audit carefully:

- scheduler locks,
- runqueue locks,
- wakeups,
- migration,
- load balancing,
- context switch,
- idle paths,
- wait queues,
- priority inheritance.

Do NOT undertake unrelated scheduler redesign.

Record scheduler issues discovered during this work.

Fix only issues required for correctness or lockdep integration unless a broader change is clearly justified.

---

# 27. VFS audit

VFS locking is a high-priority target.

Investigate:

- mount locks,
- vnode locks,
- inode locks,
- directory locks,
- page-cache locks,
- rename,
- lookup,
- unlink,
- filesystem sync,
- teardown,
- CosmoFS transaction locking.

Pay particular attention to multi-object operations.

For operations involving two directories/inodes, define deterministic ordering if required.

Never rely on accidental pointer ordering without documenting why it is valid.

---

# 28. Networking audit

Networking contains many asynchronous contexts.

Audit:

- socket locks,
- TCP control blocks,
- interface registration,
- transmit paths,
- receive paths,
- timers,
- IRQ callbacks,
- deferred processing,
- neighbor/ARP/ND state,
- device unregister,
- socket destruction.

Preserve the lifetime/quiescence model.

Pay particular attention to:

    timer → lock
    IRQ → lock
    thread → lock
    unregister → synchronize → callback

relationships.

---

# 29. Device and module audit

Inspect locking around:

- driver registration,
- device removal,
- block unregister,
- network unregister,
- IRQ teardown,
- timers,
- module unload.

Module unload is particularly important because code may disappear after quiescence.

Lockdep metadata must never retain unsafe references into unloaded module memory.

Design for this explicitly.

---

# 30. Memory allocator interaction

Lockdep itself must not depend on unsafe allocation behavior.

Determine whether dependency records require dynamic allocation.

If so, ensure allocation cannot recursively enter instrumented locks or sleep from invalid contexts.

Prefer:

- preallocated storage,
- bounded pools,
- boot-time allocation,
- safe fallback behavior,

where appropriate.

Lockdep must never become the cause of the locking bug it is trying to detect.

---

# 31. Failure behavior

Define behavior for:

- dependency graph exhaustion,
- held-lock stack overflow,
- unknown lock class,
- metadata exhaustion,
- recursion,
- dependency cycle,
- sleeping in atomic context.

For correctness violations, fail loudly in debug/test builds.

For instrumentation-resource exhaustion, avoid turning a diagnostic limitation into memory corruption.

Never silently produce incorrect dependency state.

---

# 32. Tests — deterministic unit/self-tests

Add targeted tests for at least:

### Correct nesting

    lock(A)
    lock(B)
    unlock(B)
    unlock(A)

No warning.

### Two-lock inversion

Observe:

    A → B

then:

    B → A

Must detect.

### Multi-lock cycle

Observe:

    A → B
    B → C
    C → A

Must detect.

### Recursive acquisition

Acquire non-recursive A twice.

Must detect.

### Release without ownership

Must detect.

### Sleeping under spinlock

Must detect.

### Sleeping in IRQ context

Must detect where applicable.

### IRQ-unsafe locking

Construct an invalid thread/IRQ relationship.

Must detect.

### Valid IRQ-safe locking

Must not generate a false positive.

### Preemption nesting

Validate correct and incorrect nesting.

---

# 33. Tests — subsystem integration

Exercise real kernel subsystems under lockdep.

At minimum consider:

- scheduler stress,
- process creation/destruction,
- VFS stress,
- filesystem operations,
- networking/TCP stress,
- device registration/unregistration,
- timer cancellation,
- IRQ registration/unregistration,
- module load/unload,
- lifetime/quiescence stress.

The goal is not merely to prove synthetic lockdep tests work.

Use lockdep to discover actual kernel problems.

---

# 34. Concurrency stress

Where practical, increase interleaving pressure.

Use:

- multiple CPUs,
- repeated operations,
- randomized yields,
- timer activity,
- network traffic,
- registration/unregistration loops,
- process churn.

Do not introduce nondeterminism without useful diagnostics.

If adding stress hooks, make them reproducible with seeds where possible.

---

# 35. Fault injection

Integrate with existing fault-injection infrastructure where useful.

Test lock cleanup across:

- allocation failures,
- initialization failures,
- partial device setup,
- filesystem errors,
- network errors,
- module load failures.

Error paths are frequent sources of forgotten unlocks and ordering mistakes.

---

# 36. Cross-architecture validation

Run appropriate validation on:

- x86-64 SMP,
- x86-64 UP,
- AArch64 SMP where supported,
- AArch64 UP where supported.

Do not assume x86 behavior proves AArch64 correctness.

However, do not conflate lock dependency correctness with CPU memory-order verification.

Record memory-order concerns separately.

---

# 37. Preserve performance architecture

Lockdep is primarily debugging infrastructure.

Do not unnecessarily slow release kernels.

Measure where practical:

- kernel image increase,
- lock acquisition overhead with lockdep disabled,
- lock acquisition overhead with lockdep enabled,
- memory consumed by dependency metadata.

Avoid premature micro-optimization.

Correctness comes first.

---

# 38. Do not solve unrelated deferred work

During this milestone you may discover unrelated issues.

Record them.

Do NOT expand scope automatically into:

- Linux ABI expansion,
- new filesystems,
- new network protocols,
- USB,
- graphics,
- containers,
- eBPF,
- VT-x,
- live migration,
- NUMA,
- broad scheduler redesign,
- broad VMM redesign.

Only fix unrelated issues if they block correctness or validation of this milestone.

Otherwise add them to the appropriate deferred-work record.

---

# 39. Known lifetime follow-ups

The previous lifetime/quiescence audit identified residual areas requiring future attention.

Re-check their current status but do not automatically expand scope to solve all of them.

Examples include:

- grace-period latency,
- long preemption-disabled sections,
- straggler IPI race validation,
- block submit/unregister race validation,
- TCP timer/free race validation,
- runtime VirtIO hot-unplug validation,
- memory-order model validation,
- scheduler wake/preemption latency,
- AArch64 virtio-console instability,
- ARP/ND interface lifetime strategy.

Lockdep may expose relevant evidence.

If so, document it.

---

# 40. Documentation

Create/update documentation describing:

## Lockdep design

Explain:

- lock classes,
- held-lock tracking,
- dependency graph,
- cycle detection,
- IRQ classification,
- atomic-context detection,
- preemption interaction,
- configuration,
- metadata lifetime,
- module interaction.

## Locking rules

Create a canonical locking-rules document.

It should answer questions such as:

- Can this primitive sleep?
- Can it be acquired from IRQ context?
- Does it disable preemption?
- Can user memory be touched?
- Can memory allocation occur?
- What ordering applies?
- Can callbacks run while held?

Future developers and AI coding agents should be able to consult this document.

---

# 41. Machine-readable locking knowledge

Where reasonably possible, encode locking knowledge in code rather than prose alone.

Examples:

- lock classes,
- assertions,
- context annotations,
- ownership helpers,
- IRQ-safe APIs,
- debug metadata.

Prefer:

    mechanically enforced invariant

over:

    comment saying "do not do this"

when the invariant can reasonably be checked.

This is especially important because future development may be performed by AI coding agents.

---

# 42. Required final validation

Before declaring the milestone complete, run the strongest practical validation supported by the repository.

At minimum, where currently supported:

- clean build,
- debug build,
- release build,
- x86-64 tests,
- x86-64 SMP tests,
- x86-64 UP tests,
- AArch64 tests,
- host tests,
- sanitizer tests,
- static analysis,
- module tests,
- filesystem tests,
- networking tests,
- scheduler stress,
- lifetime/quiescence tests,
- new lockdep tests.

Do not weaken an existing test to obtain green CI.

Investigate failures.

---

# 43. Run the existing kernel with lockdep enabled

This is critical.

After lockdep itself passes synthetic tests:

**boot the normal CosmoOS test environment with lockdep enabled and use it as an auditing instrument.**

Exercise as much existing functionality as practical.

Every lockdep report must be investigated.

Classify each as:

    true positive
    lock-class modeling error
    instrumentation bug
    legitimate special case
    false positive

Do not suppress warnings merely because they are inconvenient.

---

# 44. Fix discovered locking bugs

When lockdep discovers a real bug:

1. reproduce it;
2. understand the complete lock relationship;
3. determine the architectural correction;
4. add a targeted regression test where practical;
5. implement the smallest correct fix;
6. rerun lockdep;
7. rerun affected subsystem tests.

Avoid local hacks such as arbitrary try-lock loops or silently dropping work.

---

# 45. False-positive discipline

A useful lockdep system must have high signal.

Do not solve false positives with broad suppression.

If a warning is not a real deadlock possibility, determine why the model is wrong.

Potential reasons include:

- incorrect lock-class assignment,
- mutually exclusive execution states,
- IRQ state not modeled,
- recursive lock semantics,
- object lifecycle guarantees,
- impossible callback relationship.

Represent the actual invariant.

Suppress only when the invariant cannot reasonably be represented otherwise.

Document every suppression.

---

# 46. Security and robustness

Lockdep is debug infrastructure but must not introduce unsafe behavior.

Do not:

- expose kernel pointers unnecessarily to userland,
- trust user-provided metadata,
- create writable user mappings of lockdep state,
- permit graph corruption from metadata exhaustion,
- execute unsafe diagnostics from NMI/#MC context,
- dereference stale module metadata.

Treat diagnostics executed during catastrophic kernel state carefully.

---

# 47. Commit discipline

Keep changes logically separated.

A desirable sequence is roughly:

1. audit/documentation,
2. lock metadata/classes,
3. held-lock/context tracking,
4. dependency graph,
5. cycle detection,
6. sleep/atomic detection,
7. IRQ classification,
8. diagnostics,
9. synthetic tests,
10. subsystem integration,
11. discovered bug fixes,
12. final documentation/audit update.

Do not produce one enormous opaque change unless repository constraints make that unavoidable.

---

# 48. Final report

Create:

`docs/audit/<current-date>-lockdep-report.md`

The report must contain:

1. Executive summary
2. Baseline state
3. Locking architecture discovered
4. Existing problems found before implementation
5. Lockdep architecture
6. Lock-class model
7. Dependency graph implementation
8. Atomic-context model
9. IRQ-safety model
10. Preemption interaction
11. Lifetime/quiescence interaction
12. Diagnostics
13. Tests added
14. Stress testing
15. Cross-architecture results
16. Lockdep findings against the existing kernel
17. Real bugs discovered
18. Bugs fixed
19. False positives encountered
20. Performance/overhead
21. Known limitations
22. Deferred work
23. Remaining concurrency risks
24. Recommended next subsystem

Update the main deferred-work inventory accordingly.

Never mark something complete solely because code exists.

Require evidence from tests or direct inspection.

---

# 49. Completion criteria

This milestone is complete only when:

- the current locking architecture has been audited;
- important lock ordering is documented;
- lock classes exist;
- held-lock tracking works;
- dependency edges are recorded;
- arbitrary dependency cycles are detected;
- accidental recursion is detected;
- invalid release is detected where meaningful;
- sleeping-in-atomic detection works;
- IRQ-safety validation works to the extent supported by the kernel;
- preemption interaction is validated;
- lifetime/quiescence invariants remain intact;
- lockdep can be disabled cleanly;
- diagnostics are actionable;
- synthetic tests pass;
- real subsystem stress has been run with lockdep enabled;
- discovered genuine bugs have either been fixed or explicitly documented;
- x86-64 validation passes;
- AArch64 validation passes to the currently supported extent;
- existing lifetime/quiescence tests continue to pass;
- documentation is updated;
- deferred-work inventory is updated.

---

# 50. Engineering principles

Follow these throughout the work.

### Repository evidence beats assumptions

Inspect before changing.

### Correctness beats convenience

Never weaken synchronization to make a test pass.

### Small mechanisms beat giant frameworks

Build the minimum mechanism required to enforce the invariant.

### Detect bugs close to their source

Prefer immediate invariant failures over later corruption.

### Tests must prove failure detection

Do not test only successful locking behavior.

### Architecture matters

x86-64 success does not prove AArch64 correctness.

### Preserve existing subsystem boundaries

Do not turn lockdep into a reason to redesign unrelated kernel architecture.

### Avoid speculative abstractions

Add abstractions when repository evidence demonstrates their need.

### Debugging infrastructure must itself be trustworthy

Lockdep cannot depend on unsafe locking or allocation behavior.

---

# 51. Important instruction about scope

Do not attempt to complete every item in this prompt in one uncontrolled implementation pass.

Work incrementally.

The expected sequence is:

    repository investigation
            ↓
    baseline validation
            ↓
    lock architecture audit
            ↓
    design
            ↓
    minimal lockdep core
            ↓
    synthetic validation
            ↓
    atomic/IRQ integration
            ↓
    subsystem validation
            ↓
    discovered bug fixes
            ↓
    cross-architecture validation
            ↓
    final audit report

At every stage, keep the tree buildable and testable.

If the audit reveals that a prerequisite is missing or that the repository has materially diverged from this prompt, stop that implementation path, document the evidence, and choose the smallest prerequisite correction.

---

# 52. First action

Start now with repository investigation.

Do NOT begin by writing lockdep code.

First:

1. inspect the current repository;
2. read the lifetime/quiescence report and supporting documentation;
3. read the deferred-work inventory;
4. inspect synchronization primitives;
5. inspect scheduler/preemption/IRQ implementation;
6. inspect lifetime/quiescence integration;
7. map important lock relationships;
8. run or determine the current baseline validation;
9. produce the lock-discipline audit;
10. present the proposed lockdep design based on what actually exists.

Only then proceed with implementation.

At the end of the initial audit, explicitly state:

- what you found,
- which assumptions in this prompt were correct,
- which were stale,
- confirmed existing locking bugs,
- uncertain areas requiring runtime evidence,
- proposed architecture,
- files/subsystems that will change,
- tests that will prove correctness.

Then proceed incrementally with the milestone unless the evidence demonstrates that a prerequisite must be addressed first.