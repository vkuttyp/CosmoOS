# NEXT SUBSYSTEM — a call one personality has and the other does not

Constitution §68: after the audit, name the next subsystem in this shape
and wait for the instruction to build it.

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
  dropped. This is the one place the native call differs from
  `lx_mprotect`, which keeps Linux's rule of ignoring them;
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
| `libc/include/sys/mman.h` | `int mprotect(void *, size_t, int)` |
| `libc/src/` (beside `mmap`) | the wrapper |
| `libc/src/thread.c` | the comment that says the call does not exist; **no behaviour change** |
| `userland/init/init.c` | the native ABI cases below |
| `docs/kernel/syscall/api.md` | the table row and the prose note beside `munmap`'s |
| `docs/kernel/memory/api.md` | `vm_user_protect` gains its own entry. It has none today — and checking that turned up that neither `vm_user_map_anon` nor `vm_user_unmap` has one either; only `vm_user_map_anon_replace`, added by the last unit. Documenting the call this unit exposes is in scope; the other two are a pre-existing gap and are named here rather than quietly widened into it |
| `docs/kernel/memory/invariants.md` | W^X is simultaneous and user code can now reach the protect path; M18's large-page refusal is unreachable for user anon memory (always 4 KiB) and should say so |
| `docs/compat/linux/*` | the personality is no longer the only door |
| `docs/audit/2026-09-deferred-work-inventory.md` | strike §1.2's entry |
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

**In `memtest.c`**, one addition: `vm-replace-race` already runs a
protect racer against replacements, and it asserts only that protect
never returns something unexpected. It should also assert that a
protect which returns 0 **actually changed the protection** — the
racer proves the `-EBUSY` path is honoured but not that the success
path still works under contention.

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
are tested, but by a self-test that drives them in one thread; the
fuzzer should reach the new number (it takes syscall numbers from
`SYS_COUNT`, so it will, the day the number exists).

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

**Give the native call Linux's semantics** (round `len` up, ignore
unknown bits). Rejected: the native ABI's rule is that an undefined
flag bit is an error, stated in the security design and followed by
`mmap`, `mount`, `umount` and `open`. A new call should not be the
exception, and the personality keeps Linux's behaviour where Linux
programs need it.
