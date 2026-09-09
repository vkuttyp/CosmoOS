# NEXT SUBSYSTEM

Constitution §68: after the audit, name the next subsystem in this shape
and wait for the instruction to build it. This is the ninth such report
(the NIC, the USB host stack, AHCI, the machine's own console, floating
point, the signals a person can send, job control, and terminal modes;
all eight record their outcomes). Nothing in this one is implemented.

**Subsystem: address-space identifiers — ASIDs on AArch64, PCIDs on
x86-64 — so that switching processes stops emptying the TLB.**

## Problem

Every switch between two processes throws away every translation the
CPU has learned. On x86-64 `arch_mmu_activate` is one instruction, `mov
cr3`, which invalidates every non-global entry. On AArch64 it is worse
in two ways at once: the root is written with ASID 0 and followed by
`tlbi vmalle1is`, which is a full invalidate of the user half **and
inner-shareable, so it reaches every CPU in the machine**, not just the
one switching. With the boot test's default of four CPUs, each process
switch on one CPU empties the other three CPUs' user TLBs too, for a
process they may still be running.

The consequences, in order of how soon they bite:

- **The cost of a switch is paid again after it, in misses.** The
  incoming process starts cold: its stack, its text, its heap and libc
  each take a page walk (three to four memory reads on AArch64 at 4 KiB
  granules) before the first instruction that touches them runs. The
  FP/SIMD unit measured a whole thread switch at ~21,600 ns on AArch64
  under QEMU and ~2,700 ns on x86-64, and that figure does not include
  the misses that follow a *process* switch, because those land in the
  next process's own time.
- **On AArch64 the flush is the machine's, not the CPU's.** Four CPUs
  each switching a few hundred times a second is a few thousand
  machine-wide TLB invalidations a second, each a broadcast the other
  CPUs must honour mid-instruction. This is the audit's MEDIUM finding
  on `aarch64/mmu.c` and the row it wrote in the scalability table:
  "AArch64 full-TLB flush on every user switch" is one of the six things
  that put the practical ceiling at ~16 CPUs.
- **`vm_space_destroy` still shoots down.** The memory design (M35) notes
  it shoots down per chunk though no CPU has the space active; with
  ASIDs the destroy path becomes one `tlbi aside1is` for the whole
  space, and the shootdown is gone.
- **Nothing above the architecture layer can name a space to the TLB.**
  The `arch/mmu.h` context is a bare root address. So the VMM's
  shootdown rule — "the CPUs that must be told are the ones whose root
  is this space right now" — is *exactly true* only because a CPU that
  leaves a space holds none of its translations. That truth is what
  ASIDs deliberately break, and the rule has to be rewritten before the
  first ASID is handed out. That rewrite is the architectural question
  of this unit.

## Current implementation

**AArch64** (`kernel/arch/aarch64/mmu.c:385-396`):

```c
void arch_mmu_activate(const struct arch_mmu_context *ctx)
{
    if (ctx_is_kernel(ctx)) { ...; WRITE_SYSREG(ttbr0_el1, g_empty_root); }
    else WRITE_SYSREG(ttbr0_el1, ctx->root);   /* ASID 0: a full invalidate per switch */
    isb();
    tlbi_vmalle1is();    /* dsb ishst; tlbi vmalle1is; dsb ish; isb */
}
```

`TTBR0_EL1` carries ASID 0 for every user space; the kernel half lives
in `TTBR1_EL1` and its leaves are never `nG`, so they survive the flush
(invariant A5 of the AArch64 docs says so, and says why: "The single
ASID (0) and the per-switch `tlbi vmalle1is` rely on kernel entries
being global and user entries being tagged"). `TCR_EL1` is inherited
from the loader — the kernel copies it to secondary CPUs
(`smp.c:125`) and never writes it — so whether `TCR.AS` (16-bit ASIDs)
is set is a boot-time fact to be read, not a design decision already
made. The CPU under test is `cortex-a72`, which implements 16-bit ASIDs
(`ID_AA64MMFR0.ASIDBits` = 0b0010); the kernel reads `PARange` from that
register today and nothing else. `tlbi vmalle1is` also invalidates the
kernel's own entries on every CPU — "which is accepted today", the
design says, in so many words.

**x86-64** (`kernel/arch/x86_64/mmu.c:495-498`): `mov %0, %%cr3`. No
PCID anywhere: `CR4.PCIDE` is never set, `CPUID.1:ECX[17]` (PCID) and
`CPUID.7:EBX[10]` (INVPCID) are not gathered (the audit lists both under
"not gathered or unused"), and the test CPU model
`qemu64,+nx,+svm,+npt` does not expose them — TCG implements both
behind `+pcid,+invpcid`, which the boot test would have to ask for.
`arch_mmu_invalidate` says of itself "only the active context can be
invalidated on x86", and a full invalidate toggles `CR4.PGE` to reach
global entries. Kernel pages are global (`PGE`).

**The VMM's rule** (`kernel/memory/vmm.c:644-660`, design §6.4):
`vm_space.active_cpus` is the set of CPUs whose root is this space right
now; `vm_space_switch` sets the CPU's bit in `next` before loading the
root and clears it in `prev` after. `user_shootdown_targets` returns
`active_cpus | self` after a full fence. The design states the premise
in one sentence: "Without PCID or ASIDs a CPU that leaves a space holds
none of its translations, so the bit can be cleared at once." On
AArch64 the shootdown is the broadcast `tlbi` itself (no IPIs; the
statistics count the mask as acknowledgements); on x86-64 it is one IPI
per target with a one-second acknowledgement deadline.

**Sizes that matter.** AArch64: 8 or 16 ASID bits (255 or 65,535 usable;
0 reserved for the kernel's empty root). x86-64: 12 PCID bits (4,095
usable; 0 reserved). VMIDs (hypervisor stage 2) are a separate tag and
are already allocated 1–15 by the EL2 backend; SVM's guest ASIDs
(`g_asid_lock` in `svm.c`) are a separate mechanism again. Neither is
touched here.

## Why it matters

This is the first §68 report about performance rather than a missing
capability, and the constitution's order (§69, item 3: SMP/locking
scalability) puts it exactly here, after the correctness work that the
last units and the two chains' investigations have been. The audit
named it twice: as a MEDIUM finding (ASID 0, machine-wide flush) and as
one of the six scalability ceilings. It is the cheapest of the six to
remove — the algorithm is standard, the hardware does the work, and the
change is confined to the switch path, the shootdown rule and one
allocator — and it is a prerequisite for the others to be measurable:
per-CPU frame caches and slab magazines are hard to evaluate on a
machine whose TLB is emptied a thousand times a second.

There is also a correctness dividend. The shootdown rule as written is
the only place in the tree where a *statement about hardware state* ("a
CPU that leaves holds nothing") is load-bearing for isolation. Making
the rule explicit about what a CPU may still hold, and testing it with
a CPU that holds translations for a space it is not running, turns a
premise into an invariant with a check.

## Proposed design

### 1. The architectural question: what "a CPU has left the space" means

Three answers were considered; the third is proposed.

**(a) Flush the departing space's ASID on every switch-out.** Keeps the
`active_cpus` rule exactly as it is, because the premise stays true.
On AArch64 it turns the machine-wide `vmalle1is` into a local `tlbi
aside1` — a real gain, since the other CPUs are left alone — but it
still discards the very translations ASIDs exist to keep, and on x86-64
it gains nothing at all. Rejected as the design; kept as a fallback
mode (§5).

**(b) Per-CPU ASIDs** — each CPU owns a small set of slots and maps
spaces onto them (Linux's x86 model, six PCIDs per CPU). Avoids any
global allocator lock and any rollover event, at the cost of a per-CPU
table per space and a "which slot am I on this CPU" lookup per switch.
Right for a machine with hundreds of CPUs and PCIDs to spare; wrong for
one with 64 CPUs at most and 255 ASIDs at least, where the global
allocator is uncontended and the bookkeeping is one field.

**(c) Global ASIDs with generations, and a mask of CPUs that may hold
translations** — Linux's arm64 model, applied to both architectures.
Proposed:

- `struct arch_mmu_context` gains `uint32_t asid` (the tag, 0 = none
  yet) and `uint64_t asid_gen` (the generation it was allocated in). A
  space gets its ASID lazily, at its first switch-in, from a bitmap
  under one spinlock (`kernel/memory/asid.c`, generic; the width comes
  from `arch_mmu_asid_bits()` — 8, 12 or 16).
- **Rollover**: when the bitmap is full the generation advances, the
  bitmap is cleared, every space's tag is stale by construction (its
  `asid_gen` is old), and one machine-wide flush is issued (`tlbi
  vmalle1is` on AArch64; on x86-64 each CPU flushes at its next switch,
  because there is no broadcast). Rollover is therefore *exactly the
  cost of one switch today*, paid once per 255–65,535 process lifetimes
  instead of on every switch.
- **`vm_space.active_cpus` becomes `tlb_cpus`: the CPUs that may hold
  this space's translations.** Set on switch-in as today; **not cleared
  on switch-out**; cleared for every space at rollover (the flush emptied
  them) and for one space when it is destroyed (after a targeted flush).
  `user_shootdown_targets` returns `tlb_cpus | self`. The rule in design
  §6.4 becomes: "a CPU is in the mask from the moment it may hold a
  translation until the moment it provably holds none", which is what
  the old rule was silently relying on.
- **Switch-in**: if `ctx->asid_gen` is current, load the root tagged
  with the ASID and *do not flush*; if stale, allocate afresh (which may
  roll over) and then load. On AArch64: `msr ttbr0_el1, root |
  asid<<48; isb`. On x86-64: `mov cr3, root | pcid | (1<<63)` — the
  no-flush bit — when the CPU's own generation matches, else without it.
- **Invalidation becomes ASID-qualified.** AArch64: `tlbi vae1is, (asid
  << 48) | va` per page, or `tlbi aside1is` for the whole space; both
  are broadcast, both leave every other space and the kernel alone, and
  both work for a space that is *not current* on the issuing CPU — so
  the shootdown stays IPI-free on AArch64 and the mask is informational
  there (statistics), as now. x86-64: on a CPU where the space is
  current, `invlpg` as today; on a CPU in `tlb_cpus` where it is not,
  `invpcid` type 0 (address + PCID) or type 1 (whole PCID) when the CPU
  has INVPCID; without INVPCID, a per-CPU **bitmap of dirty tags**.
  **The bitmap is indexed by PCID, not by space, and that is
  load-bearing.** The IPI handler sets the bit for the space's tag; a
  `mov cr3` loading a tag whose bit is set omits the no-flush bit
  (which flushes exactly that PCID's entries) and clears the bit.
  Keyed by *space* instead — "flush when this space next switches in" —
  the mechanism has a hole at destroy: a destroyed space never switches
  in again, so its bit is never consumed, and the moment the allocator
  hands that tag to a new space the new space runs on the old one's
  translations. Keyed by tag, the bit outlives the space that set it
  and is consumed by whoever uses the tag next, which is exactly the
  CPU that would otherwise alias. The invariant is inductive: a tag's
  bit stays set on a CPU from the moment that CPU may hold stale
  entries for it until the moment a switch-in flushes them.
- **Destroy**: AArch64 `tlbi aside1is`, which is immediate and complete
  before the ASID is released. x86-64 with INVPCID: type 1 on each CPU
  in `tlb_cpus`, likewise before release. x86-64 without INVPCID: set
  the dirty-tag bit on each CPU in `tlb_cpus`, and only then release
  the tag — the release is safe precisely because the bit is keyed by
  tag and survives into the next owner's first switch-in. The
  per-chunk shootdown in `vm_space_destroy` is removed.
- **ASID 0 is the kernel's**: `g_empty_root` and the kernel context keep
  it, so a kernel thread's user half is tagged with a value no user
  space ever gets, and switching to a kernel thread needs no flush
  either (the empty root maps nothing to alias).

### 2. Hardware enabling

AArch64: at `mmu_init`, read `ID_AA64MMFR0.ASIDBits`; if 16-bit ASIDs
are implemented and `TCR.AS` is clear, set it (the kernel does not write
`TCR_EL1` today; it will, once, with the flush that any `TCR` change
requires) and pass 16 to the allocator, else 8. Secondary CPUs already
copy the boot CPU's `TCR` through the mailbox. `A1` stays 0: the ASID is
`TTBR0`'s, which is the user root.

x86-64: gather `has_pcid` and `has_invpcid` in `x86_cpu_init`; in
`x86_cpu_enable_features` set `CR4.PCIDE` when `has_pcid` — on every
CPU, and only while `CR3[11:0]` is 0 (it is: the kernel root has no
tag), which the architecture requires. Without `has_pcid` the whole unit
degrades to today's behaviour on that machine: the allocator still runs
(so the code path is the same), the tag is written into a CR3 whose low
bits the CPU ignores... no — without `PCIDE` a non-zero `CR3[11:0]` is a
`#GP`, so `arch_mmu_activate` masks the tag out when `has_pcid` is
false and the no-flush bit is never set. The boot test gets
`+pcid,+invpcid` added to the default `QEMU_CPU`, and the chain gains a
`boot nopcid x86_64` step that runs `qemu64` bare, so both paths run
every time.

### 3. What the hypervisor sees

Nothing. Guest translations are tagged by VMID (EL2) or by the SVM ASID;
`tlbi vmalle1is` at rollover invalidates VMID 0 — the host — only; the
EL2 world switch saves and restores `TTBR0_EL1` with whatever tag the
host had, as it does every other EL1 register. The one interaction to
verify by test is that a guest run followed by a host process switch on
the same CPU does not confuse the two (it cannot, by tag, but the test
is cheap and the `SP_EL2` bug of #66 lived in exactly this switch).

### 4. Lazy TLB for kernel threads

Orthogonal, small, and worth doing in the same unit because it touches
the same three lines: when the next thread is a kernel thread, do not
switch `TTBR0`/`CR3` at all — leave the previous user root and tag
loaded. The kernel thread cannot touch user addresses except through
`copy_*_user`, which is fatal on a kernel thread anyway (`current_space`
returns NULL). This removes a root write and, on x86-64, a flush from
every switch to a `netrx` worker or the writeback thread. The
`tlb_cpus` rule already covers it (the CPU keeps its bit).

### 5. A paranoid mode

`CONFIG_DEBUG` gains a boot-time knob (`sysctl debug.tlb_paranoid`, or
a fw_cfg option like the fault-injection one): flush the departing ASID
on every switch-out (design (a)). With it on, ASIDs are allocated and
tagged exactly as in normal mode but no translation ever survives a
switch, so any isolation failure that appears in normal mode and
vanishes in paranoid mode is a stale-translation bug by construction.
It is the bisecting tool for the worst risk below, and it costs one
line in the switch path.

## Affected files

| File | Change |
|---|---|
| `kernel/include/arch/mmu.h` | `asid`, `asid_gen` in `struct arch_mmu_context`; `arch_mmu_asid_bits()`; `arch_mmu_invalidate_asid(ctx)`; `arch_mmu_activate` takes the generation decision |
| `kernel/memory/asid.c` (new), `kernel/include/kernel/asid.h` (new) | bitmap allocator, generations, rollover flush hook |
| `kernel/memory/vmm.c` | `active_cpus` → `tlb_cpus` and its new lifecycle; `vm_space_switch`; destroy without per-chunk shootdown |
| `kernel/include/kernel/vmm.h` | the renamed mask and its comment |
| `kernel/arch/aarch64/mmu.c` | tagged `TTBR0` write, no per-switch `tlbi`; `vae1is`/`aside1is` invalidates; `ASIDBits` + `TCR.AS` at init |
| `kernel/arch/aarch64/include/aarch64/sysreg.h` | `ID_AA64MMFR0_ASIDBITS`, `TTBR_ASID_SHIFT`, TLBI helpers with an ASID operand |
| `kernel/arch/x86_64/cpu.c`, `include/x86/cpu.h` | `has_pcid`, `has_invpcid`, `CR4_PCIDE` |
| `kernel/arch/x86_64/mmu.c` | tagged CR3 with the no-flush bit; `invpcid`; the lazy-flush bit in the IPI handler and at switch-in |
| `kernel/arch/*/context.c` | lazy TLB for kernel threads |
| `kernel/memory/memtest.c`, `kernel/process/proctest.c` | the tests below |
| `tests/host/test_asid.c` (new), `tests/host/host.mk` | allocator unit tests |
| `scripts/qemu-run.sh` | `+pcid,+invpcid` in the default x86 CPU; `QEMU_PCID=0` for the bare model |
| the verify chain | `boot nopcid x86_64`, `boot paranoid aarch64` |
| docs | `docs/kernel/memory/design.md` §6.4 and M35; `docs/kernel/arch/aarch64/design.md` (three sentences), `invariants.md` A5; `docs/kernel/arch/x86_64/design.md`; `docs/kernel/arch/api.md`; audit rows 230, 685, 761 |

## New APIs

Kernel-internal only; no system call, no UAPI change.

```c
/* arch/mmu.h */
unsigned arch_mmu_asid_bits(void);                         /* 8, 12 or 16; 0 when tags are unusable (no PCID) */
void arch_mmu_activate(const struct arch_mmu_context *ctx, bool flush);   /* flush: this CPU must not trust old entries */
void arch_mmu_invalidate_asid(const struct arch_mmu_context *ctx);       /* the whole space, every CPU that may hold it */
void arch_mmu_flush_all_local(void);                       /* rollover on a CPU without broadcast (x86-64) */

/* kernel/asid.h */
void asid_init(unsigned bits);
bool asid_ensure(struct arch_mmu_context *ctx);            /* current generation; true if a rollover happened */
void asid_release(struct arch_mmu_context *ctx);
uint64_t asid_generation(void);
struct asid_stats { uint64_t allocs, rollovers, reuses; };
```

`vm_space.active_cpus` is renamed `tlb_cpus`; `vm_space_switch` keeps
its signature.

## Migration plan

1. **Architecture enabling, alone and first.** AArch64: read
   `ID_AA64MMFR0.ASIDBits` and set `TCR.AS` if 16-bit ASIDs are
   implemented — the kernel's first ever write to `TCR_EL1`, one bit,
   followed by the flush the architecture requires. x86-64: gather
   `has_pcid`/`has_invpcid` and set `CR4.PCIDE` where present. **No tag
   is written anywhere yet**: every root still carries 0, so a `mov cr3`
   flushes all of PCID 0 and a `TTBR0` write is followed by the same
   `vmalle1is` as today. This step changes what the hardware is
   configured to allow, and nothing about what the kernel asks of it.
   It is separated because it is the unit's only touch of boot-critical
   state: a wrong `TCR_EL1` is a hang at the next instruction, and this
   way the boot that proves it carries no other change.
2. **Allocator alone** (`asid.c`, host tests, `asid-alloc` self-test),
   sized by `arch_mmu_asid_bits()` from step 1. Still nothing uses the
   tags.
3. **The tag written into the root**, with *every* switch still
   flushing (no-flush bit never set on x86-64; the `tlbi` kept on
   AArch64). Proves the tagging and the `#GP` conditions without
   changing what the TLB holds. `boot nopcid x86_64` added here.
4. **`tlb_cpus` semantics and the ASID-qualified invalidates** on both
   architectures, still flushing on every switch. The shootdown rule
   changes here, while the old flush still makes it moot — so a mistake
   in the new rule cannot yet corrupt anything, and the tests of step 5
   are written against this step.
5. **Stop flushing on switch-in.** The unit's actual change. AArch64
   first (broadcast invalidates, no IPI logic), x86-64 second (INVPCID,
   then the dirty-tag fallback).
6. **Destroy without shootdown; lazy TLB for kernel threads.**
7. **Paranoid mode, the chain steps, the benchmark, the docs sweep.**

Each step boots green before the next starts; steps 4 and 5 are
separate commits so a bisect lands on the rule or on the flush, not on
both.

## Tests

Every one of these must fail when its bug is reintroduced, and the
reintroduction that matters most is *forgetting to tag* — writing the
root without the ASID — because that is the failure that aliases two
processes' identical addresses silently.

- **`asid-alloc`** (memtest): allocate to exhaustion with a width forced
  to 8 by a debug knob; the 256th allocation rolls over (generation +1,
  one flush counted); a released tag is reused; a context with a stale
  generation is re-tagged on `asid_ensure`. Host unit tests
  (`test_asid.c`) cover the bitmap and generation arithmetic with no
  hardware.
- **`asid-isolation`** (proctest): two processes map the *same* user
  address to different frames holding different bytes; a pinned kernel
  thread switches between the two spaces on one CPU twenty times,
  reading the byte through a user copy after each switch **without any
  flush in between**. With the tag omitted from the root write, the
  second process reads the first's byte on the first switch — the test
  fails at read 2. This is the isolation proof and it must run on both
  architectures, with the tag width the machine actually has.
- **`asid-shootdown-remote`** (proctest, needs two CPUs): CPU A switches
  into space S and touches a page (its translation is now cached on A),
  then switches to a kernel thread — S stays in A's TLB, A stays in
  `tlb_cpus`. CPU B unmaps that page in S. CPU A switches back into S
  and touches the page: it **must fault**. With `tlb_cpus` cleared on
  switch-out (the old rule) A is not told, keeps the stale entry, and
  reads the freed frame — the test fails by *not* faulting. On x86-64
  this runs three ways: INVPCID, the lazy-flush fallback (INVPCID
  masked off by a knob), and `nopcid`. This is the test M35's gap has
  been asking for since milestone 5. It carries a second case for the
  destroy path: destroy S while CPU A still holds it, spawn spaces until
  the allocator hands S's tag to a new space T, run T on A and check it
  reads its own bytes. With the dirty-tag bitmap keyed by space instead
  of by tag, T reads S's.
- **`asid-rollover-isolation`**: width forced to 8, spawn 300 short
  processes in a loop while a long-lived process checks its own byte
  after every switch: a rollover must not let a recycled tag alias the
  survivor. Reintroduce by skipping the flush at rollover.
- **`asid-guest`**: run `guest_hvc` on a CPU, then switch two host
  processes on that CPU and check `asid-isolation`'s property still
  holds. Cheap, and it covers the seam #66 lived in.
- **`tlb-quiet`**: the stats: in the steady state of `process-user`, the
  number of full flushes is zero (AArch64 `vmalle1is` count; x86-64
  CR3-without-no-flush count). Reintroduce by any flush on the switch
  path.
- **Paranoid mode**: the whole boot with `debug.tlb_paranoid=1`, as a
  chain step on AArch64 — every test above still passes, which shows the
  tests are about the rule, not about a lucky TLB.
- Existing: `process-spawn` (identical addresses across a switch, the
  test A5 already leans on), `user-vmm`, `smp-shootdown`, the process
  and signal suites unchanged.

## Benchmarks

- **`switch-bench`** (the shape of `fpu-bench`): two user processes
  pinned to one CPU, each touching eight pages of its own and yielding;
  report ns per switch *including* the misses that follow it, before and
  after, both architectures, and in paranoid mode. The report's
  prediction: AArch64 loses most of the `tlbi` cost (a broadcast with two
  `dsb`s) plus the walks; x86-64 loses the walks only.
- **`process-user`, `syscall-fuzz`, `net-bench`** durations from the boot
  log, before and after — these are the churn-heavy self-tests and the
  page-poison unit just showed they move with allocator cost.
- **Full-flush count per boot** from the new stats: today it equals the
  number of user switches; the target is the number of rollovers, which
  with 16-bit ASIDs should be zero in a boot.
- **Four-CPU interference on AArch64**: `switch-bench` on CPU 0 while
  CPU 1 runs a memory-walking loop; the loop's throughput before and
  after is the cost the broadcast has been imposing on bystanders.

Numbers go into `docs/kernel/memory/testing.md` "Measured results" as
the FP unit's did, with the QEMU version and the host noted, since TCG
does not model TLB misses the way hardware does — the *flush* cost is
real under TCG (it drops QEMU's own translation caches), the *miss* cost
is approximate. This is stated up front so the numbers are read for
what they are.

## Risks

- **A stale translation is a silent isolation failure**, the worst
  class this kernel can have, and unlike the frame corruption of #66 the
  page-poison check cannot see it: nothing is written to a free frame,
  a process merely reads (or writes) through a translation that should
  be gone. Mitigations, in order: the rule changes in step 3 while the
  old flush still masks mistakes; `asid-isolation` and
  `asid-shootdown-remote` are written before step 4 and must fail
  against a deliberately untagged root; paranoid mode bisects any later
  doubt. This risk is the reason the unit is six steps and not one.
- **x86-64 under the test CPU model has no PCID at all**, so the
  default boot would exercise only the fallback unless `QEMU_CPU`
  changes. The plan changes it and keeps a bare-model step; the risk is
  a TCG quirk in `+pcid`/`+invpcid` (it is a rarely used TCG path).
  Fallback: leave the default model alone and run PCID only in a named
  step — the fallback path would then be what ships by default on that
  machine, which is today's behaviour, not a regression.
- **`CR4.PCIDE` preconditions**: setting it with `CR3[11:0] != 0` or
  outside long mode is `#GP`; clearing it later needs the tags gone.
  Both are boot-order facts; the enable runs where `CR4.PGE` is set
  today, before any tagged root exists.
- **8-bit ASIDs on some machine** make rollover frequent (every ~255
  process lifetimes — with `syscall-fuzz` and the probes, several times
  a boot). Correct by design and still cheaper than today, but the
  `tlb-quiet` target of zero flushes then has to be stated per width.
- **`TCR_EL1` is the loader's today.** Writing it from the kernel is new;
  a wrong write is a hang at the first instruction after `isb`. The
  write is one bit, guarded by `ASIDBits`, followed by the full flush the
  architecture requires, and secondary CPUs copy the result — but this
  is the one place the unit touches boot-critical state, which is why
  step 1 is nothing but the enabling on both architectures, with no tag
  written anywhere and no other change in that boot.
- **The dirty-tag bitmap must be keyed by tag, not by space.** Keyed by
  space, a destroyed space's pending flush is never consumed (it never
  switches in again) and the next owner of that tag inherits its
  translations — cross-address-space aliasing, on exactly the machines
  with the least test coverage (PCID without INVPCID; no QEMU model in
  the chain has that pair). `asid-shootdown-remote` runs with INVPCID
  masked off for this reason, and a destroy-then-reuse case is added to
  it: destroy a space that a second CPU still holds, immediately spawn
  another until the tag is reused, and check the new space reads its own
  bytes on that CPU.
- **The rename `active_cpus` → `tlb_cpus`** is deliberately not a
  quiet one: every reader of the old name is a reader of the old rule,
  and the compiler finding each of them is the sweep.
- **The hypervisor's `TTBR0` save/restore** now carries a tag. It is
  opaque to the switch (saved and restored whole), and `asid-guest`
  checks the seam; the risk is only that the world switch's own `tlbi`
  (`vmalls12e1is`, VMID-scoped) is mistaken for a host flush — it is
  not, and the test would say so.

## Documentation the last units left, which this one falsifies

The sweep that has cost three units a review round each; listing the
sentences now so they are edited on the day the code changes:

- `docs/kernel/arch/aarch64/design.md:269-270` — "a single ASID and a
  full invalidate per switch; ASID allocation is a later optimisation";
  and `:512` in "Future extensibility" — "ASID allocation instead of the
  full invalidate per switch".
- `docs/kernel/arch/aarch64/invariants.md` A5 — "The single ASID (0)
  and the per-switch `tlbi vmalle1is` rely on...". The invariant itself
  (kernel leaves never `nG`, user leaves always) stays; its premise
  changes.
- `docs/kernel/memory/design.md` §6.4 — "Without PCID or ASIDs a CPU
  that leaves a space holds none of its translations, so the bit can be
  cleared at once"; and `:523` — "a region tree, per-CPU frame caches
  and ASIDs are" (future work).
- `docs/kernel/memory/invariants.md` M35 — the shootdown rule, and its
  gap ("no test drives a shootdown against a CPU that is running the
  space concurrently"), which `asid-shootdown-remote` closes.
- `kernel/memory/vmm.c:678` — the comment in `vm_space_switch`.
- The audit (`docs/audit/2026-09-post-roadmap-audit.md` rows 230, 685,
  761) is a dated record and is not rewritten; the outcome section of
  this report will say which rows it closes.

## Alternatives considered

- **Do nothing.** Defensible while the machine has four CPUs and the
  workload is a boot test; indefensible as the first thing a real
  workload would find, and the audit already found it.
- **Flush the departing ASID on switch-out** (design (a)). Fixes the
  broadcast, keeps nothing, changes no rule. Kept as paranoid mode,
  where its property — no translation survives a switch — is exactly
  what a debugging tool wants and a fast path does not.
- **Per-CPU ASID slots** (design (b)). The right answer at a scale this
  kernel does not have; a global allocator with 255+ tags and a
  64-CPU cap is uncontended, and one field per context is the whole
  cost.
- **PCID only where INVPCID exists.** Simpler x86-64 (no lazy-flush
  bit), but QEMU's `qemu64` has neither and real machines exist with
  PCID and without INVPCID (pre-Haswell). The lazy-flush fallback is
  twenty lines and makes the feature unconditional on PCID alone.
- **ASIDs for the kernel half too** (giving kernel entries a tag and
  dropping the `nG`/global distinction). No: the kernel half is shared
  by construction (`TTBR1`, `PGE`), and A5/M36 depend on that.

## Outcome (2026-09-09)

Built on AArch64, deliberately not on x86-64, and the difference is the
first thing the unit found.

**TCG implements PCID on no CPU model.** Not `qemu64`, not `Haswell`,
not `-cpu max`; a model that requests it is refused with "TCG doesn't
support requested feature". The report assumed the opposite and planned
around `+pcid,+invpcid`. Every environment this tree is tested in is
TCG -- the boot test, the whole chain, CI -- so an x86-64 tagged switch
path could not have been exercised anywhere, and this is the one place
in the kernel where an untested mistake is a silent loss of isolation
between processes. So it is not written. `arch_mmu_asid_bits()` returns
0 there, which is what a real machine without PCID reports, and the path
that selects -- no tag, flush every switch -- is what x86-64 has always
done and is now exercised on every boot rather than being dead code
waiting for hardware. PCID and INVPCID are detected and logged;
`CR4.PCIDE` stays clear until there is tested code behind it.

**The architectural question resolved as proposed**, and the answer was
the rule rather than the allocator. `active_cpus` became `tlb_cpus`, and
the rename was the sweep: five doc sites the compiler could not reach
still described the old rule, which is why the field was renamed rather
than redefined.

**What the report did not anticipate:**

- **A space can hold two tags at once.** After a rollover it may be
  re-tagged on one CPU while another is still running it under the old
  tag. So an ASID-qualified range invalidate -- the obvious
  optimisation, and what the report implied -- would miss the stale one.
  Range invalidates stay all-ASID (`tlbi vaae1is`, as before); only
  destruction names a tag, where nothing runs the space. Both sides of
  that dependency now say so in the code.
- **The destroy-path invalidate cannot be tested.** By the time it runs,
  the region teardown has invalidated every mapped page across every
  tag, so removing it changes nothing observable. It is kept -- so that
  destroying a space does not depend for its safety on a decision made
  in `arch_mmu_invalidate` -- and recorded as a gap rather than counted
  as proved.
- **The kernel's root was being given a tag.** `vm_space_switch` asked
  the allocator on every switch, including to the kernel space, which
  runs under tag 0 by construction and whose tag `arch_mmu_activate`
  discards. One tag per generation, never released. Found by writing the
  destroy test, not by review.
- **Timing this change under TCG is impossible**, and the numbers
  mislead in the flattering direction's opposite: a tagged switch
  measures 6-20x *slower* than a flushing one, swinging threefold
  between runs of one binary, because QEMU's software TLB is not
  ASID-tagged. The unit's claim is therefore counted, not timed:
  `asid-quiet` proves 401 switches perform zero flushes, and 401 when
  paranoid. Whole-boot totals are unchanged, so nothing real is slower.

**Not built:** PCIDs (above); lazy TLB for kernel threads, dropped
because the win shrank once the switch stopped flushing and the x86 test
CPU has no SMAP, so leaving a departed process's mappings live would
remove a real safety net on the architecture that gains nothing here.

**Five of six properties are proved by reintroducing the bug**, each
failing for its own stated reason; the sixth is the destroy invalidate
above. The proof harness itself had to be rebuilt twice: `cp` backups
silently reverted edits made while it ran, and `git checkout --`
restored to HEAD -- which, on an uncommitted branch, deleted the unit
from a file rather than the injected bug. It now refuses to start on a
dirty tree, verifies each injection by content, and checks the tree is
clean when it exits.

**And one failure that was mine but looked like someone else's.** The
chain's `boot kbd-hub aarch64` step began failing in `selftest_kmalloc`,
on an assertion that the machine's live-object count is unchanged across
its run. The tag tests leak nothing -- the count is identical entering
every test, `vm_space` live is zero, and the difference of two-to-four
objects appears in a generic bucket at a varying point inside
`kmalloc`'s own window. What they do is wake other CPUs, whose deferred
work lands wherever it lands. Four attempts to shrink that disturbance
moved the rate without removing it; ordering the tag tests after
`kmalloc` removed it. Two of those attempts were kept anyway, because
they were right independently: the measured loop no longer disables
interrupts (the counters do not move for anyone in the steady state, so
the property is proved under real scheduling, and a self-test has no
business holding interrupts off for tens of milliseconds), and it no
longer routes hundreds of switches through the kernel's root. The
underlying fragility -- a test asserting something about the whole
machine rather than about the allocator under test -- is recorded rather
than papered over.

**Chain: 40 steps, all passing**, including a new
`boot asid-paranoid aarch64`.
