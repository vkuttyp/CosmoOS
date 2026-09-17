# NEXT SUBSYSTEM — an invariant written down thirteen times, checked once, and false in half the tree

Date: 2026-09-17. Tree: `main` at 9a7a27e (after PR #160, the writeback
thread inside a mount's replay). Chosen from
`docs/audit/2026-09-deferred-work-inventory.md` §3.

**Subsystem: the UAPI's fixed-layout rule — made true on both
architectures, and checked where it cannot be evaded.**

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
over the x86-64 block and is right; it is in the list because the rule
is about the *pair*, so a fix that changes the shared size has to move
it too. Any option below that leaves x86-64 at 448 touches twelve
lines; option (c) touches all thirteen.

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

Three decisions, in order.

### 1. Which number

The rule is worth keeping — one size across architectures is what lets
the syscall, the copies and the tests not branch — so the question is
which size both blocks should be.

| option | AArch64 | x86-64 | headroom after (a64 / x86) | cost |
| --- | --- | --- | --- | --- |
| **(a) 448**, shrink AArch64 | `reserved[8]` → `reserved[2]` | unchanged | 2 / 9 slots | every document stays correct as written; AArch64 headroom drops to two fields |
| **(b) 496**, grow x86-64 | unchanged | `reserved[9]` → `reserved[15]` | 8 / 15 | thirteen lines to update; 48 more bytes per `SYS_vcpu_regs` copy |
| **(c) 512**, grow both | `reserved[8]` → `reserved[10]` | `reserved[9]` → `reserved[17]` | 10 / 17 | thirteen lines to update; a power of two; most headroom |
| **(d) drop the rule** | — | — | — | per-arch assertions; the syscall and tests grow an arch branch |

**Recommendation: (c), 512.** (a) is the smallest diff and the one the
documentation already describes, and that is exactly what makes it the
wrong choice: it buys correctness by spending the AArch64 register
file's expansion room down to two slots, on an architecture whose EL1
system-register set is the one likely to grow: the block carries **20**
named EL1 system registers, and it has been extended once already
(`f8b88b2`, the AArch64 EL2 backend) since it was introduced
(`2c5117d`). (b) fixes the number without choosing it. (c) picks a round size
with room on both sides, and 512 is a size a reader recognises as
deliberate — which is the property the current 448 was supposed to have
and never had.

(d) is the honest fallback if review prefers not to touch the ABI at
all, and it is not absurd: a register file genuinely *is* per
architecture, and the rule's benefit ("the system call, the copies and
the tests do not vary") is small because every one of those uses
`sizeof` already. But it trades a checkable cross-arch invariant for two
arch-specific ones, and the tests are then the only thing tying them
together. It is listed under Alternatives with what would change.

### 2. Where the rule is enforced

In the header that states it, not in a host test:

```c
/* kernel/include/uapi/cosmo/syscall.h, after both blocks */
_Static_assert(sizeof(struct cosmo_vcpu_regs) == 512,
               "the VMState is one size on every architecture");
_Static_assert(sizeof(struct cosmo_vm_exit) == 64, "VMExit layout");
_Static_assert(sizeof(struct cosmo_vcpu_seg) == 16, "segment layout");
```

This is the substance of the unit. An assertion in the UAPI header fires
in **every** translation unit that includes it — kernel, libc, guest
tools, host tests, both architectures, every build type — so the rule
can no longer be true only where someone happens to compile. It cannot
be skipped, and it cannot be satisfied vacuously.

The header is UAPI, so the assertion must be C11-and-later only and must
not depend on kernel headers: `_Static_assert` directly, not
`STATIC_ASSERT` from `kernel/include/kernel/compiler.h`, which UAPI
consumers do not have. (C++ consumers get `static_assert`; there are
none in this tree, and the guard is one `#if defined(__cplusplus)` if
that changes.)

### 3. What happens to the host test

`test_hv.c:75` keeps its assertion — a redundant check is not a problem
— but it stops being *the* check, and V8's "*checked by*" changes to
name the header. The deeper flaw stays worth naming in the docs: which
architecture's UAPI that file sees is decided by the build host. With
the static assertion in the header that flaw is harmless for this rule
(both arms now assert the same size, so either arm passing means the
rule holds for that arm, and every build asserts its own arm). It is
recorded in `testing.md` as a known property of the host suite rather
than fixed here, because pinning the host tests' arch is a different
unit with a much larger blast radius.

## Affected files

| file | change |
| --- | --- |
| `kernel/include/uapi/cosmo/syscall.h` | `reserved[8]`→`reserved[10]` (AArch64), `reserved[9]`→`reserved[17]` (x86-64); the comment's number; **the three `_Static_assert`s** |
| `tests/host/test_hv.c` | `448` → `512`; the assertion demoted from sole check to a redundant one |
| `docs/kernel-services/virtualization/invariants.md` | V8: the number, and *checked by* now the header, in every build |
| `docs/kernel-services/virtualization/design.md` | `:23` the annotated layout and its total; `:1275` the rule and its reason; `:1464` the quoted assertion |
| `docs/kernel-services/virtualization/api.md` | `:27`, `:30` |
| `docs/kernel-services/virtualization/architecture.md` | `:133` |
| `docs/kernel-services/virtualization/testing.md` | `:54`; plus the note that the host suite's UAPI arch follows the build host |
| `README.md` | `:477`, and the Status entry |
| `docs/audit/2026-09-deferred-work-inventory.md` | the §3 row, struck |

No kernel or libc source changes are expected: every user of the struct
uses `sizeof`. **If any file needs editing beyond the list above, that
is a finding** — it means something had hard-coded the size, which is
the hazard the rule exists to prevent, and it goes in the report as
built.

## New APIs

None. Three compile-time assertions and a different constant.

## Migration plan

1. **The assertion first, against the current sizes**, one commit that
   fails to build on AArch64. This is the proof the check is real: with
   `== 448` asserted in the header, an AArch64 kernel build stops. It is
   not pushed as a state of the tree; it is step 1 of the branch so the
   review can see the failure and the diff that resolves it.
2. **Resize both blocks to 512** and set the assertion to 512. Both
   architectures build; `make host-test` passes on an arm64 host for the
   first time.
3. **The nine documentation lines**, in one commit, swept by
   `grep -rn 448` rather than by memory — the table above is the
   checklist, and the sweep must end with no line outside
   `docs/audit/` still saying 448.
4. **Tests** (below), then README Status and the inventory row struck.

Each step builds both architectures. Step 2 runs the full gate list
including `make host-test` on this arm64 machine *and* the x86-64 host
path, since the point of the unit is that those differ.

## Tests

| test | claim | how it fails if the fix is reverted |
| --- | --- | --- |
| the header's `_Static_assert` | the VMState is one size on every architecture | revert the resize: the **build** fails, on whichever architecture is wrong. This is the bug-proof and it is compile-time, so it cannot be skipped, mis-run or flake |
| `test_hv.c:75` (existing, retargeted) | the same, at 512 | fails on an arm64 host today; passes after |
| `hv-vcpu-regs-roundtrip` (new, both arches) | a `SYS_vcpu_regs` set-then-get returns every field, and the bytes in `reserved[]` come back zero | drop a field from the copy: the round-trip reports which offset differs. This is the test the size rule is *for* — it is what would catch a resize that silently truncated the copy |

The first is unusual as a bug-proof and worth stating plainly: a
compile-time assertion's "test run" is the build, on both
architectures, which every CI step already does. That is a stronger
guarantee than a runtime check and a weaker demonstration, so the
round-trip test is there to show the struct still carries what it
claims to.

`hv-vcpu-regs-roundtrip` is the one piece of new runtime test surface.
It is small and it is the thing V8 protects; the tree has no test today
that a vCPU's register file survives a set/get at all.

## Benchmarks

None required. The copy grows by 64 bytes on AArch64 and 16 on x86-64
per `SYS_vcpu_regs`, a call made at vCPU setup and on exits the owner
inspects — not in any hot loop. If review wants a number, `el2-vcpu` and
the hv host tests already time vCPU entry and exit.

## Risks

- **It is an ABI change.** Mitigated by everything in the tree being
  built together and every consumer using `sizeof`; the affected-files
  list above says what to check, and an edit needed outside it is a
  finding rather than a nuisance.
- **The reserved area shrinks as a fraction of the struct.** It grows in
  absolute terms on both architectures (8→10 and 9→17 slots), so this
  is a presentational risk, not a real one.
- **A static assertion in a UAPI header reaches consumers we do not
  compile.** There are none outside this tree today. The C++ guard is
  named in Design §2 and costs one line if that changes.
- **Choosing 512 makes the documentation wrong in nine lines until step
  3 lands**, which is why step 3 is a single sweeping commit driven by
  grep, and why the exact count is written down here to check against.
  It also makes `syscall.h:681` wrong, which is the one line currently
  right — worth saying, because a reviewer who checks only that the
  numbers changed will not notice a line that changed *from* correct.
- **The deeper flaw is not fixed**: the host suite still compiles
  whichever UAPI arm matches the build host. This unit makes that
  harmless for this rule and records it; it does not pin the host tests'
  architecture. If review wants that instead, it is a bigger unit and
  this one should not pretend to it.

## Alternatives considered

- **(a) Shrink AArch64 to 448.** Smallest diff, no documentation churn.
  Rejected: it spends the AArch64 expansion room down to two slots to
  preserve a number nobody chose, on the architecture whose system
  register set is the one that has been growing.
- **(b) Grow x86-64 to 496.** Fixes the number without choosing it; 496
  is as arbitrary as 448 and reads like a mistake rather than a size.
- **(d) Drop the rule; assert per-architecture sizes.** Defensible — a
  register file really is per architecture, and every user already says
  `sizeof`. It would mean: V8 restated as two invariants, the header
  asserting two constants, `test_hv.c` branching, and the documentation
  saying "448 on x86-64, 496 on AArch64" in nine lines. It is strictly
  less checkable across the pair, and it is the option to take only if
  review judges the cross-arch rule to have been a mistake from the
  start. Worth a sentence in review either way, because if the rule is
  not worth keeping then the right unit is smaller than this one.
- **Do nothing; fix only the comment and the test to say 496/448.**
  This is (d) done cheaply and is what the two previous sightings
  effectively proposed by recording the discrepancy and moving on. It
  leaves `make host-test` red on arm64 unless the test branches too, and
  it leaves V8 as an invariant whose check still cannot run where it
  matters. The unit exists because the third sighting should cost more
  than the first two.
