# NEXT SUBSYSTEM — the hole between two syscalls

Constitution §68: after the audit, name the next subsystem in this shape
and wait for the instruction to build it.

**`MAP_FIXED` does not replace.** POSIX says a fixed mapping takes the
range whatever was there; this kernel refuses it. A caller that wants
to turn part of a range it already owns into something else must
therefore `munmap` a hole and `mmap` it back, and those are two
syscalls with unmapped address space between them. Another thread's
`mmap(NULL, …)` can be handed that hole, because the allocator looks
for exactly such a gap.

That is not a hypothesis. It cost three aarch64 CI failures in PR #191
and is papered over there by a retry loop.

Closes the inventory's §1.2 entry **"`MAP_FIXED` should replace, as
POSIX says, instead of returning `-EEXIST`"**.

## What is established

**`space_insert` refuses any overlap** (`kernel/memory/vmm.c:63-80`).
It walks the region list and returns `-EEXIST` the moment the new
region's footprint intersects an existing one. Every mapping path ends
there, so the refusal is the whole of the policy — there is no
replacement path anywhere in the VM layer.

**The native `mmap` passes `MAP_FIXED` straight through**
(`kernel/syscall/native.c:358-374`): it validates the hint, sets
`base = hint`, and calls `vm_user_map_anon`, which fails with
`-EEXIST` if anything is already there.

**The Linux personality already replaces — and does it in two
operations.** `compat/linux/syscalls.c:951-956`:

```c
    if (flags & LX_MAP_FIXED) {
        ...
        rc = vm_user_unmap(p->space, hint, len, 0);   /* Linux replaces what was there */
    }
    ...
    rc = vm_user_map_anon(p->space, base, len, ..., 0, ...);
```

`vm_user_unmap` takes `space->lock`, does its work, and releases it.
`vm_user_map_anon` then takes it again. **Between the two, the range
belongs to nobody.** So the Linux door does not return `-EEXIST`
where Linux would succeed — it does something worse and rarer: it can
lose the range to a concurrent `mmap(NULL, …)` and then fail the
*replacement* with `-EEXIST`, which is a failure Linux never produces.
Two doors, one missing primitive; the personality is a second door
onto the same objects, which this tree has been bitten by before.

**The pieces a replacement needs already exist.** `vm_user_unmap`
pre-allocates its split spares *before* taking the lock, splits at the
ends, unlinks every region inside the range into a `removed[]` array,
and does the page-table teardown and the record frees *after*
releasing it (`vmm.c:970-1021`). `vm_user_map_anon` allocates its
region before the lock, checks `COSMO_RLIMIT_AS`, inserts, and merges
equal neighbours (`vmm.c:863-922`). Both halves already know how to do
their allocation outside the critical section, which is what makes one
combined critical section possible.

**`vm_user_protect` exists** (`vmm.c:1023`), splits at the ends and
merges afterwards, and is used by the ELF loader (`elf.c:244`). No
syscall exposes it. This matters to the alternatives below.

## The problem

### The window is real and has been paid for

`cosmo_thread_start` places a guard page below each thread stack. With
no `mprotect`, the only way to get `PROT_NONE` below `PROT_READ|WRITE`
is to reserve the whole span `PROT_NONE` and convert the upper part:

```c
    mmap(NULL, guard + stack + tcb, PROT_NONE)   /* reserve            */
    munmap(base + PAGE, stack + tcb)             /* punch a hole       */
    mmap(base + PAGE, ..., MAP_FIXED)            /* fill it -> EEXIST  */
```

Between the punch and the fill the hole is unmapped, `vm_user_find_free`
hands it to whichever thread asks next, and the fill loses with
`-EEXIST`. PR #191 hit this three times on aarch64 CI — a thread that
mallocs continuously beside threads that start continuously is all it
takes — and shipped a bounded retry with the note that the real repair
belongs to the kernel. **This unit is that repair.** The retry, and
`STACK_MAP_ATTEMPTS`, come out.

### Every caller that owns a range pays it

The guard page is the instance we have, not the shape of the problem.
Any caller holding a reservation and wanting to change part of it —
a thread stack, an arena that wants a red zone, a loader placing
segments inside a reserved span — has to unmap first and race. POSIX
gives `MAP_FIXED` replacement precisely so that this is one atomic
step, and it is the one operation the VM layer cannot express.

### The refusal is load-bearing somewhere, and that must survive

`-EEXIST` is not merely an omission: it is Linux's
`MAP_FIXED_NOREPLACE`, and one caller wants it. The userland self-test
asserts it (`userland/init/init.c:3615`):

```c
    CHECK(cosmo_mmap((void *)fx, 4096, COSMO_PROT_READ, COSMO_MAP_ANONYMOUS | COSMO_MAP_FIXED) == -COSMO_EEXIST);
```

A unit that simply makes `MAP_FIXED` replace deletes a behaviour the
tree tests for. "Place this only if the range is free" is a real
request and the answer must stay reachable — under its own flag,
where it belongs.

## Design

**One primitive, one owner of the range throughout.** Add to `vmm.c`:

```c
/* MAP_FIXED, POSIX semantics: take [base, base+size) whatever is there.
 * The range is owned by a region at every instant -- first the ones
 * being replaced (quiesced), then the new one -- so no concurrent
 * mmap(NULL, ...) can be handed it, and no fault can populate a page
 * the teardown would then free. */
int vm_user_map_anon_replace(struct vm_space *space, uint64_t base, size_t size,
                             vm_prot_t prot, unsigned flags, const char *name);
```

The body is **three** critical sections, not one, and the reason is
`user_range_teardown`: it takes `space->lock` itself, once per chunk
(`vmm.c:709-735`), so it cannot be called with the lock held. An
earlier draft of this report had the primitive insert the new region
and then tear down; review showed that is wrong, and the two defects
it found are the shape of the design below.

**Nothing fallible may run after the first mutation.** Allocation and
limit checks all happen before anything is unlinked or split:

1. **Before the lock**, allocate the new region and the two split
   spares — `vm_user_unmap` and `vm_user_map_anon` both already do
   their allocation here, for the same reason.
2. **First critical section**, all checks before any change: sum the
   pages of the regions the range covers; check `COSMO_RLIMIT_AS`
   against `mapped_pages - covered + npages`, so replacing a range
   with one the same size cannot fail a limit it already satisfies;
   check `splits_needed` against the spares actually allocated. Only
   then split at the ends and **apply the whole page accounting now**
   — subtract `covered`, add `npages` — so the budget is held across
   the teardown and the third section needs no check. Mark every
   covered region `VM_REGION_QUIESCED` and **leave them linked**.
3. **Outside the lock**: `user_range_teardown(base, size)`. The range
   is still owned by the quiesced regions, so no `mmap(NULL, …)` can
   be handed it, and the teardown finds exactly the old pages.
4. **Third critical section**: unlink the quiesced regions,
   `space_insert` the new one — which cannot collide, because the
   range was just cleared, and cannot fail for memory, because the
   region was allocated in step 1 — and `region_merge_around`.
5. **After the lock**: free the removed records and any unused spare.

**Two replacements must not overlap, and the flag is what says so.**
Review found the hole: nothing in the three sections above stops a
second replacement quiescing the same regions and tearing down the
same range, after which one of the two final swaps meets records the
other already removed — `space_insert` failing with `-EEXIST` *after*
accounting and splits are applied, which is precisely the
failure-after-mutation this design just finished removing. Two
things close it, and they are different problems:

- **Replacement against replacement**: a `struct mutex replace_lock`
  per user space (`kernel/include/kernel/mutex.h`; `sys_mmap` runs
  where it may sleep), held for the whole operation. Strict
  serialisation, no back-off loop, and therefore no livelock to
  argue about. It does not touch ordinary mapping, which never needs
  it: an `mmap(NULL, …)` cannot be handed a range that a quiesced
  region still owns.
- **Replacement against unmapping**: `VM_REGION_QUIESCED` is an
  **ownership claim**, not only a fault suppressor. No operation may
  unlink a region carrying it except the replacement that set it, so
  `vm_user_unmap` meeting one drops the lock, yields and re-scans.
  Without this a `munmap` racing a replace could free the range, a
  third thread's `mmap(NULL, …)` could take it, and the swap would
  collide. That is a caller bug, but it must not corrupt the space.

With both, step 4 is guaranteed to find exactly the regions step 2
quiesced, which is what makes it infallible.

**`VM_REGION_QUIESCED`: a region that does not fault in.** This is
the second thing review found, and it is not a detail. The fault
handler installs a demand-zero page **under `space->lock`**
(`vmm.c:514-580`). Without the flag, a fault landing between the
insert and a given teardown chunk would install a page into the *new*
region, and that chunk would then `arch_mmu_query` it, unmap it,
`pmm_free_page` it and decrement `anon_pages` — a live region holding
a freed frame, which is kernel corruption rather than a lost user
write. Refusing `VM_REGION_POPULATED` does not help: the danger is
pages faulted in *after* insertion.

Because both the installer and the teardown take the same lock, a
flag tested under it is enough. A fault on a quiesced region
**installs nothing and returns**, so the instruction re-executes and
faults again; the window is one teardown and the flag is gone. This
is a third outcome for the fault handler, beside "serviced" and
"unserviced", and it is the one change this unit makes outside the
VM and syscall layers.

**The new region must be demand-zero.** Not because the teardown
would tear it out — with the ordering above the teardown is already
finished when the region goes in — but because populating allocates
frames, `pmm_alloc_page` can fail, and step 4 must not be able to
fail. Every fallible thing belongs before the first mutation. The
primitive refuses `VM_REGION_POPULATED` with `-EINVAL` and says
*that* in its contract. Both callers pass `0` today
(`native.c:371`, `syscalls.c:970`), so it costs nothing now and stops
a later caller reintroducing a failure after the point of no return.

**Native `mmap` gains POSIX semantics.** `sys_mmap`'s `MAP_FIXED` arm
calls the new primitive. The `-EEXIST` return disappears from that
path.

**Native `mmap` gains `COSMO_MAP_FIXED_NOREPLACE`.** A new flag bit
keeps the old behaviour, which is what the self-test is really
asserting; the test moves to it. This is the same split Linux made,
for the same reason, and it means no caller loses an answer.

**The Linux door switches to the primitive**, deleting its
`vm_user_unmap` call. Its window closes as a consequence rather than
as a separate fix — which is the point of putting the operation in the
VM layer instead of in one syscall.

**libc stops punching holes.** `cosmo_thread_start` becomes reserve
then replace:

```c
    mmap(NULL, guard + stack + tcb, PROT_NONE)        /* reserve, and KEEP it */
    mmap(base + PAGE, stack + tcb, RW, MAP_FIXED)     /* replace the upper part */
```

The reservation holds the range across both calls, so there is no
moment when another thread can take it. The `munmap`, the retry loop,
`STACK_MAP_ATTEMPTS` and the comment explaining the race all go.

## Affected files

| file | change |
| --- | --- |
| `kernel/memory/vmm.c` | `vm_user_map_anon_replace`: checks and accounting first, quiesce, teardown, swap; `VM_REGION_POPULATED` refused |
| `kernel/memory/vmm.c` (fault path) | `vm_fault_handler` gains its third outcome: a fault on a `VM_REGION_QUIESCED` region installs nothing, yields, and returns so the instruction retries |
| `kernel/include/kernel/vmm.h` | the declaration and contract, `VM_REGION_QUIESCED`, `struct vm_space::replace_lock`, and why the new region must be demand-zero |
| `kernel/memory/vmm.c` (`vm_user_unmap`) | backs off rather than unlinking a region another replacement has claimed |
| `kernel/syscall/native.c` | `MAP_FIXED` calls the new primitive; `COSMO_MAP_FIXED_NOREPLACE` accepted and validated |
| `kernel/include/uapi/cosmo/syscall.h` | `COSMO_MAP_FIXED_NOREPLACE` beside `COSMO_MAP_FIXED` |
| `compat/linux/syscalls.c` | `LX_MAP_FIXED` uses the primitive; its `vm_user_unmap` call goes |
| `libc/include/sys/mman.h` | `MAP_FIXED_NOREPLACE` |
| `libc/src/thread.c` | reserve-then-replace; the punch, the retry and `STACK_MAP_ATTEMPTS` removed |
| `kernel/memory/memtest.c` | the primitive's own tests, below |
| `userland/init/init.c` | the `-EEXIST` assertion moves to `MAP_FIXED_NOREPLACE`; a new one that `MAP_FIXED` replaces and the range reads back as zeroes |
| `docs/kernel/memory/design.md`, `invariants.md` | the replacement rule, the three-section protocol, and what `VM_REGION_QUIESCED` claims |
| `docs/libc/invariants.md` | the thread-stack invariant PR #191 added says the retry is the contract; it becomes "the reservation is held across the replace" |
| `docs/compat/linux/*` | the door that already replaced now does so atomically |
| `docs/audit/2026-09-deferred-work-inventory.md` | strike §1.2's entry |
| `README.md` | Status entry |

## Tests

**In `memtest.c`**, against a scratch space:

| case | what it establishes |
| --- | --- |
| replace one whole region | the old region is gone, the new one is there, `mapped_pages` is exact |
| replace the middle of one region | splits at both ends, three regions after, page counts exact |
| replace a range spanning several regions and a hole | every covered region unlinked, the hole tolerated (no `STRICT`) |
| replace with `VM_REGION_POPULATED` | `-EINVAL`, nothing changed |
| replace that would exceed `COSMO_RLIMIT_AS` | `-ENOMEM`, **and the old mapping is still there** — the failure must not be half-applied |
| replace when a split spare cannot be allocated | `-ENOMEM`, nothing changed |
| the replaced range reads as zeroes | the teardown really ran; the old frames are not visible through the new mapping |
| `vm_user_region_count` after a replace that merges | the merge still happens |

| a replace whose teardown races a fault on the range | the faulting thread eventually reads zeroes; **`anon_pages` balances**; no frame of the new region was freed |
| after any replace, success or failure | no region is left `VM_REGION_QUIESCED` |
| two threads replacing overlapping ranges | both return, the range ends owned by exactly one region, `mapped_pages` and `anon_pages` balance, and neither saw `-EEXIST` |
| `munmap` racing a replace of the same range | no region is unlinked out from under the replacement; the space is consistent whichever wins |

The fault-racing case is the one that distinguishes this design from
the draft review rejected, and it needs two CPUs: one replacing in a
loop, one touching the range. Its bug-proof is to clear the quiesce
check in the fault handler — the frame accounting must then go wrong.

**In `init.c`** (the native ABI, from userland): `MAP_FIXED` over a
mapping with a known byte in it succeeds and the byte is gone;
`MAP_FIXED_NOREPLACE` over the same mapping returns `-EEXIST` and the
byte survives.

**The end-to-end one, and the bug-proof that matters.** `thrtest`
already starts threads while another churns the heap — the load that
produced the three CI failures. With the punch removed there is no
window for it to lose. The proof is the mutation: restore the
`munmap`-and-fill sequence and the `EEXIST` failures come back under
that load; a second mutation, making the replace non-atomic (unlock
between unlink and insert), must also reopen it. A replacement that
is atomic only by accident of timing would pass the first and fail the
second.

## Risks

**A quiesced fault spins.** A thread faulting on the range while the
teardown runs installs nothing and re-executes the instruction, so it
faults again until the flag clears. The window is one
`user_range_teardown` over the replaced range, and the replacing
thread never waits on the faulting one, so it cannot deadlock — but
on a single CPU a tight re-fault loop must not starve the replacer.
The fault path should yield before returning on a quiesced region,
and the test below exists to show a thread does get through.

**The page accounting is applied early and held.** Step 2 charges the
new region and credits the old ones before either has happened, so
that step 4 cannot fail a limit check. Between the two, `mapped_pages`
describes the intended state rather than the current one. That is the
price of an infallible finish, and anything reading `mapped_pages`
concurrently (only `COSMO_RLIMIT_AS` does) sees a figure that is
correct for the range's owner but briefly counts the new region's
size for regions still linked. Worth stating because a future reader
of that counter will find it surprising.

**`init.c`'s SIGSEGV handler maps `MAP_FIXED` at a fixed address on
every fault** (`init.c:1783`) and currently gets `-EEXIST` on the
second and later faults, silently. Afterwards each fault replaces the
page with a fresh zero one. The handler stores into it immediately,
so the behaviour should be unchanged — but it is a live caller whose
semantics change and the suite must show it.

**The flag is a third state for the fault handler.** "Serviced",
"unserviced" and now "retry" — the one change this unit makes outside
the VM and syscall layers. A region left quiesced by a bug would hang
a process in a fault loop rather than failing it, which is a worse
symptom than a crash. The flag is set and cleared in the same
function, and a test should assert no region is left quiesced after a
replace.

**Scope.** The primitive is anonymous-memory only, matching both
callers. File mappings do not go through `vm_user_map_anon` and are
not in this unit.

## Alternatives considered

**`SYS_mprotect` instead.** `vm_user_protect` already exists, splits
and merges correctly, and is used by the ELF loader; a syscall over it
is a small unit. It would let `cosmo_thread_start` map the whole span
read/write and drop the guard page to `PROT_NONE` with no unmapping
at all — arguably a nicer libc. It is **not** this unit, for two
reasons: it leaves `MAP_FIXED` non-conforming for every other caller,
and it leaves the Linux door's non-atomic replacement in place. It
stays in the inventory on its own merits, and the two compose: with
both, the reservation trick is unnecessary.

**Retry in libc, as PR #191 shipped.** Bounded, harmless, and already
there. It is a workaround: it narrows a window rather than closing
one, it cannot help the Linux door, and it leaves every future caller
to rediscover the same race. Keeping it permanently would mean
documenting "`MAP_FIXED` may spuriously fail" as a contract, which is
the opposite of what POSIX says.

**Make `space_insert` replace.** Wrong layer. It is called from every
mapping path, holds the lock already, and cannot do the teardown or
the frees; making it replace would give every mapping silent
overwrite semantics, which is exactly the bug `-EEXIST` was protecting
against.

**Keep `-EEXIST` as the default and add a `MAP_FIXED_REPLACE` flag.**
The inverse split. Rejected because it leaves the default
non-conforming: a program written against POSIX gets a failure the
standard says cannot happen, and the Linux door would still need its
own path. The conforming behaviour should be the one you get by
default, with the restrictive one named.
