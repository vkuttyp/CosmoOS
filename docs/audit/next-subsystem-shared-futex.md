# NEXT SUBSYSTEM — a futex keyed by what the word maps

Constitution §68: after the audit, name the next subsystem in this shape
and wait for the instruction to build it. That wait is over: the
instruction was given and the unit is built. **This report is as
built** (the shared-futex unit), and the banner below records where the
build differed from the design; the sections after it are the design as
reviewed.

**What the build changed:**

0. **The key carries its reference.** `struct futex_key { obj, off,
   held }`: `held` is the vnode reference a shared key holds, so a
   waiter's key, a wake's key and a requeue's two keys are each one
   object with one release (`key_put`, never under a bucket lock);
   there is no separate `futex_key_release` function. The waiter
   carries a key, not a space and an address, and `bucket_of` and the
   two match lines take the key -- the whole change to the futex, as
   designed, plus one thing review added to the build: `futex_wait`
   classifies the word AGAIN after it has read and compared it, and a
   changed key returns 0 (the spurious wake the contract permits). The
   key is taken before the read with the space lock released between,
   so a thread that unmapped the address and `MAP_FIXED`-mapped
   something else at it in that window would have had the word read
   from the new mapping and the waiter enqueued under the old key. A
   caller racing its own `munmap` against its own wait has a bug, but
   the waiter is the one that would hang for it; the re-check costs one
   lookup on the classifying path and nothing on the private one. No
   test drives the window: it needs a remap between two instructions of
   one system call, and the re-check is the argument.
1. **The fault handler had to learn about interrupts.** The tests
   found the file-backed page fault running with interrupts masked, as
   every trap enters: the anonymous arm never minded (it neither sleeps
   nor shoots down) and the file-regions unit's tests never contended
   the cache mutex, so the sleep never happened and the shootdown's
   `arch_irq_enabled()` assert never fired -- until this unit's waiters
   and the write-back thread contended it. The arm now runs with
   interrupts as the interrupted context had them
   (`arch_trap_frame_irqs_enabled`, new on both architectures: user
   code and a copy inside a system call enable them; a fault taken
   with them masked is a context that must not sleep, and
   `might_sleep` reports it). Recorded in `docs/kernel/memory/design.md`
   §7.2.
2. **The sysctl is `vm.futex_shared_keys`** (classifications that
   yielded a shared key), and the exit check is folded into the
   existing panic line (`0 anon and 0 file pages, N shared maps
   unaccounted`).
3. **The thread-exit wake classifies too.** `clear_child_tid`'s wake
   (`kernel/process/process.c`) passes "classify" rather than
   "private": a tid word can live anywhere the program put it.
4. **The bench sizes are 50 000 wakes and 500 round trips**, half the
   report's, because the section runs inside the suite's budget; the
   numbers are per operation and unchanged by the count. **As run**
   (QEMU TCG, one boot each): x86-64 1976 ns per wake with no shared
   mapping (the integer test), 3875 with one (the walk), 131 705 ns per
   cross-process round trip; AArch64 3928, 6555 and 144 550. The walk
   costs about a wake's worth and is paid only by a process that shares;
   the private path is what libc's mutex pays, and it is the old cost
   plus one load. The `mmap` section is 1186 ms on x86-64 and 1468 on
   AArch64, most of it the round trips and the three deliberate
   timeouts.
5. **The numbers.** Invariant **I7** (`docs/kernel/ipc/invariants.md`);
   L4 amended to say it holds per key; 366 self-tests on both
   architectures, unchanged in count (the unit's tests are sections and
   `lxtest` rows).
6. **The moved waiters' references are one add.** Review of the build
   pointed at `moved` atomic increments on the destination vnode under
   both bucket locks with interrupts off. The number of holders is
   known when the walk ends, so `vnode_get_n(k2.held, moved)` takes
   them in one add before the locks drop (`kobject_get_n`, new: `n`
   references, the released-object panic of `kobject_get`). The walk
   itself is bounded by the waiters present on the word, as before; the
   reference traffic no longer scales with it. Bug-proof below.

**Bug-proofs, as run.** Each mutation applied alone on x86-64, the
debug suite booted, the file restored from HEAD; the five the report
named and the two the review-revised lifetime rule added.

| mutation | what failed |
| --- | --- |
| the key by space alone (`vm_user_futex_key` always private) | the two-process case: `wait_sleepers(fw, 0, 1)` never saw the child asleep, the wake returned 0, the child exited 20 (`-ETIMEDOUT`), `vm.futex_shared_keys` did not move -- and the unmap-under-a-waiter case's count too |
| the vnode reference not taken at classification (`held = NULL`) | **nothing**, as declared in advance: `boot-test: PASS`. The files stay open through every case, so nothing reuses the vnode before a timeout |
| `FUTEX_PRIVATE_FLAG` still masked at the Linux door | `lxtest`: all four flag checks -- the wake through the *other* key woke the waiter (1, expected 0) and its own then found nobody (0, expected 1), in both directions |
| the counter's test inverted (`shared_maps != 0` skips the walk) | the same six checks as the first row: a process that shares got the private key |
| `shared_maps` not decremented at the record's release | `KERNEL PANIC: vm_space_destroy: 0 anon and 0 file pages, 1 shared maps unaccounted` at the first exit of a process that mapped a file shared |
| the moved waiters' references not taken (the one add of item 6 removed) | `KERNEL PANIC: kobject_get on a released vnode object` in the mmap section: the moved waiters put references they never held, the vnode was released under the mapping that still held it, and the next classification's get found it gone |
| the moved waiters' old references never put after a requeue | the double requeue: `vm.cache_pages == cp_a - 1` false -- file A's vnode never released, its page never left -- and the section's final `cache_pages == cache0` |
| the per-waiter reference not taken on a requeue onto a shared word | `KERNEL PANIC: kobject_get on a released vnode object`: the moved waiter's put at dequeue took the vnode's count to zero under the open file, and the next reference to it found it released |

**The file-regions unit gave two processes one page and left them no way
to wait on it.** PR #201 built shared file mappings: a `MAP_SHARED`
mapping and `read()`/`write()` are one frame, and a spawned child's
write through its mapping is what the parent reads through its own —
"the first shared memory between two processes in this system", the
`mmap` section says. What it did not build, and named as deferred in
its report and in `docs/kernel/memory/design.md` §7.7, is the wait: the
futex is keyed by address space. `bucket_of` hashes `(space, uaddr)`
and every match is `w->space != space || w->uaddr != uaddr`
(`kernel/ipc/futex.c:44-46`, `:145`, `:218`), so a thread in one process
sleeping on a word in a shared page is invisible to a wake from another
process on the same word — the same frame, the same bytes, a different
`struct vm_space *`. The wait is not refused and the wake is not
refused; the wake simply finds nobody, and the sleeper waits for a
timeout that a program using shared memory correctly did not set.

**At the Linux door the flag that says which kind it is exists and is
ignored.** `FUTEX_PRIVATE_FLAG` is masked out before the operation is
looked at (`LX_FUTEX_CMD_MASK`, `compat/linux/linux_abi.h:105`;
`lx_futex`, `compat/linux/syscalls.c:1430`), and the design document
says so: "with or without `FUTEX_PRIVATE_FLAG`"
(`docs/compat/linux/design.md:126-128`). Linux's contract is that the
flag *narrows* a futex to the calling process, and its absence makes the
futex shared through whatever the word maps; musl sets it for a
process-private mutex and clears it for one created
`PTHREAD_PROCESS_SHARED`. Here both are private, so a musl program that
puts a process-shared mutex in a `MAP_SHARED` page — the ordinary way to
do it — gets a mutex whose other holder never wakes. That is the class
the hardening unit named: a flag accepted with the wrong meaning.

**Takes up** the clause the file-regions unit left in the inventory's
§2.2 row — "Still open from this row: a shared futex across processes
(`kernel/ipc/futex.c` keys by space)"
(`docs/audit/2026-09-deferred-work-inventory.md:124-126`) — and the
same item in `docs/kernel/memory/design.md` §7.7. A §6 first-class pick:
a correctness gap reachable today, in the tree since 2332d59, with a
deterministic two-process test the tree can build from what PR #201
added.

## What is established

**The futex is three operations over one table, and every one of them
matches on `(space, uaddr)`.** Sixty-four buckets under spinlocks; a
waiter is a stack-allocated `struct futex_waiter { space, uaddr, bucket,
thread, woken, timed_out }` on a bucket's list (`futex.c:15-31`).
`futex_wait` reads the bucket's `wake_seq`, copies and compares the word
with no lock held (a user copy may fault and may sleep — the rule L8 of
the lockdep invariants records), re-takes the lock and enqueues only if
no wake ran in between (`:58-100`); `futex_wake` bumps `wake_seq` and
wakes up to `n` matching waiters (`:133-157`); `futex_requeue` takes two
buckets in address order, compares under the queue sequence, wakes some
and moves the rest to the second word — a word requeued onto itself is
counted and left where it is, which is how the native thread door's
tests count sleepers (`:159-251`). Invariant L4 (no wake lost between
the compare and the sleep) and the native thread door's L10 rest on the
sequence numbers, not on what the key is. **The key is the one thing a
shared futex changes**, and it is confined to `bucket_of` and the two
match lines.

**Both doors pass the current process's space and nothing else.**
`sys_futex_wait`/`wake`/`requeue` (`kernel/syscall/native.c:187-227`)
and `lx_futex` (`compat/linux/syscalls.c:1427-1485`) call the three with
`process_current()->space`. The native calls have no flags word at all
(`cosmo_futex_wait(word, val, timeout_ns)`,
`libc/include/cosmo/syscall.h:502-513`); libc's mutex, condition
variable and join are built on them (`libc/src/thread.c:202-290`) over
words in the process's own anonymous memory.

**The VMM now knows what a word maps.** A `VM_REGION_FILE` region
points at a `struct vm_file_map` that names the vnode, the file offset
of its base, and whether the mapping is shared (`kernel/include/kernel/vmm.h`,
the file-regions unit): under the space lock, `space_find(uaddr)` says
whether a word lives in a shared file mapping and, if so, which file
and which offset. Two processes mapping one file `MAP_SHARED` have two
records with one `vn` and, for the same byte, one `off + (uaddr - base)`.
A vnode is a kobject with `vnode_get`/`vnode_put`, and a mapping record
holds a reference to it for its life (`vmm.c`, `region_put`).

**The tools the tests need exist.** A spawned child that maps the
section's file (`init --probe mmap-writer`, PR #201); the count of
sleepers on a word through a requeue onto itself, which the native
thread door built so a test can know a waiter is *asleep* rather than
merely arrived; `cosmo_dup_rights`; the `mmap` section of
`init --selftest` with its timing line (F13); `lxtest`'s `lx_clone`
threads for the Linux door.

## The problem

### A wake that finds nobody

Two processes, one `MAP_SHARED` page, a word in it. Process A reads the
word (0), calls `futex_wait(word, 0)`; process B writes 1 and calls
`futex_wake(word, 1)`. B's wake hashes `(B's space, B's uaddr)`, walks
that bucket for a waiter with B's space, and returns 0. A sleeps until
its timeout, or for ever. Nothing reports an error, because nothing is
an error: each call did what it was asked in the address space it was
given. The defect is that the address space is the wrong identity for a
word two spaces share, and the tree could not have said otherwise until
two spaces *could* share a word — which is why this is the unit after
the file-regions unit and not before it.

### The flag that would have said so is dropped

Linux's `FUTEX_PRIVATE_FLAG` is the program telling the kernel "this
word is mine alone, skip the lookup". Its absence is the program saying
the opposite. `lx_futex` masks it out, so the door cannot tell a
process-shared mutex from a private one, and the design document
records the masking as a feature. With shared mappings built, a Linux
program that follows the rules — a `MAP_SHARED` page, a
`PTHREAD_PROCESS_SHARED` mutex, the flag clear — is the program that
breaks.

### What the native ABI cannot say

The native calls carry no flag, by the native rule that a program does
not tell the kernel what the kernel can see for itself. That rule cuts
the other way here: the kernel *can* see what a word maps, and does not
look.

## Design

### The key names what the word maps

```c
struct futex_key {
    const void *obj;   /* private: the vm_space; shared: the vnode */
    uint64_t off;      /* private: uaddr; shared: the file offset of the word */
};
```

`bucket_of` hashes `(obj, off)`; the waiter carries a key and a match
is `w->key.obj != key.obj || w->key.off != key.off`. Nothing else in
`futex.c` changes: the sequences, the compare-then-enqueue, the
two-bucket order and the self-requeue rule are exactly as built.

**Classification** is one function in the VMM, `vm_user_futex_key(space,
uaddr, flags, &key)`: under `space->lock`, `space_find(uaddr)`; if the
region is `VM_REGION_FILE` and its record is shared, the key is
`{ vn, fmap->off + (uaddr - fmap->base) }` and **a vnode reference is
taken**; otherwise `{ space, uaddr }`. The reference is what makes
pointer identity sound: a waiter's `obj` cannot be freed and reused by
another file while the waiter holds it, and a waker's key comes from its
own mapping's record, which references the same vnode. The waiter puts
the reference after it dequeues; a waker's key lives for the call. A
word in a *private* file mapping is private: its page is the process's
own copy once written, and a wait on a not-yet-copied page is on a
frame that is the process's alone to see until it writes — Linux keys it
the same way.

**The lookup is not paid by a process with nothing to share.** The space
carries `shared_maps`, the number of shared mapping records pointing
into it, kept by `vm_user_map_file` and the record's release under the
space lock. With it zero — every process today, and every process that
never maps a file `MAP_SHARED` — `vm_user_futex_key` returns the private
key without walking the region list. libc's mutex and condition variable
therefore pay one integer test more per futex call, and the bench below
measures it rather than assumes it.

### The Linux flag is honoured, both ways

`lx_futex` keeps `FUTEX_PRIVATE_FLAG` and passes it through:
**set**, the key is private without a lookup (the program's promise,
and the cheaper path, exactly as on Linux); **clear**, the word is
classified. So a process-shared mutex in a shared page works, a private
mutex costs what it costs today, and a program that sets the flag on a
shared page gets a private futex — which is what it asked for and what
Linux gives it. `FUTEX_WAIT_BITSET`/`WAKE_BITSET` with `MATCH_ANY`,
`REQUEUE`, `CMP_REQUEUE` and the `CLOCK_REALTIME` handling are
untouched; the flag reaches them all through one parameter.

### The native calls classify, always

No new flag. The native ABI's rule is that the kernel does not ask the
program what it can see; the kernel can see what the word maps, so it
looks — and `shared_maps == 0` makes "looks" free for the processes that
have nothing to share. A `COSMO_FUTEX_PRIVATE` opt-out is named in the
alternatives and not taken: it would turn a correctness rule into a
convention the program must know.

### Requeue across kinds

`futex_requeue` classifies both words. A shared word and a private word
are two buckets like any two; the address-order lock rule and the nested
annotation are unchanged. Requeueing a waiter from a shared word to a
private one, or the reverse, moves it between the keys' buckets and
rewrites its key under both locks, as the requeue already rewrites
`uaddr` and `bucket`. **A waiter holds exactly one reference: to the vnode of its current
key, or none, and a requeue that changes the key exchanges it.** Every
waiter matched on the source word carries the source key, so all of
them hold references to one vnode or to none; the requeue therefore
deals with at most one old vnode and one new one however many waiters
it moves. Under the two bucket locks it takes one reference to the
destination key's vnode per waiter it moves onto a shared word
(`vnode_get` is an atomic increment) and counts the waiters it moved off
a shared word; after the locks are released it puts that many
references to the old vnode, because a put may release the vnode and a
release does block I/O, which cannot happen under a spinlock. The
requeue's own reference on each key protects the call, not the
waiters. Review of this report caught two earlier versions: one that
relied on the call's reference for the moved waiters, and one that let
a waiter accumulate references across repeated requeues with one slot
to hold them in — a chain shared → private → shared is legal and
unbounded, and the exchange rule handles it with one slot because each
step leaves the waiter holding the reference its key names and no
other. At dequeue the waiter puts the one it holds.

### Lifetime

A waiter holds its vnode reference from classification — or from the
requeue that moved it onto a shared word — to dequeue. If
its mapping is unmapped underneath it (another thread's `munmap`), the
word is gone but the vnode is not: the waiter sleeps to its timeout or
its kill exactly as a private waiter on an unmapped word does today, and
no pointer it holds dangles. A vnode with a waiter cannot be released,
so `pagecache_drop`'s "no mappings" assertion is unaffected: the mapping
may be gone while a waiter still references the vnode, and that is a
reference like an open file's. `shared_maps` is checked zero at
`vm_space_destroy` beside `anon_pages` and `file_pages`.

## Affected files

| file | change |
| --- | --- |
| `kernel/include/kernel/futex.h`, `kernel/ipc/futex.c` | `struct futex_key`; `bucket_of` and the match lines over it; the waiter's key and its vnode reference; the three entry points take `(space, uaddr, private)` and classify through the VMM |
| `kernel/include/kernel/vmm.h`, `kernel/memory/vmm.c` | `vm_user_futex_key`; `vm_space::shared_maps` maintained at `vm_user_map_file` and the record's release; the destroy check |
| `kernel/syscall/native.c` | the three native calls pass "classify" |
| `compat/linux/syscalls.c`, `compat/linux/linux_abi.h` | `FUTEX_PRIVATE_FLAG` kept and passed; `LX_FUTEX_CMD_MASK` keeps `CLOCK_REALTIME` only |
| `userland/init/init.c` | the `mmap` section gains the two-process futex, the private-stays-private case, the unmap-under-a-waiter case, and the bench; `init --probe mmap-futex-wait` |
| `tests/linux/lxtest.c` | the flag both ways on a shared page, with `lx_clone` threads |
| `docs/kernel/ipc/{api,design,invariants,testing}.md` | the key, the classification, invariant **I7**, the tests |
| `docs/compat/linux/{design,api,invariants,testing}.md` | the flag honoured; L4's key; the test rows |
| `docs/kernel/memory/{design,api}.md` | `vm_user_futex_key`, `shared_maps`; §7.7's item struck |
| `docs/kernel/syscall/api.md`, `docs/libc/api.md` | what a native futex on a shared page now means |
| `docs/audit/2026-09-deferred-work-inventory.md` | the §2.2 clause, marked "taken up" by this report and not struck until the build lands, struck by the build's documents commit |
| `README.md` | Status entry |

## New APIs

```c
/* kernel: the futex's identity for a user word. `private` skips the
 * lookup (the Linux flag); otherwise a word in a shared file mapping is
 * keyed by (vnode, file offset) with a reference taken, else by (space, uaddr). */
int vm_user_futex_key(struct vm_space *space, uint64_t uaddr, bool private, struct futex_key *out);
void futex_key_release(struct futex_key *key);   /* drops the vnode reference of a shared key */

int futex_wait(struct vm_space *space, uint64_t uaddr, uint32_t val, uint64_t timeout_ns, bool private);
int futex_wake(struct vm_space *space, uint64_t uaddr, unsigned n, bool private);
int futex_requeue(struct vm_space *space, uint64_t uaddr1, uint64_t uaddr2, unsigned nr_wake,
                  unsigned nr_requeue, bool cmp, uint32_t cmpval, bool private);
```

No user ABI changes: the native numbers, arguments and errnos are the
same; what changes is what a native futex on a `MAP_SHARED` page means,
and that the Linux flag means what Linux says.

## Migration plan

One pull request: the key type and the classification with every caller
passing `private = true` (nothing changes, every test passes); the two
doors switched (native: classify; Linux: the flag); the tests; the
documents. No structure a program sees changes size or number.

## Tests

| case | what it establishes |
| --- | --- |
| `mmap` section: two processes, one word | the parent maps the file shared and zeroes a word; the child (`init --probe mmap-futex-wait`) maps it shared and `futex_wait`s on 0 with a 5 s timeout; the parent counts sleepers by a requeue of the word onto itself (which crosses the process boundary only if the key does) until it reads 1, writes 1, wakes, and asserts the wake returned 1 and the child exited 0 — **the first wait across two processes in this system** |
| the wake before the sleep | the parent wakes before the child is asleep: 0 woken, the child's later wait returns `-EAGAIN` (the word changed), exit 0 — the sequence rule L4 holds across keys |
| private stays private | a private mapping of the same file: a waiter on the same file offset in it is not counted and not woken by the shared word's wake (`-ETIMEDOUT` on a short timeout), and a wake on it wakes only itself |
| a word in anonymous memory | libc's own mutex under the herd (`thrtest`, unchanged) and the sleeper count the native thread door reads: `shared_maps == 0` for that process, and the counts are what they were |
| unmap under a waiter | a thread waits on a shared word; another unmaps the page; the wait times out, the process exits cleanly, the poisoner is silent (the vnode reference outlived the mapping) |
| requeue across kinds | waiters on a shared word requeued onto a private one and woken there; the sleeper counts move with them |
| requeue onto a shared word, then the mapping goes | waiters on a private word requeued onto a word in a shared mapping; the shared mapping is unmapped by another thread; the waiters time out cleanly and the poisoner is silent — the reference each moved waiter is given is what outlives the mapping |
| requeue off a shared word, then off again | waiters on a shared word requeued onto a private word, then onto a word in a *second* shared file, then woken; the first file is unlinked and its last reference dropped before the wake: no leak of the first vnode (its release runs, observed by the file's page count returning) and no use of it — the exchange left each waiter holding only its current key's reference |
| `lxtest`: the flag both ways | on a `MAP_SHARED` page: a clone thread waits *with* `FUTEX_PRIVATE_FLAG`; a wake *without* it wakes 0, a wake *with* it wakes 1; then the reverse pair — the flag selects the key and both keys work |
| `lxtest`: a private mutex's cost | a wait/wake pair with the flag set on a shared page takes the private path (a counter, `vm.futex_shared_keys`, does not move) |
| exit | `vm_space_destroy` checks `shared_maps == 0` on every process exit |

**Bug-proofs**, each to fail for its stated reason, each reverted after:

- the key by space alone (today's code) → the two-process case: the
  sleeper count never reaches 1 and the child exits `-ETIMEDOUT`.
- the vnode reference not taken → unmap-under-a-waiter: the vnode can
  be released under the waiter; the poisoner or a use-after-free report
  on the next allocation. **May be silent** if nothing reuses the vnode
  before the timeout; the report says so, and the reference stands on
  the argument if it is.
- the per-waiter reference not taken on a private-to-shared requeue →
  the requeue-then-unmap case, with the same caveat as the row above:
  the vnode can be released under the moved waiters, shown only if
  something reuses it before the timeout.
- the old references not put after a shared-to-private (or
  shared-to-shared) requeue → the requeue-off-a-shared-word case: the
  first file's vnode never releases, its page count never returns —
  a leak a counting test can see, and this one does.
- `FUTEX_PRIVATE_FLAG` still masked → the `lxtest` flag case: the wake
  without the flag wakes the private waiter (1, expected 0).
- classification skipped when `shared_maps != 0` (the counter's test
  inverted) → the two-process case fails as the first row.
- `shared_maps` not decremented at the record's release → the exit
  check panics on the first process that mapped a file shared.

## Benchmarks

`USERBENCH: futex`: (a) 100 000 `cosmo_futex_wake` calls with no waiter
on a word in anonymous memory, in a process with `shared_maps == 0`,
before and after this unit — the cost of the one integer test on libc's
mutex path; (b) the same in a process that has one shared mapping — the
cost of the region walk, paid only by processes that share; (c) the
two-process wake-to-wake round trip through a shared word, which did not
exist before. Numbers in the as-built banner, both architectures.

## Risks

**The mutex hot path gains a test.** One load and compare of
`shared_maps` per futex call for every process that never maps a file
shared; measured by (a). If it shows, the answer is to fold the test
into the syscall door rather than the futex, not to drop it.

**A process that maps one file shared pays the walk on every futex
call, private words included.** The region list is linear and walked
under the space spinlock. Measured by (b); a program with many regions
and many futex calls would want the Linux opt-out flag, which the Linux
door has and the native door refuses on principle. If (b) is bad, the
alternative below is the escape hatch, named here so the decision is
recorded.

**Pointer identity for the shared key.** Sound while the waiter holds
its reference (the vnode cannot be reused); a waker's key is computed
under the space lock from a record that holds one too. A vnode that is
the same file reopened after a full release is a different pointer and a
different key, which is correct: nothing could have been waiting on it.

**Lifetime through requeue.** A waiter holds the one reference its
current key names; a requeue that changes the key takes the new
references under the locks and puts the old ones after them, and the
waiter's own release is at dequeue, in one place. The put after the
locks is the one step that could be forgotten, and the
requeue-then-unmap test in both directions is what would show it.

## Alternatives considered

**A native `COSMO_FUTEX_PRIVATE` flag, like Linux's.** Not taken: the
native calls have no flags word, adding one is an ABI change for an
optimisation, and a flag a program must set to be correct — or to be
fast — is a convention where the kernel can decide. Kept in reserve if
the bench says the walk is expensive for programs that share.

**Key a shared word by its frame (`struct page *`) rather than by
`(vnode, offset)`.** Rejected: the frame is stable only while mapped and
pinned, and a waiter whose page is unmapped and reclaimed would hold a
frame pointer that a later allocation reuses for anything; a vnode
reference is a lifetime the tree already has rules for.

**Key private words by frame too, so an anonymous page shared by a
future fork works.** Rejected: there is no fork and no shared anonymous
memory (the file-regions unit refuses `SHARED|ANONYMOUS`); when a way to
share anonymous memory exists it will be a file (`memfd`), and this
design keys it already.

**Do it in the two doors instead of the futex.** Rejected: the key is
the futex's identity and the requeue moves waiters between keys; two
doors computing keys in two ways is the divergence the shared `futex.c`
exists to prevent.

---

Named and deferred by this report: `memfd_create` and `shm_open` (a
file on a memory filesystem, mapped shared, which this design keys
without change); `FUTEX_WAKE_OP` and priority-inheritance futexes; a
tree in place of the linear region walk if a program that shares and
spins on futexes ever measures it.

🤖 Generated with [Claude Code](https://claude.com/claude-code)
