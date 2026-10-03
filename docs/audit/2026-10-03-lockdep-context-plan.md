# Lockdep context validation — continuation after PR #302

Base: `c4817611`. The repository investigation, primitive inventory and
baseline evidence are in the lock-discipline audit and hardening report.
This continuation preserves the existing validator and lifetime model.

## Execution order

1. Correct the demonstrated UP validation prerequisites. Clock tests run
   a local monotonic bracket control when no cross-CPU pair exists; SMP
   retains all distinct pairs and skew assertions. Cwd comparisons use
   explicit writer rounds, and the victim walker rendezvous guarantees
   progress before the mover leaves. Existing progress assertions stay.
2. Extend IRQ dependency validation, with host graph tests and real IRQ
   integration tests, then run SMP/UP on both architectures.
3. Assess callback-wait dependencies, IRQ restoration ownership, fatal
   diagnostics and statistics snapshots separately against their actual
   call paths. Do not mark these complete based on IRQ graph coverage.

## IRQ model

A blocking acquisition in IRQ context marks a class IRQ-used. Successful
trylock with interrupts enabled marks its class IRQ-enabled: an interrupt
can preempt that holder. A nonblocking trylock in an IRQ does not itself
make a class IRQ-used, since it never waits for the interrupted holder.
Failed trylocks record neither usage nor dependency edges.

Reject an edge A -> B when a previously IRQ-used node can reach A and B
can reach an IRQ-enabled node. Recheck existing dependencies when a class
first gains either usage bit; acquisition order must not determine whether
a conflict is detected. Class usage applies to all its subclasses; graph
traversal preserves exact subclass nodes. Trylock contributes no incoming
blocking edge, but subsequent blocking acquisitions still depend on locks
held by successful trylock.

Use bounded BFS over the existing graph with the existing serialized
scratch storage. A predecessor search starts from all nodes with the
required usage, rather than allocating a reverse graph. New usage and
edge validation stay under the raw lock. Reject a conflicting mutation
before publication, then report after dropping the raw lock: expected
negative tests must not poison unrelated future searches. Diagnostics
identify the IRQ-used and IRQ-enabled endpoints plus the proposed edge.

Tests must cover edge-last and usage-last conflicts, long chains,
subclasses, reverse/disconnected safe cases, successful/failed trylock,
and an IRQ trylock that must remain valid. The existing contention test's
final IRQ-used M -> IRQ-enabled L is an invalid future ordering under this
model; check that it reports IRQ conflict rather than a phantom cycle.

## Scope and evidence

No scheduler policy, quiescence or module ABI redesign. No lockstat feature
until correctness checks are validated. Runtime graph silence is coverage
of executed paths, not proof of unexecuted callbacks. Keep each completed
increment reviewable and retain outstanding criteria in the final report.
