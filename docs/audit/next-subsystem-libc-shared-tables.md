# NEXT SUBSYSTEM — the two tables threads left behind

Constitution §68: after the audit, name the next subsystem in this shape
and wait for the instruction to build it. **This report is as built**
(PR #191), and the banner below records where the build differed from
it — including the part that matters most, which is that **the
headline defect is not demonstrated by a test**.

**What the build changed, each found by building rather than reading:**

1. **The use-after-free IS demonstrated, after two corrections, and
   an earlier version of this banner said it was not.** The first
   build's readers looked up a name added *before* the padding, so
   `getenv` found it at the front and never walked the part of the
   array being reallocated — review found that, and it meant "does
   not reproduce" was uninformative rather than a result. With the
   observed name moved after the padding it still did not reproduce,
   and **that** is what identified the real reason: `setenv` copies
   the old array's pointers into the new one and frees only the
   array, so a reader on the stale array reads pointers that are all
   still correct and gets the right answer out of freed memory.

   The wrong answer needs the block **reused and overwritten** first.
   A thread churning the heap in the same size class arranges that,
   and then the unlocked build dies: **`#GP` at `0x40a940`, signal
   11, exit status 139, on three runs out of three**, dereferencing a
   `0x5A5A…` pointer read from the freed array. Deterministic, and it
   is now the strongest proof in the unit rather than the missing
   one.
2. **The deterministic construction I proposed in review does not
   work, and the reason is worth more than the construction was.**
   Review round 2 asked how the `unsetenv` window would be forced;
   I answered that the test would own the walk and pause mid-array.
   It cannot: **a walker the test owns never takes the library's
   lock**, so the lock the fix adds cannot protect it and the test
   fails identically with and without the fix — a test of nothing. A
   deterministic version has to pause *inside* `getenv`, which needs
   the libc test seam I had argued was disproportionate. The test now
   uses the real `getenv` and is honestly probabilistic.
3. **`atexit-concurrent` needed a start barrier to contend at all.**
   The first build started eight threads in a loop and joined them in
   another; each finished before the next existed, so an unlocked
   `atexit` passed. With a barrier the eight enter together and the
   unlocked build loses three handlers — **the drain runs 6 of 9** and
   the marker fails. That is the bug-proof of record.
4. **The acceptance count cannot detect a lost update, but it does
   detect the overflow.** Every thread's `atexit` *returns* 0 even
   when its slot is overwritten, so "8 of 8 accepted" is true on a
   broken build and the **drain count** is what catches a lost
   handler. The same count catches the other defect for free, which
   the design said it could not: an unlocked build accepts
   **thirty-three** registrations into a table of thirty-two, so the
   accounting is the canary the design thought it needed memory
   inspection for.
5. **The verdict had to move into the drain.** `main` printed
   `THREADTEST: PASS` and then called `exit`, so a drain that lost a
   handler or deadlocked could not reach the marker. The last handler
   prints the verdict now, which also means the LIFO order matters:
   the checking handler is registered **first**.

Chosen from `docs/audit/2026-09-deferred-work-inventory.md` §1.3.

**Subsystem: `environ` and the `atexit` list, the two process-global
tables in libc that native threads made unsafe and nobody locked.**

This is not a new hazard discovered by reading. It is the *same* hazard
the threads unit already recognised twice, in the same library, and
left in a third place. `libc/src/malloc.c` opens with:

> One lock covers the whole allocator (invariants L8). Native threads
> made an unlocked free list a way to corrupt a heap silently, which is
> worse than any other, so it is locked here rather than left to a rule
> callers must know.

`libc/src/stdio.c` took a lock in the same unit. `errno` went
per-thread in the unit after it. `libc/src/stdlib.c` kept `environ`,
`g_env_owned`, `g_atexit[]` and `g_natexit` exactly as they were.

## What is established

**Threads are real.** `cosmo_thread_start`, `cosmo_thread_join` and
`cosmo_thread_id` are the public interface
(`libc/include/cosmo/thread.h`), shipped by the native-threads unit,
and `thrtest` proves them from userland behind a boot marker.

**The mutex the fix needs already exists and is already used twice.**
`cosmo_mutex_t g_io = COSMO_MUTEX_INIT` in `stdio.c`,
`cosmo_mutex_t g_lock = COSMO_MUTEX_INIT` in `malloc.c`. There is no
new primitive, no new dependency, and a precedent for the shape.

**The invariant that governs this says "three".** L8
(`docs/libc/invariants.md`) is titled *"The allocator, stdio and
`errno` are each safe from more than one thread"* and ends the
enumeration with **"All three are done"**. The library has five
process-global mutable things, not three. The two it does not name are
the subject here, and the inventory row states the gap in the same
words: *"`atexit`'s table and the environment remain process-global and
unsynchronised"*.

So this report is not arguing that the hazard exists — the tree already
says so in two places. It is about closing it and making L8 count
correctly.

## The problem

### `setenv` frees the array a concurrent reader is walking

`libc/src/stdlib.c`, growing the environment:

```c
    if (g_env_owned)
        free(environ);
    environ = nenv;
```

`getenv` and `env_count` walk `environ[i]` with no lock. A thread
inside either, when another calls `setenv` with a name not already
present, is walking freed memory. **This is a use-after-free in the
allocator's own heap**, and it is the same class the threads unit
called "worse than any other" when it locked malloc — reached here
through the table malloc was locked to protect.

It needs no unlucky interleaving to be wrong: the reader holds a
pointer into a block that `free` has returned to the free list, and the
next `malloc` from any thread can hand that block out.

### `unsetenv` shifts the array under a reader

```c
    memmove(&environ[i], &environ[i + 1], (n - i) * sizeof(char *));
```

A concurrent `getenv` can see an entry twice or miss one entirely, and
a reader that has already passed `i` runs against a stale tail because
the array shrinks without the terminator moving first.

**What `unsetenv` does *not* do is invalidate memory, and an earlier
draft of this report said it did.** It shifts pointer *values* within
the array and frees no string, so a `char *` a reader has already
loaded stays valid. The hazard here is an inconsistent traversal — a
wrong answer — and saying "use-after-free" about it would have sent an
implementer looking in the wrong place and blurred the one place the
use-after-free is real, which is `setenv` freeing the array itself.

### `atexit` loses handlers, and can write past its array

```c
    if (g_natexit >= ATEXIT_MAX)
        return -1;
    g_atexit[g_natexit++] = fn;
```

`g_natexit++` is a read-modify-write. Two threads registering at once
can both read the same index and one handler is silently dropped —
which for `atexit` means a file not flushed or a lock not released at
exit, with nothing to indicate it. Worse, the bound check and the
increment are separate: two threads can both pass `>= ATEXIT_MAX` at
31 and write `g_atexit[32]`, **one past the end of a static array**.

### `exit` runs the list while another thread may be adding to it

```c
    while (g_natexit > 0)
        g_atexit[--g_natexit]();
```

A registration racing this can be skipped, or run twice, or land in a
slot the loop has already passed.

### One thing that is safe, and safe by accident

`getenv` returns a pointer *into* the environment's string, and
`setenv` replacing a value does:

```c
        environ[i] = e;   /* the old string may be the kernel's: leaked, not freed */
```

The old string is **leaked**, so a pointer a reader already holds stays
valid. That is the correct behaviour and it is held up by a deliberate
leak rather than by a rule. The build should keep the leak and say why,
because the obvious tidying — freeing the old string — would turn a
safe return value into a dangling one.

### Why it has not bitten

**Nothing in the tree uses threads and the environment together.**
`setenv` and `getenv` are called by `init` and the shell, neither of
which creates threads; `thrtest` creates threads and does not touch the
environment. The race is **latent and reachable**, not observed, and
this report says so rather than implying a failure it cannot point to.
That is also why it is worth doing now: the cost of closing it is a
lock, and the cost of finding it later is a use-after-free in a program
nobody suspects.

## Design

**One lock for `stdlib.c`'s tables, in the shape `malloc.c` and
`stdio.c` already use.** A file-static `cosmo_mutex_t` with
`COSMO_MUTEX_INIT`, taken by the **public entry points**: `setenv`,
`unsetenv`, `getenv`, `atexit` and `exit`'s drain.

**`env_count` is an unlocked helper and must stay one.** Both
mutators call it, so a version that took the lock itself would
reacquire a non-recursive mutex and every environment mutation would
deadlock against itself — not occasionally, always. A first draft of
this design listed `env_count` among the functions that take the
lock, which is that bug written down; review caught it. This is
exactly the split `malloc.c` uses and this report quotes two sections
above — *"the public functions take the lock once and call an
unlocked core, because `calloc` and `realloc` are written in terms of
`malloc` and `free` and the mutex is not recursive"* — so the pattern
was already in front of me. **Any helper added under the lock follows
the same rule**, and the build should keep the locked and unlocked
halves visibly separated rather than relying on the reader to know
which is which.

**Two tables, and the choice of one lock or two is a real one.** The
environment and the `atexit` list share nothing, so two locks would be
defensible and would avoid a program that registers handlers from one
thread while another reads the environment contending. One lock is the
proposal, because the contention is theoretical, `stdio.c` covers an
entire subsystem with one, and a second lock is a second chance to take
them in the wrong order. **If the build finds a reason for two, the
banner should record it.**

**The recursion trap, named because the mutex is not recursive.** L8
already records that malloc's is not, which is why its public functions
take the lock once and call an unlocked core. Two paths here have the
same shape and one is worse:

- `setenv` calls `malloc` — a different lock, so no self-deadlock, but
  the order `env → malloc` must be the only order. Nothing takes
  malloc's lock and then calls `setenv`, and the build should check
  rather than assume.
- **`exit` must not hold the atexit lock while running a handler.** A
  handler is arbitrary program code and may call `atexit` or `getenv`.
  Holding across the call deadlocks the program at exit, which is the
  worst possible time. The drain therefore takes the lock, removes one
  handler, releases, and calls it — so the list is consistent at every
  moment and no user code runs under the lock.

**`getenv`'s return value stays a pointer into the table, and the
contract is written down.** A lock inside `getenv` protects the walk,
not the pointer it returns; POSIX allows a later `setenv` to invalidate
it. Here it happens to stay valid because the old string is leaked, and
the design keeps that deliberately — the alternative is a copy, which
changes the signature's ownership, or a dangling pointer.

**What this does not do:** no per-thread environment, no `clearenv`, no
fix for the leak (it is load-bearing, see above), and no change to
`exit`'s ordering guarantees beyond making the list safe to read.

## Affected files

| file | change |
| --- | --- |
| `libc/src/stdlib.c` | the lock, taken by the public entry points `setenv`, `unsetenv`, `getenv`, `atexit` and `exit`'s drain — **`env_count` stays unlocked** and is called under the mutators' lock, or they deadlock against themselves; the drain restructured so no handler runs under it |
| `docs/libc/invariants.md` | **L8** counts five, not three, and the two new ones get the same "shape is set by its consequence" treatment |
| `userland/tests/thrtest.c` | the cases below, behind the existing `THREADTEST: PASS` marker |
| `docs/libc/design.md` | the `getenv` contract and the deliberate leak, beside the allocator's and stdio's locking rules |
| `docs/libc/testing.md` | the new `thrtest` cases |
| `README.md` | the Status entry |
| `docs/audit/2026-09-deferred-work-inventory.md` | strike the §1.3 row |

No kernel change. This is entirely inside the C library.

## Tests

`thrtest` is the right home: a kernel self-test cannot make a *user*
thread, which is why that program exists.

| test | asserts |
| --- | --- |
| `env-grow-under-readers` | one thread calling `setenv` with fresh names while others loop in `getenv`; every reader either finds its name or does not, and none reads a freed pointer |
| `env-unset-under-readers` | against `unsetenv`, whose hazard is a **wrong answer** and not freed memory: a reader must never fail to find a name that was never removed. The reader is the test's own copy of `getenv`'s walk, paused at a chosen index, so the interleaving is forced rather than hoped for — see below |
| `atexit-concurrent` | N threads each registering a distinct handler; **exactly** the number registered run at exit, and none runs twice |
| `atexit-bound` | more registrations than `ATEXIT_MAX`, concurrently: the surplus is refused with `-1` and nothing is written past the array |
| `exit-drain-reentrant` | a handler that itself calls `atexit` and `getenv` completes rather than deadlocking — the case the drain's shape exists for |

**The bug-proof, as run.** The section below was written before the
build and was wrong about which tests prove anything. What the
measurements say:

| mutation | result |
| --- | --- |
| `atexit` unlocked (its read-modify-write split by a delay) | **`THREADTEST: FAIL 3`**. The flood is accepted **24 of 24** and the table reports holding **33** against an `ATEXIT_MAX` of 32 — the write past the end of a static array, seen from userland — and the drain runs **31 of 33**, two handlers lost. Both defects, countable |
| `setenv` **and** `getenv` unlocked, **with the heap churned** | **the process dies**: `#GP` at `0x40a940`, signal 11, status 139, three runs of three. The use-after-free, reproduced |
| `setenv` and `getenv` unlocked, **without churn** | passes — and the reason is the finding: the stale array's pointers are still correct, because `setenv` frees the array and never a string |
| `unsetenv` unlocked | passes. Not a proof on its own; it shares the grow test's churn hazard but removes no array |

**So three of the five are proofs**, where the report promised three
deterministic and one probabilistic — right about the count and wrong
about which. `unsetenv` alone is the regression test, and the
environment's headline defect went from "argued from the code" to
"kills the process on demand" once the missing ingredient was
identified.

**The bound test sees the overflow without a canary, which I had said
it could not.** The design assumed a write past `g_atexit[31]` would
need memory the test cannot inspect. It does not: the test counts what
`atexit` *accepted*, and an unlocked build accepts thirty-three
registrations into a table of thirty-two. The accounting is the
canary.

**The three conditions, and why two of them had to be arranged.** A
use-after-free is observable only if the freed block is reused, *and*
its contents change, *and* the reader looks after both. The reader
looking was the first correction (the observed name moved behind the
padding); the reuse was the second (a churn thread in the same size
class). Neither is exotic — any other thread allocating does the
second — but neither happens by itself in a test whose only
allocations are the environment's own.

**What this says about the hazard in the field** is worth more than
the test: a program whose threads only touch the environment will
probably never see this, and a program whose threads also allocate —
which is most of them — is one `setenv` away from dereferencing a
recycled block. That is the argument for a lock in a cold path, and
it is now measured rather than asserted.

**No libc test seam was needed after all.** The design floated a
debug-only park hook inside `getenv` as the way to force the window,
and called it disproportionate. It is also unnecessary: churn plus a
reader that actually walks the array reproduces the fault every time,
with nothing added to the library.

## Risks

- **A lock in `getenv` makes it not async-signal-safe.** POSIX does not
  require it to be, and nothing in this tree calls it from a handler,
  but the build should grep rather than assume — a signal handler that
  calls `getenv` while the main thread holds the lock deadlocks that
  thread.
- **Deadlock at exit is the failure this design most has to avoid**,
  which is why no handler runs under the lock. `exit-drain-reentrant`
  is the test for it and is the one to write first.
- **Contention is not a concern and saying so is part of the design.**
  These are cold paths — a program reads its environment at start-up
  and registers handlers once. A lock here costs nothing measurable,
  which is the argument for one lock rather than two.
- **It may find that nothing in the tree can reach the race**, since no
  current program mixes threads and the environment. That is a result
  about coverage, not about correctness: the library is a public
  interface and the next program to do both is not this tree's to
  predict.

## Alternatives considered

- **Per-thread environments.** What some systems do, and much larger:
  it changes `execve`'s inheritance, `spawnvp`'s `envp` and the meaning
  of `environ` as a public symbol. The defect here is a missing lock,
  and the fix should be the size of the defect.
- **Atomics instead of a lock for `atexit`.** A compare-exchange on
  `g_natexit` would close the lost update and the bound, and would not
  help the environment at all, so the file would still need a lock —
  and two synchronisation mechanisms in one file is worse than one.
- **Document the tables as single-threaded and refuse to fix them.**
  What L8 used to do for the whole library: it read *"the library is
  single-threaded and says so"*. That was honest when it was true, and
  the threads unit retired it for the allocator and stdio on the
  grounds that a rule callers must know is not a safety property. The
  same argument applies here; taking the other side now would be
  inconsistent with the invariant's own history.
- **Wait for a program that hits it.** The cost of waiting is a
  use-after-free found in something else's bug report, and the cost of
  acting is a mutex in a cold path.
