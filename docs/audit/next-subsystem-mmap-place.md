# NEXT SUBSYSTEM — a placement is inserted under the hold that chose it

Constitution §68 report. It takes up a defect the tree found on
2026-09-23 and recorded in `docs/testing/flakes.md` ("`thrtest` cannot
start a thread", the fourth sighting), and adds it to the deferred-work
inventory's §3 as a row marked *taken up*; the build strikes it.

## Problem

`sys_mmap`'s non-fixed path — the path every `malloc` growth, every
thread-stack reservation and every hint-less file mapping takes — is two
calls under two holds of the space lock:

```c
    base = vm_user_find_free(p->space, from, len);          /* takes and releases space->lock */
    ...
    rc = vm_user_map_anon(p->space, base, len, vprot, 0, "mmap");   /* takes it again; space_insert */
```

`kernel/syscall/native.c`, the `else` branch of `sys_mmap`; the Linux
door's `lx_mmap` (`compat/linux/syscalls.c`) is the same two calls. Between
them the range the first call chose belongs to nobody. Two threads asking
for a placement at once can both be handed the same first-fit hole, and
the loser's `space_insert` returns `-EEXIST` — for a request that named
no address, on a range the kernel itself proposed a microsecond earlier.

**It reached userland.** `cosmo_thread_start` reserves a stack with
`mmap(NULL, …, PROT_NONE)`; `thrtest`'s `env_churn` thread mallocs
beside it, which is `mmap(NULL, …)` too. One boot of a documentation-only
branch:

```
thrtest: FAIL env_reader start at line 1169: rc -17
```

`-17` is `EEXIST`. The MAP_FIXED unit (PR #193) removed the *other*
race on that line — the punch-and-fill in libc, where a `MAP_FIXED` that
refused to replace lost to a hole — and with it the sixteen-attempt
retry that had shipped to make losing harmless. That retry had been
absorbing this race as well, without anyone knowing it existed. Now
there is nothing left to absorb it, and a thread start fails.

### Measured

`tools/mmap-place-probe.py`, shipped with this report, injects a
self-test that runs exactly the syscall's two calls from N kernel
threads on one scratch user space, 2000 rounds each, one page per
placement, and counts what the second call answers:

| architecture | threads | placements offered | inserted | `-EEXIST` |
| --- | --- | --- | --- | --- |
| x86-64 | 2 | 4000 | 2091 | **1909** (48 %) |
| x86-64 | 4 | 8000 | 3686 | **4314** (54 %) |
| AArch64 | 2 | 4000 | 2156 | **1844** (46 %) |
| AArch64 | 4 | 8000 | 3376 | **4624** (58 %) |

Zero `other`, zero `nofree`: every failure is the collision and nothing
else, on a space with room for all of them.

Every `-EEXIST` is a range the kernel proposed and then refused. This is
the syscall's own sequence, not an approximation of it; the user-visible
form differs only in how often two threads ask at once. **And when they
do ask at once, about half of them lose.** The word "race" suggests a
window a few instructions wide; this one is the whole of a lock
release, a return, a region allocation and a lock acquisition, and two
threads on two CPUs land in it as often as not. `thrtest` sees it once
in many boots only because its churn and its thread starts rarely
coincide, not because the collision is rare when they do.

### Why it is a unit and not a patch

- **Correctness.** A request that names no address cannot legitimately
  fail because the address is taken. POSIX gives `mmap(NULL, …)` one
  failure for exhaustion, `ENOMEM`; `EEXIST` is not in its vocabulary,
  and the Linux door returning it is a failure Linux never produces —
  the same argument the MAP_FIXED report made for the fixed path.
- **The same mistake in the same file, one branch over.** That report
  fixed replace-as-two-holds and left choose-as-two-holds standing. The
  tree even knows the shape: the `MAP_FIXED` unit's own filler racer
  (`kernel/memory/memtest.c`, `repl_filler`) does find-then-map and
  silently tolerates its map failing, with a comment that a hole
  "belongs to nobody" in between.
- **Reachable by any multi-threaded program**, and the more it allocates
  the more reachable: a thread start beside a malloc is the ordinary
  shape of a threaded program's first millisecond.
- **Both doors**, and the file path too: `vm_user_map_file` at a chosen
  base has the same gap in front of it.

## Current implementation

`vm_user_find_free(space, from, size)` (`kernel/memory/vmm.c:2225`) takes
`space->lock`, walks the region list for the first gap of `size` plus a
guard page at or above `from`, releases the lock, and returns the
address or 0. It makes no claim.

`vm_user_map_anon(space, base, size, …)` builds the region, takes the
lock, checks `COSMO_RLIMIT_AS`, `space_insert`s — which walks the list
and returns `-EEXIST` on any overlap — and releases. `vm_user_map_file`
does the same for a file region. `vm_user_map_anon_replace` is the
fixed path and owns its range throughout (M40); it is not involved.

**The callers of `vm_user_find_free`:**

| caller | threads that can race it | verdict |
| --- | --- | --- |
| `sys_mmap`, non-fixed (`native.c:472-483`) | any thread of the process | **the defect** |
| `lx_mmap`, non-fixed (`compat/linux/syscalls.c:1027-1035`) | any thread of the process | **the defect** |
| `process_create_from_images` (`process.c:581`), the ET_DYN interpreter's base | none: the process has no thread yet | sound, and stays so only while that is true |
| `repl_filler` (`memtest.c:625`), a test adversary | by design | tolerates its own loss on purpose |

Both doors try the caller's hint first and `USER_MMAP_BASE` second, each
attempt a find; the map then follows whichever succeeded.

**libc.** `cosmo_thread_start` is reserve-then-replace since the
MAP_FIXED unit, with no retry: its comment says "there is nothing left to
lose", which is true of the punch and false of the reservation.

## Why it matters

A threaded program on this system can fail to start a thread because a
sibling allocated at the wrong microsecond, with an errno that says the
address it never asked for is taken. It has already happened in the
tree's own test suite, on a correct kernel, and until this report the
record called it a flake with a fix that had shipped. A kernel that
promises an address and then refuses it has no invariant about
placement at all; M40 gives the fixed path one and this gives the other
path its counterpart.

## Design

### 1. One operation: choose and insert under one hold

```c
/* First-fit placement of `size` bytes at or above `from`, inserted under
 * the same hold of space->lock that chose it, so no other placement can
 * be handed the range in between. *base is the address. -ENOMEM if no
 * gap of that size exists above `from`, or the AS limit refuses; the
 * caller may try again from a lower `from`. Never -EEXIST: a range this
 * function proposes is a range it has taken. */
int vm_user_map_anon_free(struct vm_space *space, uint64_t from, size_t size, vm_prot_t prot,
                          unsigned flags, const char *name, uint64_t *base);
int vm_user_map_file_free(struct vm_space *space, uint64_t from, size_t size, vm_prot_t prot,
                          vm_prot_t maxprot, unsigned flags, struct vnode *vn, uint64_t off,
                          const char *name, uint64_t *base);
```

Inside, the search `vm_user_find_free` does becomes a locked helper,
`range_first_fit_locked`, and the two map functions gain a form that
calls it and `space_insert`s without releasing the lock between. Nothing
about the placement policy changes: first fit, a guard page between
regions, the hint tried first — the *answer* is what the caller would
have got, and now it is also what the caller gets.

`vm_user_find_free` stays, for the interpreter's base (single-threaded
by construction, and the comment says so) and for the filler racer,
whose purpose is to take what it is offered and see what happens. Its
comment gains one sentence: its answer is advisory, and a caller with
other threads must use the `_free` forms.

### 2. Both doors, both kinds

`sys_mmap` and `lx_mmap`'s non-fixed branches become two attempts each
of one atomic operation: from the hint, then from `USER_MMAP_BASE`, as
today; `-ENOMEM` from the first is what triggers the second. The fixed
branches are untouched. `brk` (`lx_brk`) maps at a computed address it
already owns the neighbourhood of and is not a placement.

### 3. The invariant

**M46. A placement the kernel chooses is inserted under the hold that
chose it.** The non-fixed paths of both doors never return `-EEXIST`;
an address the kernel proposes is an address it has taken. M40 is the
fixed path's half of the same rule.

### 4. The §70 gate

**Correctness.** The lock is already held for the insert; the search
moves inside the same hold. Nothing new is locked and no order changes.

**Concurrency.** Any number of threads placing at once. The hold is
longer by one list walk, which the search already did under the lock
anyway; the two walks become one region walk that finds and one insert
walk — or one walk that does both, since first-fit and insertion visit
the same prefix of the list.

**Ownership, lifetime.** Unchanged: the region is the space's from the
insert on.

**Failure.** `-ENOMEM` for no gap or the AS limit, as today. `-EEXIST`
is no longer a possible answer from either door's non-fixed path, and
the tests say so.

**Performance.** One spinlock acquire and one list walk fewer per
placement. Not measured as a win; measured to show it is not a loss.

## Affected files

| file | change |
| --- | --- |
| `kernel/memory/vmm.c`, `kernel/include/kernel/vmm.h` | `range_first_fit_locked`; `vm_user_map_anon_free`, `vm_user_map_file_free`; `vm_user_find_free`'s comment |
| `kernel/syscall/native.c` | `sys_mmap`'s non-fixed branch: two atomic attempts |
| `compat/linux/syscalls.c` | `lx_mmap`'s non-fixed branch: the same |
| `libc/src/thread.c` | the comment that says nothing is left to lose names what was |
| `kernel/memory/memtest.c`, `kernel/core/selftest.c`, `kernel/include/kernel/selftest.h` | `mmap-place-race` |
| `userland/init/init.c` | the `mmap` section: two threads placing at once, no `EEXIST` |
| `tests/linux/lxtest.c` | the same through the Linux door |
| `docs/kernel/memory/design.md`, `invariants.md` (**M46**), `testing.md` | as built |
| `docs/testing/flakes.md` | the `thrtest` entry: the fourth sighting's row closed |
| `docs/audit/2026-09-deferred-work-inventory.md` | the §3 row this report adds as *taken up*; the build strikes it |
| `README.md` | Status entry |
| `tools/mmap-place-probe.py` | shipped with this report |

## APIs

### New

`vm_user_map_anon_free` and `vm_user_map_file_free`, above. Nothing
else is added.

### Existing, relied on

`vm_user_find_free` is unchanged in signature and behaviour. It stays
because two callers may use it — the interpreter's base at process
creation, before the process has a second thread, and the MAP_FIXED
unit's filler racer, whose purpose is to take what it is offered — and
its comment will say that its answer is advisory.

## Migration plan

1. **The `_free` forms**, with `mmap-place-race` against them: N threads
   placing on one space, every placement inserted, none `-EEXIST`.
   Then the bug-proof: the test pointed at the old pair, which must show
   the probe's rate.
2. **Both doors** switched, with the user-level racers at each. Then the
   door bug-proofs: one door left on the old pair, caught by that door's
   racer and not the other's.
3. **The comments** in libc and at `vm_user_find_free`.
4. Docs, inventory, README, banner; release builds, `gmake host-test`,
   every mutation alone with the boot confirmed.

## Tests

| test | what it proves | bug-proof |
| --- | --- | --- |
| `mmap-place-race` (kernel) | N threads, R rounds each of `vm_user_map_anon_free` on one space: `N×R` insertions, zero `-EEXIST`, and the space's `mapped_pages` equals the count — the placements are all distinct because every insert succeeded, and an overlapping insert cannot | the test pointed at `vm_user_find_free` + `vm_user_map_anon`: `-EEXIST` at the probe's rate, which is **about half of all placements** with two threads. A rate, said so: the split cannot be held with a seam because the fix removes the gap the seam would sit in; a rate of one in two over thousands of rounds is decisive in every boot |
| `init --selftest`, `mmap` section | two native threads each `mmap(NULL, page)` in a loop and record every errno: none is `EEXIST` | the native door left on the old pair: `EEXIST` at a rate this racer measures in its own line |
| `lxtest`, threads section | the same through `LX_mmap` | the Linux door left on the old pair: caught here and not by the native racer, which is what "both doors" means |

The two user-level racers are rate-based, and the report says so rather
than dressing them up: their job is to say which door forgot. At one
collision in two, a few hundred placements from two threads cannot miss
a door left on the old pair, and each racer asserts on its own count.

## Benchmarks

`mmap`/`munmap` of one page in a loop, both doors, before and after: one
lock acquisition fewer per call, expected within noise, measured so the
claim is a number.

## Risks

- **The hint-first, base-second retry.** Two atomic attempts are still
  two; a placement that fails from the hint and succeeds from the base
  is fine, and one that fails both is `-ENOMEM` as today. What must not
  happen is a fallback that reads the first attempt's failure as
  anything but "no gap there".
- **The file form.** `vm_user_map_file` has a long tail (the text-mapping
  clash, the cache lock) after its insert; the `_free` form must choose
  and insert in the space-lock hold and then continue exactly as the
  base-given form does. If that means restructuring `vm_user_map_file`
  into "choose" and "the rest", the rest is shared, not copied.
- **`vm_user_find_free` left callable** is a door someone will use with
  threads later. Its comment is the guard; the census in this report is
  the record of who may.

## Alternatives considered

- **Restore libc's retry.** It hid this race for weeks; hiding it again
  is not a fix, and the Linux door has no libc of ours to retry in.
- **Take the range with `MAP_FIXED_NOREPLACE` semantics after finding
  it.** That is the old pair with a different errno.
- **A per-space placement lock separate from `space->lock`.** Two locks
  for one list; the search already runs under the space lock, so making
  the insert part of the same hold costs nothing and adds no order.

🤖 Generated with [Claude Code](https://claude.com/claude-code)

https://claude.ai/code/session_014qfcm8FQycZFpYeonUcCz2
