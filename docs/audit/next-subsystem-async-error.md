# NEXT SUBSYSTEM — a hardware error one process caused, and everyone pays for

Date: 2026-09-17. Tree: `main` at 98c1314 (after PR #171, the socket's
verdict). Chosen from `docs/audit/2026-09-deferred-work-inventory.md` §3.

**Subsystem: the asynchronous hardware error. An SError on AArch64 and a
machine check on x86-64 both end in `panic` today — including the
contained, attributable ones, where the architecture has told us exactly
which process to blame and that the rest of the machine is fine.**

Takes up the correctness clause of §3's AArch64-hardening row, *"a
user-triggerable SError panics the kernel"*. That row also names UAO,
E0PD, BTI, PAC, device-tree parsing and PSCI variations; this unit takes
**only** the SError clause and strikes only that, because the others are
feature work and this one is a defect.

## Problem

### 1. Every SError panics, whatever it was

`kernel/arch/aarch64/vectors.S` builds a frame for the SError and FIQ
slots so the panic can print it, and says so in its own preamble: *"The
slots for SP0 and AArch32 lower EL states, FIQ and SError also build a
frame so the panic can print it, then never return."* The dispatcher
agrees:

```c
/* kernel/arch/aarch64/trap.c:122-141 */
void aarch64_trap_entry(struct arch_trap_frame *frame)
{
    switch (frame->kind) {
    case AARCH64_ENTRY_EL1_SYNC:  handle_sync(frame, false); return;
    case AARCH64_ENTRY_EL0_SYNC:  handle_sync(frame, true);  return;
    case AARCH64_ENTRY_EL1_IRQ:
    case AARCH64_ENTRY_EL0_IRQ:   frame->vector = VEC_SPURIOUS; handle_irq(frame); return;
    default:
        frame->vector = VEC_SYNC_BASE + ARCH_TRAP_GENERAL_PROTECTION;
        panic_frame(frame, "exception in an unsupported vector slot %llu (EC 0x%x)", ...);
    }
}
```

There are four `AARCH64_ENTRY_*` kinds and sixteen vector slots
(`trapframe.h:35-39`). Everything that is not EL0/EL1 sync or IRQ —
**every SError, from either exception level** — reaches that `default`
and stops the machine. Note what it reports while doing so: the frame is
relabelled `ARCH_TRAP_GENERAL_PROTECTION`, so a machine halted by an
asynchronous abort says "general protection" in its own panic line.

### 2. x86-64 has the same gap, by a different road

`#MC` is a paranoid vector: it arrives on an IST stack at
`x86_trap_paranoid` (`x86_64/trap.c:114`), which dispatches it through
`interrupt_dispatch` like any other. Nothing registers a handler for
vector 18 — `user_exception_handler` (`process.c:156`) is registered
against `ARCH_TRAP_DIVIDE_ERROR`, `INVALID_OPCODE`,
`GENERAL_PROTECTION`, `DEBUG` and `PAGE_FAULT`, and that list has no
entry for a machine check. So an unregistered vector 18 ends in
`arch_trap_unhandled` → `panic_frame`, which the file's own comment
states as the rule.

The two architectures therefore fail the same way for the same reason,
and neither of them is an oversight in the *trap* code: nobody has
decided what a contained hardware error should do.

### 3. The architecture already says which ones are survivable

This is what makes it a defect rather than a policy nobody has written.
For an SError, `ESR_EL1` carries `IDS` (bit 24) and, when `IDS` is 0 and
the CPU implements FEAT_RAS, `AET` (bits 12:10) and `EA` (bit 9). The
`AET` encoding is exactly the classification this unit needs:

| `AET` | RAS name | what it means | what should happen |
| --- | --- | --- | --- |
| 0b000 | UC | **Uncontainable** | panic: the machine's state is not trustworthy |
| 0b001 | UEU | Unrecoverable, **uncontained** | panic |
| 0b010 | UEO | Restartable | the error is contained; the interrupted context may continue |
| 0b011 | UER | **Recoverable** | contained and attributable: kill the process, keep the machine |
| 0b110 | CE | Corrected | log it and continue; nothing is wrong yet |

x86-64's `MCG_STATUS` says the same thing in two bits: `RIPV` (the
return address is valid) and `EIPV` (the error is attributable to the
instruction at that address). `RIPV=1` is "execution can continue", which
is the same statement `UER` makes.

So on both architectures the hardware distinguishes *"this machine is
finished"* from *"this process is finished"*, and this kernel currently
reads neither.

### 4. It is reachable from user mode

An asynchronous abort is what a CPU reports for an external abort that
could not be attributed synchronously — a bad physical address, a device
that answered late, an uncorrectable memory error on a line a program
touched. A user program that maps a device the kernel let it map, or that
reads a page whose backing memory has decayed, can produce one. The
inventory's phrasing is *"a user-triggerable SError panics the kernel"*,
and the consequence is the ordinary one for this class: an unprivileged
program can end the machine, and every other process on it, without any
privilege. That is a denial of service with no defence in the tree.

## Why it matters

- **It is a correctness gap reachable today**, which §6 puts ahead of
  feature work. The machine dies for something one process caused, and
  the hardware said so.
- **The policy already exists and this is the one class that escapes
  it.** `user_exception_handler` is the contract: *a user program that
  divides by zero, executes an invalid opcode, violates protection or is
  single stepping dies with `COSMO_EXIT_FAULT`; the same exception from
  kernel mode is a kernel bug and panics* (`process.c:141-149`). A
  contained SError from EL0 is that sentence's own case, and it is not in
  the list — not by decision, but because nothing dispatches it there.
- **The panic lies about itself.** Relabelling the frame
  `ARCH_TRAP_GENERAL_PROTECTION` means the one diagnostic a rare hardware
  fault leaves behind names the wrong exception class. Whoever reads that
  panic next will spend their time in the wrong place, which is the
  defect this repository has spent four units removing from other
  reports.
- **The two architectures disagree about nothing here**, which makes it
  cheap: one classification, one policy, two small arch shims.

## Design

### 1. A trap kind, so the existing contract can reach it

`enum arch_trap_kind` (`kernel/include/arch/trap.h:21-27`) gains
`ARCH_TRAP_ASYNC_ERROR`, and `ARCH_TRAP_KIND_COUNT` moves with it. That
is the whole integration: the kind gets a vector number through
`arch_trap_vector`, `interrupt_dispatch` routes it, and
`user_exception_handler` gains one line mapping it to **`SIGBUS`**
(`signal.h:32`), which is the signal this class has always meant. The
kill is queued, not taken in the handler — which is already what that
function does, and matters more here than anywhere else, because an
SError can arrive in any context including one holding a run-queue lock.

### 2. One classifier, arch-shaped in and arch-free out

```c
/* kernel/include/arch/trap.h */
enum arch_async_error {
    ARCH_ASYNC_CORRECTED,     /* nothing is wrong yet: count it and continue */
    ARCH_ASYNC_CONTAINED,     /* attributable: kill the process, keep the machine */
    ARCH_ASYNC_UNCONTAINED,   /* the machine's state is not trustworthy: panic */
};
enum arch_async_error arch_async_error_class(const struct arch_trap_frame *);
```

AArch64 reads `ESR_EL1`: `IDS == 1` means the syndrome is
implementation-defined and nothing may be assumed — **uncontained**, the
safe answer. `IDS == 0` reads `AET` by the table above. A CPU without
FEAT_RAS reports no `AET`, so every SError on it is uncontained, which is
correct and is the only answer available.

x86-64 reads `MCG_STATUS`: `RIPV == 0` is uncontained; `RIPV == 1` with
`EIPV == 1` is contained; the corrected case comes from the bank's
`MCi_STATUS.UC == 0`.

**The rule the classifier obeys, and the reason it is a separate
function: anything not positively known to be contained is uncontained.**
A missing feature, an unknown encoding and a reserved value all end in
panic. The failure this unit must not introduce is a machine that
continues after an error it did not understand.

### 3. Where the decision is taken

| from | class | what happens |
| --- | --- | --- |
| EL0 / user | corrected | counted, logged at `kdebug`, return |
| EL0 / user | contained | `SIGBUS`, `COSMO_EXIT_FAULT`, the machine continues |
| EL0 / user | uncontained | panic |
| EL1 / kernel | corrected | counted, logged, return |
| EL1 / kernel | contained | **panic** — a contained error in kernel state is still kernel state, and `user_exception_handler` already panics for a kernel-mode exception of any other class |
| EL1 / kernel | uncontained | panic |

The asymmetry in row five is deliberate and is the conservative choice:
"contained" says the *machine* is intact, not that the kernel's own data
structures are. A kernel that continues past a corrupted page of its own
is the failure mode this table exists to avoid.

### 4. The panic stops lying

`aarch64_trap_entry`'s `default` arm keeps panicking for the slots that
should never be taken (SP0, AArch32) and stops relabelling them
`ARCH_TRAP_GENERAL_PROTECTION`. SError gets its own kind and its own
name in `kind_names` (`trap.c:28`), so `arch_trap_name` reports what
happened.

### 5. What this unit does not do

- **No RAS error record parsing.** `ERR<n>_STATUS`, the ACPI tables and
  the polling of corrected-error counters are a subsystem, not a clause.
  This unit reads the syndrome the exception itself carries.
- **No FIQ handling.** FIQ shares the `default` arm today. This kernel
  routes everything to IRQ through the GIC and takes no FIQ, so it stays
  a panic — but it stays one *deliberately*, with a comment saying so,
  instead of by falling off the end of a switch.
- **No `ESB` / `DISR_EL1` synchronization.** Deferring an SError across a
  critical section is a real technique and a separate unit; without
  FEAT_RAS there is nothing to defer into.
- **No recovery of the faulting page.** "Contained" here means the
  process dies, not that the kernel repairs anything.

## Tests

The honest split, because this is hardware the test host does not have:

| test | claim | how it fails if the change is reverted |
| --- | --- | --- |
| `trap-async-class` (host or kernel unit) | the classifier maps every `AET` encoding, `IDS = 1`, and every reserved value to the right class | a table test over the encodings from the ARM ARM; revert it and the reserved values stop being uncontained |
| `trap-async-el0` (aarch64) | a virtual SError injected while EL0 runs kills **that process** and the machine survives | without the dispatch it panics, and the boot test fails on the panic rather than on an assertion |
| `trap-async-el1` (aarch64) | the same injected with EL1 running panics | without the EL1 arm the kernel continues past a kernel-state error |
| `trap-async-x86` | vector 18 through the paranoid path reaches the policy with a frame, and a kernel-mode frame panics | `int $18` drives the vector exactly as `arch_test_paranoid_entry` drives `int $2` today (`x86_64/trap.c:156`) |

**The injection.** `HCR_EL2.VSE` makes the hypervisor deliver a virtual
SError to EL1, and this kernel has an EL2 stub with a small call ABI
already (`HV_EL2_CALL_RUN`, `_TLBI`, `_VGIC`, `_HANDBACK` —
`kernel/include/arch/el2.h:51-71`). One more call sets `VSE`, which makes
the trigger deterministic rather than a wait for hardware.

**What the tests do not cover, stated rather than implied.** The CI CPU
models are `cortex-a72` (default) and `cortex-a76` (`make test-guard`),
and `cortex-a72` is ARMv8.0 with no FEAT_RAS — so on the default boot an
injected SError carries no `AET` and classifies as uncontained, which
exercises the *panic* arm and not the *contained* one. Whether QEMU's
`cortex-a76` implements FEAT_RAS well enough to set `VSESR_EL2` is
**checked in step 1 of the plan and not assumed here**; if it does not,
the contained arm is reached only by the classifier's table test and the
report as built must say so plainly rather than claim a coverage it does
not have. x86-64's `int $18` drives the vector and the policy but sets no
MCE banks, so its classifier input is stubbed: the policy is tested, the
bank decoding is reviewed.

That is a weaker testing story than the last four units and the report
says so up front. It is still much stronger than the current state, in
which the behaviour is untested *and* wrong.

## Affected files

| file | change |
| --- | --- |
| `kernel/include/arch/trap.h` | `ARCH_TRAP_ASYNC_ERROR`, `enum arch_async_error`, `arch_async_error_class` |
| `kernel/arch/aarch64/trap.c` | the SError kinds dispatched rather than defaulted; `kind_names`; the ESR classifier |
| `kernel/arch/aarch64/vectors.S` | the preamble's claim about SError stops being true and says what is true |
| `kernel/arch/aarch64/hv_el2.c`, `hv_el2_switch.S`, `kernel/include/arch/el2.h` | one EL2 call that sets `HCR_EL2.VSE`, for the test |
| `kernel/arch/x86_64/trap.c` | `#MC` registered; `MCG_STATUS` classification |
| `kernel/process/process.c` | `user_exception_handler`: the new kind maps to `SIGBUS` |
| `kernel/core/selftest.c`, the arch test files | the four tests |
| `docs/kernel/arch/aarch64/design.md`, `docs/kernel/arch/*/invariants.md` | the classification and the policy table |
| `docs/audit/2026-09-deferred-work-inventory.md` | §3's row: the SError clause struck, the rest left |
| `README.md` | the Status entry |

## New APIs

`enum arch_async_error`, `arch_async_error_class(frame)`,
`ARCH_TRAP_ASYNC_ERROR`, and one EL2 call number for the test injection.
No syscall, no uapi change.

## Invariant

**A1. An asynchronous hardware error ends the machine only when the
hardware says the machine is unsound.** The classifier returns
*uncontained* for everything it does not positively recognise —
`IDS = 1`, a reserved `AET`, a CPU without FEAT_RAS, `RIPV = 0` — and
*contained* only for the encodings the architecture defines as
attributable. A contained error taken from EL0 kills that process with
`SIGBUS`; taken from EL1 it panics, because "the machine is intact" is
not "the kernel's state is intact". Check: `trap-async-class` over the
encodings, `trap-async-el0` and `trap-async-el1` by injection,
`trap-async-x86` for the policy. Gap: no CI CPU model is known to
implement FEAT_RAS, so the contained arm may be reachable only in the
table test — the unit as built must say which.

## Migration plan

1. **Find out what the CI CPUs implement.** Read `ID_AA64PFR0_EL1.RAS`
   on both boots and print it. Everything below is shaped by the answer,
   and guessing it is how this report would go wrong.
2. The classifier and its table test — pure, arch-shaped in, arch-free
   out, and testable before anything is dispatched anywhere.
3. The AArch64 dispatch, the trap kind, the `SIGBUS` mapping, and the
   EL2 injection call; the two injection tests.
4. x86-64: register `#MC`, classify from `MCG_STATUS`, the policy test.
5. Docs: invariant A1, the arch design documents, the inventory clause,
   the README entry.

## Risks

- **A wrong "contained" is worse than the present panic.** A machine
  continuing past an error it misread can corrupt a filesystem, and the
  crash suite would not necessarily catch it. The mitigation is the
  classifier's default and nothing else, which is why it is a separate
  function with a table test rather than a condition inside a dispatcher.
- **The hardware is not in CI.** No test host produces a real SError or a
  real machine check, so the unit is tested by injection and by table,
  and the report as built must state what remains reviewed rather than
  tested. This is the same shape as the pointer-auth mask, which shipped
  wrong twice for exactly this reason
  (`docs/audit/next-subsystem-*`, the feature-register work).
- **An SError can arrive anywhere**, including inside the scheduler. The
  kill is queued through the existing deferred path, which the current
  code already relies on for `#DB`; if that path turns out not to be safe
  from this context, that is the finding and the unit reports it rather
  than working around it.

## Alternatives considered

- **Leave it.** The row has been open since the audit. The cost is that
  an unprivileged program can end the machine, and the fix is small and
  bounded.
- **Panic with a better message and stop there.** Half the value for most
  of the work: the panic's label is wrong today and fixing that alone
  leaves the denial of service.
- **A full RAS subsystem** — error records, ACPI, corrected-error
  polling. That is where this leads and it is not one unit. This one
  reads the syndrome already in the frame.
