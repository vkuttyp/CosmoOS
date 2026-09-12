# NEXT SUBSYSTEM — a thread pointer, and `errno` per thread

Constitution §68: after the audit, name the next subsystem in this shape
and wait for the instruction to build it. This is that report, and
nothing in it is implemented.

**Subsystem: the last third of what the threads unit owed. `errno` is one
global (`libc/src/errno.c`, `int errno;`), so a threaded native program
must keep every call that can fail on one thread -- a constraint
`docs/libc/invariants.md` L8 records and that
`docs/kernel/process/design.md` §12 names as the first follow-up. The
allocator and stdio took locks in that unit because an unlocked free list
corrupts memory silently; `errno` was left because it costs a *wrong error
code* rather than corruption, and because fixing it needs something the
machine does not have: **a native thread pointer**. The kernel keeps one
per thread already (`thread.tls_base`, loaded into `TPIDR_EL0` and
`MSR_FS_BASE` on every switch to user) and `SYS_thread_create` takes its
value, but there is **no way for a program to set its own**, so a process's
first thread can never have one. This unit adds that one syscall,
`SYS_set_tls`, and builds a per-thread block in libc over it: `errno`
becomes `(*__errno_location())`, the block is a page beside each thread's
stack (and one page at startup for the first thread), and a thread without
a block falls back to today's global rather than faulting. It is also the
prerequisite for the unit everyone actually wants -- `vmctl` with a thread
per vCPU -- because that program would be a multi-threaded native program
using libc, which is precisely what is unsafe until this lands.**

## Problem

- **A threaded program cannot use `errno`.** Every libc call that fails
  writes it, through one function (`__syscall_ret`), into one global. Two
  threads failing at once leave one of the two values, and neither knows
  which. The constraint is documented, and documentation is not a fix --
  the threads unit's own review made that argument about the allocator and
  it applies here too, with the difference that this one cannot corrupt
  memory.
- **The first thread of a process has no thread pointer, and cannot get
  one.** `struct cosmo_thread` carries `tls` for threads libc creates, and
  the kernel loads `thread.tls_base` on every return to user, but nothing
  exposes *setting* it. The Linux personality has `arch_prctl(ARCH_SET_FS)`
  and `clone(CLONE_SETTLS)`; a native program has neither. So even a
  program willing to do the work by hand cannot.
- **The unit that motivated threads is blocked by it.** `vmctl --machine`
  runs a guest's vCPUs on one thread, a tick each, with a fairness rule and
  a bounded power-off grace that exist only because of that. Giving each
  vCPU a thread is the named next consumer -- and each of those threads
  would call libc (`SYS_vcpu_run`, reads, writes, `printf`) and would read
  `errno` to tell `-EINTR` from a real failure. Converting `vmctl` before
  this lands would be building on the hazard.
- **Two more shared things are waiting behind it.** `strerror`'s static
  buffer and `getcwd(NULL)`'s storage are the other unsynchronised state L8
  names. Neither can be fixed without somewhere per-thread to put them,
  which is what this unit builds.

## Current implementation

**`errno`** is `int errno;` in `libc/src/errno.c`, declared `extern int
errno` in `libc/include/errno.h`, and assigned in **25 places across ten
files** -- `conv.c`, `dirent.c`, `errno.c`, `malloc.c`, `process.c`,
`signal.c`, `socket.c`, `stdio.c`, `stdlib.c`, `unistd.c` -- of which the
one that matters most is the translation every syscall failure passes
through:

```c
long __syscall_ret(long r)
{
    if (r < 0 && r > -4096) {
        errno = (int)-r;
        return -1;
    }
    return r;
}
```

Twenty-five writers is what makes this unit small rather than large,
because **none of them has to change**: every one assigns through the
name, so making `errno` a macro over an accessor redirects them all at
once. (An earlier draft of this report said there was exactly one writer.
There is one that matters; there are twenty-five that exist, and the
difference would have mattered if the plan had been to edit them.)

**The kernel's side already exists.** `struct thread` has `tls_base`;
`arch_thread_switch_prepare` writes it to `MSR_FS_BASE` on x86-64
(`kernel/arch/x86_64/user.c:220`) and `TPIDR_EL0` on AArch64
(`kernel/arch/aarch64/user.c:26`, `:127`) for every thread with a process.
`SYS_thread_create` takes `tls` in `struct cosmo_thread` and hands it to
`process_add_thread`, which stores it. `arch_set_tls_base` exists for the
Linux personality's `arch_prctl`. **No native syscall reaches any of it.**

**libc installs nothing.** `__libc_start` (`libc/src/stdlib.c:23`) sets
`environ`, calls `__stdio_init()` and then `main`; `cosmo_thread_start`
(`libc/src/thread.c`) maps a stack with a guard page and passes `tls = 0`.
So every native thread runs with a zero thread pointer today.

**What the threads unit did leave behind**: the allocator and stdio each
take one `cosmo_mutex_t`, `cosmo/thread.h` avoids `errno` by returning
`-errno`, and `thrtest` keeps printing to one thread. L8 records all of it,
and names `errno` as what remains.

## Why it matters

- **It finishes a documented obligation.** L8 said that on the day user
  threads arrived, `errno` would become thread-local and the allocator and
  stdio would take locks. Two thirds landed. Leaving the third indefinitely
  makes the invariant a wish.
- **It unblocks the hypervisor's userland.** The `vmctl` conversion is the
  reason the threads unit was chosen, and it needs this first.
- **It is the cheapest possible version of a TLS story.** No ELF `PT_TLS`,
  no `__tls_get_addr`, no compiler `__thread`: one syscall, one page per
  thread, and an accessor. A program that later wants real `__thread` is
  not blocked by any of it -- the thread pointer will already be there, and
  that unit becomes "teach `spawn` about `PT_TLS`".
- **It removes a footgun from a public API.** `cosmo/thread.h` currently
  carries a warning telling callers which libc functions they may not use
  from two threads. After this, that warning shrinks to `strerror` and
  `getcwd(NULL)` -- and those become fixable.

## Proposed design

### One syscall

```c
#define SYS_set_tls 87  /* (uint64_t base) -> 0: the calling thread's thread pointer */
```

`SYS_COUNT` 87 → 88. It sets `thread_current()->tls_base` through
`arch_set_tls_base`, so the value takes effect immediately and on every
later switch to this thread. `base` must be 16-byte aligned and within the
caller's address space (`-EINVAL`, `-EFAULT`); **0 is legal** and means "no
thread pointer", which is what every thread has today.

It affects **only the calling thread** -- like `sigprocmask` and unlike the
syscall filter -- because a thread pointer is the definition of per-thread
state. `SYS_thread_create`'s `tls` field is unchanged and remains the way a
*new* thread gets one before its first instruction; this syscall is how a
thread that already exists gets one, which in practice means the process's
first thread, from `__libc_start`.

### A block libc owns, found without a second syscall

```c
/* libc/include/cosmo/tcb.h -- the layout is libc's, not the kernel's. */
struct __cosmo_tcb {
    struct __cosmo_tcb *self;   /* x86-64 reads this at %fs:0 */
    int err;                    /* errno */
    unsigned tid;               /* cached, so thread_id() costs no syscall */
    char reserved[104];         /* to 128 bytes; a program's own storage starts here */
};
```

`__errno_location()` returns `&tcb->err`, and finding the block differs by
architecture for a reason:

- **AArch64** reads `TPIDR_EL0` directly (`__builtin_thread_pointer()`).
- **x86-64** cannot read the FS *base* without `rdfsbase` (which needs
  `CR4.FSGSBASE` and is not guaranteed), so the block's **first word is a
  pointer to itself** and the accessor loads `%fs:0`. This is the standard
  trick and the only reason the layout has a `self` field.

**A thread with no block falls back.** `__errno_location()` returns the
address of a single global when the thread pointer is 0 -- today's
behaviour exactly, for any thread that libc did not create and any binary
built before this unit. Nothing faults, and the fallback is what makes the
change safe to land at all.

### Where the block comes from

- **The first thread**: `__libc_start` maps one page and calls
  `SYS_set_tls` **before `__stdio_init` and before `main`**, so no libc
  call can set `errno` into the fallback and then have it become
  invisible.
- **Every thread libc creates**: `cosmo_thread_start` already maps
  `guard + stack`; it maps `guard + stack + one page` instead, puts the
  block in the extra page and passes its address as `cosmo_thread.tls`, so
  the block exists before the thread's first instruction and is freed by
  the `munmap` the join already does.
- **A thread created by a raw `SYS_thread_create`** gets whatever `tls` the
  caller passed, including 0, and then the fallback applies.

### `errno` becomes an accessor

```c
/* libc/include/errno.h */
int *__errno_location(void);
#define errno (*__errno_location())
```

Every existing use compiles unchanged, `__syscall_ret` needs no edit, and
`int errno;` goes. The one visible consequence is that `&errno` is no
longer a link-time constant -- which POSIX has required of `errno` for
decades, and nothing in this tree takes its address.

### What stays shared, and what this unit does not do

`strerror`'s static buffer and `getcwd(NULL)`'s storage stay shared: both
now *can* be fixed, and doing it here would be a second subsystem in one
unit. `cosmo_thread_id()` gains the cached `tid` (a syscall saved on every
call) because the block has to carry something more than `errno` to justify
128 bytes, and the tid is the field the threads unit already makes every
thread know. Compiler `__thread` is not attempted: that needs `spawn` to
honour `PT_TLS`, a real TCB layout and the linker's TLS relocations, and
this design is deliberately the one that does not block it.

### The §70 gate

**Correctness.** One writer (`__syscall_ret`) and one accessor, and the
accessor has two cases: a block or the fallback. The block is installed
before the thread's first instruction in both paths that install one.

**Concurrency.** Each thread writes its own `err` and nothing else touches
it. The fallback global can be written by several block-less threads at
once, which is exactly today's behaviour and no worse.

**Ownership.** The block belongs to whoever mapped it: libc for the first
thread (never freed, the process's lifetime) and for each thread it
creates (freed by the join's `munmap`). A program that calls `SYS_set_tls`
itself takes ownership of `errno`'s correctness with it, which is why the
reserved space is documented from the start: a program's own per-thread
storage goes *after* libc's prefix, in a block it allocates, with libc's
fields left intact.

**Lifetime.** A thread's block dies with its stack mapping. The first
thread's lives as long as the process. A signal handler runs on the
interrupted thread and sees that thread's block, which is the behaviour a
handler wants.

## Affected files

| file | change |
| --- | --- |
| `kernel/include/uapi/cosmo/syscall.h` | `SYS_set_tls` (87), `SYS_COUNT` 87→88 |
| `kernel/syscall/native.c` | the handler: validate, `arch_set_tls_base` |
| `libc/include/errno.h` | `__errno_location`, `#define errno` |
| `libc/src/errno.c` | the accessor, the fallback, `int errno;` removed |
| `libc/include/cosmo/tcb.h` (new) | `struct __cosmo_tcb` and the reserved-prefix rule |
| `libc/src/tcb.c` (new) | the accessor's arch halves and the installer |
| `libc/src/stdlib.c` | `__libc_start` installs the first thread's block first |
| `libc/src/thread.c` | one more page in the mapping; `tls` passed; `cosmo_thread_id` reads the cache |
| `libc/include/cosmo/syscall.h` | the `SYS_set_tls` stub |
| `libc/libc.mk` | `tcb.c` |
| `userland/tests/thrtest.c` | the new steps |
| `docs/libc/invariants.md` | L8 completed: the constraint goes |
| `docs/libc/architecture.md`, `-/api.md`, `libc/README.md` | `errno` is per-thread |
| `docs/kernel/process/design.md`, `-/testing.md` | §12's follow-up, satisfied; the new steps |
| `README.md` | Status entry |

## New APIs

One syscall (`SYS_set_tls`), one libc header (`cosmo/tcb.h`), one accessor
(`__errno_location`). No new device, no new ioctl, no ABI break: `errno`
keeps its name and its type, and `SYS_thread_create`'s `tls` field keeps
its meaning.

## Migration plan

1. **`SYS_set_tls`** alone, with its validation and a test that a thread
   can set and re-set its own pointer and that a bad one is refused. No
   libc change; nothing depends on it yet.
2. **The block and the accessor**, with the fallback, but nothing
   installing a block: `errno` behaves exactly as before, through one more
   indirection. The tree stays green and every existing test still passes.
3. **`__libc_start` installs the first thread's block.** From here the main
   thread's `errno` lives in its block.
4. **`cosmo_thread_start` installs one per thread**, and `cosmo_thread_id`
   reads the cached tid.
5. **The tests**, then the bug-proofs.
6. **L8 and the documents**, including the warning in `cosmo/thread.h`,
   which shrinks rather than disappears.

## Tests

In `thrtest`, which already owns the threaded-libc questions:

1. **Two threads, two `errno`s**: each provokes a different failure in a
   loop (`close(-1)` for `-EBADF`, an unmapped `mmap` for `-ENOMEM`) and
   reads its own value back every iteration. A shared `errno` loses this
   within a few iterations; a per-thread one never does.
2. **The first thread's `errno` survives a thread's**: main provokes one
   error, a thread provokes another and exits, main's value is still its
   own.
3. **A thread with no block does not fault**: a raw `SYS_thread_create`
   with `tls = 0`, whose entry calls something that fails and then reads
   `errno`. It gets the fallback and lives.
4. **`SYS_set_tls`'s validation**: unaligned is `-EINVAL`, outside the
   caller's space is `-EFAULT`, 0 is accepted, and setting it twice works.
   After a deliberate 0, `errno` still works (the fallback), which is the
   property that makes the syscall safe to expose.
5. **The cached tid agrees with the syscall**: `cosmo_thread_id()` equals
   `SYS_thread_self` for the first thread and for a created one.
6. **`strerror` and `perror` still work** from one thread, and the
   documents still say they are that thread's alone.

**Bug-proofs**: the accessor returning the global (step 1 loses a value);
the fallback removed (step 3 faults at the thread pointer's zero); the
`self` word not written on x86-64 (`%fs:0` reads whatever the stack left,
so `errno`'s address is garbage -- an argument for the fallback being a
*check* on the pointer, not on the word); the block installed *after*
`__stdio_init` (a failure inside startup lands in the fallback and the
main thread's first `errno` read disagrees with it); the extra page not
mapped (the block overlaps the thread's stack and the two corrupt each
other, which step 1 should see as a wrong value rather than a crash); and
the block not freed on join (the address space grows across a thousand
threads, which a counting test catches).

## Benchmarks

`errno` access goes from a global load to a thread-pointer read plus an
offset -- one extra instruction on AArch64, a `%fs`-relative load on
x86-64. Measured the way the last three units were: the suite's own
per-test timing across three consecutive runs on each arch, with the claim
being that nothing moves outside its run-to-run spread. `cosmo_thread_id()`
gets *faster* (a syscall becomes a load), which the thread benchmark in
`thrtest` can show.

## Risks

- **A program that sets its own thread pointer breaks `errno`.** The
  reserved-prefix rule is the answer and it must be documented from the
  first commit, not after someone does it.
- **The fallback is load-bearing and easy to get wrong.** It must be
  chosen on the *pointer*, not on anything read through it, or a garbage
  pointer becomes a fault instead of a fallback.
- **Startup ordering.** Anything in `__libc_start` that can fail before the
  block is installed writes the fallback, and the main thread would then
  read a different location. Installing first is the rule; the bug-proof
  exists to keep it.
- **x86-64's `%fs:0` convention couples libc to its own layout.** Changing
  the block's first field later would break every compiled binary. The
  `self` pointer is therefore permanent, which is a cost worth naming.
- **The Linux personality also uses `tls_base`** (`arch_prctl`,
  `CLONE_SETTLS`) and must keep working: a Linux binary's TLS is its own
  and this unit must not touch `arch_set_tls_base`'s behaviour, only reach
  it from one more place.

## Alternatives considered

- **Compiler `__thread` with ELF `PT_TLS`.** The real answer eventually,
  and far larger: `spawn` must parse and place the TLS image, the linker's
  relocations must work for static binaries, and every thread needs its
  image copied. This unit is the prerequisite for it, not a detour around
  it -- the thread pointer it adds is what `PT_TLS` would use.
- **Leave `errno` shared and forbid it in threaded code.** Where the
  threads unit landed, and a review rejected the same argument about the
  allocator. The difference in consequence (a wrong code, not corruption)
  bought one unit of delay, not indefinite delay.
- **A syscall to *read* `errno`'s address** (`SYS_errno_location`).
  A syscall per error is absurd, and it would put a libc detail in the
  kernel.
- **Per-thread storage indexed by `thread_self`** (a table in libc keyed by
  tid). No new syscall, but a lookup per `errno` access and a lock on the
  table -- which is the thing being removed.

Named and deferred: compiler `__thread` and `PT_TLS`; `strerror` and
`getcwd(NULL)` moving into the block; a `SYS_set_tls`-shaped way to get the
*block's size* so a program can allocate its own with libc's prefix; and
the `vmctl` conversion, which this unit unblocks and which remains its own
report.
