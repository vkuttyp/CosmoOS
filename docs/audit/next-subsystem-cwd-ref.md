# NEXT SUBSYSTEM — the reference a path walk never takes

Constitution §68: after the audit, name the next subsystem in this shape
and wait for the instruction to build it. This is that report, and
**nothing in it is implemented**.

**Subsystem: the mutable per-process objects a system call dereferences
without owning.** There is one today — the current directory — and
sixteen call sites read it with no lock and no reference, then hand the
raw pointer to the VFS. A second thread calling `chdir` swaps that
pointer and drops what may be its last reference, and
`vnode_put` on the last reference unhashes the vnode and frees it. **That
is a use-after-free reachable by any unprivileged program with two
threads**, and native threads are what made it reachable.

## Problem

Every relative-path system call resolves against the process's current
directory. All of them read it like this:

```c
rc = vfs_open(process_current()->cwd, path, flags, mode, &f);   /* native.c:394 */
```

No lock is taken and no reference is acquired. The pointer is loaded once
and passed into a path walk that may block — on a disk read, on a mount's
lock, on a permission check that touches the vnode's fields.

Meanwhile `process_chdir` (`kernel/process/process.c`) replaces it:

```c
arch_irq_state_t s = spin_lock_irqsave(&cur->lock);
struct vnode *old = cur->cwd;
cur->cwd = vn;
strlcpy(cur->cwd_path, newpath, sizeof(cur->cwd_path));
spin_unlock_irqrestore(&cur->lock, s);
if (old)
    vnode_put(old);        /* <- may be the last reference */
```

The swap is correctly locked. The **put is not the problem either**. The
problem is the other thread, which is already inside `vfs_open` holding a
pointer to `old`.

And `vnode_put` really does free:

```c
void vnode_put(struct vnode *vn)
{
    struct mount *mnt = vn->mnt;
    if (!kobject_put_and_lock(&vn->obj, &mnt->lock, &s))
        return;
    if (!list_empty(&vn->hash_link)) {
        list_remove(&vn->hash_link);       /* the hash holds no reference */
        mnt->nr_vnodes--;
    }
    spin_unlock_irqrestore(&mnt->lock, s);
    kobject_release_final(&vn->obj);       /* freed */
}
```

The mount's hash is a **weak** cache: the vnode is removed from it on the
final put rather than keeping it alive. So there is no second reference to
save the racing thread.

### The shape of the attack, which needs no privilege

```c
/* thread A */                      /* thread B */
for (;;) {                          for (;;)
    chdir("/tmp/a");                    open("f", O_RDONLY);   /* relative */
    chdir("/tmp/b");
}
```

`/tmp/a`'s vnode is referenced by the cwd and by nothing else once the
path walk that created it has finished. Thread A's second `chdir` drops
that reference while thread B is inside `vfs_open` using it.

### `process_chdir` races itself, too

Before it takes the lock, it reads **two** pieces of the state it is about
to change:

```c
int rc = path_normalize(cur->cwd_path, path, newpath, sizeof(newpath));
...
rc = vfs_lookup(cur->cwd, path, &vn);
```

`cur->cwd_path` is a 1024-byte buffer that another thread's `chdir` is
`strlcpy`ing into. A reader can see a **torn path** — the head of the new
directory and the tail of the old — and normalise a relative path against
a directory that never existed. `cur->cwd` is the same unowned pointer as
every other site.

## Current implementation

**Sixteen call sites**, and the second door is the larger half:

| file | sites | calls |
| --- | --- | --- |
| `kernel/syscall/native.c` | 6 | `vfs_open` (394), `vfs_stat` (418), `vfs_mkdir` (467), `vfs_unlink` (474), `vfs_rmdir` (481), `vfs_rename` (491) |
| `compat/linux/syscalls.c` | 10 | `openat` (306), `stat`/`lstat` (368, 397), the `mkdir`/`rmdir`/`unlink`/`access` group (444–451), `mkdirat` (469), `unlinkat` (481–482), `newfstatat` (495) |
| `kernel/process/process.c` | 1 | `process_chdir`'s own two unlocked reads |

**The Linux door is not an afterthought here.** `compat/linux` has had
real threads longer than the native ABI has: `LX_CLONE_THREAD`
(`syscalls.c:1459`) shares the address space and the process, so a
threaded Linux binary has been able to reach this since before
`SYS_thread_create` existed. A fix confined to `native.c` would be
**half-enforced**, which is a mistake this repository has made before and
written down (`docs/audit/next-subsystem-threads.md`, the "second door"
lesson).

### What is already right, and is the model for the fix

`sys_getcwd` takes the lock to read the path (and is the one site that
needs the path alone, so it keeps doing exactly this):

```c
arch_irq_state_t s = spin_lock_irqsave(&p->lock);
size_t n = strlcpy(buf, p->cwd_path, sizeof(buf));
spin_unlock_irqrestore(&p->lock, s);
```

So the discipline exists in the tree; it is applied at one site out of
seventeen. This unit is not inventing a rule, it is finishing one.

### What is *not* affected, checked rather than assumed

- **`p->root`, `p->mntns`, `p->utsns`** are referenced objects on the same
  structure and are **set only at spawn** (`process.c:501`, `504`, and the
  namespace fields' own comments). Immutable for a process's life, so an
  unlocked read of them is sound and stays sound.
- **`p->handles`** is a table with its own spinlock, and `handle_lookup`
  returns a *referenced* object which every caller releases — the pattern
  this report wants for the cwd already exists there.
- **`p->space`** carries a spinlock and is not swapped.
- **`p->syscall_mask`** is read without the lock, and that is deliberate
  and safe: writes take `p->lock` and are per-word atomic stores, reads are
  per-word atomic loads, and the mask only ever narrows. Its comment says
  "written only by the process itself ... so a reader needs no lock", which
  was a *single-threaded* argument and is now the wrong reason for a right
  conclusion. The comment should be corrected while this unit is in the
  area; the code should not.

## Why it matters

1. **It is a use-after-free, not a stale read.** The freed object is a
   `struct vnode`, and the racing thread is inside the VFS reading its
   `type`, `mode`, `mnt` and `ops` — including a function pointer it will
   call. A freed-and-reallocated vnode is an attacker-influenced set of
   those fields.

2. **No privilege is required and no unusual configuration.** Two threads,
   `chdir`, and any relative path. The threading arc that just completed
   (`-threads.md`, `-errno-tls.md`, `-pt-tls.md`, `-condvar.md`) made
   writing that program easy and ordinary; before it, a native program had
   one thread and this code was correct.

3. **§69 puts it first.** "Correctness/security audit", then "kernel object
   lifetime hardening". This is both, and it is the kind of defect the
   audit order exists to catch: not a missing feature, but an invariant
   that silently stopped holding when something else was added.

4. **It generalises into a rule the kernel does not currently state.** The
   handle table already says a lookup returns a reference the caller
   releases. Nothing says the same for the other mutable per-process
   pointers, so the next one added will be written the same way. The rule
   is worth writing down more than this one bug is worth fixing.

## Design

### One accessor, and the raw field becomes private

```c
/* kernel/include/kernel/process.h */

/* The calling process's current directory, **referenced**. Never NULL for
 * a user process. The caller releases it with vnode_put.
 *
 * A system call may not read `p->cwd` directly: another thread of the
 * same process can replace it and drop the last reference while the
 * pointer is in flight. */
struct vnode *process_cwd_get(void);

/* The directory **and** its path, from one acquisition of the lock: a
 * referenced vnode, with its normalised absolute path copied into `path`
 * (VFS_PATH_MAX). The caller releases the vnode with vnode_put.
 *
 * These are one call and not two on purpose -- see below. */
struct vnode *process_cwd_snapshot(char *path, size_t len);
```

**The two must be taken together, and a first draft of this design got
that wrong.** It offered a `process_cwd_path()` beside `process_cwd_get()`
and had `process_chdir` call them in sequence. Each is individually
correct and the pair is not: they are two acquisitions of the lock, so
the path can come from one directory and the vnode from another. Starting
in `/a`, a thread resolving `chdir("x")` normalises `/a/x`; another thread
chdirs to `/b`; the first thread's `process_cwd_get()` then answers `/b`,
the lookup resolves `/b/x`, and what gets published is the path `/a/x`
against the vnode of `/b/x`. **`getcwd` would then disagree with every
relative open in the same process** -- a worse failure than the one this
unit exists to fix, because it is silent and persistent rather than a
crash. Review found it in the report, which is the cheapest place to find
it.

So the snapshot is the primitive, and `process_cwd_get` is the degenerate
case of it for the callers that need no path. Normalisation and lookup
still run outside the lock; they just run against a pair that agree.

Implementation is four lines and unremarkable:

```c
struct vnode *process_cwd_get(void)
{
    struct process *p = process_current();
    arch_irq_state_t s = spin_lock_irqsave(&p->lock);
    struct vnode *vn = p->cwd;
    if (vn)
        vnode_get(vn);
    spin_unlock_irqrestore(&p->lock, s);
    return vn;
}
```

Every call site becomes:

```c
struct vnode *cwd = process_cwd_get();
rc = vfs_open(cwd, path, flags, mode, &f);
vnode_put(cwd);
```

### `process_chdir` stops racing itself

It takes its normalisation base and its lookup base from **one** snapshot:

```c
char base[VFS_PATH_MAX];
struct vnode *cwd = process_cwd_snapshot(base, sizeof(base));
rc = path_normalize(base, path, newpath, sizeof(newpath));
if (rc == 0)
    rc = vfs_lookup(cwd, path, &vn);
vnode_put(cwd);
```

The pair is coherent by construction: whatever else happens afterwards,
`base` is the path *of* `cwd` and not of some other directory.

Two concurrent `chdir`s then both succeed, and the process ends at one of
the two directories rather than at a torn mixture of both. **That is the
guarantee, and it is worth stating precisely**: this unit does not make
concurrent `chdir` atomic with respect to a concurrent path walk. A walk
that began before a `chdir` completes resolves against the *old*
directory, which is what every Unix does and what a program racing itself
should expect. What it guarantees is that the old directory is **alive**
for the whole walk.

### Making the mistake unavailable

The accessor is only half the fix; the other half is that the raw field
must stop being reachable, or the seventeenth site will be written the
same way as the first sixteen. The report proposes **renaming the field**:

```c
struct vnode *cwd_locked;   /* p->lock; use process_cwd_get() */
```

A rename is a compile error at every existing reader, which is exactly the
sweep this unit needs, and it makes a future reader's `p->cwd` fail to
build rather than fail at runtime. The name is ugly on purpose.

### The rule, written where the next author will meet it

`docs/kernel/process/design.md` gains a short section, and
`docs/kernel/process/invariants.md` an invariant: **a system call may not
dereference a mutable per-process pointer without taking a reference under
the process lock.** With the census above — which fields are mutable
(`cwd` alone today), which are fixed at spawn (`root`, `mntns`, `utsns`),
and which have their own discipline (`handles`, `space`) — so that the
next field added is classified when it is added.

### The §70 gate

**Correctness.** The unsafe window is between the load of `p->cwd` and the
last use of it inside the VFS. Taking a reference under the lock that the
swapper also takes closes it completely: the swapper cannot drop the old
reference until it has the lock, and it cannot get the lock until the
reader has taken its reference or has not yet loaded the pointer.

**Concurrency.** Any number of threads walking paths, any number calling
`chdir`. The lock is held across a pointer load and a refcount increment —
no allocation, no blocking, no nesting.

**Ownership.** The process owns one reference to its cwd; each in-flight
system call owns one more for the duration of its walk.

**Lifetime.** A vnode outlives every walk that started while it was the
cwd. The last put is then whichever finishes last — the `chdir` that
replaced it or the walk that was using it — which is the ordinary
refcount rule and needs no special case.

**Failure.** `process_cwd_get` cannot fail for a user process. It returns
NULL only for a kernel thread, which has no cwd and reaches none of these
paths; callers assert rather than branch, since a NULL there is a kernel
bug and not a user error.

**Security.** This *is* the security answer: it removes a use-after-free
reachable without privilege. It grants nothing and changes no policy.

**Performance.** One uncontended spinlock acquire and one atomic increment
per relative-path system call, against a path walk that already takes the
mount lock and may touch a disk. Benchmarks below say what to measure
anyway rather than asserting it is free.

**Scalability.** `p->lock` is per process and held for two instructions.
Threads of one process contend it; threads of different processes do not.

## Affected files

| file | change |
| --- | --- |
| `kernel/include/kernel/process.h` | `process_cwd_get`, `process_cwd_path`; `cwd` renamed to `cwd_locked` with the rule beside it |
| `kernel/process/process.c` | the two accessors; `process_chdir` stops reading the live fields |
| `kernel/syscall/native.c` | 6 sites take and release a reference |
| `compat/linux/syscalls.c` | 10 sites, the same — **the half a native-only fix would miss** |
| `kernel/syscall/syscall.c` | the syscall filter's "written only by the process itself, so a reader needs no lock" comment: right conclusion, single-threaded reason |
| `docs/kernel/process/design.md`, `-/invariants.md` | the rule, and the census of which per-process fields are mutable |
| `userland/tests/` | the new test program (see Tests) |
| `README.md` | Status entry |

## New APIs

Two kernel-internal functions. **No new system call, no ABI change, no
structure a program can see.** `struct process` is kernel-private, so
renaming a field costs nothing outside the kernel.

## Migration plan

1. **The accessors**, and `cwd` renamed. The rename makes the build fail
   at all sixteen sites, which is the point: the compiler produces the
   worklist rather than a `grep` I might trim.
2. **The native door's six sites.**
3. **The Linux door's ten.** Deliberately its own step, so that "the
   native half is done" can never be mistaken for "done".
4. **`process_chdir`'s own two reads.**
5. **The test**, and the bug-proofs — which for this unit means running the
   test against the *unfixed* tree, so ordering it after the fix is
   deliberate (see Tests).
6. **The documents**, including the census and the corrected filter
   comment.

## Tests

The defect is a race, so the test has to make it likely rather than
certain, and then the bug-proof has to make it certain.

1. **The racer**: a native program with two threads, one alternating
   `chdir` between two directories it creates, the other opening a
   relative path in a loop, for a bounded number of rounds. On a fixed
   tree it must complete with every `open` either succeeding or failing
   `-ENOENT` — never a fault, never a wrong file.

2. **The same program under the Linux door**, because the Linux personality
   is where threads have been reachable longest. `compat/linux`'s test
   programs already build with `clone`; this is one more.

3. **`chdir` racing `chdir`**, and **the transitions must be relative** —
   which is a requirement on the test and not a detail of it. Written the
   obvious way, with `chdir("/tmp/r/a")` and `chdir("/tmp/r/b")`, this
   test **cannot fail for the bug it names**: an absolute path bypasses
   the normalisation base entirely, so a torn `cwd_path` is never read,
   and `getcwd` already copies under the lock. It would pass against the
   unfixed tree.

   So the layout is fixed and the moves are relative:

   ```
   /tmp/r/a        two siblings, so that from either one
   /tmp/r/b        `../a` and `../b` are both valid moves
   ```

   Two threads alternate `chdir("../a")` and `chdir("../b")`; both
   normalise against the base, and both remain valid from either
   directory, so no move can fail for an ordinary reason and mask the
   race. A third thread reads `getcwd` and requires it to be **exactly**
   `/tmp/r/a` or `/tmp/r/b` — never a mixture, and never a path that
   normalising a torn base would produce (`/tmp/r/a/b`, `/tmp/r/`, or a
   prefix of either directory).

   The general form of this trap is worth stating, because it is the
   third time in three units that a test could have passed without
   exercising its subject: **when a test's inputs have a form that
   bypasses the mechanism under test, the report has to forbid that form,
   not hope the implementer avoids it.**

4. **The reference outlives the walk**: a directory whose only reference is
   the cwd, `chdir`ed away from while a walk is inside it. With the mount's
   vnode count exported (it exists: `mnt->nr_vnodes`), the test can assert
   the vnode is *not* freed while a walk holds it.

**Bug-proofs.** Each must fail *for its own stated reason*:

- The fix reverted at one native site (`vfs_open` reads `p->cwd_locked`
  directly) → test 1 faults or trips the poison checker. **This is the
  proof that matters, and it is the one at risk of being vacuous**: a race
  that does not happen proves nothing. The frame poisoner
  (`docs/audit/`, the rare-crash unit) is the tool this tree already has —
  poison freed frames and verify on allocation, which turned a 1-in-30
  crash into a deterministic 8-second failure once before.
- The fix reverted at one *Linux* site → test 2 fails and test 1 passes,
  which is the whole argument for step 3 being separate.
- `process_chdir` reading `cur->cwd_path` unlocked again → test 3 sees a
  torn path, **provided its moves are relative**; with absolute moves this
  proof cannot fail, which is why the layout above is part of the test's
  specification rather than left to the implementation.
- `process_chdir` taking the path and the vnode from **two** acquisitions
  of the lock rather than one snapshot → a `getcwd` that disagrees with a
  relative `open` in the same process. This is the design defect review
  found in the report, so it gets a proof of its own rather than a
  promise: test 3's third thread additionally opens a file it created in
  the directory `getcwd` reports, and requires that it exists.
- `process_cwd_get` taking the reference *outside* the lock → the window
  narrows but does not close, and the test becomes flaky rather than
  failing. **Named because it is the wrong kind of proof**: a bug-proof
  that only sometimes fails is not evidence, and this one must be argued
  from the code rather than demonstrated.

**On vacuity, which this unit is unusually exposed to.** A race test that
passes proves nothing on its own — it may have won every time. So the
test's value is conditional on the reverted-fix run *failing*, and if that
run does not fail, the test is not yet a test. The honest fallback, if the
race cannot be made to fail reliably even with poisoning, is to say so in
the report and keep the test as a regression rather than claiming it as a
proof — the shape `tests/hv/aarch64/guest_psci_race.S` already uses for a
window it could not reach.

## Benchmarks

1. **A relative-path `stat` loop**, before and after: the added cost is one
   uncontended `spin_lock_irqsave`/`vnode_get` pair per call. Expected to
   be lost in the noise of a path walk; measured because "expected to be"
   is not a number.
2. **Two threads of one process** doing relative-path calls at once, to
   show `p->lock` contention is not introduced at a rate that matters — it
   is held for two instructions, but it is now on every path-walking
   syscall of every thread.

## Risks

- **The race may not reproduce.** It is a few instructions wide on one
  side and a whole path walk on the other, which is favourable, but
  favourable is not certain. Mitigated by the frame poisoner, and by the
  report saying in advance what the outcome is if it still will not fail.
- **Sixteen mechanical edits invite a seventeenth mistake.** Mitigated by
  the rename: a site that is missed does not compile.
- **`process_cwd_get` returning NULL for a kernel thread** is a branch
  every caller would otherwise have to write. Asserting instead is right
  only if no kernel thread reaches these paths; that must be checked
  rather than assumed, and it is the one place this design could be wrong.
- **The fix is on every relative-path system call**, which is a hot path
  in a shell-heavy workload. If the benchmark surprises, the alternative
  is a per-thread cached reference invalidated by a generation counter —
  more machinery, and not justified before the number says so.

## Alternatives considered

- **Make `chdir` take a "no walks in flight" lock.** A reader-writer lock
  over the whole path walk. Correct, and far heavier: it puts a shared
  lock on every relative-path syscall and blocks `chdir` behind disk I/O.
  The refcount is what refcounts are for.
- **Never free a vnode that has been a cwd.** Removes the crash by leaking.
  Rejected: it is a resource leak dressed as a fix, and it would not help
  the torn-path half at all.
- **RCU-style deferred free for vnodes.** A real answer, and a much larger
  one — it needs a quiescence mechanism for the VFS (this kernel has
  `kernel/quiesce.h`, so it is not unthinkable). Out of proportion to a
  bug that one reference fixes, and it would still leave the torn `cwd_path`
  read.
- **Fix the native door now and the Linux door later.** Explicitly
  rejected. The Linux door is where threads have been reachable longest,
  and "a check only in `native.c` is half-enforced" is a lesson this
  repository has already paid for once.

---

Named and deferred by this unit: a general audit of *kernel-object*
pointers reachable from more than one thread beyond `struct process`; the
per-thread cached cwd reference, if the benchmark asks for it; and RCU for
the VFS.

🤖 Generated with [Claude Code](https://claude.com/claude-code)

https://claude.ai/code/session_01FtzXcfogMnEqCnyAVzZYFj
