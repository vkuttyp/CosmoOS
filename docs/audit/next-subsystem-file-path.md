# NEXT SUBSYSTEM — a read that fills its buffer, and an error that reaches close

Date: 2026-09-14. Tree: `main` at 8fc864d (after PR #136, the lockup
unit). Chosen from `docs/audit/2026-09-deferred-work-inventory.md` §3:
the audit's two MEDIUM findings on the file path that no milestone took
up.

**Subsystem: the syscall layer's read and write bounce, sized to the
request instead of to a console line; and a write-back error that
reaches the program -- recorded on the vnode, reported once to each
open file by `fsync` and by `close`, counted when nothing can report
it.** Nothing in this report is built; the migration plan is the plan,
and the "as built" and "as run" sections are filled by the
implementation pull request.

Two findings from the 2026-09-05 audit sit in the inventory's §3 with
the note "not in any milestone", and both are still exactly as found:

1. **`read` returns at most 1 KiB per call for every object type**
   (audit 4.2, MEDIUM). `syscall_obj_read` copies through a 1 KiB stack
   buffer, `IO_CHUNK`, sized "so one console line fits a single read",
   and makes one object call of at most that (`kernel/syscall/native.c:45,
   99-113`). A program that asks for 64 KiB of a file gets 1 KiB and
   asks again: 64 system calls, each a handle lookup, a range check, a
   page-cache lookup and a copy, for what one call could do.
   `syscall_obj_write` loops the same chunk (`native.c:52-78`): one
   `copy_from_user` and one object write per kilobyte. The Linux
   personality inherits both (`compat/linux/syscalls.c:220-221`), its
   `readv`/`writev` chunk on top of them (`:293-294`), and its `pread`
   bounces through one page (`:793-806`). libc hides the cost from
   `stdio` (`fread` reads `BUFSIZ` per call and loops, `libc/src/stdio.c:219`)
   and nothing hides it from a program that calls `read` itself.
2. **`close()` cannot report write-back errors** (audit 8.2, MEDIUM).
   Dirty pages are written back at `fsync`, at `vfs_sync`, and at the
   file's last reference; `file_release` runs that last write-back and
   drops its result (`kernel-services/vfs/vfs.c:854-866`), and
   `vnode_release` runs one more and then drops whatever is still dirty
   without a word (`vfs.c:39-59`, `pagecache_drop`). `sys_close` returns
   what `handle_close` returns, which is 0 or `-EBADF`
   (`kernel/object/handle.c:138-156`); the release runs inside the
   `kobject_put` and has no return path. A program that writes a file
   and closes it -- the common shape, without `fsync` -- is told nothing
   when the device refuses the write. `fsync` does report, but only its
   own attempt's failure (`file_sync`, `vfs.c:1145-1156`): an error met
   by another file's close on the same vnode, or by `vfs_sync`, is never
   seen by the file that wrote the data.

**Inventory entries this unit is to close** (struck through in its
documents commit when built, not before): §3 "VFS: `close()` cannot
report write-back errors" and §3 "`read` returns at most 1 KiB per call
through the stack bounce buffer".

**Built: PR #138 (2026-09-14).** The design below is as proposed; "As
run" records what the build measured. Differences from the plan, each
found by building:

1. **The record is the page cache's, not the vnode's** (Greptile's
   round-1 finding on the report, built as revised): `pagecache_sync`
   records under `pc->lock`; `struct pagecache` carries `wb_err` and
   `wb_seq`; `pagecache_error_since` and `pagecache_wb_seq` read them.
2. **An unlinked file's pages are neither written back nor counted.**
   The first run's loss lines made the question concrete: which drops
   are data lost? A named file's (`nlink > 0`) are; an unlinked file's
   have no reader left. The vnode's release now makes its last attempt
   and counts only for a named file, which also closes the audit's 8.2
   LOW ("`vnode_release` writes back dirty pages of an `nlink==0`
   file"); `pagecache_drop(vn, lost)` takes the decision as an argument.
   The suite's one real loss stays visible: `cosmofs-reserve` fills a
   disk on purpose and its last file's sixteen pages fail at release
   with `-ENOSPC` -- the line every boot now prints, silent before.
3. **The kernel tests prove the helper and the object side; the
   syscall's wiring is proved from user mode.** `syscall_obj_read` takes
   a user address, which the kernel's own thread has none of, so
   `read-bounce` proves the sizing and the fallback of
   `syscall_bounce_get` directly and `file_read`'s 64 KiB from a 200 KiB
   file; `fs_selftest` in `init --selftest` reads 64 KiB in one `read`,
   3 KiB from the end, and 300 from a pipe, and prints the `USERBENCH`
   lines. The planned `read-fill`/`read-bounce-fallback` pair is the one
   test `read-bounce`.
4. **The bug-proof for the fallback is on the helper**: an injected
   allocation failure that returns no buffer fails `read-bounce`'s cap
   check, since the syscall path cannot be driven from the kernel.
5. **The bench measures the object path in the kernel and the whole
   syscall from user mode**, not the syscall from the kernel; the two
   figures together are the answer the audit asked for.
6. **The Linux `pread`/`pwrite` had their own 4 KiB heap bounce**
   (`rw_at`), and the private file mapping's fill a page: both now use
   the shared helper. The report's citation of `lx_pread` at `:793-806`
   was the mapping's fill; `rw_at` is at `:226-264`.
7. **`file_flush` skips `ops->sync`** (no commit at close: the
   transaction model is unchanged); `file_sync` keeps it.

**Why these, of the inventory's open items.** With the watchdog row and
the latent spin closed by the last unit, §3's remaining correctness
entries are these two, the SMEP/SMAP and unknown-flag acceptances, the
non-coherent DMA syncs, and the never-tested paths of §4. The two file
findings are the ones with a *program-visible* consequence today on the
machines the suite runs (data silently lost at close; sixty-four calls
for one) and a deterministic test in reach: the RAM block device
completes a bio in its submitter's context (`kernel/block/ramblk.c:76-126`)
and the block fault injection already scopes a failure to one thread
(`kernel/core/faulttest.c:185-245`), so "the device refused this
write" is a line in a test, not a wait. They share the file path and
the same test fixture, which is why they are one unit and not two.

## Problem

### The bounce

```c
/* kernel/syscall/native.c */
#define IO_CHUNK 1024   /* one console line (TTY_LINE_MAX) fits a single read */

static int64_t syscall_obj_read(struct kobject *obj, uint64_t ubuf, size_t len)
{
    ...
    char tmp[IO_CHUNK];
    size_t n = len < IO_CHUNK ? len : IO_CHUNK;
    int64_t rc = io->read(obj, tmp, n);
    ...
    if (rc > 0 && copy_to_user(ubuf, tmp, (size_t)rc))
        rc = -EFAULT;
    return rc;
}
```

One object call, at most a kilobyte, whatever was asked. The write side
loops: `while (done < len) { copy_from_user(tmp, ..., n); io->write(obj,
tmp, n); ... }` with the same `n`. The object interface takes a kernel
buffer (`struct kobject_io_type`, `kernel/include/kernel/object.h:86-89`),
so a bounce is inherent to the design; its *size* is the finding. The
socket path already sizes its bounce to the request with `kmalloc`
(`native.c:774`, `SOCK_IO_CHUNK`), so the precedent for a heap bounce in
this file exists.

Who pays: every `read` of a file, a pipe or a socket through the file
handle path; `cat`, `cp`, `pkg` reading a package, the shell reading a
script, `init` reading `rc`; every Linux program's `read`/`write`/`readv`.
Nothing is wrong, only slow, and slow by a factor that a benchmark will
state (the audit asked for exactly this benchmark: "`read`/`write`
bandwidth at 1 KiB chunking", audit §17).

### The error that nobody hears

`pagecache_sync` (`kernel-services/vfs/pagecache.c:321-386`) writes dirty
pages in ascending order and stops at the first error, leaving the failed
page and those after it dirty -- correct: a later sync retries them.
Four callers:

| caller | when | holds | the error goes |
| --- | --- | --- | --- |
| `file_sync` (`vfs.c:1145`) | `fsync` | `vn->lock` | to the caller, this attempt only |
| `file_release` (`vfs.c:854`) | the file's last reference: `close` of the last handle, `exit` | `vn->lock` | dropped |
| `vnode_release` (`vfs.c:39`) | the vnode's last reference | nothing (no other reference exists) | dropped, and then the dirty pages with it |
| cosmofs's `sync` (`cosmofs_core.c:776-795`), which `vfs_sync` (`vfs.c:600`) reaches through `mnt->fs->sync` | `sync`, unmount | `vn->lock` per vnode | to whoever called `sync`, not to the writer |
 So the program that writes and closes learns nothing; the
program that writes, `fsync`s and gets 0, then has its pages fail at a
neighbour's close, learns nothing; and a page that fails at the vnode's
release is lost with a count of zero anywhere.

What the block layer offers: `bio_complete` takes a status
(`kernel/block/blk.c:535-547`), cosmofs's data write returns it up
through `write_one_page` and `cfs_writepage` without poisoning the mount
(`cosmofs.c:1752-1771`, and `cfs_writepages` after it; `cfs_fail` is reached only from rename's
recovery and the commit's post-superblock failure), and the fault
injection makes it happen on demand (`FI_BLK_COMPLETE`, `blk.c:543`).
`faulttest.c:185-245` already drives cosmofs on a RAM block device
through injected completion errors and checks that "writes and syncs
report `-EIO` or succeed, nothing else". It never checks close, because
close cannot say.

## Current implementation

| piece | where | today |
| --- | --- | --- |
| `syscall_obj_read` / `syscall_obj_write` | `kernel/syscall/native.c:45-113` | a 1 KiB stack bounce; read one chunk, write a loop of chunks |
| `lx_read`, `lx_write`, `rw_vec`, `lx_pread` | `compat/linux/syscalls.c:220-221, 270-300, 793-806` | the same two functions; `readv`/`writev` iterate them; `pread` bounces a page from `kmalloc` |
| `struct kobject_io_type` | `kernel/include/kernel/object.h:86-105` | `read`, `write`, `stat`, `ready`, `set_nonblock`, `wait_queue`; no close-time hook |
| `handle_close` | `kernel/object/handle.c:138-156` | clears the slot, `kobject_put`; returns 0 or `-EBADF` |
| `sys_close`, `lx_close` | `native.c:360`, `syscalls.c:338` | `handle_close` |
| `file_sync` | `vfs.c:1145-1156` | `pagecache_sync` then `ops->sync`; this attempt's error |
| `file_release` | `vfs.c:854-866` | the last write-back, result dropped |
| `vnode_release` | `vfs.c:39-59` | a write-back, then `pagecache_drop` |
| `pagecache_sync` | `pagecache.c:321-386` | stops at the first error; failed pages stay dirty |
| `pagecache_stats` | `kernel/include/kernel/pagecache.h:62-68` | hits, misses, writebacks, pages, reclaimed, budget refusals; nothing for pages lost |
| `struct file` | `vfs.h:151-159` | `vn`, `pos`, `flags`, `lock`, `priv`, `dev_open` |
| `faulttest`'s block section | `kernel/core/faulttest.c:185-245` | cosmofs on `ramblk`, `FI_BLK_COMPLETE` every 2nd for 8, `FI_BLK_SUBMIT` |
| `fsync-handle` | `kernel-services/vfs/vfstest.c:226-262` | the syscall's steps from a handle table: lookup, `file_sync`, `-EBADF` |

## Why it matters

- **A write the device refused is data lost in silence.** The kernel
  knows; the program that wrote it is the one party not told. Every
  filesystem this kernel mounts is on a device that can say no, and the
  fault injection says no on demand -- the tests in `faulttest.c` prove
  the *filesystem* survives the refusal and never ask what the *program*
  was told.
- **`fsync` returning 0 is a promise this kernel does not keep** when
  the pages it covers fail later at a neighbour's close: the second
  file's `fsync` after that sees only its own attempt.
- **Sixty-four system calls for one** is a cost paid by every reader of
  a file bigger than a kilobyte, including every Linux program, and the
  audit's benchmark for it does not exist. The bounce is a design
  choice; its size is a leftover from the console.
- **Both are program-visible today** on the QEMU machines the suite
  runs, and both have a deterministic fixture in the tree.

## Design

### A bounce sized to the request

The rule: **one system call, one object call, a bounce as large as the
request up to `IO_BOUNCE_MAX` (64 KiB); the stack for a kilobyte and
less, the heap above; a heap allocation that fails degrades to the
stack chunk, never to an error.**

```c
/* kernel/syscall/native.c */
#define IO_CHUNK      1024            /* on the stack: a console line, a small read */
#define IO_BOUNCE_MAX (64u * 1024u)   /* from the heap: one object call for a big read */

struct io_bounce { char *buf; size_t cap; bool heap; };   /* kernel/include/kernel/syscall.h */

void syscall_bounce_get(struct io_bounce *b, char *stack, size_t len)
{
    b->buf = stack; b->cap = IO_CHUNK; b->heap = false;
    if (len > IO_CHUNK) {
        size_t want = len < IO_BOUNCE_MAX ? len : IO_BOUNCE_MAX;
        char *p = kmalloc(want, 0);       /* the page path above 8 KiB */
        if (p) { b->buf = p; b->cap = want; b->heap = true; }
    }
}
void syscall_bounce_put(struct io_bounce *b) { if (b->heap) kfree(b->buf); }
```

`syscall_obj_read` becomes: get a bounce, one `io->read(obj, buf,
min(len, cap))`, one `copy_to_user`, put the bounce. What one object
call returns is what the read returns: a pipe, a tty or a socket that
has 300 bytes returns 300 bytes exactly as today (their semantics do not
change, only the ceiling), and a file returns up to 64 KiB, which is
what `pagecache_read` already copies across pages in one call
(`pagecache.c`, `pagecache_read`). `syscall_obj_write` keeps its loop
but the chunk is the bounce: a 64 KiB write is one `copy_from_user` and
one object write; a larger one is a few. The `KASSERT(rc <= n)` and the
`-EIO` for an object that reports more than offered stay.

**Why 64 KiB.** Above `kmalloc`'s largest slab class (8 KiB) an
allocation is pages from the buddy allocator, so the bounce costs an
allocation of up to sixteen pages per call; the benchmark below prices
it. 64 KiB is `BUFSIZ`'s order of magnitude times a page count that
stays cheap, and it is a ceiling, not a promise: a request for a
megabyte returns 64 KiB and the program asks again, sixteen calls where
it made a thousand.

**Why not copy straight from the page cache to the user.** The largest
win would be a `read_user` operation on the file object that copies
from the cached page to user memory with no bounce at all. It is an
interface change for every I/O object type, it puts `copy_to_user` --
which can take a demand-zero fault and allocate -- under the page
cache's lock, and its gain over one 64 KiB bounce is one memory copy
per 64 KiB. Named as the follow-up, with the benchmark that would
justify it.

**Why not a per-thread bounce.** 64 KiB per thread that ever read more
than a kilobyte, kept for the thread's life, to save one allocation per
big read. The benchmark decides whether the allocation shows; the
per-call heap bounce is the simpler ownership (no per-thread state, no
lifetime to get wrong on exit) and is what the socket path already
does.

**The Linux personality** inherits the change through the same two
functions; `readv`/`writev` iterate them per vector and gain the same
ceiling per vector. `pread`'s page bounce becomes the same helper, which
for that reason is not `static`: `syscall_bounce_get`/`_put` and
`IO_BOUNCE_MAX` are declared in `kernel/include/kernel/syscall.h`
beside `syscall_handle_read`, defined once in `native.c`, and
`compat/linux/syscalls.c` calls them -- one limit and one fallback for
both personalities, so `pread` of a page-and-a-half is one call.

**The tty and the pipe** are untouched: a canonical-mode `tty_read`
returns one line whatever the request; a pipe returns what it holds up
to the request. The only reader whose *result* changes is one that
asked for more than a kilobyte from an object that had more than a
kilobyte to give.

### An error that reaches the program

The rule, in three parts:

1. **The page cache remembers.** Every write-back failure is recorded
   where it is seen: `pagecache_sync` itself, on the `writepage` or
   `writepages` error that stops its loop, sets `pc->wb_err` (the errno)
   and increments `pc->wb_seq`, under `pc->lock`, which it holds there.
   No caller records and no caller's locking matters: the four callers
   above hold different things (`vnode_release` holds nothing, and
   needs nothing), and the one lock they all pass through is the page
   cache's own. A small accessor, `pagecache_error_since(pc, seen,
   &err)`, reads the pair under the same lock and says whether a
   failure was recorded after sequence `seen`.
2. **Each open file hears once.** `struct file` gains `wb_seq_seen`, set
   at open to the page cache's `wb_seq` (an error before this open is
   not this file's). `file_sync` and `file_flush` take `f->lock` and
   then `vn->lock` -- the order every file operation that takes both
   already uses (`file_pwrite`, `vfs.c:1197-1198`) -- run the write-back
   as today, and then consult the record: if a failure was recorded
   after `wb_seq_seen` (this attempt's own, or a neighbour's since), they
   return that errno **and advance `wb_seq_seen` to the current
   sequence**, so the same failure is reported once and the next
   successful `fsync` returns 0. A file's own failed attempt is thus
   reported through the same rule as a neighbour's, and marks itself
   seen. This is the shape of Linux's `errseq_t` without the wrapping
   arithmetic, because a 32-bit sequence under a mutex does not wrap in
   this kernel's lifetime.
3. **`close` asks before it lets go.** `struct kobject_io_type` gains an
   optional `int (*flush)(struct kobject *obj)`, called by
   `handle_close` on the object it is about to put, *before* the put;
   its return value is `close`'s return value, and the handle is gone
   either way (POSIX: `close` may fail with `EIO`, and the descriptor is
   closed regardless). The file's `flush` is `file_flush`: for a regular
   vnode with dirty pages and a `writepage`, the write-back under
   `f->lock` then `vn->lock`, then the once-per-file report above.
   `file_release` keeps its last-reference write-back -- for a file that
   reached zero references some other way (a `dup`ed handle closed last,
   an exiting process) -- whose failure `pagecache_sync` records like
   any other; `vnode_release`'s last attempt records the same way, and
   what it then drops is **counted** (`pagecache_stats.dropped_dirty`,
   per page) and **said once per event**
   (`kwarn("vfs: %u dirty page(s) of inode %llu on %s lost: write-back
   failed (%d)")`), because at that point no file exists to tell.

`exit`'s close-all (`handle_table_destroy`) calls `handle_close`'s
sibling that ignores the flush result: nobody is there to read it; the
count and the line are what remains. `dup2` replacing an open handle
ignores the flush result too, as Linux does. The Linux personality's
`close` is `handle_close` (`syscalls.c:338`) and inherits everything.

**What does not change.** `pagecache_sync` still stops at the first
error and leaves the failed pages dirty, so a `close` that returned
`-EIO` because of a transient refusal has *not* thrown the data away:
the file object's release retries once more, and a later `fsync` on
another open file retries again; only the vnode's release, when the
last reference goes, gives up -- and counts. Nothing in cosmofs's
transaction model moves: a data-page refusal stays a returned error,
not a poisoned mount.

**Why a hook on the object type and not a file-specific close syscall.**
`sys_close` and `lx_close` are `handle_close`, and `handle_close` knows
objects, not files. A `flush` hook is the smallest generic addition: a
pipe, a socket, a device leave it NULL and close as today; a file uses
it; a future object with buffered state (a tap, a console) can use it.
It is the shape Linux gives `f_op->flush` for the same reason.

### The §70 gate

**Correctness.** A read returns what one object call returns, up to
64 KiB; a write-back error is recorded where the data lives (the
vnode), reported once to each file that was open when it happened, and
counted when no file can be told. `close` returns the error and closes.

**Concurrency.** `wb_err`/`wb_seq` are written by `pagecache_sync`
under `pc->lock`, the one lock every write-back passes through, and
read through `pagecache_error_since` under the same; `wb_seq_seen` is
per file, written under `f->lock`, which `file_sync` and `file_flush`
take before `vn->lock` in the order the file operations already use
(`vfs.c:1197-1198`), and at open before the file is visible. The
bounce is per call, on the calling thread's stack or heap; no shared
state. `handle_close` calls `flush` outside the table lock, on the
reference it took from the slot, before `kobject_put` -- where it
already calls the put "outside the lock: release may block".

**Ownership / Lifetime.** The heap bounce is freed on every path out of
the two functions (one exit each). The flush hook runs on an object the
closer still references; the release runs after it.

**Failure.** A heap bounce that cannot be allocated degrades to the
stack chunk: a read under memory pressure is slow, never `-ENOMEM`. A
flush that fails still closes. A write-back that fails leaves pages
dirty for the next attempt.

**Security.** The bounce is kernel memory the user never sees; the
copies go through `copy_to_user`/`copy_from_user` as today, with the
same range check first. A larger bounce is a larger `kmalloc` under a
user's control: bounded at 64 KiB per call, per thread, freed before
the call returns, and the thread's memory rlimit does not count it --
stated, and the same as the socket path's bound today. No new syscall,
no new flag.

**Performance.** The point of the first half; the benchmark below is
the evidence. The second half adds one compare per `fsync`/`close` and
one write-back that `file_release` was already doing, moved earlier.

**Scalability.** Per call, per file, per vnode; nothing global.

**Portability.** Generic code only.

**Testing.** Deterministic: `ramblk` completes in the submitter's
context, `FI_BLK_COMPLETE` with `only = thread_current()` refuses
exactly the write-backs the test issues; a ramfs file of a known size
answers a 64 KiB read with a known count.

## Affected files

| file | change |
| --- | --- |
| `kernel/syscall/native.c`, `kernel/include/kernel/syscall.h` | `IO_BOUNCE_MAX`, `struct io_bounce`, `syscall_bounce_get`/`_put` (declared in the header, defined once); `syscall_obj_read` one call up to the bounce; `syscall_obj_write` looping the bounce |
| `compat/linux/syscalls.c` | `lx_pread` through the same bounce helper (one call up to 64 KiB) |
| `kernel/include/kernel/object.h` | `flush` in `struct kobject_io_type` |
| `kernel/object/handle.c`, `kernel/include/kernel/handle.h` | `handle_close` calls `flush` before the put and returns its result; `handle_table_destroy` ignores it |
| `kernel/include/kernel/vfs.h` | `file.wb_seq_seen`; `file_flush` |
| `kernel-services/vfs/vfs.c` | `file_flush`; `file_sync` reports once (both under `f->lock` then `vn->lock`); `vfs_open` sets `wb_seq_seen`; `vnode_release` counts and says what it drops; the file type's `flush` |
| `kernel-services/vfs/pagecache.c`, `kernel/include/kernel/pagecache.h` | `pagecache.wb_err`, `pagecache.wb_seq`, recorded by `pagecache_sync` under `pc->lock`; `pagecache_error_since`; `pagecache_stats.dropped_dirty`; `pagecache_drop` counts dirty pages it drops |
| `kernel-services/vfs/vfstest.c` | `read-fill`, `read-bounce-fallback`, `wb-error-fsync`, `wb-error-close`, `wb-error-once`, `wb-error-lost`, `read-bench`, `write-bench` |
| `kernel/core/selftest.c`, `kernel/include/kernel/selftest.h` | registration |
| `userland/init/init.c` | `fs_selftest`: a 64 KiB read of a file larger than that returns 64 KiB (the user-mode side of the first half) |
| `docs/kernel-services/vfs/{design,api,invariants,testing}.md` | the error record, the flush, the counts; the tests |
| `docs/kernel/syscall/{api,architecture}.md` | `read`/`write` return up to 64 KiB per call; `close` may return `-EIO` |
| `docs/kernel/object/api.md` | the `flush` hook |
| `docs/verification/design.md` | the block injection's new users |
| `README.md`, `docs/README.md`, `docs/audit/2026-09-deferred-work-inventory.md` | the Status entry; the index; the two §3 rows struck through |

**No change** to any I/O object's `read`/`write`, to the page cache's
write-back order or its stop-at-first-error rule, to cosmofs, to the
socket path's own bounce, or to the UAPI (no new number, no new flag;
`close` returning `-EIO` is within the ABI as documented).

## New APIs

Kernel-internal only.

| API | where | contract |
| --- | --- | --- |
| `int (*flush)(struct kobject *obj)` | `kernel/object.h`, `struct kobject_io_type` | optional; called by `handle_close` before the put; its result is close's result; the handle closes regardless |
| `int file_flush(struct file *f)` | `kernel/vfs.h` | the file type's `flush`: write back dirty pages, then report a recorded error once to this file |
| `pagecache.wb_err`, `pagecache.wb_seq` | `kernel/pagecache.h` | the last write-back error and its sequence; recorded by `pagecache_sync`, under `pc->lock` |
| `bool pagecache_error_since(struct pagecache *pc, uint32_t seen, int *err, uint32_t *now)` | `kernel/pagecache.h` | under `pc->lock`: whether a failure was recorded after `seen`, its errno, and the current sequence |
| `file.wb_seq_seen` | `kernel/vfs.h` | the sequence this file has been told about; set at open |
| `pagecache_stats.dropped_dirty` | `kernel/pagecache.h` | dirty pages dropped at a vnode's release after a failed write-back |
| `IO_BOUNCE_MAX`, `struct io_bounce`, `syscall_bounce_get`/`_put` | `kernel/syscall.h` | 64 KiB; the ceiling of one read or one write chunk; the helper both personalities use |

## Migration plan

1. **The bounce.** `io_bounce`, the two functions, `lx_pread`. Tests
   `read-fill` and `read-bounce-fallback`; the user-mode read in
   `fs_selftest`. `read-bench` and `write-bench` at 1 KiB, 4 KiB, 64 KiB
   requests, run before the change (the baseline is recorded in the
   report from the old code, since the benchmark is new) and after.
   Boots green on both architectures.
2. **The record and `fsync`.** `pagecache_sync` records; `wb_seq_seen`;
   `file_sync`'s once-report in the file-then-vnode order; `vfs_open`'s
   seen. Tests `wb-error-fsync` and `wb-error-once`.
3. **`close`.** The `flush` hook, `handle_close`, `file_flush`,
   `file_release` and `vnode_release` recording, the count and the line.
   Tests `wb-error-close` and `wb-error-lost`. `faulttest`'s block
   section gains the close it could not check.
4. **Documents.** As listed; the two inventory rows struck through with
   this PR's number; the README Status entry; the index; the report as
   built.

Each step is a commit that boots green on both architectures.

## Tests

In `kernel-services/vfs/vfstest.c`, registered after `fsync-handle`. The
write-back tests mount cosmofs on a `ramblk` (the fixture of
`faulttest.c:185`), which completes every bio in the submitter's context
in its default mode, so `faultinject_set(FI_BLK_COMPLETE, 1, budget,
thread_current())` refuses exactly the next `budget` write-backs this
thread issues and nothing else on the machine. The read tests use a
ramfs file (no device) of a known content.

| test | asserts | bug-proof |
| --- | --- | --- |
| `read-fill` | a 200 KiB ramfs file of a known pattern: `syscall_obj_read` for 64 KiB returns 64 KiB with the right bytes; for 100 KiB returns 64 KiB; for 512 bytes returns 512; at 3 KiB from the end returns 3 KiB; a pipe holding 300 bytes answers a 64 KiB request with 300 (the ceiling rose, the semantics did not) | cap the bounce at `IO_CHUNK` -> the 64 KiB request returns 1 KiB |
| `read-bounce-fallback` | `FI_KMALLOC` for this thread, one failure: a 64 KiB read returns 1 KiB (the stack chunk), not `-ENOMEM`; the next returns 64 KiB | return the allocation failure -> `-ENOMEM` |
| `wb-error-fsync` | write 3 pages; one refused completion; `file_sync` is `-EIO`; `nr_dirty` is still 3 (nothing thrown away); `file_sync` again is 0; after umount and mount the pages read back | do not advance `wb_seq_seen` when reporting the file's own failure -> the second `fsync` returns `-EIO` again; swallow the record in `pagecache_sync` -> the first `fsync` returns 0 |
| `wb-error-once` | files A and B open on one vnode; A writes; one refused completion at A's `fsync` (`-EIO`); B's `fsync` succeeds in writing and returns `-EIO` once (the error was recorded while B was open); B's second `fsync` is 0; C, opened after, `fsync`s to 0 | do not record in `pagecache_sync` -> B's first `fsync` is 0; do not set `wb_seq_seen` at open -> C reports an error older than itself |
| `wb-error-close` | a file in a handle table, dirty; one refused completion; `handle_close` returns `-EIO`; a second `handle_close` on the same slot is `-EBADF` (closed regardless); the release's own retry wrote the data (injection exhausted): after umount and mount it reads back; `dropped_dirty` unchanged | `handle_close` without the flush -> returns 0 |
| `wb-error-lost` | as above with two refused completions (the flush and the release both fail), the table destroyed: `dropped_dirty` grows by the dirty page count, the `lost` line is logged once, the mount is not poisoned (a new file writes and syncs) | drop without counting -> the count is unchanged |
| `read-bench`, `write-bench` | print, assert nothing: a 1 MiB ramfs file read and written through `syscall_obj_read`/`_write` at 1 KiB, 4 KiB and 64 KiB requests, MiB/s and calls per MiB, and the same over a cosmofs file on `ramblk`; the shape of `blk-bench` | -- |
| `fs_selftest` (`init --selftest`) | a 64 KiB `read` of `/boot/...` (a file larger than that) returns 64 KiB | -- (the kernel test above is the proof; this is the syscall's wiring) |

**Vacuity, named in advance.** `read-fill` compares content, not only
counts, so a bounce that returns 64 KiB of the wrong page fails. The
`wb-error-*` tests assert `faultinject_stats().hits` equals the budget
after each phase, so a test in which the injection never fired -- the
write-back never happened, or happened on another thread -- fails on
that count rather than passing on a 0 that meant "nothing was tried".
`wb-error-lost` also asserts the mount is usable afterwards, so a
"count" that came from a poisoned mount's refusals is told apart from a
dropped page.

### As run

**The suite** (2026-09-14): 265 self-tests pass on both architectures.

**The benchmarks**, x86-64 under TCG (a single boot; the spread across
boots is the network benchmarks' order, a few percent):

```
read-bench: ramfs 1 KiB requests: 1024 calls, 22641 us, 44 MiB/s
read-bench: ramfs 4 KiB requests: 256 calls, 7098 us, 140 MiB/s
read-bench: ramfs 64 KiB requests: 16 calls, 3146 us, 317 MiB/s
read-bench: cosmofs cold 1 KiB requests: 1024 calls, 90798 us, 11 MiB/s
read-bench: cosmofs cold 64 KiB requests: 16 calls, 68830 us, 14 MiB/s
read-bench: cosmofs warm 1 KiB requests: 1024 calls, 20308 us, 49 MiB/s
read-bench: cosmofs warm 64 KiB requests: 16 calls, 3035 us, 329 MiB/s
write-bench: ramfs 1 KiB requests: 1024 calls, 24398 us, 40 MiB/s
write-bench: ramfs 64 KiB requests: 16 calls, 6901 us, 144 MiB/s
USERBENCH: read 200 KiB at 1 KiB requests: 200 calls, 5031 us, 38 MiB/s
USERBENCH: read 200 KiB at 4 KiB requests: 50 calls, 2050 us, 95 MiB/s
USERBENCH: read 200 KiB at 64 KiB requests: 4 calls, 1765 us, 110 MiB/s
```

AArch64:

```
read-bench: ramfs 1 KiB requests: 1024 calls, 15570 us, 64 MiB/s
read-bench: ramfs 64 KiB requests: 16 calls, 2609 us, 383 MiB/s
read-bench: cosmofs cold 1 KiB requests: 1024 calls, 74914 us, 13 MiB/s
read-bench: cosmofs warm 1 KiB requests: 1024 calls, 15460 us, 64 MiB/s
read-bench: cosmofs cold 64 KiB requests: 16 calls, 57700 us, 17 MiB/s
read-bench: cosmofs warm 64 KiB requests: 16 calls, 2706 us, 369 MiB/s
write-bench: ramfs 1 KiB requests: 1024 calls, 19138 us, 52 MiB/s
write-bench: ramfs 64 KiB requests: 16 calls, 5956 us, 167 MiB/s
USERBENCH: read 200 KiB at 1 KiB requests: 200 calls, 3846 us, 50 MiB/s
USERBENCH: read 200 KiB at 64 KiB requests: 4 calls, 1270 us, 153 MiB/s
```

The per-call cost is what the figures show: in the kernel the copy of
1 MiB costs seven times more in 1 KiB calls than in 64 KiB ones; from
user mode the whole syscall three times. A cold cosmofs read is the
device, not the call (11 to 14 MiB/s at every request size: the block
reads dominate); warm it is the copy again. The heap bounce's allocation
does not show against the copy (the 64 KiB row is the fastest).

**The bug-proofs** (each injected, the suite booted):

| injection | failed as |
| --- | --- |
| the bounce capped at the stack chunk | `read-bounce`: the 64 KiB request's cap check |
| the allocation failure returned instead of the fallback | `read-bounce`: the fallback check (no buffer) |
| the record swallowed in `pagecache_sync` | `wb-error-fsync`: `wb_errors` unchanged; `wb-error-once`: B's first `fsync` is 0 |
| a file's own failure not marking itself seen | `wb-error-once`: A's second `fsync` is `-EIO` again |
| an opener starting at sequence 0 | `wb-error-once`: C reports an error older than itself |
| `handle_close` without the flush | `wb-error-close`: `close` returns 0 |
| a drop without the count | `wb-error-lost`: `dropped_dirty` unchanged |

## Benchmarks

1. **`read-bench` / `write-bench`**, the audit's missing "`read`/`write`
   bandwidth at 1 KiB chunking": a 1 MiB file, three request sizes, on
   ramfs (the copy alone) and on cosmofs over `ramblk` (with the page
   cache and the filesystem under it). Before the change every request
   size costs the same per byte (every call moves a kilobyte); after it
   the 4 KiB and 64 KiB rows should show the per-call cost gone, and the
   64 KiB row should show whether the heap bounce's allocation is
   visible against the copy (it is sixteen pages from the buddy
   allocator against a 64 KiB `memcpy`; expected to be small; measured,
   not assumed). Both architectures, five boots, min-max and median as
   the network benchmarks report.
2. **`net-bench`**, unchanged: the socket path has its own bounce and
   is not touched; its figures must stay within their spread.
3. **The close path**: `wb-error-close` prints the time of a `close`
   with three dirty pages, before (the release's write-back) and after
   (the flush's); the same work, moved earlier -- expected equal.

## Risks

- **A larger heap allocation per call, under a user's control.**
  Bounded (64 KiB), transient (freed before the call returns), one per
  thread at a time, and it degrades to the stack chunk when refused.
  The socket path has made the same choice since Phase 8.
- **A program that treats `close`'s `-EIO` as "still open".** POSIX
  says the state of the descriptor is unspecified after a failed
  `close`; this kernel documents that it is closed, as Linux does, and
  libc's `close` wrapper returns the error unchanged.
- **`close` now blocks for the write-back where the release did.** The
  same work at the same moment for the common case (the last handle);
  a `dup`ed file's first close now writes back where before only the
  last did -- so an error is reported to the closer that could hear it.
- **An error reported once is an error reported to one `fsync` of one
  file.** A program with two files on the vnode sees it on each; a
  program that opens after the error does not see it. This is the
  errseq contract and it is stated in the API doc; the alternative
  (report forever) makes every later `fsync` fail for a page that has
  since been written.
- **`dropped_dirty` counts what is gone, it does not save it.** The
  vnode's release is the end of the line; the unit makes the loss
  visible, not impossible. Reclaim (the page-cache limit) never evicts
  a dirty page, so the count grows only there.
- **Bug-proofs that need an allocation failure** use `FI_KMALLOC`
  scoped to the test thread, which the fault-injection tests already
  do; a test that cannot make the allocation fail would be vacuous and
  is not written.

## Alternatives considered

- **Loop `read` over chunks until the request is full.** Wrong for a
  pipe, a tty or a socket: the second chunk blocks after the first
  returned data, or returns less and the loop has to know why. One
  object call is the semantics every object already has; only the
  ceiling changes.
- **A `read_user` operation on the file object** (no bounce): the
  larger gain, an interface change for every object type, and
  `copy_to_user` under the page-cache lock. The follow-up, with the
  benchmark that would justify it.
- **A per-thread bounce**: state with a lifetime, for one allocation per
  big read. The benchmark decides whether the allocation shows; if it
  does, this is the next step.
- **`errseq_t` exactly** (the error and the sequence in one word with a
  "seen" bit, lock-free): the same contract; the lock this kernel
  already holds at every write-back makes the arithmetic unnecessary.
- **A close syscall that knows files**: `sys_close` is `handle_close`,
  and so is the Linux personality's; a hook on the object type reaches
  both and any future object with buffered state.
- **Keep the pages after a failed release** (never drop): a leak with
  no reader; the vnode is gone. Count and say.
- **Fix only `fsync`** (leave `close` silent): the common program calls
  `close`, not `fsync`; half the finding.
