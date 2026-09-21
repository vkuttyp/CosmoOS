# NEXT SUBSYSTEM — file-backed regions: the mappings the constitution requires

Constitution §68: after the audit, name the next subsystem in this shape
and wait for the instruction to build it. **This report is as built**
(the file-regions unit), and the banner below records where the build
differed from the design; the sections after it are the design as
reviewed, kept as the record of what was argued before the code
existed.

**What the build changed:**

0. **The cache half of the fault is three calls, not one.** The design
   named `pagecache_map_page`; as built it is `pagecache_lock` (the
   reclaim every entry to a cache runs, then the mutex),
   `pagecache_fault_page` (the bound, `get()`, the dirty mark, the
   reference) and `pagecache_unlock`, so the caller's install sits
   between two calls it makes itself rather than inside one that returns
   holding a lock. The copy-on-write copy is made by the fault handler
   from the cache frame under the mutex, and the cache frame's reference
   is put again before the space lock is taken: a private copy never
   references the cache.
1. **A private write finding the cache frame present replaces the PTE;
   a shared write raises it; anything else present drops what phase two
   made.** Exactly as review corrected the report (round one), with one
   more case the build met: memory for a private copy running out is the
   anonymous rule (`-ENOMEM` -> `SIGSEGV` on a user touch, `-EFAULT` in a
   copy), while a page the *file* cannot supply is `SIGBUS`. The report
   had folded both into `SIGBUS`; the fault distinguishes them.
2. **The seam is armed and released in the kernel, never written from
   user space.** `sysctl` is read-only in this ABI, so `debug.file_fault_hold`
   reports the state (0 idle, 1 armed, 2 held) and the kernel self-test
   arms it (`vm_test_file_hold_arm`) before spawning the child; the hold
   is released by the next FILE install in that space or by any unmap or
   replacement of part of it, which is exactly the two events the proofs
   need and needs no write. Event-driven, as designed. The child spins
   on the state reading 2 before it acts.
3. **Three held-fault variants, not two.** Review of the report's first
   draft asked for the re-find to be proved for the stated reason, and
   "unmap under a held fault" cannot show it -- with the region gone
   there is nothing to install into, correct code or not. The third
   variant replaces the range with a `MAP_FIXED` mapping of *another
   file* while the fault is held: the re-find sees a different vnode,
   installs nothing, and the retry reads the other file's byte. A fault
   trusting its first phase would install the first file's page under
   the second file's name, and that is what the "no re-find" mutation
   did (below). `vm_user_map_file`'s replacement path releases the hold
   for it.
4. **The injected read failure fires on any miss, and the kernel test
   makes the file.** `FI_FILE_READPAGE` (`file-readpage`) is checked
   before the "inside the file, and a readpage exists" test, so a hole
   on ramfs -- which has no `readpage` -- fails too and the proof needs
   no disk. The child could not make the file itself: its own write to
   create the hole was the first miss and spent the injected failure,
   which is how the test failed on its first run.
5. **`vm.cache_writebacks` and the counters exist as sysctls**, as the
   review-revised report said; `pinned_skips` was added to
   `pagecache_stats` for `pagecache-pinned`, which the report had not
   listed. The bench maps 2 MiB, not 4: the section runs inside the
   per-suite budget and a larger file measured nothing different.
6. **The harness has a list of the user-mode sections and its own
   unit test hard-coded ten**; the section joined the list and the test
   now derives its counts and names from it. Reading the section list
   from `init.c` would have been the other answer and was not taken:
   the list in the harness is what makes a row that compiles to nothing
   visible.
7. **The numbers.** `SYS_msync` 96, `SYS_COUNT` 97; `COSMO_MAP_SHARED`
   `1 << 3`, `COSMO_MAP_PRIVATE` `1 << 4`; invariants **M42** (a shared
   page's PTE gains write only in the fault handler), **M43** (a frame
   is referenced per mapping and freed on the last put), **M44** (a FILE
   fault installs only what the region it re-finds maps, below the bound),
   **V33** (the cache tells every mapping before it frees or cleans a
   page); 366 self-tests on both architectures; the user-mode suite's
   `mmap` section 264 ms on x86-64 and 300 ms on AArch64. The
   `arch_mmu_protect` "not called by any current code path" gap in the
   memory testing document was false before this unit (`vm_user_protect`
   calls it) and is struck.
8. **The bench, as run** (`USERBENCH: mmap 2048 KiB`, QEMU TCG, one
   boot each): x86-64 `read()` of the cached file 15644 us, first touch
   through a shared mapping 6991 us (13654 ns/page), the dirtying write
   of every page 7434 us (14520 ns/page); AArch64 10523 us, 6118 us
   (11949 ns/page), 10384 us (20281 ns/page). The first touch of a page
   costs about what reading it through `read()` costs per 64 KiB
   request divided by sixteen -- the fault is not cheaper than the copy
   under TCG, where a trap is expensive, and the report expected nothing
   else. The demand-zero fault path gained one `kind` test before its
   arm and no measurable cost: the suite's `process-user` line is within
   its run-to-run spread (4107 / 4149 / 4202 ms across three boots
   against 3688 ms before the section existed, the difference being the
   section).
9. **What review found on the report, kept here because the build
   followed it:** the private-write PTE rule (item 1), the two-pass
   `msync` (built exactly as the revised design says, with the cursor
   resolving the region that *contains* the start address first), and
   the `SYS_COUNT` wording.

**Bug-proofs, as run.** Each mutation applied alone on x86-64, the debug
suite booted, the file restored from HEAD before the next; the eight the
report named and a ninth for what self-review found.

| mutation | what failed |
| --- | --- |
| `pagecache_sync` not lowering the PTEs | the `mmap` section: `vm.file_dirty_faults == df0 + 2` (the second write did not fault) and `vm.cache_writebacks == wb1 + 1` (the third `MS_SYNC` wrote nothing) |
| the fault marking nothing dirty (`pagecache_fault_page(..., false, ...)`) | the same section, a different check first: `wb1 == wb0 + 1` (the first `MS_SYNC` wrote nothing) -- then the two above. The counter tells the two mutations apart, as the report said it would |
| no `pmm_page_get` at install | `pagecache-pinned` (`held->refcount == 2` false), then **the poisoner**: the section's first `munmap` put the cache's only reference and freed the frame under the cache, and `KERNEL PANIC: pmm: use after free of pfn 58547 ... 8 byte(s) at offset 96-104 (poison 5a)` with the dump showing `a5` at offset 100 -- the section's own `sh[100] = 0xA5`, written into a frame the cache still believed it held |
| `pagecache_truncate` freeing without unmapping | `KERNEL PANIC: pmm: freeing pfn 48151 with refcount 2` from `remove_entry`, on the truncate child: the mapping's reference was still on the frame, exactly the count check M43 leans on; the suite stopped there |
| no bound (`vn->size` alone) | **nothing**, as declared in advance: `boot-test: PASS`. The window is between a filesystem's trim and its size drop and no seam sits there |
| phase three by kind alone (no vnode, index or sharing) | `vm-file-fault-hold`: the remap variant's `status == 0` -- the held fault installed the first file's page under the second file's name and the child read the wrong byte. The race and unmap variants passed the mutation, which is why the remap variant exists |
| `maxprot` ignored | the `mmap` section's `mprotect(rs, READ|WRITE) == -EACCES`, and `lxtest`'s `LX_mprotect(ros, ...) == -13` |
| `SHARED\|ANONYMOUS` accepted | the `mmap` section's `cosmo_mmap(..., ANONYMOUS \| SHARED) == -EINVAL` |
| `vm_user_protect` giving a private region's cache frames the asked protection (the self-review finding, item 10) | the `mmap` section: `vm.file_cow_faults == cowp + 1` (no copy was made), `file_rd(fd, 0, ...) == first` (**the file changed**: the write went through the cache frame) and `sh[0] == first` (the shared mapping saw the private mapping's write) |

11. **Review of the build found three more, all fixed with a check
    each.** The copy-on-write path that *replaces* a present cache frame
    did not check `COSMO_RLIMIT_MEM` while the not-present copy did, so
    a process could read every page of a private mapping and then write
    them all past its limit (now checked; `mmap-mem-limit`, a child
    ending in 139). A `write()` into a page some mapping executes from
    changed instructions with no instruction-cache maintenance, and the
    writer need not be the executing process, so the fault-time sync of
    M41 was not enough: the cache now asks each mapping record whether
    its region over the written page is executable and synchronises by
    the frame's direct-map alias (`arch_mmu_sync_icache_kernel`, new on
    both architectures; `vm.cache_exec_syncs`; a regression check, since
    TCG cannot show coherence). And the Linux door decided "shared" on
    the `MAP_SHARED` bit alone, accepting no type and `SHARED|PRIVATE`:
    the low four bits are now validated as Linux does -- 1, 2, or 3
    (`MAP_SHARED_VALIDATE`, shared), anonymous or not, else `-EINVAL`
    (`lxtest`). A fourth finding was wording in the inventory row.
10. **Self-review found a defect the report did not name, and the build
    fixed it before the tests could.** `vm_user_protect` applied
    `prot & ~WRITE` to a *shared* FILE region and `prot` to everything
    else -- so a private region's page installed read-only for a read
    and then `mprotect`ed writable had its cache frame's PTE raised, and
    the next write went through to the file. The rule as built is per
    frame, not per region: a cache frame's PTE never gains write from
    `mprotect`, shared or private, and only a copy-on-write copy takes
    the protection as asked. The `mmap` section proves it (a private
    read, `mprotect(RW)`, a write: `vm.file_cow_faults` +1, the file
    untouched), and the mutation that restores the old rule fails that
    check: the three lines in the last row above -- no copy, the file changed, the shared mapping saw it.

**The VMM has two kinds of region and the constitution requires four
things it cannot do with them.** §14 of the constitution lists what the
VMM *must* support — not "eventually", that list is separate and is
huge pages, deduplication, NUMA and compression — and four entries of
the must-list are `file-backed mappings`, `shared mappings`,
`copy-on-write` and `demand paging` (`prompts/MASTER PROMPT…md:579-581`,
`:66-67`). The tree has demand paging for zero pages and nothing else:
`enum vm_region_kind` is `VM_REGION_PHYS` and `VM_REGION_ANON`
(`kernel/include/kernel/vmm.h:25-28`), the fault handler has one
populating arm and it is the demand-zero one (`kernel/memory/vmm.c:571`),
and the memory design says of that arm that it "is the one that will
later grow anonymous private memory and CoW; its structure … is the
skeleton for that" (`docs/kernel/memory/design.md:338-340`). That was
written in Phase 4 and nothing has grown from it.

**Both doors say so, in opposite ways.** The native `mmap` refuses every
mapping that is not anonymous with a comment that has outlived the thing
it waited for: `return -EINVAL; /* file mappings arrive with the VFS */`
(`kernel/syscall/native.c:364`) — the VFS, the page cache, `cosmofs` and
the writable root all arrived, and the comment did not notice. libc has
`MAP_PRIVATE 0` and no `MAP_SHARED` at all (`libc/include/sys/mman.h:12`).
The Linux personality maps a file by allocating an anonymous region and
copying the bytes into it through a bounce buffer (`fill_from_file`,
`compat/linux/syscalls.c:887`, called at `:989`), then refuses
`MAP_SHARED | PROT_WRITE` with `-EOPNOTSUPP` because "a writable shared
mapping would need page-cache-backed regions this kernel does not have"
(`:936-943`). That refusal is honest; what sits beside it is not. A
**read-only** `MAP_SHARED` mapping is accepted and is the same eager copy
(`docs/compat/linux/design.md:351-360`, "which is the same thing for a
file nobody else writes") — so a program that maps a file `MAP_SHARED`
and then watches it for another process's `write()` watches a snapshot
forever. POSIX says a shared mapping and `write()` see one file; this one
does not, and nothing tells the program.

**Takes up** the inventory's §2.2 row "shared mappings: `MAP_SHARED` is
still private; no shared-memory primitive of any kind (which also rules
out shared futexes across processes)"
(`docs/audit/2026-09-deferred-work-inventory.md:114-116`) — and corrects
it: natively `MAP_SHARED` is not "private", it is unreachable, and the
kernel does have the half of the mechanism that row implies is missing
(the page cache, below). Also the VFS design's "Future extensibility"
line "`mmap` of files (the page cache already owns frames)"
(`docs/kernel-services/vfs/design.md:666-667`), which is the whole
argument in six words, and the audit's `mmap` row "MAP_SHARED ≡ PRIVATE"
(`docs/audit/2026-09-post-roadmap-audit.md:601`). This is a §6 pick from
section 2 taken as a correctness gap: a flag accepted with the wrong
meaning at one door, and a required capability absent at both. It is the
third unit in the memory arc — after `MAP_FIXED` replacing (PR #193) and
`mprotect` (PR #195) made the native door say what POSIX says, this makes
it say what §14 says.

## What is established

**The page cache owns frames, tracks dirtiness and writes back.** Each
regular vnode carries a `struct pagecache` (`kernel/include/kernel/vfs.h:112`):
a hash of `pc_entry { index, struct page *page, dirty, on_lru }` under a
mutex (`kernel/include/kernel/pagecache.h:21-45`). A miss reserves the
mount's page budget, allocates a zeroed frame, calls the filesystem's
`readpage` for an index inside the file's size — **and inserts a zero
page for an index beyond it** (`kernel-services/vfs/pagecache.c:172-230`;
the size test is at `:214`), which is right for `write()` extending a
file and is the one fact below that a mapping must not inherit. Writes
mark entries dirty and take them off the global LRU; `pagecache_sync`
writes dirty pages in ascending order through `writepage`/`writepages`
and marks them clean (`:321-395`); `pagecache_truncate` drops the
entries past the new size and zeroes the tail (`:414-434`);
`pagecache_reclaim` evicts clean LRU-tail pages of mounts without
`MOUNT_CACHE_IS_STORE`, taking the owning cache's mutex by trylock and
re-validating the entry under it (`:82-137`). `read()` and `write()` go
through this cache (`pagecache_read`/`pagecache_write`), so **the frame
the cache holds is already the one place a file's bytes live in memory**;
a mapping that installs that frame is coherent with `read()` and
`write()` by construction, with nothing to synchronise.

**Frames are reference counted, and the last put frees.** `struct page`
has an atomic `refcount` (`kernel/include/kernel/page.h:30`);
`pmm_page_get` panics on a free frame and `pmm_page_put` frees on the
last reference with one atomic decrement — "the vnode_put race of #49 in
another coat" (`kernel/memory/pmm.c:339-359`). Nothing in the VMM uses
either: anonymous frames are owned by exactly one mapping and freed with
`pmm_free_page` at teardown (`vmm.c:732-757`, `user_range_teardown`:
frames collected under the space lock, the range shot down with it
released, the frames freed last). A cache frame mapped into `n` spaces
needs the count and has it.

**The fault handler is allowed to sleep, and today never does.** Every
user copy asserts `might_sleep()` — "a demand fault allocates: never
under a spinlock" (`kernel/syscall/uaccess.c:45,55,65`) — so the rule
that a demand fault may block is already the tree's, checked by lockdep
on every copy. The handler itself takes `space->lock` (a spinlock), finds
the region and, for ANON, allocates and maps under it (`vmm.c:544-600`);
a fault taken with that lock held is a panic (`:541-542`). It has one
non-installing arm already: a `VM_REGION_QUIESCED` region makes a user
fault return without installing so the instruction retries (`:557-567`),
which is the shape a fault that has to drop the lock and come back
needs, and it exists.

**Regions split, and the pieces must carry what the whole carried.**
`region_split` copies `prot`, `cache`, `kind`, `flags`, `phys`, `name`
(`vmm.c:869-884`); `vm_user_protect` and `vm_user_unmap` split to the
page (`:1251`, `:996`); the replacement path claims a range with
`VM_REGION_QUIESCED` and tears it down with the range owned throughout
(`:1104`, invariant M40). Regions are freed on four paths — `vm_user_unmap`,
the replacement, `vm_space_destroy`, and the merge helpers that
`vm_user_protect` and `vm_user_unmap` call — each collecting `freed[]`
under the lock and releasing after. **The space
already refuses to die with frames unaccounted**: `vm_space_destroy`
panics if `anon_pages` is not zero after the teardown (`:793`). That is a
deterministic leak detector for anonymous frames, and it costs one more
counter to cover file frames.

**Signals, rights and limits are in place.** `SIGBUS` exists and is in
the synchronous set the delivery path handles at once (`kernel/process/signal.c:416`);
the fatal hook builds a `SIGSEGV` with `fault_addr` and Linux's
MAPERR/ACCERR code (`kernel/process/process.c:120-134`) and takes no
signal parameter. `file_of(fd, HANDLE_RIGHT_READ)` is how the Linux door
resolves the fd (`syscalls.c:946`), `HANDLE_RIGHT_WRITE` exists beside it
(`kernel/include/kernel/handle.h:25-26`), and `struct file` keeps its
`O_*` flags (`vfs.h:206`). The per-space `limit_mapped_pages` is charged
at region creation (`vmm.c:903`, `COSMO_RLIMIT_AS`) whatever backs the
region, and the mount's page budget bounds what one filesystem can hold
in the cache (`pagecache.c:184-196`).

**Two things the built tree does that this unit must know.** First:
**both filesystems trim the cache before they lower the size** —
`ramfs_truncate` calls `pagecache_truncate(vn, size)` and then sets
`vn->size` (`kernel-services/vfs/ramfs.c:304-305`); `cfs_truncate` trims
first too (`kernel-services/filesystem/cosmofs/cosmofs.c:1984`). A
`read()` never cared, because it is bounded by whatever the size is when
it runs and the trim is idempotent. A fault that reads the size to decide
between installing a page and `SIGBUS` does care: in the window between
the trim and the size drop it would see the old size and install a page
past the new end, which nothing then unmaps. Second: **the futex is keyed
by `(space, uaddr)`** (`kernel/ipc/futex.c:44-46`, `bucket_of`), so two
processes sharing a page through this unit do not yet share a futex on
it. Named below as deferred, not solved here.

## The problem

### The required list has three holes, and one is a wrong answer

File-backed mappings, shared mappings and copy-on-write are absent, and
demand paging exists for one kind of memory. The audit of Prompt #2 §8
asked whether the abstractions "can support them without breaking the
ABI" and the answer was yes — the region has a `kind`, the fault handler
switches on it, the page cache owns frames. Supporting is not having. The
one door that accepts `MAP_SHARED` gives a program a private copy under
that name, which is the class of defect the termios unit refused to ship
("omitted rather than accepted and ignored", inventory §1.4) and the
hardening unit made a rule for native flags (`native.c:355-357`).

### Every mapping of a file costs the file's size, up front, twice

The Linux door's copy is eager and complete: a 6000-byte file mapped over
8 KiB is read whole at `mmap` time and lives twice, once in the cache
and once in the region. A dynamic linker maps its libraries this way
(`docs/compat/linux/design.md:345-360`). The cost is proportional to the
file and paid before the first instruction, where a demand-paged mapping
pays per page touched and shares the frame with the cache. No benchmark
measures this today, because no alternative exists to measure against.

### The primitive everything else waits on

"No shared-memory primitive of any kind" is the row's own phrase. There
is no `fork`, so anonymous memory cannot be inherited, and a shared
anonymous mapping has nothing to share with; the only object two
processes can both name is a file. A shared file mapping is therefore
**the** shared-memory primitive for this system — `shm_open` is a file on
a memory filesystem, `memfd` is an unnamed one — and a shared futex
across processes, when it comes, is a futex whose key is the frame
rather than the space. Neither can start before this does.

## Design

### A third kind of region, and what the vnode knows about it

`VM_REGION_FILE`. A region of this kind points at a **mapping record**,
`struct vm_file_map { struct vnode *vn; struct vm_space *space;
vaddr_t base; size_t size; uint64_t off; bool shared; vm_prot_t maxprot;
unsigned regions; struct list_node link; }`, created by `mmap` and
linked on the vnode's cache (`pagecache.mappings`, under the cache
mutex). The record holds a reference to the vnode for as long as any
region points at it; `regions` counts those, changed only under the
space lock — `region_split` copies the pointer and increments, every
site that frees a region decrements, and the site that takes it to zero
unlinks the record from the vnode **after** releasing the space lock
(the cache mutex is never taken under it) and drops the vnode reference.
The record is what the vnode walks when it needs to reach every mapping
of a file: it describes the range as first mapped, and a piece of it that
was since unmapped or reprotected is simply a range whose PTEs are absent
or different, which every operation below tolerates. Regions of this kind
never merge: two mappings are two objects, and nothing measured wants
them joined.

`shared` distinguishes a shared mapping from a private one; `maxprot` is
the most a later `mprotect` may grant — `R|X` for a shared mapping of a
file opened read-only, `R|W|X` (less W^X) otherwise — and is `RWX` for
every anonymous region, so `vm_user_protect` gains one check and no
special case.

### The fault, in two phases under one lock order

The order is **`vnode.lock → pagecache.lock → vm_space.lock`**. The
first arrow is the existing rule (`pagecache.h:7-8`); the second is new
and is the whole concurrency design: **the install happens under the
cache lock**, so a truncate or a write-back, which also hold it, are
serialised against faults on the same file without a third mechanism.

1. Under `space->lock`: find the region. If it is FILE, not
   `VM_REGION_QUIESCED`, and the access is within `prot`: take a vnode
   reference, note `(vn, index = (addr − base)/PAGE + off/PAGE, shared,
   write)`, release the lock.
2. `might_sleep()`. Take `pc->lock` (after `reclaim_if_needed`, as every
   other entry to a cache does). Compare the index with **the bound**
   (below); past it, release, drop the reference, and end the access
   with `SIGBUS`. Otherwise `get()` the entry — a hit, or a miss that
   reads the page in with the mutex held, as `read()` does today. Then:
   - **shared, write**: `mark_dirty`, install with the region's `prot`;
   - **shared, read** (or a write the region forbids): install with
     `prot & ~WRITE`, so the first write faults and dirties it;
   - **private, read**: install the cache frame with `prot & ~WRITE`;
   - **private, write**: allocate an anonymous frame, copy the cache
     page into it, install the copy with `prot`: copy-on-write. The
     copy is what the region owns from then on; the cache frame is not
     referenced.
   A cache frame that is installed gets `pmm_page_get` here, under the
   mutex.
3. Still under `pc->lock`, take `space->lock` again and **re-find**. The
   region at the address must be FILE, the same vnode, the same file
   index for this address, and the same sharing; anything else — the
   range was unmapped, replaced, or remapped to something else in the
   gap — drops the reference taken in step 2 and returns without
   installing, and the instruction retries against whatever is there
   now, exactly as the quiesced arm does. If the PTE is already
   present, what happens depends on what is present and why, and only
   one of the three cases raises a PTE in place:
   - **shared, write, the cache frame present read-only**: the page
     was installed clean and is dirty now (step 2 marked it); the PTE
     is raised to writable. This is the one in-place upgrade, and it is
     what "a shared page's PTE gains write only in the fault handler"
     means.
   - **private, write, the cache frame present read-only**
     (`PG_PAGECACHE` on the frame `arch_mmu_query` returns): the
     private read that installed it must not become a write through
     it. The PTE is **replaced**: unmap it, drop that mapping's
     reference to the cache frame (`pmm_page_put`, `file_pages--`),
     map the copy made in step 2 (`anon_pages++`), shoot down. Raising
     the PTE would write the file through a private mapping.
   - **anything else present** — a second thread installed the same
     page first, or a private copy already stands where this write
     lands: nothing is mapped twice. The reference or the copy from
     step 2 is dropped, and the instruction retries against a PTE that
     already permits it (a private copy is always mapped writable).
   Otherwise `arch_mmu_map`; `file_pages++` (or `anon_pages++` for a
   copy); for an executable mapping, the instruction-stream sync M41
   requires. Release both.

Faults on different files run in parallel; faults on one file are
serialised by its cache mutex, which is what `read()` on one file already
is. The identity check in step 3 is by `(vnode, index, shared)`, not by
region pointer, because region structures come from a cache and a pointer
can be reused: if the range was unmapped and remapped to the same file at
the same offset with the same sharing, installing the page is correct
for the new mapping too.

### The bound the cache owns

The index test in step 2 cannot be `vn->size` alone, because of the
trim-then-shrink order both filesystems have. The cache keeps
`trim_bound`, `UINT64_MAX` until `pagecache_truncate(vn, size)` sets it
to `size` under the mutex, and lifted back to `UINT64_MAX` by
`pagecache_write` when it grows the file. The fault installs only below
`min(vn->size, trim_bound)`. The bound masks exactly the window between a
trim and the size drop that follows it and nothing else: after the size
is lowered, `vn->size` governs; after a growth, the bound is gone.
`vn->size` is read as an aligned word without the vnode lock, which both
architectures do atomically. A page whose index is inside the bound but
whose tail is past the size is installed with the tail the cache already
zeroes — the same bytes `read()` would return.

### Truncate unmaps before it frees

`pagecache_truncate`, under the mutex and before `remove_entry`, walks
`pc->mappings`: for each record whose range reaches the dropped indices,
take that space's lock, unmap the corresponding pages in every region
that points at the record (`arch_mmu_query` for the frame, `arch_mmu_unmap`
for the PTE, `file_pages`/`anon_pages` down by what each frame was),
release, shoot down, and `pmm_page_put` each frame collected — the
existing teardown shape, reused, with the frame's kind read from a new
page flag `PG_PAGECACHE` set on a cache frame at `get()` and cleared when
the cache frees it. A copy-on-write frame in the dropped range goes too:
the file no longer has those bytes and POSIX says a reference past the
end is `SIGBUS`, private or shared. Only then does `remove_entry` free the
cache's own reference, which is now the last. Nothing can fault a page
back in the meantime: a fault needs the mutex the truncate holds.

### Reclaim skips a mapped frame

`entry_reclaimable` gains `page->refcount == 1`: under the owning cache's
mutex, which reclaim holds when it decides, a count of one means no
mapping holds the frame and none can be added. A mapped clean page stays
resident; the limit is soft already ("a soft cap until writeback exists",
`pagecache.c:145`), and evicting a mapped page needs the reverse mapping
this unit deliberately keeps at record granularity — named as deferred.

### A shared page is dirty when a write fault said so

Hardware dirty bits are not used: AArch64's are optional (DBM is
ARMv8.1 and TCG's handling is not what a proof should rest on), and a
software rule is the same on both architectures and observable by a
test. The rule: **a shared file page's PTE gains write only in the fault
handler** (a new invariant). It is installed read-only when clean; the
write fault marks the entry dirty and raises the PTE; `pagecache_sync`,
after a run of pages is written and before it marks them clean, walks
`pc->mappings` and lowers every present PTE of those pages to
`prot & ~WRITE` in every region that points at each record (under that
space's lock; one shootdown per run per space), so the next write faults
again and the page is dirty again. `vm_user_protect` on a shared FILE
region applies `prot & ~WRITE` to the PTEs whatever `prot` says, and
records `prot` in the region: raising to writable is the fault's job and
the fault will find the entry already dirty or not. `write()` to a page
that is mapped clean needs no PTE change: the frame's bytes change, the
mapping sees them, the entry is dirty by `pagecache_write`'s own
`mark_dirty`. A private mapping needs none of this: a copy is anonymous
memory and the cache frame under a private read mapping is never
written through it.

### `msync`, at both doors

`SYS_msync` 96 `(addr, len, flags)` (`SYS_COUNT` → 97), and `LX_msync`
26 over the same kernel function, `vm_user_msync`. It cannot call
`pagecache_sync` from under the space lock — the lock order is
`vnode.lock → pagecache.lock → vm_space.lock`, `pagecache_sync` sleeps
under the cache mutex and is entered under the vnode lock as `file_sync`
enters it — so it is **two passes**. First, under `space->lock`: check
the range is wholly mapped (`-ENOMEM` otherwise, before anything is
written), and release. Then a cursor walk, the cursor starting at
`addr`: under `space->lock`, find the FILE region that **contains** the
cursor (`space_find(cursor)`, which is how an address in the middle of
a region is resolved everywhere else) or, if the cursor is in an
anonymous region or a gap, the first FILE region whose base lies after
it and inside the range; take a
vnode reference through its record (`vnode_get`, under the lock, so the
vnode cannot go with a concurrent unmap), set the cursor to the region's
end, release; `mutex_lock(&vn->lock)`, `pagecache_sync(vn)`,
unlock, `vnode_put`; repeat until the cursor leaves the range. A region
unmapped between the two holds is not a problem: the reference kept the
vnode, and syncing a file is a file operation that owes nothing to the
mapping that named it. `MS_SYNC`: that walk, returning the first error —
the whole file's dirty pages for each vnode met, which is more than
asked and never less. `MS_ASYNC`:
the dirty pages are already known to the cache and reach the filesystem
by `vfs_sync`, the writeback thread or the last close, which is what
"scheduled" means here; the call returns 0 without writing. `MS_INVALIDATE`:
nothing to do, because the mapping *is* the cache — honoured by
definition, stated as such. Both `SYNC` and `ASYNC` set, or an undefined
bit, `-EINVAL` (the native rule); a range not wholly mapped `-ENOMEM`
(POSIX); an anonymous range in it is skipped. The error a `MS_SYNC`
returns is the write-back error as `pagecache_sync` records it; the open
file's once-only report (`wb_seq`) is untouched, so a program that also
calls `fsync` hears it there too, as it does today for `write()`.

### The two doors

**Native.** `COSMO_MAP_SHARED (1 << 3)` and `COSMO_MAP_PRIVATE (1 << 4)`;
`sys_mmap` reads `fd` and `off` from `a[4]`, `a[5]`. Rules, each
`-EINVAL` unless said otherwise: a file mapping names exactly one of the
two; `COSMO_MAP_SHARED` with `COSMO_MAP_ANONYMOUS` is refused — there is
no `fork`, so a program asking for anonymous memory shared with another
process must not be told yes; `COSMO_MAP_PRIVATE` with anonymous is
accepted, being what anonymous memory is; `off` page aligned; the fd
needs `HANDLE_RIGHT_READ` (`-EBADF`); a vnode that is not a regular file
`-ENODEV`; `PROT_WRITE` on a shared mapping needs the file opened for
writing and `HANDLE_RIGHT_WRITE` (`-EACCES`); W^X as everywhere. libc:
`MAP_SHARED`, `MAP_PRIVATE` become the kernel's bits, `mmap` passes `fd`
and `off` through, `msync` and `MS_*` are added. `COSMO_MAP_FIXED` over a
file goes through the replacement path with the range owned throughout —
the replacement is generalised to take the new region's description
rather than assuming anonymous, so M40 holds for file mappings.

**Linux.** `lx_mmap` maps a file as a FILE region: `MAP_PRIVATE` is a
copy-on-write mapping and `MAP_SHARED` a shared one; `MAP_SHARED |
PROT_WRITE` on a file opened for writing succeeds; on a file opened
read-only `-EACCES` (Linux's errno). `fill_from_file` is deleted. The
dynamic linker's private mappings (`PT_INTERP`, PIE) become demand-paged
copy-on-write mappings of the same files, which is what they are on the
system they were written for; the existing `lxtest` dynamic-linking
cases are the regression suite for that change and are run unchanged.
`MAP_SHARED | MAP_ANONYMOUS` stays what it is: private to the process,
unobservable without `fork`, documented as such.

### Signals

`vm_user_hooks::fatal` gains a signal argument. A fault that no region
services stays `SIGSEGV`; a FILE fault that is past the bound, or whose
page cannot be obtained — `readpage` failed, the mount's budget refused
the page, the entry could not be allocated — is `SIGBUS` with
`fault_addr` and code `BUS_ADRERR`. The process ends with `128 + 7` under
the default action, which is how a test sees it from outside.

### Accounting

`space->file_pages`, beside `anon_pages`: up at install of a cache frame,
down at teardown and at truncate's unmap, checked zero at
`vm_space_destroy` with the same panic that checks `anon_pages` — so a
leaked file frame is found at the exit of the process that leaked it,
every run, and not by a counting test that has to be remembered.
Copy-on-write frames are `anon_pages` and count against
`COSMO_RLIMIT_MEM` like any private memory; cache frames do not — they
are bounded by the mount's budget and by `limit_mapped_pages`, which is
charged at `mmap` for the whole range whatever backs it. Global:
`vm.file_faults`, `vm.file_cow_faults`, `vm.file_dirty_faults`,
`vm.file_fault_retries` (step 3 found the world changed),
`vm.file_sigbus`, and `vm.cache_writebacks` (the cache's existing
write-back count, so a test can see that a sync wrote), as sysctls
beside `vm.cache_pages`.

### One seam, for two proofs

Debug builds only: `debug.file_fault_hold`. Written 1, it arms the
calling process; the next FILE fault taken by any thread of that process
blocks **after step 1 and before step 2** — the space lock released, the
vnode reference held, no cache lock yet — on a completion that a write
of 0 signals. Event-driven, not timed: the held thread moves when the
test says so and not before. It is the one place a second actor can act
between the fault's decision and its install, and two tests below use it
for the two things that can happen there: another thread installs the
same page first, or the range is unmapped.

### What changes for a private mapping, stated

A private file mapping stops being a snapshot. A page not yet written
through it shows a later `write()` to the file, as it does on every
system that demand-pages private mappings, and POSIX leaves it
unspecified. `L14` ("a private file mapping is a snapshot the file never
sees") is rewritten to say what is now true: a private mapping's
**written** pages are the mapping's own and never reach the file.

## Affected files

| file | change |
| --- | --- |
| `kernel/include/kernel/vmm.h`, `kernel/memory/vmm.c` | `VM_REGION_FILE`; `struct vm_file_map`; `fmap` and `maxprot` on the region; the FILE arms of the fault (two phases, re-find, present-PTE upgrade, COW); `vm_user_map_file` and the replacement path generalised; `region_split` and every region-free site carrying the record; `user_range_teardown` putting frames by kind; `vm_user_protect` checking `maxprot` and stripping write from shared FILE PTEs; `vm_user_msync`; `file_pages` and the destroy check; the fatal hook's signal; the counters |
| `kernel/include/kernel/page.h`, `kernel/memory/pmm.c` | `PG_PAGECACHE` |
| `kernel/include/kernel/pagecache.h`, `kernel-services/vfs/pagecache.c` | `mappings` list and `trim_bound` on the cache; `pagecache_map_page` (steps 2–3's cache half: bound, `get`, dirty, reference); `pagecache_truncate` unmapping before freeing; `pagecache_sync` write-protecting after a run; `entry_reclaimable`'s reference test; `pagecache_write` lifting the bound; the flag on cache frames |
| `kernel/syscall/native.c`, `kernel/include/uapi/cosmo/syscall.h` | `COSMO_MAP_SHARED`, `COSMO_MAP_PRIVATE`, `fd`/`off`, the rules; `SYS_msync` 96, `COSMO_MS_*`; the `vm.file_*` sysctls; `debug.file_fault_hold` (debug) |
| `kernel/process/process.c` | `hook_fatal(addr, flags, frame, sig)` |
| `kernel/core/faultinject.c`, `kernel/include/kernel/faultinject.h` | `FI_FILE_READPAGE`: the next cache miss's `readpage` fails |
| `compat/linux/syscalls.c`, `compat/linux/linux_abi.h` | `lx_mmap` over `vm_user_map_file`; `fill_from_file` deleted; `lx_msync`; `LX_MS_*` |
| `libc/include/sys/mman.h`, `libc/include/cosmo/syscall.h`, `libc/src/` | `MAP_SHARED`/`MAP_PRIVATE`, `mmap` passing `fd`/`off`, `msync`, the raw wrapper; the `SYS_COUNT` mirror |
| `userland/init/init.c` | a `mmap` section of `init --selftest` (timed like the others, F13); `mmap-bench` under `USERBENCH` |
| `tests/linux/lxtest.c` | the `-EOPNOTSUPP` case becomes success; coherence, COW, `msync`, `EACCES` cases |
| `kernel-services/vfs/vfstest.c`, `kernel/core/selftest.c` | `pagecache-pinned` (a referenced frame survives a forced reclaim) |
| `tests/boot/run_boot_test.py` | the section's line and the bench line as markers |
| `docs/kernel/memory/{design,api,invariants,testing}.md` | the kind, the fault, the lock order, M42–M44, the tests |
| `docs/kernel-services/vfs/{design,api,invariants,testing}.md` | the record list, the bound, truncate and sync, V33; the "Future extensibility" line struck |
| `docs/compat/linux/{design,api,invariants,testing}.md` | the file `mmap` rewritten as built; L14 rewritten; `msync` |
| `docs/kernel/syscall/api.md`, `docs/libc/{api,testing}.md`, `docs/userland/testing.md` | the call, the flags, the section |
| `docs/audit/2026-09-deferred-work-inventory.md` | the §2.2 row struck; the §2.6 `msync` word struck |
| `README.md` | Status entry |

## New APIs

```c
/* uapi */
#define COSMO_MAP_SHARED  (1 << 3)   /* a file: writes reach the file and every other mapping */
#define COSMO_MAP_PRIVATE (1 << 4)   /* a file: copy-on-write; anonymous: what anonymous is */
#define SYS_msync 96                 /* (addr, len, flags) -> 0 */
#define COSMO_MS_ASYNC 1, COSMO_MS_INVALIDATE 2, COSMO_MS_SYNC 4

/* kernel */
int vm_user_map_file(struct vm_space *, uint64_t base, size_t len, vm_prot_t prot, vm_prot_t maxprot,
                     unsigned flags /* VM_MAP_SHARED, VM_MAP_REPLACE */, struct vnode *, uint64_t off,
                     const char *name);
int vm_user_msync(struct vm_space *, uint64_t base, size_t len, unsigned flags);
/* the cache half of a fault: under pc->lock on return when it succeeds; the caller installs and unlocks */
int pagecache_map_page(struct vnode *, uint64_t index, bool write, bool shared, struct page **out);
```

No existing prototype changes except `vm_user_hooks::fatal` (a signal
argument) and the replacement helper's arguments, both kernel-internal.

## Migration plan

One pull request, in the order a bisect would want: the cache's list,
bound and flag (inert without a FILE region); the region kind, the
record, the teardown by kind, the destroy check; the fault arms behind
`vm_user_map_file` with no caller; the Linux door switched to it
(`fill_from_file` deleted, `lxtest` updated in the same commit, since the
`-EOPNOTSUPP` assertion inverts); the native door and libc; `msync`; the
tests and the bench; the documents. Nothing on disk changes; no
filesystem format, no ABI number is reused.

## Tests

| case | what it establishes |
| --- | --- |
| `mmap` section: shared write, then `read()` | a byte written through a `MAP_SHARED` mapping is what `read()` returns, with no `msync` between: one frame |
| `write()`, then the mapping | a byte written by `write()` after the mapping exists is what the mapping reads: coherence in the other direction, the case the Linux door gets wrong today |
| two processes | a spawned child maps the same file `MAP_SHARED`, writes, exits; the parent sees the write. **The first shared memory between two processes in this system**, and the test says so |
| private, written | a write through `MAP_PRIVATE` does not reach the file (`read()` unchanged) and stays in the mapping; an unwritten page of the same mapping shows a later `write()` (the copy is per page) |
| offset | a mapping at `off = PAGE_SIZE` reads the file's second page |
| past the end | a two-page mapping of a one-page file: the second page is `SIGBUS` (a child, status `128 + 7`), not zeros — the bound, and `get()`'s zero page not inherited |
| truncate under a mapping | truncate to 0, touch: `SIGBUS`, not the old bytes; on both filesystems |
| `msync` writes | on `cosmofs` (`vda`): write through the mapping, `MS_SYNC`, `vm.cache_writebacks` moved by the pages dirtied; a second `MS_SYNC` with nothing written since writes nothing |
| re-dirty after `msync` | write, `MS_SYNC`, write again, `MS_SYNC`: the second sync writes the page again (`vm.cache_writebacks` +1, `vm.file_dirty_faults` +1): the PTE was lowered and the fault raised it |
| rights | fd opened `O_RDONLY`: `MAP_SHARED|PROT_WRITE` `-EACCES`, `MAP_PRIVATE|PROT_WRITE` succeeds; `mprotect(PROT_WRITE)` on the read-only fd's shared mapping `-EACCES` (maxprot) |
| the native rules | file with neither or both of SHARED/PRIVATE `-EINVAL`; `SHARED|ANONYMOUS` `-EINVAL`; an unaligned `off` `-EINVAL`; a bad fd `-EBADF`; a directory `-ENODEV`; an undefined flag bit still `-EINVAL` (the probe rule) |
| `msync` rules | `SYNC|ASYNC` `-EINVAL`; an undefined bit `-EINVAL`; an unmapped page in the range `-ENOMEM`; an anonymous range 0 |
| the lost race, held | debug: `debug.file_fault_hold` arms; thread A faults and holds after step 1; thread B touches the same page and completes; release: A finds the PTE present, `vm.file_fault_retries` +1, one frame, refcount back to 1 after `munmap` |
| unmap under a held fault | the same hold; B `munmap`s the range; release: A re-finds nothing and installs nothing (`retries` +1), the retry is `SIGSEGV` in the child — the page did not land in a range no region owns |
| `readpage` fails | `debug.faultinject` `file-readpage`: the touch is `SIGBUS`, `vm.file_sigbus` +1, nothing installed |
| leak | 200 cycles of map/write/unmap, shared and private, in a child: the child's exit runs the `file_pages == 0` check by construction, which is the deterministic half; after the file is unlinked, `vm.cache_pages` is back at its start (the `ramfs` frames go with the vnode), which is the observable half |
| `pagecache-pinned` (kernel) | `pagecache_map_page` a frame, `pagecache_set_limit` below the cached total, read another large file: the referenced frame is not reclaimed; `pmm_page_put`, read again: it is |
| `lxtest` | `MAP_SHARED|PROT_WRITE` succeeds and is coherent with `write`; the private cases as before; the dynamic-linking cases unchanged |
| the mapped-pages limit | `COSMO_RLIMIT_AS` refuses a file mapping past it as it refuses an anonymous one |

**Bug-proofs**, each to fail for its stated reason, each reverted after:

- `pagecache_sync` not lowering the PTEs → the re-dirty test: the second
  `MS_SYNC` writes nothing and `vm.file_dirty_faults` stays 0, because
  the PTE stayed writable and no fault marked the page.
- the present-PTE write arm raising without `mark_dirty` → the same
  test fails the other way: `file_dirty_faults` +1, `vm.cache_writebacks` +0. The
  counter is what tells the two mutations apart, and the report says so.
- no `pmm_page_get` at install → `pagecache-pinned`: the frame is
  reclaimed under the mapping and the free panics on its count, or the
  poisoner reports the touch.
- `pagecache_truncate` freeing without unmapping → truncate-under-a-
  mapping reads the old bytes; and `pmm_free_page` panics on a frame
  whose count is 2.
- no bound (`vn->size` alone) → **no test fails**, and the report says
  so in advance. The window is between `pagecache_truncate` and the size
  drop inside the filesystem's truncate (`ramfs.c:304-305`), and the
  fault seam is in the fault, not there; showing it would need a second
  seam inside `ramfs_truncate`, which is not built. This bracket stands
  on the code order it cites, under the rule the mprotect unit set for a
  proof the environment cannot show.
- step 3 without the re-find → unmap-under-a-held-fault: the page lands
  in a range with no region; `vm_space_destroy` panics with
  `file_pages` 1.
- `maxprot` ignored → `mprotect(PROT_WRITE)` on the read-only fd's
  shared mapping succeeds and a write reaches a file opened for reading.
- `SHARED|ANONYMOUS` accepted → the probe test fails; the point is the
  refusal.

## Benchmarks

`mmap-bench` under `USERBENCH`, on the boot's `ramfs`: (a) first-touch
cost per page of a 4 MiB cached file through a shared mapping, against
`read()` of the same bytes into a buffer (`read-bench` exists and is the
baseline); (b) the cost of `MS_SYNC` over 1024 dirty pages and of the
1024 re-dirtying faults that follow, on `cosmofs`; (c) the demand-zero
fault cost before and after this unit, because the fault handler grew an
arm and the report must say what that cost, even if the answer is
nothing measurable. Numbers recorded in the as-built banner, both
architectures.

## Risks

**The fault handler now sleeps, and a copy under a lock will be caught
rather than hidden.** Today no fault sleeps, so a `copy_to_user` made
with a spinlock held would work by luck; its `might_sleep` would report
in a debug build only if lockdep is watching that path. After this unit
such a copy that touches a file mapping deadlocks or corrupts in
release. The `might_sleep` in every copy is the guard and it is already
there (`uaccess.c:45,55,65`); the unit adds one more in the FILE arm and
boots the debug suite, which exercises every copy site, before anything
else. A report of a copy under a lock is a finding to fix, not a reason
to make the fault not sleep.

**A frame in the page cache is now also user memory.** The filesystem's
`writepage` reads a page a user may be writing at that instant; what
reaches the disk is some interleaving of the two. This is what a shared
mapping is on every system that has one, and `cosmofs`'s compression
compresses whatever bytes it read, so the record stays self-consistent.
Stated in the design document; not solved, because it is not a defect.

**The cache mutex is held across a page-table install, and across
`readpage`.** The second is today's behaviour for `read()`; the first
adds a spinlock section under the mutex, which the lock order permits
and lockdep will check. Faults on one hot file from many CPUs serialise
on it as `read()`s do. Measured by the bench, named as the scalability
limit; a per-index lock is the answer if it ever shows.

**Two filesystems change how they truncate — no, they do not**: the
bound is in the cache, so `ramfs_truncate` and `cfs_truncate` keep their
order and their failure semantics. The alternative (lower the size first)
was rejected for that reason: `cfs_truncate` can fail after the trim, and
moving the size in front of the failure changes what a failed truncate
leaves behind.

**The dynamic linker's mappings change kind.** Every Linux binary with an
interpreter now runs on demand-paged copy-on-write mappings instead of
copies. The behaviour is the one the binaries were built for, and the
`lxtest` dynamic cases run unchanged; if one of them fails, that is the
first thing the build reports and the unit stops there.

**Reclaim can be starved by mappings.** A process mapping more of a
reclaimable filesystem than the cache limit pins it, and the limit is
soft. Bounded by the mount's page budget and `COSMO_RLIMIT_AS`; the
security design's §3 limit becomes "clean and unmapped". Eviction of
mapped pages is deferred with the page-level reverse map it needs.

## Alternatives considered

**Hardware dirty bits instead of a write fault.** Rejected: DBM is
optional on AArch64, its emulation under TCG is not what a proof rests
on, and a software rule is one rule on two architectures with a counter
a test can read. The cost is one fault per page per write-back cycle,
measured by the bench.

**A page-level reverse map (frame → every PTE).** Rejected for this unit:
the only operations that need to reach every mapping are truncate and
write-back, and both run over a file, where a record per mapping and a
walk of the space's regions under its lock is exact and small. Eviction
of mapped pages would want the finer map, and is deferred with it.

**Keep the eager copy for `MAP_PRIVATE` and add only `MAP_SHARED`.**
Rejected: two implementations of one call, the copy pays the file's size
twice up front, and copy-on-write is on §14's must-list by name.

**Anonymous shared memory (`memfd`, `shm_open`) first.** Rejected as the
first step because it is the second: with no `fork`, an unnamed shared
mapping is inherited through an fd, and an fd names a file — so `memfd`
is a file on a memory filesystem mapped `MAP_SHARED`, i.e. this unit
plus a name. It is the natural next unit and is named as such.

**Split file regions in two phases instead of a mapping record.**
Rejected: `vm_user_protect` and `vm_user_unmap` would have to drop the
space lock, take the cache mutex, retake and re-validate at three sites,
to keep a list the vnode could walk. The record keeps the list under the
mutex and the count under the space lock, and the sites touch the count
only.

**Lower `vn->size` before trimming, in both filesystems.** Rejected, see
Risks: the trim-then-shrink order is entangled with `cfs_truncate`'s
failure path, and a bound owned by the cache masks the window without
touching either filesystem.

---

Named and deferred by this report: `memfd_create` and `shm_open` (a file
on a memory filesystem, mapped shared); a futex keyed by frame for
`MAP_SHARED` pages, so two processes can wait on one word
(`futex.c:44-46` keys by space); the ELF loader mapping `PT_LOAD`
segments as file regions instead of populated copies (`elf.c:222`), so
text is demand-paged and shared between processes running one binary;
eviction of mapped pages under pressure, with the page-level reverse map;
`mremap`; a `PHYS` region for user space so a device (the framebuffer)
can be mapped; `MAP_POPULATE`; `madvise` on file regions.

🤖 Generated with [Claude Code](https://claude.com/claude-code)
