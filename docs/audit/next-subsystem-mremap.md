# NEXT SUBSYSTEM — the Linux personality has no mremap

> Constitution §68 report. This PR adds the report and the probe
> (`tools/mremap-probe.py`); the `vm_user_region_at` snapshot helper, the
> `lx_mremap` handler and the `lxtest` check described under "Design" and the
> edits in "Affected files" are planned work that lands in the implementation
> PR that follows, gated on CI. As committed here, `mremap` returns `-ENOSYS`.

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
- **It is in pattern and the mechanism exists.** Grow is `vm_user_map_anon` at
  the mapping's end (which merges), shrink is `vm_user_unmap` of the tail; both
  are the primitives `mmap`/`munmap` already use. This unit adds the door and a
  small snapshot helper, not a new VM mechanism.

## The implementation before this unit

| piece | where | what it does |
|---|---|---|
| grow / extend | `vm_user_map_anon(space, base, size, prot, flags, name)` (`vmm.h`) | maps demand-zero pages at an exact base, merging with an adjacent same-kind/prot/flags/name region; `-EEXIST` if the range overlaps |
| shrink | `vm_user_unmap(space, base, size, flags)` (`vmm.h`) | unmaps a range, splitting regions at the ends (Linux-munmap semantics without `VM_UNMAP_STRICT`) |
| region lookup | `vm_find_region(space, va)` (`vmm.h`) | the region containing `va` — but returns a live pointer after dropping the space lock, unsafe to read under concurrent unmap |
| page rounding | `page_align_up(x)` (`types.h`), as `lx_mmap` uses | round a length up to a page |
| dispatch | `linux_table[LX_NR_MAX]`; unlisted → `lx_unknown` | `mremap` is unlisted → `-ENOSYS` |

## Design

### 1. A region-snapshot helper

`vm_find_region` hands back a pointer into the region list after releasing the
space lock, so reading its fields races a concurrent `munmap`/`mremap` that
could free the region. The unit adds a small helper that copies the fields out
**under the lock**:

```c
struct vm_region_info { uint64_t base; size_t size; vm_prot_t prot;
                        enum vm_region_kind kind; unsigned flags;
                        const char *name; };   /* the grow re-uses it so the new piece merges */
bool vm_user_region_at(struct vm_space *space, uint64_t va, struct vm_region_info *out);
```

It returns whether a region contains `va` and fills `*out` by value — no live
pointer escapes the lock, so `lx_mremap` reasons about a stable snapshot.

### 2. The Linux door (bounded: anonymous, whole-region, in place)

`lx_mremap(old_addr, old_size, new_size, flags, new_addr)`:

- `flags` must be a subset of `MREMAP_MAYMOVE | MREMAP_FIXED`; `MREMAP_FIXED` is
  rejected `-EINVAL` (v1 does not place at a chosen address).
- `old_addr` must be page-aligned and `new_size` non-zero (`-EINVAL`); both
  sizes are rounded up to pages.
- Snapshot the region at `old_addr` with `vm_user_region_at`; `-EFAULT` if
  none. It must be **`VM_REGION_ANON`** and the request must name the **whole**
  region (`old_addr == info.base && old_size == info.size`); otherwise `-EINVAL`
  (a `VM_REGION_FILE` or `VM_REGION_PHYS` mapping, and a sub-range resize, are
  out of scope for v1). The kind check does not distinguish `MAP_SHARED` from
  `MAP_PRIVATE` anonymous mappings — the VM region carries no such flag — but
  it need not: with no `fork`, an anonymous `MAP_SHARED` mapping has no second
  sharer, so resizing it in place is the same operation as for a private one.
- **Same size**: return `old_addr` (no-op).
- **Shrink** (`new_size < old_size`): `vm_user_unmap(space, old_addr + new_size,
  old_size - new_size, 0)`; return `old_addr`.
- **Grow** (`new_size > old_size`): `vm_user_map_anon(space, old_addr +
  old_size, new_size - old_size, info.prot, 0, info.name)`. If it merges (it
  will, same kind/prot/name) the mapping is extended in place; return
  `old_addr`. `-EEXIST` means the space after is occupied — return `-ENOMEM`.

`mremap` returns the (unchanged) address on success, so the handler returns
`old_addr`.

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
| `kernel/memory/vmm.c`, `kernel/include/kernel/vmm.h` | `vm_user_region_at` + `struct vm_region_info` |
| `compat/linux/linux_abi.h` | `MREMAP_MAYMOVE`/`MREMAP_FIXED`/`MREMAP_DONTUNMAP` |
| `compat/linux/syscalls.c` | `lx_mremap` + `[LX_mremap]` in the table |
| `tests/linux/lxtest.c` | the mremap checks |
| `README.md` | Status entry |

No syscall-number header change: `LX_mremap` is already defined (25 / 216).

## APIs

The Linux `mremap(2)` system call, in place (grow/shrink) on an anonymous
mapping. No native ABI change: it composes the existing `vm_user_map_anon`/
`vm_user_unmap` primitives; the gap this closes is the Linux one.

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
- shrink not unmapping the tail (return without `vm_user_unmap`): the
  shrink test's "freed page faults" check fails (the page is still mapped).
- the blocked-grow path returning the address instead of `-ENOMEM` when
  `vm_user_map_anon` gives `-EEXIST`: the blocked-grow check fails.

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
- **Snapshot-then-act race.** `lx_mremap` snapshots the region, then calls
  `vm_user_unmap`/`vm_user_map_anon`, each atomic under the space lock — but not
  the two together. A thread that `munmap`s this mapping and maps something else
  in the same range between the snapshot and the call races: a shrink would then
  `vm_user_unmap` whatever now occupies the tail, and a grow's
  `vm_user_map_anon` would `-EEXIST` (→ `-ENOMEM`) or merge with the wrong
  neighbour. No memory is corrupted (the snapshot is by value and every map/
  unmap is internally locked), but a concurrent resize/unmap of the *same*
  mapping is a caller error with an unspecified result. A fully atomic resize
  would need a single `vm_user_remap` primitive under one hold of the space
  lock — a follow-up; it is not needed for the single-threaded `realloc` path.

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
- **Reading the region through `vm_find_region` directly.** Its pointer is live
  past the lock; reading it under a concurrent unmap is a use-after-free, so the
  unit adds the by-value `vm_user_region_at` snapshot instead.
