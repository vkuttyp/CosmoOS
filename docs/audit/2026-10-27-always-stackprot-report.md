# Roadmap 1.2's first unit, fourth PR: the stack protector (BUILD-1), 2026-10-27

The last PR of the "Always, whichever direction" unit (after #464, #465
and #466; BUILD-2 was #463). BUILD-1: the kernel built with
`-fno-stack-protector`; the line asks for the stack protector with
per-thread canaries.

## Scope and baseline

Branch `always-stackprot` from main at `6df2fce8`. A test commit that
fails on main, then the fix. `tools/stackprot-probe.py --old` reverts the
fix in a throwaway worktree, keeping the test.

## The test

A crash-test kernel (`CRASH_TEST=4`, `make test-stackprot`, a CI step
beside the panic-path test, both architectures) overruns a 16-byte
`volatile` stack buffer by 48 bytes in a `noinline` function called from
kmain. The harness (`--expect-panic smash`) requires the panic to be the
stack protector's, at that function's return, and forbids a page-fault
panic and the "overrun went unnoticed" line. On main, x86-64 carried on as
if nothing had happened, and AArch64 died later of an unrelated exception.

## The design

- **Flags.** The kernel and its modules: `-fstack-protector-strong`
  (every function with a local array or an address-taken local), with the
  guard in this CPU's per-CPU block. The loader and user space keep
  `-fno-stack-protector`.
- **Per thread.** `struct percpu` gains `stack_canary` at offset 24, the
  offset the flags name (`PERCPU_CANARY_OFFSET`, asserted in `percpu.h`).
  Each thread draws its own canary at `thread_alloc` (low byte zero, as
  glibc's), and it is put in the block when the thread is switched in
  (`schedule_internal`, `sched_start_cpu`). A canary never changes under a
  thread's live frames: thread 0 keeps the boot context's, drawn from the
  cycle counter before the pool exists (`percpu_init_boot`), and an AP's
  bring-up frames use one drawn when its block is made.
- **x86-64.** LLVM puts the guard in a segment register
  (`-mstack-protector-guard=tls`, `%gs:24`) only for targets that have a
  TLS guard slot (glibc Linux, Fuchsia, Android); for
  `x86_64-unknown-none` it silently used one global `__stack_chk_guard`,
  found by the first link. So the kernel and modules compile for
  `x86_64-unknown-linux-gnu` (`KERNEL_CC_TARGET`), as Linux's own kernel
  does, with `-ffreestanding`; nothing in the tree tests `__linux__`, and
  libc and user space keep `KERNEL_TARGET`.
- **AArch64.** The `sysreg` guard takes only `SP_EL0` (`tpidr_el1` was
  refused). The kernel runs on `SP_EL1`, so `SP_EL0` is free at EL1:
  `arch_percpu_install` points it at the per-CPU block, and every entry
  from EL0 does so again after saving the user's in the frame
  (`vectors.S`); the return to EL0 already put the user's back. The EL2
  world switch saves and restores it with the rest of EL1's state.
- **Before the block.** `__no_stack_protector` on every function that runs,
  or keeps a frame live, before its CPU's block is installed (the start and
  AP entry functions, `gdt_init`, `gdt_init_cpu`, `idt_load`,
  `arch_console_early_init`, `percpu_init_boot`, `percpu_get`,
  `arch_percpu_install`) and on `__stack_chk_fail`. The disassembly shows
  no canary load in any of them, and about 2,800 checks in each kernel.

## Validation (2026-10-27, macOS host)

| Check | x86_64 | aarch64 |
|---|---|---|
| `tools/stackprot-probe.py --old` | PASS: the boot fails, no stack-protector panic | PASS, the same |
| `tools/stackprot-probe.py` (`make test-stackprot`) | PASS: `KERNEL PANIC: stack-protector: a stack canary was overwritten` | PASS, the same |
| debug `make test` (4 CPUs) | PASS (180.7 s); no lockdep report beyond the self-test's expected ones | PASS (191.2 s), the same |
| debug `QEMU_SMP=1 make test` | PASS (159.5 s) | PASS (163.7 s) |
| `make test-smp2` | PASS (178.7 s) | PASS (195.1 s) |
| `make test-crash` (debug) | PASS | PASS |
| `make test-wxn` (debug) | (AArch64 only) | PASS |
| `BUILD=release make test` | PASS (18.4 s) | PASS (21.9 s) |
| `make analyze` | clean: 7 baselined, 0 unexpected | clean, the same |
| `make host-test`, `make fuzz` | PASS, PASS | (host) |

## Backlog

BUILD-1 is done, and with it the unit's twelve lines: CFS-1 and CFS-3 (B2,
B1), BUILD-2 (#463), TEST-1, TEST-2, TEST-3, TEST-5 (#464), LX-1, PKG-1
(#465), NET-6, SEC-4 (#466), BUILD-1 (this PR).
