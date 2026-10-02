# NEXT SUBSYSTEM — the Linux personality has no mremap

> Constitution §68 report. This PR adds the report and the probe
> (`tools/mremap-probe.py`); the `vm_user_region_at` snapshot helper, the
> `lx_mremap` handler and the `lxtest` check described under "Design" and the
> edits in "Affected files" are planned work that lands in the implementation
> PR that follows, gated on CI. As the report was committed (before PR #295),
> `mremap` returned `-ENOSYS`.
>
> **Built in PR #295.** The implementation added the `lx_mremap` handler
> (in-place grow/shrink of a whole anonymous mapping) and wired `[LX_mremap]`;
> both arches boot PASS and `host-test` passes. Review replaced the
> "Design" sketch's snapshot-then-resize (a `vm_user_region_at` helper
> feeding separate `vm_user_unmap`/`vm_user_map_anon` calls) with a single
> `vm_user_remap` primitive that finds, validates and resizes the region
> under one hold of the space lock — closing the snapshot-then-act race the
> "Risks" section had deferred, and extending the region record itself so a
> grow preserves its flags instead of leaving two regions. `lx_mremap` also
> bounds the lengths against the user window before rounding, so a size near
> `UINT64_MAX` can no longer round to zero. Every present-tense statement
> below describes the state the report measured, before this implementation.

## Problem

`mremap(2)` (x86-64 25, AArch64 216) has a syscall number (`LX_mremap`) but no
handler and no `linux_table` entry, so the dispatcher's `lx_unknown` returns
`-ENOSYS`. `mremap` resizes an existing mapping in place — grow it when the
space after it is free, shrink it, or (with `MREMAP_MAYMOVE`) relocate it. It is
how a program enlarges a large buffer without a copy; glibc's `realloc` uses it
for `mmap`-backed allocations. Without it, that path falls back to
allocate-copy-free, and code that calls `mremap` directly gets `-ENOSYS`.

The VM layer already has the primitives a bounded, in-place `mremap` needs. A
user anonymous mapping grows by mapping fresh demand-zero pages at its end —
`vm_user_map_anon` places a region at an exact base and **merges it with the
adjacent region of the same kind, prot, flags and name** (`vmm.h`), which is
exactly an in-place extension; it returns `-EEXIST` if the range is occupied,
which is how "the space after is not free" is detected atomically. A mapping
shrinks by unmapping its tail — `vm_user_unmap` splits a region at the ends. No
new resize or move primitive is required for the anonymous, in-place case.

## Probe

`tools/mremap-probe.py` adds one check to `tests/linux/lxtest.c` (which runs in
the standard boot); `LX_mremap` is already defined, so no syscall-number header
changes:

```
LXMREMAP: mremap unimplemented -> -ENOSYS; no in-place resize
```

The check `mmap`s a page, then asserts `mremap` of it returned `-ENOSYS` at the
report commit, before the implementation PR. The marker prints only when the
syscall really returned `-ENOSYS`, so grepping it cannot show a false result
after a failed check.

## Why it matters

- **Resize without a copy.** `mremap` is how a program grows a large mapping in
  place; its absence forces an allocate-copy-free on every growth and makes
  direct callers fail. `realloc` of `mmap`-backed blocks is the common path.
- **It is in pattern and the mechanism exists.** Grow extends the region at the
  mapping's end, shrink trims it and tears down the tail — the region-list and
  teardown machinery `mmap`/`munmap` already use. The implementation (the PR
  that follows this report) adds the door and one `vm_user_remap` primitive that
  composes that machinery under the space lock, not a new VM mechanism.

## The implementation before this unit

| piece | where | what it does |
|---|---|---|
| grow / extend | `vm_user_map_anon(space, base, size, prot, flags, name)` (`vmm.h`) | maps demand-zero pages at an exact base, merging with an adjacent same-kind/prot/flags/name region; `-EEXIST` if the range overlaps |
| shrink | `vm_user_unmap(space, base, size, flags)` (`vmm.h`) | unmaps a range, splitting regions at the ends (Linux-munmap semantics without `VM_UNMAP_STRICT`) |
| region lookup | `vm_find_region(space, va)` (`vmm.h`) | the region containing `va` — but returns a live pointer after dropping the space lock, unsafe to read under concurrent unmap |
| page rounding | `page_align_up(x)` (`types.h`), as `lx_mmap` uses | round a length up to a page |
| dispatch | `linux_table[LX_NR_MAX]`; unlisted → `lx_unknown` | `mremap` is unlisted → `-ENOSYS` |

## Design

> **As built (PR #295).** The sections below sketch the design the report
> planned: a `vm_user_region_at` snapshot feeding separate `vm_user_unmap`/
> `vm_user_map_anon` calls. Review replaced that with a single
> `vm_user_remap` primitive — see the "Built" note at the top. The text here
> is updated to match what shipped.

### 1. An in-place resize primitive

`vm_find_region` hands back a pointer into the region list after releasing the
space lock, so reading its fields — then acting on them with a *second* locked
call — races a concurrent `munmap`/`mremap`/`mmap` of the same range. Rather
than snapshot and act, the unit adds one primitive that does the whole thing
under the space lock:

```c
int vm_user_remap(struct vm_space *space, uint64_t base, size_t old_size, size_t new_size);
```

It finds the region at `base`, checks it is a whole `VM_REGION_ANON` mapping
(`-EFAULT`/`-EINVAL`/`-EBUSY`), and resizes it by extending or trimming the
region record itself. A grow runs under one lock hold after checking the space
above is free. A shrink must free the tail, and `user_range_teardown` takes the
lock per chunk, so it runs outside the hold: the tail is first split into its
own region and marked `VM_REGION_QUIESCED` under `space->replace_lock` — the
claim-then-free discipline `map_replace` uses — so nothing can map the range or
fault it in while the teardown runs, and the record is unlinked only once the
teardown is done.

### 2. The Linux door (bounded: anonymous, whole-region, in place)

`lx_mremap(old_addr, old_size, new_size, flags, new_addr)`:

- `flags` must be a subset of `MREMAP_MAYMOVE | MREMAP_FIXED`; `MREMAP_FIXED` is
  rejected `-EINVAL` (v1 does not place at a chosen address).
- `old_addr` must be page-aligned and both lengths non-zero (`-EINVAL`); each
  length is bounded against the user window (`> VM_USER_HI - VM_USER_LO` is
  `-EINVAL`) **before** rounding up to pages, so a length near `UINT64_MAX`
  cannot round to zero.
- Call `vm_user_remap(space, old_addr, old_size, new_size)`, which validates
  under the lock: a region must exist at `old_addr` (`-EFAULT`), be
  **`VM_REGION_ANON`** and name the **whole** region (`old_addr == base &&
  old_size == size`); otherwise `-EINVAL` (a `VM_REGION_FILE`/`VM_REGION_PHYS`
  mapping or a sub-range resize is out of scope for v1), and `-EBUSY` if a
  `MAP_FIXED` replacement owns it. The kind check does not distinguish
  `MAP_SHARED` from `MAP_PRIVATE` anonymous mappings — the VM region carries no
  such flag — but it need not: with no `fork`, an anonymous `MAP_SHARED`
  mapping has no second sharer, so resizing it in place is the same operation
  as for a private one.
- **Same size**: validated, then a no-op (an unmapped address still gets
  `-EFAULT`, not a bogus success).
- **Shrink** (`new_size < old_size`): trim the region record, tearing down the
  tail under the claim described in §1.
- **Grow** (`new_size > old_size`): check the space above is free and extend the
  region record in place; `-ENOMEM` if it is occupied. Because the region grows
  in place, its flags (guard pages, name) are preserved and there is never a
  second region to fail a later whole-region check.

`mremap` returns the (unchanged) address on success, so `lx_mremap` returns
`old_addr` when `vm_user_remap` returns 0.

### 3. MREMAP_MAYMOVE

`MREMAP_MAYMOVE` is accepted but v1 never relocates: when an in-place grow
cannot be satisfied it returns `-ENOMEM` rather than moving the mapping. This is
safe for the dominant caller — `realloc` treats `-ENOMEM` from `mremap` as "grow
failed" and falls back to allocate-copy-free — and a moving `mremap` (find a new
hole, remap the pages, unmap the old) is a larger, separate unit. A program that
*requires* the move gets a clean `-ENOMEM`, not a wrong result.

## Affected files

| file | change |
|---|---|
| `kernel/memory/vmm.c`, `kernel/include/kernel/vmm.h` | `vm_user_remap` (in-place whole-anon resize under the space lock) |
| `compat/linux/linux_abi.h` | `MREMAP_MAYMOVE`/`MREMAP_FIXED`/`MREMAP_DONTUNMAP` |
| `compat/linux/syscalls.c` | `lx_mremap` + `[LX_mremap]` in the table |
| `tests/linux/lxtest.c` | the mremap checks |
| `README.md` | Status entry |

No syscall-number header change: `LX_mremap` is already defined (25 / 216).

## APIs

The Linux `mremap(2)` system call, in place (grow/shrink) on an anonymous
mapping. One new native entry point, `vm_user_remap`, which composes the
existing region-list and teardown machinery (`region_split`,
`region_merge_forward`, `user_range_teardown`, the `VM_REGION_QUIESCED` claim)
under the space lock; no on-disk or user ABI change. The gap this closes is the
Linux one.

## Tests

Planned for the implementation (`tests/linux/lxtest.c`, which runs in the
standard boot).

| test | proves |
|---|---|
| `mremap` grow | `mmap` 1 page (write a sentinel), `mremap` to 2 pages returns the same address; the sentinel survives and the new page is readable/writable |
| `mremap` shrink | `mremap` back to 1 page returns the same address; the first page still holds its sentinel, and a read of the freed second page faults (the shrink really unmapped it) |
| `mremap` blocked grow | place a mapping immediately after a first one, then `mremap`-grow the first into it: `-ENOMEM` (the space after is not free), even with `MREMAP_MAYMOVE` |
| `mremap` errors | `MREMAP_FIXED` is `-EINVAL`; `mremap` of an unmapped address is `-EFAULT`; a non-page-aligned `old_addr` is `-EINVAL` |

**Planned mutations** (each alone, boot confirmed):
- grow mapping fresh (non-merging) pages but not preserving the old region's
  contents — e.g. `VM_REGION_POPULATED` zeroing over the whole range: the grow
  test's sentinel-survives check fails.
- shrink not trimming the region (a no-op resize that reports success): the
  shrink test's `mprotect`→`-ENOMEM` check fails (the tail is still mapped).
- the grow ignoring the free-space check and extending over an occupied range:
  the blocked-grow check fails (it returns the address, not `-ENOMEM`).

## Benchmarks

None.

## Risks

- **In place only; `MREMAP_MAYMOVE` never moves.** A grow that the space after
  cannot absorb is `-ENOMEM` (§3); `realloc` handles that, and a relocating
  `mremap` is a follow-up. `MREMAP_FIXED` and `MREMAP_DONTUNMAP` are rejected.
- **Whole anonymous region only.** v1 resizes a mapping named by its exact base
  and full size, and only `VM_REGION_ANON`; a sub-range resize or a
  `VM_REGION_FILE`/`VM_REGION_PHYS` mapping is `-EINVAL`. Anonymous `MAP_SHARED`
  is resized like any anonymous mapping (no `fork`, so no second sharer). This
  covers the `realloc`/large-buffer case; the general form is a later unit.
- **Concurrent resize (resolved as built).** The report planned a
  snapshot-then-act handler and noted its race: snapshot the region, then call
  `vm_user_unmap`/`vm_user_map_anon` separately, so a concurrent `munmap`+remap
  of the same range could make a shrink tear down the replacement's pages. As
  built this does not happen: `vm_user_remap` finds, validates and resizes under
  one hold of the space lock, and the shrink keeps the tail claimed
  (`VM_REGION_QUIESCED`) while its teardown runs, so a concurrent `mmap(NULL,
  …)` cannot be handed the range. No `vm_user_remap` follow-up is outstanding —
  it is this unit's primitive.

## Alternatives considered

- **A moving `mremap` in v1.** Relocation needs to find a new hole, repoint or
  copy the mapping's pages, and unmap the old — materially more than the
  in-place case, and the anonymous page copy has its own hazards. Deferred; the
  in-place grow/shrink is what `realloc` and large-buffer growth use.
- **`signalfd` or `setsockopt` expansion instead.** `signalfd` needs a new
  signal-arrival waitqueue and a multi-instance pending queue (the kernel keeps
  a per-signal bitmask); `setsockopt` has almost no per-socket backing to honor
  (`SO_REUSEADDR`/`TCP_NODELAY`/buffer sizes would stay `-ENOPROTOOPT`). `mremap`
  reuses mature VM primitives, so it is the better-bounded pick.
- **Reading the region through `vm_find_region`, then acting.** Its pointer is
  live past the lock, and a snapshot-then-act handler (even reading the fields
  by value) still races a concurrent resize across the two calls. The unit does
  the lookup, the checks and the resize inside one `vm_user_remap` under the
  space lock instead.
