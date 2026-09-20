# NEXT SUBSYSTEM — a call one personality has and the other does not

Constitution §68: after the audit, name the next subsystem in this shape
and wait for the instruction to build it. **This report is as built**
(the `mprotect` unit), and the banner below records where the build
differed from the design — including two proofs the design promised
that this environment cannot give.

**What the build changed:**

1. **The sync lives at the two syscall doors, not in
   `vm_user_protect`.** The design said "`vmm.c` or the arch layer".
   `vm_user_protect` is also called by the ELF loader on a space that
   is not current, and the maintenance is by virtual address in the
   current regime, so it goes where the calling process's context is
   guaranteed: `sys_mprotect` and `lx_mprotect`, both, after a
   successful protect that adds `VM_PROT_EXEC`, via
   `arch_mmu_sync_icache_user` (a no-op on x86-64).

2. **Two of the four mutations cannot be observed under QEMU, and the
   unit says so instead of claiming them.** Removing the sync
   entirely passes: TCG invalidates translated code on data writes,
   so the write-then-execute self-test is a regression test that the
   path runs without trapping, not a proof of coherence. And removing
   the user-access bracket around the maintenance did **not** fault
   under the guard boot with PAN present and on — so the bracket is
   kept because EL1 touches EL0-accessible memory only inside it
   everywhere else, not because it was shown to be needed. The design
   asserted that PAN would fault the EL1 access; that is not
   established here, and the invariant (M41) records the gap. The two
   that do bite: accepting an undefined `prot` bit fails the native
   flags case, and pointing the fuzzer at the scratch page it writes
   into makes it protect that page and then write to it —
   `syscall-fuzz` fails on the child's `status == 0`, with the fault
   at `g_fz_page + 4000`, `fz_string`'s own offset.

3. **`docs/compat/linux/api.md` was stale about `vm_user_protect`.**
   It said the range had to be exactly one region (`-EINVAL`
   otherwise, "a recorded VMM limit"), which has not been true since
   the function learned to split — `design.md` in the same directory
   said so. Fixed in passing, and noted because the affected-files
   table below did not know it was wrong.

4. **The syscall table was missing row 92.** `getsockopt` was recorded
   only in the prose paragraph that carries the count; adding 93
   beside it is when that showed. Both rows are in the table now.

5. **`thread.c`'s comment described a punch that #193 had removed.**
   Updating it for the new syscall found it still narrating reserve,
   punch a hole, fixed-map into the hole. It now describes
   reserve-and-replace, says the call exists, and says why libc keeps
   the sequence anyway.

**A Linux program running on this kernel can change the protection of
its own memory. A native program cannot.** `lx_mprotect`
(`compat/linux/syscalls.c:1012-1031`) validates and calls
`vm_user_protect`; the native ABI has no such call at all. The
machinery is built, tested and in use — only one of the two doors
opens onto it.

**Takes up** the inventory's §1.2 entry **"`SYS_mprotect` for native
programs"** (README.md:1486), deferred there and again by the
`MAP_FIXED` unit, which considered it and said why it was not that
unit.

## What is established

**`vm_user_protect` exists and works** (`kernel/memory/vmm.c:1250`).
It validates the range and W^X, pre-allocates two split spares before
taking `space->lock`, splits at the ends so the range is covered by
whole regions, changes `prot` on each, flips the leaf permissions,
shoots down, and merges equal neighbours afterwards. Its contract is
already written (`kernel/include/kernel/vmm.h`): `-EINVAL` for W+X or
a bad range, `-ENOMEM` if a page of the range is unmapped **or a
split cannot be allocated, with nothing changed**.

**It has a real caller.** The ELF loader uses it to drop each segment
to its final protection after writing it (`kernel/process/elf.c:244`)
— which is the sequential write-then-execute pattern this unit would
be exposing to user code, already relied on inside the kernel.

**It is covered.** `selftest_user_vmm` protects the middle of a
region to `PROT_NONE` and back, checks the three-way split and the
merge, that all four frames stay attached, that `arch_mmu_query`
reports no permissions, that protect across a gap is `-ENOMEM` and
W+X `-EINVAL`, and that a `PROT_NONE` reservation can be split by
protect.

**Since PR #193 it also refuses a claimed range** with `-EBUSY`: a
region a `MAP_FIXED` replacement has quiesced is not another caller's
to split, because `region_split` copies flags and a stranded claim
hangs the range for ever. That is a new error a native `mprotect`
would inherit and must account for.

**W^X is simultaneous, not historical.** `vm_user_map_anon`,
`vm_user_map_anon_replace` and `vm_user_protect` each refuse
`WRITE|EXEC` in the same call (`vmm.c:891`, `:1109`, `:1255`), and on
AArch64 `SCTLR_EL1.WXN` makes a writable page non-executable in
hardware (`docs/kernel/security/design.md`, "WXN"; `make test-wxn`
requires the panic). Writing a page and *then* making it executable —
having dropped write — is allowed, and is what the loader does.

**The syscall filter needs nothing per-call.** It is a bitmask over
syscall numbers (`syscall_filter_install`), so a new number is
filterable the day it exists.

## The problem

### The asymmetry is the wrong way round

This tree's usual failure is a check enforced in `native.c` and
missing from the Linux personality — a second door onto the same
objects. Here the personality is the *complete* one. A native program
that wants a guard page, a read-only table after initialisation, or a
JIT buffer has no call to make, while the same program compiled
against Linux headers does.

### `MAP_FIXED` replacement is not a substitute for it

The unit that just landed makes `mmap(MAP_FIXED)` take a range it
already owns. It is tempting to think protection changes can be built
on it. They cannot: **replacement destroys the contents.** The new
region is demand-zero by construction — that is what makes its
finishing swap infallible. Anything that needs the bytes to survive
the call needs `mprotect`, and that includes every use above.

### libc pays for the gap

`cosmo_thread_start` reserves guard + stack + TCB `PROT_NONE` and
replaces the upper part, purely because it cannot protect the guard
page in place (`libc/src/thread.c:78`: *"There is no mprotect
syscall, so the guard costs a reservation…"*). With `mprotect` the
whole trick becomes one map and one protect.

Whether libc should then change is a real question and this report
answers it: **not in this unit.** PR #193 made the reservation safe
and proved it; swapping it for a different sequence immediately
afterwards spends that proof for a syscall saved on a cold path. The
comment should stop saying the call does not exist, and the change
itself belongs to a later unit with its own argument.

## Design

**One syscall, number 93, `SYS_COUNT` 93 → 94.**

```c
#define SYS_mprotect  93  /* (void *addr, size_t len, int prot) -> 0 */
```

**`sys_mprotect` in `kernel/syscall/native.c`**, alongside `sys_mmap`
and `sys_munmap`, doing what the native ABI does everywhere and
nothing more:

- reject any `prot` bit this kernel does not define, per the native
  flags rule (`docs/kernel/security/design.md`, "Unknown flag bits"),
  so a program can probe for a future bit and none is silently
  dropped. **The Linux door already does this**: `lx_prot`
  (`compat/linux/convert.c:200-203`) returns −1 for any bit outside
  `READ|WRITE|EXEC` and `lx_mprotect` turns that into `-EINVAL`. An
  earlier draft of this report claimed the two doors would differ
  here, which was an assumption about Linux's usual rule rather than
  a reading of the code; review caught it. They agree, and the native
  call is simply following the ABI's own rule;
- require `addr` page-aligned and `len` non-zero and page-aligned —
  `munmap`'s rule, not Linux's round-up, again for consistency within
  the ABI;
- require the range inside the user window (`user_range_ok`);
- translate `COSMO_PROT_*` to `VM_PROT_*` and call `vm_user_protect`,
  returning its result unchanged.

W+X, the unmapped-page case and the quiesced case are all decided by
`vm_user_protect` and are not re-implemented here. That is the shape
the `MAP_FIXED` unit argued for and it applies unchanged: the policy
lives in the VM layer, and each door only translates.

**`mprotect` in libc** (`libc/include/sys/mman.h`, `libc/src/mman.c`
or wherever `mmap` lives), returning 0 or −1 with `errno`, POSIX
signature.

**Errors, and one that POSIX does not name.** `EINVAL` (alignment,
length, undefined `prot` bit, W+X), `ENOMEM` (a page of the range
unmapped, or a split spare cannot be allocated — nothing changed),
and **`EBUSY`** when a `MAP_FIXED` replacement holds the range. POSIX
has no `EBUSY` for `mprotect`; it is reachable only by racing
`mprotect` against a fixed mapping over the same range from another
thread, which is already a caller-side race, and the alternative —
blocking — would mean sleeping inside a call that otherwise never
does. It is documented rather than hidden, in the ABI table and in
the libc header.

## Affected files

| file | change |
| --- | --- |
| `kernel/include/uapi/cosmo/syscall.h` | `SYS_mprotect 93`, `SYS_COUNT` → 94 |
| `kernel/syscall/native.c` | `sys_mprotect`, and its entry in the dispatch table |
| `kernel/memory/vmm.c` or the arch layer | instruction-stream synchronisation when a protection change adds `VM_PROT_EXEC` — userland cannot do it without `SCTLR_EL1.UCI`, which this kernel does not set |
| `userland/init/init.c` (fuzzer) | `SYS_mprotect` in `allowed[]`, with constrained arguments |
| `libc/include/sys/mman.h` | `int mprotect(void *, size_t, int)` |
| `libc/src/` (beside `mmap`) | the wrapper |
| `libc/src/thread.c` | the comment that says the call does not exist; **no behaviour change** |
| `userland/init/init.c` | the native ABI cases below |
| `docs/kernel/syscall/api.md` | the table row and the prose note beside `munmap`'s |
| `kernel/include/kernel/vmm.h` | `vm_user_protect`'s contract listed only `-EINVAL` and `-ENOMEM` while the implementation has returned `-EBUSY` since PR #193 — **fixed in this report's commit**, because a false contract in a public header should not wait for the unit that consumes it |
| `docs/kernel/memory/api.md` | `vm_user_protect` gains its own entry. It has none today — and checking that turned up that neither `vm_user_map_anon` nor `vm_user_unmap` has one either; only `vm_user_map_anon_replace`, added by the last unit. Documenting the call this unit exposes is in scope; the other two are a pre-existing gap and are named here rather than quietly widened into it |
| `docs/kernel/memory/invariants.md` | W^X is simultaneous and user code can now reach the protect path; M18's large-page refusal is unreachable for user anon memory (always 4 KiB) and should say so |
| `docs/compat/linux/*` | the personality is no longer the only door |
| `docs/audit/2026-09-deferred-work-inventory.md` | strike §1.2's entry **and add a row for the follow-up this report defers**: `cosmo_thread_start` to map read/write and protect its guard page, dropping the reservation-and-replace. Striking one entry while deferring new work without recording it is how a deferral becomes a loss, and review caught that this table did exactly that |
| `README.md` | Status entry |

## Tests

**In `init --selftest`** (the native ABI, from userland), all on a
page the test maps itself:

| case | what it establishes |
| --- | --- |
| `RW` → `R`, then write | the write faults; the byte written before still reads back |
| `R` → `RW`, then write | it succeeds, and the old contents survived the round trip |
| → `PROT_NONE`, then read | it faults |
| `PROT_NONE` → `RW` | back to usable, contents preserved from before the `PROT_NONE` |
| `W|X` | `-EINVAL` |
| an undefined `prot` bit | `-EINVAL` (the native flags rule) |
| unaligned `addr`, zero `len` | `-EINVAL` |
| a range with an unmapped page | `-ENOMEM`, **and the mapped part is unchanged** |

**The one that is not a permissions check.** Write a small function's
bytes into a `RW` page, `mprotect` it to `R|X`, call it. This is the
sequential write-then-execute pattern the loader already relies on,
it is the reason `mprotect` is wanted, and on AArch64 it is also the
check that `WXN` does not forbid it once `W` is gone. Without this
the unit could ship with W^X enforced so eagerly that the call is
useless for its main purpose.

**It needs cache maintenance on AArch64, and userland cannot do it.**
Bytes written as data are not visible to the instruction fetcher
until the data cache is cleaned to the point of unification, the
instruction cache is invalidated for the range, and an `isb` runs.
Two rounds of review sharpened this and both corrections matter.

The sequence **does** already exist: `kernel/core/main.c:247` runs
`dc cvau / dsb ish / ic ivau / dsb ish / isb` before executing
freshly written code in the WXN crash test. An earlier draft said
there was nothing to reuse, which was my search being too narrow
twice — it looks in `libc/`, `userland/`, `tests/` and
`kernel/arch/aarch64/`, and the code is in `kernel/core/`.

But that sequence runs at **EL1**, and **EL0 may not run it here**.
`dc cvau` and `ic ivau` are permitted from user mode only when
`SCTLR_EL1.UCI` is set; this kernel does not set it and does not
even define the bit (`aarch64/sysreg.h` defines `M`, `A`, `C`, `SA`,
`SA0`, `I`, `WXN`, `SPAN`, `RES1` — no `UCI`). A user-mode test that
simply inlines the sequence would **trap**, not validate anything.

**So the kernel does the maintenance, not the caller.** When
`mprotect` adds `VM_PROT_EXEC` to a range, the kernel synchronises
the instruction stream for that range before returning. This is the
right answer rather than the convenient one:

- it makes the syscall correct for **every** caller, not just this
  test — any JIT would otherwise have to know the rule, and could
  not obey it anyway without `UCI`;
- the alternative, enabling `SCTLR_EL1.UCI`, widens what EL0 may do
  to the cache hierarchy and is a security-relevant `SCTLR` change
  that deserves its own argument and its own unit, not a line in
  this one;
- x86-64 needs nothing, so it is one `#if` in one place in the
  kernel instead of one in every program.

That is a real addition to the unit's scope, surfaced before any
code was written, and it is the part of this report most likely to
be wrong in a way that only hardware will show: it should be
implemented against the ARM ARM's rules for the point of
unification, not from the crash test's sequence copied by eye.

**In `memtest.c`**: nothing, and the reason is worth writing down.
An earlier draft proposed that `vm-replace-race`'s protect racer
should assert that a protect returning 0 **actually changed the
protection**. That assertion cannot hold: the moment it returns, the
competing thread may replace the whole range, so the observation
races and the test would reject correct behaviour on an unlucky
schedule. Review caught it. Either the observation happens under a
synchronisation the racer does not have, or it does not happen —
and adding synchronisation would remove the contention the test
exists to create. The success path under contention stays covered
the way it already is: every replacement must return 0, and the
region list and `mapped_pages` must agree at the end.

## Risks

**`EBUSY` leaking into a POSIX call.** Discussed above. The risk is a
program looping on it forever; the mitigation is that it is
documented in both the ABI table and the header, and that it requires
a concurrent `MAP_FIXED` over the same range.

**Making W^X look stronger than it is.** Refusing `W|X` in one call
while permitting write-then-protect-to-execute is the correct and
conventional policy, but a reader can easily believe the first fact
means pages can never become executable after being written. The
invariants should say which of the two it is, because this unit is
what makes the distinction reachable from user code.

**A larger attack surface on a path that was kernel-only.**
`vm_user_protect` has had exactly one caller and one personality
reaching it. Exposing it natively means arbitrary user ranges,
arbitrary alignments and concurrent callers. The split/merge paths
are tested, but by a self-test that drives them in one thread.

**The fuzzer will not reach it by itself.** An earlier draft said it
would, on the assumption that it walks `SYS_COUNT`; it does not —
`userland/init/init.c:3884` has an explicit `allowed[]` list and
`SYS_COUNT` only sizes the coverage histogram. `SYS_mprotect` must be
added to that list, and **with constrained arguments**: an
unconstrained one would eventually protect the fuzzer's own stack or
text away and kill the run, which is why `setrlimit` is already
excluded by name there. The same constraint the memory calls already
get.

**Nothing about large pages.** M18 refuses to split a large leaf, and
user anonymous memory is mapped in 4 KiB pages, so the case is
unreachable — but it is unreachable by *circumstance*, not by a
check, and if user mappings ever grow large pages `mprotect` of a
sub-range becomes `-EINVAL` with no explanation. Worth a sentence in
M18 rather than a surprise later.

## Alternatives considered

**Leave it, and let libc keep the reservation trick.** That is what
the last two units did, and the trick now works. But it leaves a
native program unable to do something the same kernel does for Linux
programs, and every future caller that wants a read-only table or a
JIT page has to discover the gap again.

**Build it on `MAP_FIXED` replacement.** Impossible, and worth
recording so nobody tries: replacement gives back demand-zero memory.
The contents are the whole point of a protection change.

**Change `cosmo_thread_start` in the same unit.** Tempting — it is
the caller that pays for the absence, and one map plus one protect is
simpler than reserve plus replace. Rejected: PR #193 proved the
current sequence against four mutations days ago, and rewriting it
immediately spends that for a syscall on a cold path. It is a good
follow-up with its own argument, and the inventory should carry it.

**Give the native call Linux's `len` semantics** (round up rather
than require a page multiple). Rejected: `munmap` requires a page
multiple and the two calls should agree with each other. Note that
the *other* half of this alternative — ignoring unknown `prot` bits
— is not a difference at all: `lx_prot` already rejects them, so
both doors refuse and only the `len` rule differs.
