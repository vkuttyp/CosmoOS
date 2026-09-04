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
| `kernel/interrupt/` | Vector-to-handler dispatch |
| `kernel/diagnostics/` | Console, logging, printf, panic, self-tests, crash test |
| `kernel/memory/` | Physical memory (bootmem, zones, buddy), virtual memory (page-table takeover, arena, faults), slab heap and kmalloc; host unit tests in `tests/host/` |

Further subsystem directories are added as subsystems come into existence
(`kernel/scheduler/`, `kernel/process/`, and so on).

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
