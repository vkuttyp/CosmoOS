# NEXT SUBSYSTEM — a thread pointer, and `errno` per thread

Constitution §68: after the audit, name the next subsystem in this shape
and wait for the instruction to build it. **This report is as built.** The
design below is what shipped; six things differ from what was written, and
each is marked where it appears:

1. **The block needs `__attribute__((aligned(16)))` on the type.** The
   report specified a 16-byte-aligned thread pointer but gave the struct no
   alignment, and its natural alignment is 8 because it leads with a
   pointer. One program out of the whole suite -- `sh` -- was linked at an
   8-mod-16 address and died at startup. The exit-127 branch the report
   argued for but expected never to run is what named it, in one line.
2. **`cosmo_thread_start` does not install the block; the kernel does.**
   The report had libc adopt it; passing the address as `cosmo_thread.tls`
   is simpler and means the block is live *before* the thread's first
   instruction rather than after its first libc call.
3. **The tid is cached lazily, on the first call that asks for it.** The
   creator cannot fill it -- it does not know the tid until
   `thread_create` returns, and by then the thread may already be reading
   it -- and a first version had the thread cache its own in the
   trampoline. Review killed that: a tid read costs a syscall, and during
   `__libc_start` that made installing the thread pointer *two* calls, the
   second of which is not in the filter's always-allowed set. Zero means
   "not asked yet", which is unambiguous because no thread's id is ever
   zero.
4. **The freed-block proof is a direct assertion, not a counting test.**
   The report proposed a thousand create/join cycles against the address
   space; a write from the block's address after the join is `-EFAULT`,
   which is shorter and stronger -- and a count would have needed a limit
   to count against, since `getrlimit` reports the limit, not the usage.
5. **One bug-proof failed to fail, and the test changed rather than the
   code.** Putting the block inside the stack corrupts `err` silently on
   AArch64, where the `self` word is unused, and every assertion that sets
   `errno` and reads it straight back still passed. The step now asserts
   the *layout* -- a thread finds its own block through `&errno` and checks
   it is above a local. A sixth proof, installing the first thread's block
   after `__stdio_init` instead of before, **does not fail today** and is
   recorded as an untested precaution below.
6. **The second `errno` worker overflows `strtoll` for `ERANGE`** rather
   than provoking `-ENOMEM` from an unmapped `mmap`. `strtoll` reaches no
   kernel, so the two threads are not merely racing inside one syscall
   path -- and a failing `mmap` two thousand times over is a syscall each.

**One thing the report missed entirely** is in its own section below:
`SYS_set_tls` has to be always-allowed by the syscall filter, which review
found and the suite could not see.

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
stack and a `.bss` object for the first thread, and the accessor is a
single unconditional load. **A first draft of this report promised a
fallback for a thread with no block; a review showed it cannot exist on
x86-64** -- `%fs:0` with a zero base dereferences address zero before any
check can run -- so the design guarantees a block instead of testing for
one, which is both simpler and faster. It is also the
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

## Design (as built)

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
/* libc/include/cosmo/tcb.h -- the layout is libc's, not the kernel's.
 * As built the struct also carries __attribute__((aligned(16)))
 * (difference 1): the thread pointer must be 16-byte aligned and this
 * struct's natural alignment is 8, because it leads with a pointer. */
struct __cosmo_tcb {
    struct __cosmo_tcb *self;   /* x86-64 reads this at %fs:0 */
    int err;                    /* errno */
    unsigned tid;               /* the cached id; 0 means "not asked yet" */
    char reserved[112];         /* to 128: 8 + 4 + 4 + 112. A program's own storage
                                   starts at offset 128, never inside this. */
};
```

`__errno_location()` returns `&tcb->err`, and finding the block differs by
architecture for a reason:

- **AArch64** reads `TPIDR_EL0` directly (`__builtin_thread_pointer()`).
- **x86-64** cannot read the FS *base* without `rdfsbase` (which needs
  `CR4.FSGSBASE` and is not guaranteed), so the block's **first word is a
  pointer to itself** and the accessor loads `%fs:0`. This is the standard
  trick and the only reason the layout has a `self` field.

**There is no fallback, because on x86-64 there cannot be one.** An
earlier draft of this report promised that `__errno_location()` would
return a global when the thread pointer is 0. That is impossible on
x86-64: reading `%fs:0` with `FS_BASE == 0` dereferences virtual address
zero, which is unmapped, so the thread faults *before* the accessor can
test what it loaded. There is no fault-free way to ask "do I have a thread
pointer?" on that architecture without `rdfsbase`, which needs
`CR4.FSGSBASE` and is not guaranteed.

So the design removes the need for a fallback instead of patching one:

- **The first thread's block is `static`**, in libc's `.bss`. No mapping,
  no allocation, nothing to fail: `__libc_start` sets the thread pointer to
  `&__cosmo_main_tcb` before `__stdio_init` and before `main`. Every
  process therefore has a thread pointer from its first libc call onward,
  including every program built before this unit -- they gain one by being
  relinked, and nothing about them changes otherwise.
- **Every thread libc creates has one**, from the extra page in the
  mapping `cosmo_thread_start` already makes.
- **A thread created by a raw `SYS_thread_create` with `tls = 0` must not
  call libc.** That is a contract, stated in `cosmo/thread.h` beside the
  syscall's own description, and it is the same kind of contract as "the
  stack you pass must be big enough": libc cannot check it and will fault
  if it is broken. For a program that wants such a thread to use libc,
  `cosmo_tcb_install(void *block, size_t len)` is named below -- it sets
  the pointer from storage the caller owns.

That is stricter than the earlier draft and simpler: one unconditional
load, no branch on every `errno` access, and no code path that depends on
address zero being readable.

### Where the block comes from

- **The first thread**: a `static struct __cosmo_tcb __cosmo_main_tcb` in
  libc, whose address `__libc_start` installs with `SYS_set_tls`
  **before `__stdio_init` and before `main`**. Static rather than mapped
  on purpose: a mapping can fail, and a startup path that has to handle
  failing to give the process an `errno` is a path with no good answer --
  it cannot report the failure through `errno`, and continuing would fault
  on the first error. A `.bss` object cannot fail, which removes the
  question rather than answering it. (An earlier draft mapped a page here
  and left that failure undefined; a review asked what it would do, and
  the honest answer was to make it unreachable.)

  What remains is `SYS_set_tls` itself failing on a `.bss` address, which
  the kernel can only do if the object is somehow outside the caller's
  space or misaligned -- neither possible for a linked static object. The
  policy is stated anyway, because "cannot happen" is where a missing
  branch hides: `__libc_start` writes one line to file descriptor 2 and
  exits **127**, because it cannot report the failure through `errno` (the
  thing it just failed to provide) and a program that continued would
  fault on its first error. That is a deliberate, visible death rather
  than an undefined one, and it is the only startup failure this design
  leaves.
- **Every thread libc creates**: `cosmo_thread_start` already maps
  `guard + stack`; it maps `guard + stack + one page` instead, puts the
  block in the extra page *above the stack* and passes its address as
  `cosmo_thread.tls`, so **the kernel installs it** (difference 2) before
  the thread's first instruction, and it is freed by the `munmap` the join
  already does. The creator fills `self` and `err`; the **tid** is cached
  lazily, on the first call that asks (difference 3), because the creator
  does not know it until `thread_create` returns and the thread may already
  be reading it -- and because a read during startup would make installing
  the thread pointer two syscalls instead of one. That mapping can fail, and it
  already has a failure path: `thread_start` returns `-ENOMEM` and no
  thread is created.
- **A thread created by a raw `SYS_thread_create`** must carry a block
  whose **prefix is libc's** to call libc at all, which `cosmo/thread.h`
  and `cosmo/tcb.h` both state. `tls = 0` is the obvious case; the one
  review had to point out is a `tls` pointing at a layout of the caller's
  own, which was harmless before `errno` moved behind the thread pointer
  and is not now -- libc reads and writes that memory as its own block.
  `cosmo_tcb_install(void *block, size_t len)` is the way out for either:
  it checks the length against the prefix, writes the `self` word and calls
  `SYS_set_tls`.

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
call after the first, since the cache is filled lazily) because the block has to carry something more than `errno` to justify
128 bytes, and the tid is the field the threads unit already makes every
thread know. Compiler `__thread` is not attempted: that needs `spawn` to
honour `PT_TLS`, a real TCB layout and the linker's TLS relocations, and
this design is deliberately the one that does not block it.

### The §70 gate

**Correctness.** One accessor with **no cases**: an unconditional load of
the thread pointer and an offset. Every thread libc knows about has a
block before its first instruction -- the first thread's is a `.bss`
object installed before anything else runs, and a created thread's is in
the mapping that carries its stack. A thread libc did not make and that
did not install one is outside the contract, and the contract says so
where the syscall is described.

**Concurrency.** Each thread writes its own `err`; nothing else reads or
writes it. There is no shared location left for `errno` to land in, which
is the point of the unit.

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
| `libc/src/errno.c` | **as built only `int errno;` removed** -- the accessor and the block live in `tcb.c`, not here |
| `libc/include/cosmo/tcb.h` (new) | `struct __cosmo_tcb` (`aligned(16)`), `COSMO_TCB_SIZE`, the reserved-prefix rule, `cosmo_tcb_install` |
| `libc/src/tcb.c` (new) | the accessor's arch halves, the static first-thread block, `__cosmo_tcb_init`, the tid cache, `cosmo_tcb_install` |
| `libc/src/libc.h` | **not in the report**: the internal declarations (`__cosmo_tcb_init`, `__cosmo_tcb_tid`) |
| `libc/src/stdlib.c` | `__libc_start` installs the first thread's block first, and the exit-127 policy |
| `libc/src/thread.c` | one more page in the mapping; `tls` passed so the kernel installs it; `cosmo_thread_id` reads the block's lazily-filled cache |
| `kernel/security/…` docs, `userland/init/init.c` | **not in the report**: `SYS_set_tls` joins the always-allowed set, and `--filter inherit-start` asserts it |
| `libc/include/cosmo/thread.h` | **not in the report's table** (the migration plan named it): the L8 warning shrinks to the raw-thread contract |
| `libc/include/cosmo/syscall.h` | the `SYS_set_tls` stub |
| `libc/libc.mk` | `tcb.c` |
| `userland/tests/thrtest.c` | steps 12-16, and the thread-bound step renumbered to 17 so it stays last |
| `docs/libc/invariants.md` | L8 completed: the constraint goes |
| `docs/libc/architecture.md`, `-/api.md`, `-/design.md`, `libc/README.md` | `errno` is per-thread (`design.md` **not in the report's table**) |
| `docs/kernel/process/design.md`, `-/testing.md` | §12's follow-up satisfied and a new **§13** for this unit; the new steps |
| `README.md` | Status entry |
| `docs/audit/next-subsystem-errno-tls.md` | this report, converted to as-built |

## New APIs

One syscall (`SYS_set_tls`), one libc header (`cosmo/tcb.h`), one accessor
(`__errno_location`). No new device, no new ioctl, no ABI break: `errno`
keeps its name and its type, and `SYS_thread_create`'s `tls` field keeps
its meaning.

## Migration plan (followed, except step 4)

1. **`SYS_set_tls`** alone, with its validation and a test that a thread
   can set and re-set its own pointer and that a bad one is refused. No
   libc change; nothing depends on it yet.
2. **The block, the accessor, and the first thread's install together**,
   because they cannot be separated: the accessor is unconditional, so it
   is only correct once something has installed a block, and the `.bss`
   object plus `SYS_set_tls` in `__libc_start` is that something. At the
   end of this step every single-threaded program has a per-thread
   `errno` -- which is every program, until step 4 -- and the whole suite
   is the regression test for it.
3. **`cosmo_tcb_install`**, so a thread made outside libc's wrapper can
   opt in, with the length check its bug-proof needs.
4. **`cosmo_thread_start` gives every thread one**, and `cosmo_thread_id`
   reads the cached tid. *As built this step differs twice*: the block is
   passed as `cosmo_thread.tls` and **the kernel installs it** rather than
   libc adopting it (difference 2), and the tid cache is **lazy** rather
   than filled at install time (difference 3).
5. **The tests**, then the bug-proofs.
6. **L8 and the documents**, including the warning in `cosmo/thread.h`,
   which shrinks rather than disappears.

## Tests

In `thrtest`, which already owns the threaded-libc questions -- shipped as
steps 12 to 16, with the thread-bound step renumbered to 17 so that it
stays **last**: it exhausts threads on purpose, and every step below
creates one.

1. (step 13) **Two threads, two `errno`s**: each provokes a different failure in a
   loop and reads its own value back every iteration. A shared `errno`
   loses this within a few iterations; a per-thread one never does. *As
   built the second worker overflows `strtoll` for `ERANGE` rather than
   provoking `-ENOMEM` from an unmapped `mmap`* (difference 6): `strtoll`
   touches no kernel at all, so the two threads are not merely racing
   inside one syscall path, and an `mmap` that fails costs a syscall two
   thousand times over. The first thread provokes a third code,
   `EBADF` from a different descriptor, while both run.
2. (step 14) **The first thread's `errno` survives a thread's**: main provokes one
   error, a thread provokes another and exits, main's value is still its
   own.
3. (step 15) **A hand-made thread installs its own block**: a raw
   `SYS_thread_create` with `tls = 0`, whose entry calls
   `cosmo_tcb_install` on a buffer of its own and then provokes an error
   and reads `errno` -- the path a program outside libc's wrapper must
   take. The *un*installed case is deliberately **not** tested, because
   the contract is that it faults: on x86-64 it dereferences address zero,
   and a test that asserted a fault would be asserting the absence of a
   fallback this design does not have.
4. (step 12) **`SYS_set_tls`'s validation**: unaligned is `-EINVAL`, outside the
   caller's space is `-EFAULT`, and setting it twice works -- the second
   value takes effect, which is what `cosmo_tcb_install` relies on. Zero
   is accepted by the kernel (it is what every thread starts with) and the
   test sets it only on a thread it then lets exit without touching libc,
   because that is the whole of what zero now means.
5. (step 16) **The cached tid agrees with the syscall**: `cosmo_thread_id()` equals
   `SYS_thread_self` for the first thread and for a created one.
6. **`strerror` and `perror` still work** from one thread, and the
   documents still say they are that thread's alone.
7. **The startup policy is reachable in a test**, even though the failure
   is not: a program that calls `SYS_set_tls` with a deliberately bad
   address gets `-EFAULT`/`-EINVAL` and can then report it itself, which
   is the same check `__libc_start` makes before it decides to exit 127.
   The exit path itself is argued rather than tested, since nothing can
   make a `.bss` address invalid -- named here so it is not mistaken for
   something the suite proves.

**One thing the report missed entirely**, found in review: `SYS_set_tls`
must join `SYS_exit`, `SYS_sigreturn` and `SYS_thread_exit` in the native
personality's **always-allowed** set. Every native program installs its
block in `__libc_start`, before `main`, so a filter that omitted number 87
killed every child of a filtered process during startup -- and the
inherited-filter test could not see it, because its child is *expected* to
die of SIGSYS and a death in startup wears the same status as the death it
means to provoke. The kernel log said `number 87 is outside its filter`
where it used to name the call the test was about. A new case,
`init --filter inherit-start`, asserts the thing directly: a child of a
filter naming only spawn and wait must reach its own `main` and exit with a
status of its own.

**Bug-proofs**, as run:

| the bug | what failed |
| --- | --- |
| the accessor reads a single global | steps 13 and 14: the main thread's own loop loses values, the `ERANGE` thread loses values, and main's `errno` no longer survives a thread's |
| the `self` word is not written | **x86-64 loses every user process to SIGSEGV** (status 139) while **AArch64 passes the whole suite** -- the word is load-bearing on exactly one architecture, which only the pair of runs shows |
| no extra page: the block lives in the stack | step 16's layout assertion. This is the proof that failed to fail first: with the block at the *bottom* of the top page a 16 KB-stack worker never reaches it, and even directly under the stack pointer the corruption is invisible on AArch64, where `self` is unused. The step now checks the layout as a layout |
| the block is not freed on join | step 16: a write from the block's address after the join succeeds instead of returning `-EFAULT` |
| `cosmo_tcb_install` does not check the length | step 15: a buffer shorter than the prefix is accepted |
| the first thread's block is installed *after* `__stdio_init` | **nothing fails.** Recorded rather than hidden: nothing in `__stdio_init` sets `errno` today, so the ordering is unobservable. It is a precaution against a first block that is ever made dynamic, or startup code that ever sets `errno` -- and it belongs with the exit-127 path below as argued rather than tested |

## Benchmarks

`errno` access goes from a global load to a thread-pointer read plus an
offset -- one extra instruction on AArch64, a `%fs`-relative load on
x86-64.

**What was actually measured: nothing beyond the suite's own timing.** The
report proposed three consecutive runs per architecture against the
run-to-run spread; the boot harness's per-test timing across the runs this
unit took shows no test moving outside its usual spread, and that is the
whole of the evidence. It is recorded this way rather than as a benchmark
that was run, because it was not.

`cosmo_thread_id()` is also **not** simply "a syscall becomes a load", as
the report first claimed. The cache is filled lazily (difference 3), so the
first call on each thread still costs `SYS_thread_self` and every call
after it is a load. A thread that never asks pays nothing at all, which the
eager version did not manage.

## Risks

- **A program that sets its own thread pointer breaks `errno`.** The
  reserved-prefix rule is the answer and it must be documented from the
  first commit, not after someone does it.
- **There is no fallback, so a thread without a block faults on its first
  `errno`** -- on x86-64 at address zero, which is at least a recognisable
  crash. That is the price of an unconditional accessor, and the
  alternative does not exist on x86-64: `%fs:0` with a zero base
  dereferences address zero before any check could run. The contract is
  therefore the mitigation, and `cosmo_tcb_install` is the way out for a
  program that needs one.
- **Startup ordering.** Anything in `__libc_start` that sets `errno`
  before the block is installed writes the `.bss` object *as itself*
  rather than through the thread pointer -- harmless only because they are
  the same object for the first thread. Install first anyway: a later
  change that makes the first thread's block dynamic would turn this into
  a real bug. **Its bug-proof does not fail**, and the table above says so:
  nothing in `__stdio_init` sets `errno` today, so the ordering is a
  precaution the suite cannot check, not a property it proves.
- **This breaks a raw thread that carries a TLS layout of its own and calls
  libc**, and that is a deliberate compatibility break rather than an
  oversight. Before this unit libc never read the thread pointer, so such a
  thread worked by accident; now libc reads and writes that memory as its
  own block, overwriting whatever the caller keeps at the `errno` and tid
  offsets and answering `cosmo_thread_id()` with nonsense.

  **It cannot be detected**, for the same reason the zero case cannot:
  asking "is this libc's block?" means dereferencing the pointer, and on
  x86-64 the first dereference *is* `%fs:0`. A magic number in the block
  would not help -- reading it requires the load that already faulted. So
  the contract is the whole of the mitigation, stated in `cosmo/thread.h`,
  `cosmo/tcb.h` and §13, with `cosmo_tcb_install` as the way to comply.

  **Measured in-tree impact: none.** The only non-zero `cosmo_thread.tls`
  in the tree is the block libc itself passes (`libc/src/thread.c`), and
  the only raw `thread_create` outside libc is `thrtest`'s, which passes
  zero on purpose to exercise the contract. The Linux personality's TLS is
  its own and untouched -- `arch_prctl` and `CLONE_SETTLS` reach
  `arch_set_tls_base` exactly as before.
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
