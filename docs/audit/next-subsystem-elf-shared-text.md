# NEXT SUBSYSTEM — a program's text belongs to the file, not to each process that runs it

> **BUILT.** This is the report as written, with an as-built banner.
> What the build changed, and what it found:
>
> 1. **The qualification compared the wrong size, and the first version
>    shared nothing.** `struct elf_segment`'s `memsz` is the *page
>    rounded* span, so `filesz == memsz` is false for every real text
>    segment and the test reported "no shareable executable segment".
>    The file's own `p_memsz` is kept as `file_memsz` now and that is
>    what the rule uses. The padding between `filesz` and the end of its
>    last page is not a zero tail: it is the file's next bytes.
> 2. **"Shared and executable in its `maxprot`" was the wrong
>    discriminator for text**, and the boot said so. `maxprot` is
>    permissive by default -- RWX for a shared mapping of a writable
>    file -- so every shared file mapping counted as text and ordinary
>    writes to ordinary mapped files were refused.
> 3. **Narrowing it to the actual `prot` was still wrong.** A program
>    may map a file executable and write to it on purpose, and the page
>    cache syncs the instruction cache for exactly that case; refusing
>    those writes broke that behaviour and the test that proves it. The
>    discriminator is `VM_MAP_TEXT`, passed by `elf_load_into` and by
>    nothing else: what must not change underneath a process is the
>    program it is *running*, and only the loader can identify that.
> 4. **The writable segment is not a private file mapping**, as the
>    report proposed, and the reason is that the win was somewhere else.
>    Its file content is twenty-four bytes; what cost 27 pages was the
>    zero tail being populated. The copy path is split where the pages
>    the file's bytes touch end, and the tail beyond them is left
>    demand-paged -- an anonymous page arrives zero, which is what a
>    zero tail needs, with no copy to get wrong. **That is where most of
>    the saving came from: 40 pages became 16.**
> 5. **`ETXTBSY` had to be added in three places**, not one: the
>    kernel's errno list, the user ABI's, and libc's.
> 6. **Found in the previous unit, and fixed here because it failed a
>    boot of this one:** `bench-balance` asserted 85% and read 84% on a
>    boot where the threads had spread to all four CPUs. Two 500 ms
>    samples of the same work differ by more than fifteen points on this
>    host, so that threshold separated noise rather than behaviour. It
>    is 70% now, above the working case's floor and far above the 53%
>    that no balancing produces.
>
> 7. **One mutation survives, and it is equivalent rather than
>    uncaught.** Deleting the "not writable" half of `seg_shareable`
>    changes nothing for the binary under test: its only writable
>    segment also has a zero tail, so the *other* half of the condition
>    already refuses it and the deleted one is never the deciding test.
>    `elf-data-private` asserts the property directly instead -- a store
>    in one process's data segment does not reach another's -- and it
>    was written because that mutation survived, which is what a
>    surviving mutation is for even when it turns out to be equivalent.
>    Making it decisive needs a binary with a writable segment and no
>    `.bss`, which is a synthetic ELF and is not built here.
>
> **Measured, per additional process running `init`:**
>
> | | x86-64 | AArch64 |
> | --- | --- | --- |
> | before this unit | 89 pages | 89 pages |
> | read-only segments shared | 40 | 40 |
> | zero tail demand-paged | **16** | **16** |
>
> against a target of 45. Not done, and deliberately: `memfd`/`shm_open`
> (the inventory row's other half), a private file mapping for writable
> segments (item 4), and the boot-archive door, which has no file and
> still copies.

Constitution §68 report. The inventory's memory row
(`docs/audit/2026-09-deferred-work-inventory.md` §2.2) closed most of
itself with the file-regions and shared-futex units, and left two things
the first of them named as deferred:

> `memfd`/`shm_open` and the ELF loader mapping `PT_LOAD` segments as
> file regions — named as deferred in that report and still open.

This is the second. The machinery it needs was built by that unit:
`VM_REGION_FILE` over the page cache's own frames, shared mappings
coherent by construction, private ones copy-on-write, demand-paged. The
loader has not used it, and still does what it did before there was a
page cache: allocate anonymous memory, populate it eagerly, and copy
every byte of the image into it.

## What is established (before this unit)

- **File-backed regions exist and are proved.** `vm_user_map_file`
  installs a `VM_REGION_FILE` over the page cache's frames: shared with
  `VM_MAP_SHARED`, otherwise copy-on-write, demand-paged, with
  `maxprot` bounding what `mprotect` may later ask for (invariants
  M42–M44, V33; `docs/audit/next-subsystem-file-regions.md`).
- **The filesystem that holds the binaries participates.** `/` is
  `ramfs`, `/boot` holds 75 files from the boot archive, and `ramfs`
  goes through `pagecache_truncate`, so its files have page-cache pages
  to map.
- **The loader already has the file.** `process_spawn` resolves the
  path and holds `struct vnode *exe` before it reads a byte.
- **W^X is enforced at parse time.** `elf_validate` refuses a `PT_LOAD`
  that is both writable and executable, and refuses segments whose
  `vaddr` and `offset` are not congruent modulo the page size — which
  is exactly the precondition a file mapping needs.

## The problem

### Every process carries its own copy of a program that never changes

`elf_load_into` (`kernel/process/elf.c`) maps each `PT_LOAD` as
anonymous memory with `VM_REGION_POPULATED`, walks the region frame by
frame through the direct map, and `memcpy`s the file's bytes in. Two
processes running the same binary hold two complete copies of its text.
Ten hold ten.

It also populates eagerly, so a segment's every page is allocated and
filled at load whether or not the program ever touches it — and the
largest segment of a typical binary here is mostly `.bss`, which has no
file content at all.

### Measured

`tools/elf-share-probe.py` (shipped with this report) spawns copies of
one program one at a time and reports the free-frame count around each,
so the cost of a copy is a subtraction rather than an estimate. On
x86-64, `init`:

```text
ELFSHARE image segments 3 filesz 194762 memsz 311296 (76 pages)
ELFSHARE copy 1 free_before 57424 free_after 57335 cost 89 pages
ELFSHARE copy 2 free_before 57335 free_after 57246 cost 89 pages
ELFSHARE copy 3 free_before 57246 free_after 57157 cost 89 pages
ELFSHARE copy 4 free_before 57157 free_after 57068 cost 89 pages
```

**The fourth copy costs exactly what the first did.** Nothing is shared
between them, and the identical cost is the evidence: 89 pages, 356 KiB,
per process running a program that is the same bytes every time.

Of those 89, the image accounts for 76, and its own program headers say
what they are for:

| segment | flags | filesz | memsz | pages | can it be shared? |
| --- | --- | --- | --- | --- | --- |
| text | `R X` | 143,610 | 143,610 | 36 | **yes**: read-only, executable, no zero tail |
| rodata | `R` | 51,128 | 51,128 | 13 | **yes**: read-only |
| data + bss | `RW` | **24** | 108,968 | 27 | no — but 27 pages are populated for 24 bytes of file |

So **49 of the 89 pages each copy costs are byte-identical read-only
pages of one file**, and 27 more are eagerly allocated for a segment
whose file content is twenty-four bytes.

**AArch64 is the same shape**, which is what says this is the loader and
not an accident of one toolchain: 38 pages of text and 12 of rodata,
both with `filesz == memsz`, and again a writable segment of 27 pages
holding twenty-four bytes of file — 50 shareable of 77.

| | x86-64 | AArch64 |
| --- | --- | --- |
| image pages | 76 | 77 |
| shareable (read-only, no zero tail) | 49 | 50 |
| writable segment's file bytes | 24 | 24 |

## Current implementation

```c
int elf_load_into(struct vm_space *space, const void *image, const struct elf_info *info)
{
    for (each PT_LOAD) {
        vm_user_map_anon(space, s->vaddr, s->memsz, VM_PROT_RW, VM_REGION_POPULATED, "elf-segment");
        /* copy filesz bytes frame by frame through the direct map */
        vm_user_protect(space, s->vaddr, s->memsz, seg_prot(s->flags));
    }
}
```

The interface is the cause: `elf_load_into` takes `const void *image`, a
pointer to bytes, so it cannot map a file even where one exists. Both
doors give it bytes — `process_spawn` reads the file it has already
opened into memory, and `process_create_from_elf` hands it a pointer
into the boot archive.

## Why it matters

1. **It is the largest per-process cost in the system, and it is
   waste.** 49 pages of every 89 are a second copy of bytes that cannot
   change.
2. **It bounds how many processes this kernel can run.** A shell, its
   children and a test suite are all the same handful of binaries; the
   cost is multiplied by processes rather than by programs.
3. **Eager population makes spawn slower than it needs to be.** 76
   pages allocated and filled before the first instruction, when the
   measured file content of the writable segment is 24 bytes.
4. **The machinery is built and unused.** The file-regions unit paid for
   `VM_REGION_FILE`, its COW path and its `maxprot` rule; the loader is
   the caller it was written for and the one it never got.
5. **Constitution §14** lists file-backed mappings and copy-on-write in
   the VMM's *must* list. They exist; the program loader, which is the
   most obvious consumer of both, does not use them.

## Design

### 1. Segments come from the file when there is a file

`elf_load_into` grows a vnode parameter. When it is non-NULL each
`PT_LOAD` becomes a `VM_REGION_FILE` at the segment's file offset;
when it is NULL the loader copies as it does today, and the boot-archive
door keeps working unchanged.

The congruence `elf_validate` already enforces — `vaddr ≡ offset (mod
PAGE_SIZE)` — is what makes this a mapping rather than a rearrangement.

### 2. Read-only segments are shared; writable ones are private

| segment | mapping | why |
| --- | --- | --- |
| `R`, `R X` | `VM_MAP_SHARED`, `maxprot` without `W` | the frames are the page cache's, one set for every process; `maxprot` is what stops a later `mprotect` from turning shared text writable |
| `RW` | private (copy-on-write) | a write must not reach the file or another process |

The `maxprot` rule is the whole safety argument for sharing text, and it
is already built and tested.

### 3. The zero tail, which is where this goes wrong if it is rushed

`memsz > filesz` means the segment ends in zeroes that are not in the
file. Two cases, and only one is a problem:

- **A private segment** (`RW`): map the file part copy-on-write, map the
  rest anonymous, and zero the tail of the last file page *in the
  private copy*. Writing there faults in a private page first, so
  nothing reaches the file.
- **A shared segment** (`R`, `R X`): there must be no tail to zero.
  This is not an assumption — `elf_validate` can require
  `filesz == memsz` for any segment the loader intends to share, and
  refuse to share one that has a tail (falling back to a copy for it).
  The measured binary satisfies it: text and rodata both have
  `filesz == memsz`.

### 4. A running program's file can now change underneath it

This is a **behaviour change and the unit's real risk**. Today a process
holds a copy, so writing to `/bin/sh` cannot affect a running shell.
With shared text it can: the pages are the file's.

The report proposes the interlock rather than the silence: a write to a
file some process is executing is refused with `-ETXTBSY`, as POSIX
describes and as Linux does. Without it this unit makes a program's
instructions mutable by anyone who can write its file.

**And it must not be a counter**, because a counter beside a lifetime
goes stale. The tree already keeps what is
needed: every file mapping is a `struct vm_file_map` holding a vnode
reference, linked onto a per-vnode list under `pagecache_lock(vn)` and
unlinked under the same lock before `vnode_put`
(`kernel/memory/vmm.c`). So the rule is a property of that list, and
its three parts are:

- **Registration** happens where `m` is linked, under
  `pagecache_lock(vn)`, and marks the mapping as text — shared, and
  executable in its `maxprot`.
- **The check** happens under the same lock. The write path already
  holds `vn->lock`, and the documented order is
  `vnode -> pagecache -> vm_space`, so a writer may take
  `pagecache_lock(vn)` to ask. Check and write are then atomic with
  respect to a mapping being created, which is the race a separate
  counter would lose.
- **Release** happens where `m` is unlinked, under that lock and
  *before* `vnode_put` — which is already the order the teardown uses.
  It matters: a mapping that has been unlinked can no longer fault a
  page in, so the list going empty there is the truth rather than an
  optimistic guess, and a release that lagged the teardown would leave
  a file permanently busy.

Stated as an invariant for the implementation to carry: **a file is
busy exactly while a text mapping of it is on its page-cache list**, and
both the answer and the write are taken under that list's lock.

### 5. Demand paging comes for free, and is measured separately

Nothing is populated. The first instruction faults its page in. The
report expects this to dominate the spawn-latency benchmark and says so
in advance, so that a win there is not read as a win from sharing.

### 6. What it does not do

- **No `memfd`/`shm_open`.** The other half of the inventory row; a
  separate unit.
- **No dynamic linking, no shared libraries.** `PT_INTERP` is parsed and
  the interpreter is a program like any other; nothing here changes how
  it is found or run.
- **No page-cache eviction policy change.** Mapped pages are already
  pinned by the file-regions unit's rules.
- **No ASLR.** Placement is untouched.
- **No change to the boot-archive door**, which has no vnode and keeps
  copying. Whether `/boot`'s ramfs files could serve it instead is a
  question this report deliberately leaves open: it is a second design,
  and the measurement above does not need it.

## Affected files

| file | change |
| --- | --- |
| `kernel/process/elf.c` | `elf_load_into` takes a vnode; file-backed segments, the private zero tail, the copy path kept for a NULL vnode |
| `kernel/include/kernel/elf.h` | the signature, and what a shared segment must satisfy |
| `kernel/process/process.c`, `kernel/process/spawn.c` | pass the `exe` vnode the spawn path already holds |
| `kernel/memory/vmm.c` | mark a text mapping where `m` is linked, clear it where it is unlinked, both under `pagecache_lock(vn)` |
| `kernel-services/vfs/*` | `vnode_text_busy`, and the `-ETXTBSY` check on the write path |
| `kernel/process/proctest.c` | the tests below |
| `docs/kernel/process/design.md`, `invariants.md` | how a program is loaded now, and the two rules that keep it safe |
| `docs/kernel/memory/design.md` | the loader as a `VM_REGION_FILE` caller |
| `docs/audit/2026-09-deferred-work-inventory.md` | §2.2's remaining half struck through |
| `README.md` | Status entry |
| `tools/elf-share-probe.py` | shipped with this report; the unit's own benchmark replaces it |

## APIs

**One changed, one new.** `elf_load_into` exists
(`kernel/include/kernel/elf.h`) and this unit widens its signature; it
is listed here because callers must change, not because the function is
new.

### Changed: `elf_load_into` gains a vnode

```c
/* Map every segment of a validated image into `space`. With `vn`, the
 * segments come from that file's page cache -- read-only ones shared,
 * writable ones copy-on-write -- and nothing is populated. With NULL,
 * the image's bytes are copied as before (the boot archive, which has
 * no file). */
int elf_load_into(struct vm_space *space, const void *image,
                  const struct elf_info *info, struct vnode *vn);
```

### New: asking whether a file is being executed

```c
/* Whether any process is executing this file: true while a shared,
 * executable mapping of it is on its page-cache mapping list. Called
 * with pagecache_lock(vn) held, by a writer that already holds
 * vn->lock. A write to a busy file is -ETXTBSY. */
bool vnode_text_busy(struct vnode *vn);
```

Nothing else is added. `struct vm_space`, `struct vnode` and
`VM_REGION_FILE` are all existing types this unit only uses.

## Migration plan

1. **`elf_load_into`'s vnode parameter, NULL at every call site.** No
   behaviour change; the copy path is what runs. Boot both architectures.
2. **Shared read-only segments**, with the `filesz == memsz` requirement
   and the fallback. The probe's numbers must drop for copies 2 and
   after, and the frame-identity test below is what proves it is
   sharing rather than merely costing less.
3. **Private writable segments and the zero tail**, with the COW test.
4. **`-ETXTBSY`**, with its test, before anything ships: shared text
   without it is a mutable-instructions bug wearing a memory
   improvement's clothes.
5. **Demand paging** (drop `VM_REGION_POPULATED`), measured separately.
6. **Docs, inventory, README, banner; release builds, `gmake host-test`,
   every mutation alone.**

## Tests

| test | what it proves | bug-proof |
| --- | --- | --- |
| `elf-shared-text` | two processes running one binary map the **same frames** for its text: `arch_mmu_query` on both spaces returns one physical address | revert to the anonymous copy: the addresses differ |
| `elf-text-cost` | the second copy costs measurably fewer pages than the first, by the free-frame count, and the difference is the read-only segments' size | as above |
| `elf-data-cow` | a write to the data segment in one process is not seen by the other, and does not reach the file | drop `VM_MAP_SHARED`'s absence — map the writable segment shared: the other process sees the write |
| `elf-zero-tail` | the bytes between `filesz` and `memsz` read as zero in every process, including the partial last page | skip the tail zeroing: the test reads the file's next bytes |
| `elf-text-ro` | `mprotect(PROT_WRITE)` on shared text fails (`maxprot`) | widen `maxprot`: it succeeds, and one process can then rewrite another's instructions |
| `elf-txtbsy` | writing a running program's file fails with `-ETXTBSY`, and succeeds once it exits -- the second half being what proves the mapping list is consulted rather than a flag that never clears | make `vnode_text_busy` return false: the write succeeds and the running process's text changes underneath it |

## Benchmarks

| claim | before (x86-64, `init`) | target |
| --- | --- | --- |
| pages per additional copy | 89 | ≤ 45, the read-only 49 removed |
| pages populated at load | 76 | the touched set only, reported |
| spawn latency | eager copy of 76 pages | reported before and after, and attributed to demand paging rather than to sharing |

The same rows on AArch64. The probe's own output is the before; the
unit's benchmark is the after, in the same boot where possible.

## Risks

- **Shared text makes a program's instructions the file's.** Without
  `-ETXTBSY` a write to `/bin/sh` rewrites a running shell. The plan
  puts the interlock before the sharing ships, and the test for it is
  not optional.
- **`maxprot` is the only thing standing between shared text and a
  writable shared mapping.** It is built and tested, and this unit adds
  a test of its own rather than trusting that.
- **A segment with a zero tail must not be shared.** The check is
  `filesz == memsz` and the fallback is the copy path, so a binary that
  does not satisfy it loads exactly as it does today.
- **Demand paging changes when faults happen**, including inside the
  first instruction fetch. The tests that measure spawn latency will
  move; the report expects that and asks for the attribution rather than
  the number alone.
- **The page cache now holds pages that processes are executing.**
  Eviction must not take a mapped page, which the file-regions unit
  already settled; this unit adds the loader as a second caller of the
  same rule and should confirm it rather than assume it.
- **The boot-archive door keeps copying**, so two loaders exist. That is
  deliberate and named; the risk is that they drift, which the tests
  above mitigate by running against the VFS door that real programs use.

## Alternatives considered

- **Share text by deduplicating identical anonymous pages**, as KSM
  does. It would find these pages, and it would spend a scanner's time
  finding what the filesystem already knows. The file *is* the identity.
- **Copy, but lazily.** A private file mapping without the shared case
  would get demand paging and keep the duplication: 49 of the 89 pages
  would still be per-process. Half the work for none of the sharing.
- **Map the boot archive's bytes directly**, skipping the VFS. It would
  make the archive door share too, and it would make the loader depend
  on a memory layout rather than a file. The ramfs at `/boot` already
  exposes those bytes as files, which is the same idea with a vnode.
- **Leave it and build `memfd` first.** `memfd` adds a capability;
  this removes a cost every process already pays. The measurement is the
  argument: 49 pages per process, on a machine whose free-frame count
  the probe prints in five digits.
- **Refuse to write a running program's file by locking it at spawn**,
  rather than deriving it from the mappings. A lock held for a process's
  lifetime is a different object with a different failure mode (what
  releases it if the process is killed?); the mapping list is torn down
  by the same path that tears down the address space, so it cannot
  outlive what it describes.
- **A counter on the vnode, incremented by the loader.** It was the
  report's first proposal and it is wrong: a counter beside a lifetime
  drifts from it. A writer can race its increment, and a decrement that
  does not follow the region's teardown leaves the file busy forever.
  The mapping list already has the lifetime and the lock; the answer
  should be read from it (found in review of this report).
