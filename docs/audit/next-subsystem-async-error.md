# NEXT SUBSYSTEM — a hardware error one process caused, and everyone pays for

Date: 2026-09-17. Tree: `main` at 98c1314 (after PR #171, the socket's
verdict). Chosen from `docs/audit/2026-09-deferred-work-inventory.md` §3.

**Subsystem: the asynchronous hardware error. An SError on AArch64 and a
machine check on x86-64 both end in `panic` today — including the ones
the hardware has already **corrected**, and including the ones it
describes precisely, under a panic line that names the wrong exception
class.**

Takes up the correctness clause of §3's AArch64-hardening row, *"a
user-triggerable SError panics the kernel"*. That row also names UAO,
E0PD, BTI, PAC, device-tree parsing and PSCI variations; this unit takes
**only** the SError clause and strikes only that, because the others are
feature work and this one is a defect.

**Built as PR #173.** Everything after this section is the report as
written; this section is what the building changed.

### As built

**Step 1 answered, and it changed the plan for the better.** The report
refused to assume what the CI CPUs implement. They report:

| boot | |
| --- | --- |
| aarch64 `cortex-a72` (default) | `ras:0` — **no FEAT_RAS**, as expected |
| aarch64 `cortex-a76` (`make test-guard`) | **`ras:1`** — FEAT_RAS present |
| x86-64, both CPU models | **10** machine-check banks |

So the *corrected* arm is reachable by real injection after all, on the
guard boot, and not only by a table. The report said it must say which,
and it is this.

**And the injection then failed, which was the most useful thing that
happened.** Two reasons, both real and neither in the report:

- **`HCR_EL2.VSE` is inert without `HCR_EL2.AMO`.** The host runs with
  `HCR_EL2 = RW` and nothing else (`hv_el2_switch.S`, the return-to-host
  path), and a virtual SError is generated only while `AMO` is 1. The
  injection call sets both, and a second call takes them back: `VSE` is
  not self-clearing, so while it is set the abort is pending continuously
  and is re-taken on every return with the mask clear. The test asserts
  the count *moved*, not that it moved by one.
- **EL1 runs with `PSTATE.A` masked for the kernel's entire life.**
  `entry.S` does `msr daifset, #0xF`; the only unmask anywhere is
  `daifclr, #2`, which is IRQ. So the kernel does not take an
  asynchronous abort while it runs — it stays pending. **EL0 does not
  have that property**: user mode is entered with `SPSR = 0`, DAIF clear,
  so an SError there is taken immediately, and that is the path this unit
  governs today.

That second one refines the report's own premise. "Every SError panics"
is true of EL0 and, at EL1, is true only of an SError that something
unmasks. Whether EL1 should unmask `A` is a real decision with its own
risk — the kernel would then take an abort at any instruction — and
I-ARCH-16 records it as a gap rather than settling it in a unit that was
not about it.

**The invariant is I-ARCH-16, not A1.** The report named it A1;
`docs/kernel/arch/aarch64/invariants.md` already uses A1–A24, and the
rule is cross-arch anyway, so it belongs in the generic document. All
seventeen code references were swept, not the ones remembered.

**`#MC` was never registered.** Writing the design document found it: the
classifier and the test hook existed, so every test passed, and vector 18
was still unregistered — the unit changed nothing on x86-64. The handler
is registered now, from an arch-neutral `arch_async_error_init` called
after `interrupt_init`, and **the registration is asserted**, because it
is the one part of x86's dispatch that software can check. Without that
assertion this gap would have shipped.

**The corrected path counts and does not print.** The report's Risks
section asked whether `kdebug` is safe from an arbitrary context; the
answer taken is not to find out — on x86-64 the handler runs on the
machine-check IST stack through the paranoid entry, which I-ARCH-7 says
must not fault.

**A regression of mine, caught where the code warned it would be.** The
first cut appended the RAS and MCA facts to the `hardening:` line, whose
comment says *"the guard boot's harness requires it whole"* — and both
guard boots failed on `missing marker /^\[ INFO\] hardening: x86-64: nx
smep smap umip$/`. They have their own `async-error:` line now, which is
better placed anyway: neither is a hardening feature.

### The four bug-proofs, each run

| revert | what failed |
| --- | --- |
| x86's at-least-one-valid-bank requirement | `trap-async-class`: *"no valid bank: 'every valid bank is clean' is vacuously true of none"* |
| the FEAT_RAS requirement | `trap-async-class`: *"no FEAT_RAS: there is no AET to have read"* |
| the SError vector back in the `default` arm | **`KERNEL PANIC: exception in an unsupported vector slot 7 (EC 0x2f)`** — the original defect, dead in 8.8 s |
| `arch_async_error_init` not called | `trap-async-inject`: *"no machine-check handler registered: vector 18 still panics through arch_trap_unhandled"* |

The first attempt at proof one is worth recording: it removed the
requirement in a way that left a variable unused, so `-Werror` rejected
it and the run produced no failing assertion at all. Read only for the
assertion, that looks like a proof that did not fire.

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
| 0b010 | UEO | Restartable | the error is contained — but see Design §3: *contained* is not *attributable* |
| 0b011 | UER | **Recoverable** | the same; this unit panics on both and says why |
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
privilege.

**This unit does not close that**, and the first draft of this report
claimed it would. Ending the machine is the *correct* response to an
uncontained error, and telling a contained one apart from an attributable
one needs the RAS error records this unit excludes (Design §3). What it
closes is the case next to it: an error the hardware **corrected** —
where nothing is wrong and the machine dies anyway.

## Why it matters

- **It is a correctness gap reachable today**, which §6 puts ahead of
  feature work. The machine dies for something one process caused, and
  the hardware said so.
- **A corrected error kills the machine.** The hardware found a fault,
  repaired it, and reported it for the record; this kernel panics. There
  is no reading of that which is right, and it needs only the classifier.
- **The panic lies about itself.** Relabelling the frame
  `ARCH_TRAP_GENERAL_PROTECTION` means the one diagnostic a rare hardware
  fault leaves behind names the wrong exception class. Whoever reads that
  panic next will spend their time in the wrong place, which is the
  defect this repository has spent four units removing from other
  reports.
- **The two architectures disagree about nothing here**, which makes it
  cheap: one classification, one policy, two small arch shims.

## Design

### 1. A trap kind, and a dispatch point of its own

`enum arch_trap_kind` (`kernel/include/arch/trap.h:21-27`) gains
`ARCH_TRAP_ASYNC_ERROR`, and `ARCH_TRAP_KIND_COUNT` moves with it, so the
kind gets a vector number through `arch_trap_vector` and
`interrupt_dispatch` can route it.

**It does not reuse `user_exception_handler`,** and the first draft of
this report said it would. That function maps a vector to a signal
unconditionally and sends every kernel-mode frame to
`arch_trap_unhandled`; it has no way to ask what class of error this is,
so routing the new kind through it would kill a process for a *corrected*
error and continue past an uncontained one. Review caught that, and the
fix is a handler of the async kind's own, registered like any other, that
consults the classifier **first** and then decides. The generic mapping
stays exactly as it is for the five kinds it already serves.

### 2. One classifier, arch-shaped in and arch-free out

```c
/* kernel/include/arch/trap.h */
enum arch_async_error {
    ARCH_ASYNC_CORRECTED,     /* nothing is wrong yet: count it and continue */
    ARCH_ASYNC_CONTAINED,     /* the machine is intact -- but see §3: not attributable, so this unit still panics */
    ARCH_ASYNC_UNCONTAINED,   /* the machine's state is not trustworthy: panic */
};
enum arch_async_error arch_async_error_class(const struct arch_trap_frame *);
```

AArch64 reads `ESR_EL1`: `IDS == 1` means the syndrome is
implementation-defined and nothing may be assumed — **uncontained**, the
safe answer. `IDS == 0` reads `AET` by the table above. A CPU without
FEAT_RAS reports no `AET`, so every SError on it is uncontained, which is
correct and is the only answer available.

x86-64 reads more than the draft of this report did. `MCG_STATUS`'s
`RIPV` alone is not a conservative test, and review said why: it says
nothing about whether the processor's context is corrupt, whether a
record is even valid, or what the *other* banks reported. The classifier
walks them:

- **`MCG_CAP.Count`** gives the number of banks, and every one of them is
  read. An uncontained error recorded in a bank this classifier did not
  look at is the failure mode that makes "check `RIPV`" unsafe.
- **`MCi_STATUS.VAL == 0`** — no record; that bank says nothing.
- **`MCi_STATUS.PCC`** — *processor context corrupt*. Uncontained,
  whatever `RIPV` says. This is the bit whose absence made the draft's
  test wrong rather than merely incomplete.
- **`MCi_STATUS.OVER`** — a record was overwritten, so what is there is
  not the whole story. Uncontained.
- **`MCi_STATUS.UC`** — uncorrected. With `MCG_STATUS.RIPV == 0`,
  uncontained; the aggregate rule below covers the rest.
- **`MCG_STATUS.RIPV`/`EIPV`** — read last, and only able to make an
  error *less* severe, never more.

The aggregate is the conservative one, and it is stated as a positive
requirement rather than as "every valid bank is clean" — because that
phrasing is **vacuously true when no bank is valid**, which would
classify a machine check carrying no record at all as corrected and let
execution continue past an error nothing described. Review caught that
in the first revision of this section, and it is the same shape as a
test that passes by asserting nothing.

**Corrected** requires all four, positively:

1. **at least one bank with `VAL == 1`** — something has to have been
   reported, or there is nothing to have understood;
2. every valid bank has `UC == 0`;
3. no bank has `PCC` or `OVER`;
4. `MCG_STATUS.RIPV == 1`.

**Uncontained** otherwise — including the empty case, where no bank is
valid. One bank's silence never outvotes another's report, and the
silence of all of them is not a clean bill of health.

**The rule the classifier obeys, and the reason it is a separate
function: anything not positively known to be contained is uncontained.**
A missing feature, an unknown encoding and a reserved value all end in
panic. The failure this unit must not introduce is a machine that
continues after an error it did not understand.

### 3. What the syndrome does **not** say, and what that costs

The first draft of this report had a six-row table in which a *contained*
SError taken while EL0 ran killed that process with `SIGBUS`. Review
refused it, correctly, and the reason is the word in the middle of the
subsystem's own name: **asynchronous**.

`AET = UER` says the error is *recoverable*. It does not say **who
caused it**. The frame identifies the context that was interrupted when
the abort was *delivered*, which for an imprecise or deferred error need
not be the context that provoked it — a DMA from a device, or an
uncorrected line written long ago and read now by the memory controller,
lands on whoever happens to be running. Killing that process would
punish a bystander *and* leave the real source untouched, which is worse
than the panic it replaced: the machine would continue, quietly wrong,
having blamed the wrong program.

The architecture does supply attribution, and it is not in `ESR_EL1`: it
is in the RAS error records (`ERR<n>_STATUS`, `ERR<n>_ADDR`), which this
report has already excluded as a subsystem of their own. So the honest
policy is shorter than the draft's:

| class | what happens, at either exception level |
| --- | --- |
| corrected (`CE`) | counted, logged at `kdebug`, **execution continues** — the hardware fixed it and nothing is wrong yet |
| everything else | **panic**, named as an asynchronous abort and printing the syndrome |

**No process is killed by this unit**, because nothing in the frame
entitles it to choose one. That is a smaller unit than the draft claimed
and it is the one the evidence supports.

What remains worth doing, and is not small:

- **A corrected error currently kills the machine.** The hardware
  detected a fault, *repaired it*, reported it for the record — and this
  kernel panics. That is the clearest defect in the row and it is fixed
  by the classifier alone.
- **The panic names the wrong thing** (Problem §1), so the one artefact a
  rare hardware fault leaves behind sends its reader to the wrong place.
- **The seam exists afterwards.** The classifier and the dispatch point
  are what an attribution unit would need, and it can be written when
  there are error records to read.

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
- **No recovery of anything.** A corrected error is logged and counted;
  the kernel repairs nothing and reclaims nothing. The page a corrected
  error touched is left exactly as it was.
- **No process is killed** — Design §3. That is not a simplification to
  be lifted later by this unit; it is the attribution unit's to lift.

## Tests

The honest split, because this is hardware the test host does not have:

| test | claim | how it fails if the change is reverted |
| --- | --- | --- |
| `trap-async-class` (table test) | every `AET` encoding, `IDS = 1` and every reserved value map to the right class | revert the default and the reserved values stop being uncontained — the assertion is on the *reserved* rows, which is where a table test earns its keep |
| `trap-async-class-x86` (table test) | over synthetic `MCG_STATUS`/`MCi_STATUS`/`MCG_CAP` values: `PCC`, `OVER`, an uncontained bank *after* a clean one, and **no valid bank at all** each give uncontained; a single valid clean bank with `RIPV` gives corrected | drop any one of `PCC`, `OVER`, the multi-bank walk or the at-least-one-valid requirement and exactly one row fails — one row per bit, so the test says which. The no-valid-bank row is the one a "every valid bank is clean" rule passes vacuously |
| `trap-async-corrected` (aarch64) | a virtual SError classified corrected is counted, logged, and **execution continues** | without the dispatch it panics, and the boot test fails on the panic rather than an assertion |
| `trap-async-panic` (aarch64) | an SError that is not positively corrected panics, and the panic names an asynchronous abort | without the classifier's default a machine continues past an error it did not understand — the test asserts the *name*, since the defect it replaces was a panic that said "general protection" |
| `trap-async-x86` | vector 18 through the paranoid path reaches the new handler with a frame | `int $18` drives the vector exactly as `arch_test_paranoid_entry` drives `int $2` today (`x86_64/trap.c:156`) |

**The injection.** `HCR_EL2.VSE` makes the hypervisor deliver a virtual
SError to EL1, and this kernel has an EL2 stub with a small call ABI
already (`HV_EL2_CALL_RUN`, `_TLBI`, `_VGIC`, `_HANDBACK` —
`kernel/include/arch/el2.h:51-71`). One more call sets `VSE`, which makes
the trigger deterministic rather than a wait for hardware.

**What the tests do not cover, stated rather than implied.** The CI CPU
models are `cortex-a72` (default) and `cortex-a76` (`make test-guard`),
and `cortex-a72` is ARMv8.0 with no FEAT_RAS — so on the default boot an
injected SError carries no `AET` and classifies as uncontained, which
exercises the *panic* arm and not the *corrected* one. Whether QEMU's
`cortex-a76` implements FEAT_RAS well enough to set `VSESR_EL2` is
**checked in step 1 of the plan and not assumed here**; if it does not,
the corrected arm is reached only by the classifier's table test and the
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
| `kernel/core/` (the async handler's home) | a handler for the new kind that consults the classifier before deciding; `user_exception_handler` is **not** touched |
| `kernel/core/selftest.c`, the arch test files | the four tests |
| `docs/kernel/arch/aarch64/design.md`, `docs/kernel/arch/*/invariants.md` | the classification and the policy table |
| `docs/audit/2026-09-deferred-work-inventory.md` | §3's row: the SError clause struck, the rest left |
| `README.md` | the Status entry |

## New APIs

`enum arch_async_error`, `arch_async_error_class(frame)`,
`ARCH_TRAP_ASYNC_ERROR`, and one EL2 call number for the test injection.
No syscall, no uapi change.

## Invariant

**I-ARCH-16. An asynchronous hardware error lets the machine continue only when
the hardware says it corrected the error, and is never blamed on a
process.** The classifier returns *corrected* only for a syndrome that
positively says so — `AET = CE` with `IDS = 0` on AArch64; on x86-64
**at least one valid bank**, every valid bank `UC == 0`, no `PCC`, no
`OVER` and `RIPV == 1`, across all `MCG_CAP.Count` banks. The first of
those four is not redundant: without it "every valid bank is clean" is
true of *no banks at all*, and a machine check carrying no record would
classify as corrected. Everything else is uncontained and panics:
`IDS = 1`, a reserved `AET`, a CPU without FEAT_RAS, an invalid record,
no record, a bank this classifier has not read. **No process is killed**,
at either exception level, because an asynchronous abort's frame names
the context that was interrupted and not the one that caused it —
attribution needs the RAS error records, and the unit that reads them is
the one that may kill. Check: `trap-async-class` and
`trap-async-class-x86` over the encodings and the bank combinations,
`trap-async-corrected` and `trap-async-panic` by injection,
`trap-async-x86` for the dispatch. Gap: no CI CPU model is known to
implement FEAT_RAS, so the *corrected* arm may be reachable only in the
table test — the unit as built must say which.

## Migration plan

1. **Find out what the CI CPUs implement.** Read `ID_AA64PFR0_EL1.RAS`
   on both boots and print it, and `MCG_CAP` on x86-64. Everything below
   is shaped by the answer, and guessing it is how this report would go
   wrong.
2. The classifier and its table test — pure, arch-shaped in, arch-free
   out, and testable before anything is dispatched anywhere.
3. The AArch64 dispatch, the trap kind, the async handler of its own
   (not `user_exception_handler`), and the
   EL2 injection call; the two injection tests.
4. x86-64: register `#MC`, classify by walking every `MCG_CAP.Count`
   bank for `VAL`/`PCC`/`OVER`/`UC` before reading `MCG_STATUS`, and the
   dispatch test.
5. Docs: invariant I-ARCH-16, the arch design documents, the inventory clause,
   the README entry.

## Risks

- **A wrong "corrected" is worse than the present panic.** A machine
  continuing past an error it misread can corrupt a filesystem, and the
  crash suite would not necessarily catch it. The mitigation is the
  classifier's default and nothing else, which is why it is a separate
  function with a table test rather than a condition inside a dispatcher,
  and why the x86 side reads every bank rather than one status register.
- **The hardware is not in CI.** No test host produces a real SError or a
  real machine check, so the unit is tested by injection and by table,
  and the report as built must state what remains reviewed rather than
  tested. This is the same shape as the pointer-auth mask, which shipped
  wrong twice for exactly this reason
  (`docs/audit/next-subsystem-*`, the feature-register work).
- **An SError can arrive anywhere**, including inside the scheduler, and
  the handler runs in that context. Since no process is killed there is
  no deferred signal to queue — but the corrected path still logs, and
  `kdebug` from an arbitrary context is the risk that replaces it. The
  unit must establish that the logging path is safe from there, or count
  without printing and leave the printing to a reader; if neither is
  safe, that is the finding and the unit reports it rather than working
  around it.

## Alternatives considered

- **Leave it.** The row has been open since the audit, and a corrected
  error — one the hardware repaired — still ends the machine.
- **Kill the interrupted process on a contained error.** This was the
  first draft and review refused it: an asynchronous abort's frame names
  the context interrupted at delivery, not the one that caused the error,
  so the kill would land on a bystander and leave the source running. It
  is the right behaviour once there is attribution, and attribution is
  the next unit, not this one.
- **Panic with a better message and stop there.** That is most of what
  this unit does and it would be a defensible smaller one; the classifier
  is what makes the message right, and once it exists the corrected case
  costs one arm of a switch.
- **A full RAS subsystem** — error records, ACPI, corrected-error
  polling. That is where this leads, it is where attribution lives, and
  it is not one unit. This one reads the syndrome already in the frame.
