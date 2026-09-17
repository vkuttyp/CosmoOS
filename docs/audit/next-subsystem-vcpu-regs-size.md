# NEXT SUBSYSTEM — an invariant written down thirteen times, checked once, and false in half the tree

Date: 2026-09-17. Tree: `main` at 9a7a27e (after PR #160, the writeback
thread inside a mount's replay). Chosen from
`docs/audit/2026-09-deferred-work-inventory.md` §3.

**Subsystem: the UAPI's VMState layout rule — stated truthfully for
each architecture, and checked where it cannot be evaded.**

This report closes the inventory's §3 row that reads "`struct
cosmo_vcpu_regs` is **496 bytes on AArch64 and 448 on x86-64**, while
its own comment and `tests/host/test_hv.c:75` say both are 448".

## Problem

`struct cosmo_vcpu_regs` is the VMState: the UAPI type, the kernel API
type and the arch interface's state type, with no translation layer
between them. It is defined twice, once per architecture, because a
register file is per architecture — and the header states a rule about
the pair:

```c
/* ... Both blocks are 448 bytes so the system call, the copies and the
 * tests do not vary with the architecture
 * (docs/kernel-services/virtualization/design.md). */
#if defined(__aarch64__)
struct cosmo_vcpu_regs {         /* 448 bytes */
```

The rule is false. Measured, on this tree, with the tree's own header:

| architecture | `sizeof(struct cosmo_vcpu_regs)` | comment says |
| --- | --- | --- |
| x86-64 | 448 | 448 |
| AArch64 | **496** | 448 |

The AArch64 block is 54 eight-byte fields plus `reserved[8]` = 62 slots
= 496 bytes. The x86-64 block is 47 plus `reserved[9]` = 56 slots = 448.
Nobody sized the reserved array to land on the stated number.

### It is not one wrong comment

The number is written down in **thirteen lines across eight files** —
nine in documentation, four in code. Twelve of them are wrong today:

| where | what it says |
| --- | --- |
| `kernel/include/uapi/cosmo/syscall.h:664` | the rule: "Both blocks are 448 bytes so the system call, the copies and the tests do not vary with the architecture" |
| `kernel/include/uapi/cosmo/syscall.h:668` | `/* 448 bytes */` over the **AArch64** block — wrong, it is 496 |
| `kernel/include/uapi/cosmo/syscall.h:681` | `/* 448 bytes */` over the **x86-64** block — the one line that is **correct** today, and the one the fix makes wrong |
| `tests/host/test_hv.c:75` | `CHECK(sizeof(struct cosmo_vcpu_regs) == 448)` |
| `docs/kernel-services/virtualization/invariants.md:89` | **V8, "The UAPI layouts are fixed"**, *checked by: the host test* |
| `docs/kernel-services/virtualization/design.md:23` | the annotated layout, `/* 448 */` |
| `docs/kernel-services/virtualization/design.md:1275` | "Both blocks stay 448 bytes so the system-call shape..." |
| `docs/kernel-services/virtualization/design.md:1464` | the assertion, quoted |
| `docs/kernel-services/virtualization/api.md:27,30` | "448 bytes, the VMState"; "both 448 bytes" |
| `docs/kernel-services/virtualization/architecture.md:133` | "the VMState, 448 bytes" |
| `docs/kernel-services/virtualization/testing.md:54` | "**Layouts**: `sizeof(...) == 448`" |
| `README.md:477` | "both 448" |

`invariants.md` V8 is the load-bearing one: this is a *stated invariant*
with a named check. The check does not hold it.

The thirteenth line is worth separating out. `syscall.h:681` says 448
over the x86-64 block and is **right**; it is in the list because the
rule is about the *pair*, so any fix that changes the shared size would
have to move it too. The recommended fix does not: it touches the twelve
that are wrong and leaves that one alone. Which line is already correct
turns out to decide the whole unit — see *Design*.

### And the check cannot fire where it is false

`make host-test` builds for the **build host**, with no `-arch`. So
`test_hv.c` compiles whichever arm of the `#if` the host matches:

- on an **x86-64** host (which is what CI runs — the runner is x86-64
  Linux), it sees the 448-byte x86 block, and the assertion passes;
- on an **AArch64** host, it sees the 496-byte AArch64 block, and
  `make host-test` **fails**, which is the state of the tree today for
  anyone developing on an arm64 machine.

So the one check of V8 passes in CI for the same reason it is vacuous
there: CI never compiles the block that violates it.

There is a sharper point underneath. `test_hv.c` is a test of the x86-64
**SVM** backend — `HOST_HV_SRCS` is `... kernel/arch/x86_64/svm_npt.c
tests/host/test_hv.c` — and the struct is used in that file for nothing
but this one `sizeof`. Which architecture's UAPI it checks is decided by
who ran `make`, not by the code under test. On an arm64 host the file
asserts the layout of the *AArch64* register file while testing the
*x86-64* nested page tables.

## Current implementation

`struct cosmo_vcpu_regs` crosses `SYS_vcpu_regs` (47) in both
directions, by `sizeof` on both sides of the copy. Within one build that
is self-consistent: the kernel, libc and the guest tools are compiled
from the same header for the same architecture, so **no running guest is
affected today, and this is not a live corruption**. What is broken is:

1. a stated invariant (V8) that is false;
2. its only check, which cannot run where it fails;
3. `make host-test`, which is red on every AArch64 development machine,
   and **not only for this one suite**: `test_hv` is ninth of the
   twenty-three in `HOST_TESTS`, and the target stops there, so the
   **fourteen suites ordered after it never run at all** on an arm64
   host — `test_vmx`, `test_hv_s2`, `test_reloc_aarch64`, `test_virtq`,
   `test_cred`, `test_quiesce`, `test_lockdep`, `test_lockup`,
   `test_lz4`, `test_chacha20`, `test_fbvalid`, `test_fdt`,
   `test_vblk_dev`, `test_vnet_dev`;
4. nine documentation lines, in six files, that assert a wrong number —
   two of which (`design.md:1275` and the header comment it cites) give
   the *reason* the rule exists, so a reader takes it as deliberate.

The tree already knows how to hold a layout rule. `_Static_assert` is
used for exactly this, in exactly this kind of place:

```
kernel/arch/aarch64/include/aarch64/trapframe.h:23
  _Static_assert(sizeof(struct arch_trap_frame) == 0x130, "trap frame layout");
kernel/arch/aarch64/include/aarch64/hv_ctx.h:132
  _Static_assert(sizeof(struct hv_sysregs) == HV_CTX_SYS_COUNT * 8, "hv_sysregs order");
kernel/arch/x86_64/user.c:136
  _Static_assert(offsetof(struct arch_user_regs, rsp) == 0x38 && ...);
```

`kernel/include/kernel/compiler.h:41` even wraps it as
`STATIC_ASSERT(cond, msg)`. The UAPI's own layout rule is the one such
invariant that is left to a comment and a host test.

## Why it matters

- **An invariant with a check that cannot fail is worse than no
  invariant.** V8 says "*checked by*: the host test". A reader of
  `invariants.md` is entitled to treat that as verified. It is verified
  on one architecture and unverifiable on the other, and the one it
  cannot check is the one that breaks it. This is the same shape as the
  fsck unit's subject — invariants the checker takes on trust — one
  layer down, in the ABI.
- **It is a UAPI.** `reserved[]` exists so fields can be added without
  breaking the ABI. On AArch64 the reserved area is eight slots and the
  struct is 48 bytes larger than every document says; anyone sizing a
  buffer, a copy or a compatibility shim from the documented number is
  wrong by 48 bytes on one architecture. Nothing does that today. The
  rule exists precisely so that nothing has to think about it.
- **Fourteen host suites have never run on this machine.** This is the
  part that is worth more than the ABI fix. `make host-test` is a CI
  step and a gate a unit is supposed to pass before pushing; on arm64 it
  dies at the ninth of twenty-three suites, so everything after
  `test_hv` — the VMX and stage-2 page-table tests, the AArch64
  relocation tests, virtqueue, credentials, quiesce, lockdep, lockup,
  LZ4, ChaCha20, framebuffer validation, FDT, and the two virtio device
  models — has never been executed here. CI runs them on x86-64, so they
  are not unrun in absolute terms; they are unrun in the one place a
  developer iterates, which is where a test earns its keep. And because
  the gate is red for an unrelated reason, the habit becomes to skip it
  — which is how this survived being noticed **twice**, in
  `next-subsystem-snap-deadlist.md:699` and
  `next-subsystem-unmount-leak.md:763`, each time recorded in passing by
  a unit that had other work to do.

  **Measured while writing this report**: with the assertion corrected
  locally to 496 and nothing else changed, `make host-test` runs to
  completion on this arm64 machine with every suite passing. So the
  fourteen are blocked, not broken — there is no second defect hiding
  behind this one, and the unit's scope is exactly what it looks like.
  That is a fact worth having before committing to the work, and it is
  the kind of thing that is cheap to check and expensive to assume.
- **It is small, bounded and provable.** Unlike most of §3 this is a day
  of work with an exact success condition.

## Design

Two decisions, and the first one is not the one this report first
reached for.

### 1. Which size — and the answer is "both, as they are"

The obvious unit is to make the rule true by resizing a block. **That is
the wrong unit**, and the reason is three lines above the struct, in the
header's own preamble:

```
 * Shared verbatim between the kernel and user space. This is user ABI:
 * numbers and structures here are stable; add, never renumber.
```

So the tree already promises that these structures are stable. Measure
the two blocks against that promise rather than against the comment:

| | actual size | documented | which one is wrong |
| --- | --- | --- | --- |
| x86-64 | 448 | 448 | **nothing** — correct and stable |
| AArch64 | **496** | 448 | the **documentation** |

The AArch64 ABI is 496 and has been since `f8b88b2` introduced the EL2
backend. Nothing is broken in it; what is broken is every sentence that
describes it. And x86-64's block is correct today, so any option that
moves both to a shared number **breaks a correct, stable ABI in order to
make a comment true**.

`SYS_vcpu_regs` has no room to absorb that. It takes no buffer size and
no version, and copies `sizeof(struct cosmo_vcpu_regs)` in both
directions (`kernel-services/virtualization/hvsys.c:171-194`); the only
bound is `user_range_ok(ptr, sizeof(...))`, which checks that the range
is user memory, not that the caller allocated that much. A kernel built
with a larger struct writes past a smaller caller's buffer on get and
reads past it on set. Adding a size or version argument to make a
resize safe is a bigger unit than this finding justifies, and it would
be spending an ABI change to fix prose.

**So: change neither block.** Fix the twelve lines that are wrong, keep
the thirteenth (`syscall.h:681`) that is right, and drop the cross-arch
equality rule, which was never true and never bought anything — every
consumer already says `sizeof`, so "the system call, the copies and the
tests do not vary with the architecture" describes code that would not
have varied either way.

> This reverses what the first draft of this report recommended (512 for
> both blocks, with a compatibility discussion deferred). Review was
> right and the draft was wrong: it had noticed that the documentation
> disagreed with the code and concluded that the code should move. The
> preamble is what settles it, and it was three lines above the struct
> the whole time. The four options are kept under *Alternatives* with
> what each would cost, because the choice is the substance of this unit
> and a reader should see it made rather than asserted.

### 2. Where the rule is enforced

This part is unchanged, and it is the substance. The invariant moves out
of a comment and a host test into the header that states it:

```c
/* kernel/include/uapi/cosmo/syscall.h, after both blocks */
#if defined(__aarch64__)
_Static_assert(sizeof(struct cosmo_vcpu_regs) == 496, "AArch64 VMState layout");
#else
_Static_assert(sizeof(struct cosmo_vcpu_regs) == 448, "x86-64 VMState layout");
#endif
_Static_assert(sizeof(struct cosmo_vm_exit) == 64, "VMExit layout");
_Static_assert(sizeof(struct cosmo_vcpu_seg) == 16, "segment layout");
```

An assertion in the UAPI header fires in **every** translation unit that
includes it — kernel, libc, guest tools, host tests, both
architectures, every build type. The rule can no longer be true only
where someone happens to compile, and it cannot be satisfied vacuously.
That is what invariant V8 always claimed and never had.

Two constants instead of one is a real cost and worth naming: the pair
is no longer tied together by a single number, so nothing stops the two
blocks drifting to unrelated sizes. That is acceptable because nothing
in the tree wants them equal — and it is now *stated* rather than
assumed, which is the opposite of the situation today.

The header is UAPI, so this must be C11-and-later and must not depend on
kernel headers: `_Static_assert` directly, not `STATIC_ASSERT` from
`kernel/include/kernel/compiler.h`, which UAPI consumers do not have.
(C++ consumers get `static_assert`; there are none in this tree, and the
guard is one `#if defined(__cplusplus)` if that changes.)

### 3. What happens to the host test

`test_hv.c:75` becomes correct on both hosts by asserting the same pair
the header does — or, better, by deleting the line, since the header it
includes now asserts it unconditionally and a duplicate check that can
drift is worse than none. The report recommends **deleting it** and
having `testing.md` say where the rule now lives.

The deeper flaw stays worth recording: which architecture's UAPI that
file sees is decided by the build host, not the code under test. With
the assertion in the header it is harmless for this rule, because every
build asserts its own arm correctly. It is noted in `testing.md` as a
known property of the host suite rather than fixed here; pinning the
host tests' architecture is a different unit with a much larger blast
radius.

## Affected files

| file | change |
| --- | --- |
| `kernel/include/uapi/cosmo/syscall.h` | the rule at `:664` restated per architecture; `/* 448 bytes */` at `:668` → `/* 496 bytes */`; `:681` **unchanged**; **the four `_Static_assert`s** |
| `tests/host/test_hv.c` | `:75` deleted; the header it includes now asserts this in every build |
| `docs/kernel-services/virtualization/invariants.md` | V8: two sizes, and *checked by* the header, in every translation unit |
| `docs/kernel-services/virtualization/design.md` | `:23` the annotated layout's total; `:1275` the rule and its stated reason; `:1464` the quoted assertion |
| `docs/kernel-services/virtualization/api.md` | `:27`, `:30` — **and an `ABI stability:` line, which this file lacks** though `docs/README.md:66` requires one |
| `docs/kernel-services/virtualization/architecture.md` | `:133` |
| `docs/kernel-services/virtualization/testing.md` | `:54`; plus the note that the host suite's UAPI arch follows the build host |
| `README.md` | `:477`, and the Status entry |
| `docs/audit/2026-09-deferred-work-inventory.md` | the §3 row, struck |

**No structure changes and no kernel or libc source changes.** Every
user of the struct uses `sizeof`, and nothing resizes. If any file needs
editing beyond the list above, that is a finding — it means something
hard-coded the size, which is precisely the hazard, and it goes in the
report as built.

The missing `ABI stability:` line in `api.md` was found while checking
this and is folded in rather than left: `docs/README.md:66` requires
every `api.md` to state it, `docs/compat/linux/api.md` does so six
times, and the virtualization one — which documents a UAPI the header
calls stable — states it nowhere.

## New APIs

None. Four compile-time assertions, one corrected comment, and a rule
restated as two.

## Migration plan

1. **The assertions first, at the sizes the tree actually has** — 496
   and 448. Both architectures build immediately, because both
   assertions are already true. This is the whole safety argument for
   the unit: it adds a check and changes nothing.
2. **Prove the check is real**: temporarily assert 448 on AArch64 and
   watch the build fail; temporarily assert 496 on x86-64 and watch it
   fail there. Recorded in the commit message, not pushed as a state of
   the tree. This is the bug-proof — see Tests.
3. **Delete `test_hv.c:75`.** `make host-test` now passes on an arm64
   host for the first time, and the fourteen suites behind it run.
4. **The twelve wrong lines**, in one commit, swept by `grep -rn 448`
   rather than by memory — the table in *Problem* is the checklist. The
   sweep ends when the only surviving `448` outside `docs/audit/` is
   `syscall.h:681` and the x86-64 halves of the restated sentences.
5. **The `ABI stability:` line** in `api.md`.
6. `hv-vcpu-regs-roundtrip` (below), then README Status and the
   inventory row struck.

Each step builds both architectures. Step 3 runs the full gate list
including `make host-test` on this arm64 machine *and* the x86-64 host
path, since the point of the unit is that those differ.

## Tests

| test | claim | how it fails if the fix is reverted |
| --- | --- | --- |
| the header's `_Static_assert`s | each architecture's VMState is the size the documentation says | change either constant, or a field, and the **build** fails on that architecture. Compile-time, so it cannot be skipped, mis-run or flake |
| `make host-test` on an arm64 host | the host suite runs to completion | measured already: it does, once `test_hv.c:75` stops asserting a falsehood |
| `hv-vcpu-regs-roundtrip` (new, both arches) | a `SYS_vcpu_regs` set-then-get returns every field the backend does not document as normalised, and `reserved[]` reads back zero | drop a field from the copy: the round-trip names the offset that differs |

On the bug-proof: a compile-time assertion's "test run" is the build, on
both architectures, which every CI step already does. That is a stronger
guarantee than a runtime check and a weaker demonstration, which is why
step 2 of the plan deliberately breaks the build both ways and records
what it said.

**The round-trip test cannot assert that every field survives**, and
this is checked rather than assumed. The backends normalise on purpose:

| field | what happens | where |
| --- | --- | --- |
| `pending_irq` | ignored on set | the header says so |
| `rflags` | `(in \| 0x2) & ~(1<<3 \| 1<<5 \| 1<<15)` — the architecturally fixed bits | `kernel/arch/x86_64/svm.c:479` |
| `efer` | `in & ~EFER_SVME & ~EFER_LMA` | `svm.c:488` |
| `cr8` | read back as `vmcb->control.v_tpr & 0xF`, four bits | `svm.c:436` |

So the test asserts the *documented* behaviour: every other field
round-trips byte for byte, `reserved[]` reads back zero, and each
normalised field comes back as the rule above says it should — which
makes the test a check of that documentation too, rather than a hole in
it. A VMX backend that rejects unsupported `EFER` bits gets the same
treatment when it exists; today it is never executed
(`README.md:452`).

`hv-vcpu-regs-roundtrip` is the one piece of new runtime test surface,
and it is the thing V8 protects: the tree has no test today that a
vCPU's register file survives a set/get at all.

## Benchmarks

None, and nothing to measure: **no structure changes size**, so no copy
changes, in `SYS_vcpu_regs` or anywhere else. The unit adds compile-time
assertions and edits prose.

## Risks

- **Two constants can drift apart.** Real, and the cost of keeping both
  ABIs. Mitigated by both being asserted in the header in every build,
  which is strictly more checking than the single number had; and the
  pair being unequal is now stated rather than assumed.
- **Deleting `test_hv.c:75` removes a visible check.** It removes a
  *duplicate* one, and the replacement runs in every translation unit
  instead of one host binary. `testing.md` says where it went so a
  reader looking for it finds it.
- **The documentation churns in twelve lines**, which is why step 4 is a
  single sweeping commit driven by grep, with the exact count written
  down here to check against, and with the one line that must **not**
  change (`syscall.h:681`) named.
- **The cross-architecture rule is gone.** If a future unit wants the
  blocks the same size, it now has to add a size or version to
  `SYS_vcpu_regs` first, and this report should be read as saying that
  is the correct order — not as closing the door.
- **The deeper flaw is not fixed**: the host suite still compiles
  whichever UAPI arm matches the build host. This unit makes that
  harmless for this rule and records it; it does not pin the host
  tests' architecture.

## Alternatives considered

The first draft of this report recommended **(c)**. Review pointed out
that `SYS_vcpu_regs` carries no size or version, so a resize is
unguarded against a caller built from the older header — and following
that led to the header preamble, which says these structures are stable.
That is what moved the recommendation to (d). The rejected options, with
what each would actually cost:

- **(a) Shrink AArch64 to 448** (`reserved[8]` → `reserved[2]`). The
  smallest diff and the one the documentation already describes.
  Rejected twice over: it breaks the AArch64 ABI that has shipped since
  `f8b88b2`, and it spends that register file's expansion room down to
  two slots on the architecture whose EL1 register set is the one that
  has grown — the block already carries **20** named EL1 system
  registers.
- **(b) Grow x86-64 to 496** (`reserved[9]` → `reserved[15]`). Breaks
  the ABI that is currently correct, to match one that is not
  documented, and fixes the number without choosing it.
- **(c) Grow both to 512** (`reserved[8]` → `[10]`, `reserved[9]` →
  `[17]`). What the first draft recommended: a round size with headroom
  on both. It breaks **both** ABIs, and on x86-64 it breaks a block that
  every document describes correctly today. A `SYS_vcpu_regs` copy would
  grow by 16 bytes on AArch64 and 64 on x86-64 — worth stating in the
  right order, because the draft had it backwards, and the larger growth
  falls on the architecture that needed no change at all.
- **Fix only the comment and the test to say 496/448, and stop.** This
  is (d) without the header assertions, and it is what the two previous
  sightings effectively proposed by recording the discrepancy and moving
  on. It leaves V8 an invariant whose check still cannot run where it
  matters, and leaves the next person to find the same thing a third
  time. The assertions are the unit; the corrected prose is the tidying
  that comes with them.
