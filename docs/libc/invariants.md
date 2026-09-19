# libc: invariants

Rules that must not be broken without changing this document and the
code together. Each names how it is checked today and what is not yet
covered.

**L1. Programs reach the kernel only through the library.** Every
system call a program makes goes through a wrapper in
`libc/include/cosmo/syscall.h` called from `libc/src/*`; no program
under `userland/` contains inline `syscall` assembly, and the only
program that names `cosmo_*` wrappers directly is `init --selftest`,
whose purpose is to check kernel error codes exactly (constitution
section 46). Check: review (`grep -r cosmo_ userland/` finds only
`init.c`). Gap: no build rule forbids `cosmo/syscall.h` in a program.

**L2. Headers carry the ABI, never kernel internals.** The public
headers include `uapi/cosmo/syscall.h` (the ABI shared with the kernel)
and nothing else from `kernel/include/`; `struct stat`, `struct
sockaddr`, `struct dirent` and `struct spawn_handle` are laid out as
their UAPI counterparts (`struct cosmo_stat`, `struct cosmo_sockaddr`,
`struct cosmo_spawn_handle`) and are passed to the kernel by cast, so a
change to a UAPI structure changes both. Check: `init --selftest` uses
`struct sockaddr` through the library and `struct cosmo_stat` through
the raw wrappers on the same kernel; review of `libc/include/`. Gap: no
`_Static_assert` ties the sizes together yet.

**L3. Every failure sets `errno` and returns the conventional sentinel;
nothing in the library exits or prints on its own except `abort` and a
failed `assert`.** `__syscall_ret` is the single translation point for
system calls; allocation failures set `ENOMEM`; `fopen` mode errors set
`EINVAL`. Check: `init --selftest` checks `errno` after every expected
failure (`EBADF`, `EPIPE`, `ECHILD`, `ESRCH`, `EACCES`, `ENOENT`,
`EINVAL`, `ERANGE`, `ENOTDIR`, `ENOTTY` through `isatty`). Gap: none.

**L4. The formatting engine never reads past its inputs or writes past
its output.** `vsnprintf` counts every character and writes only below
`cap - 1`; `%s` honours the precision with `strnlen`; `%.*s`/`%*d`
take their sizes from the arguments; the stream sinks flush a fixed
256-byte buffer. Check: `test_libc` on the host under ASan and UBSan
(truncation to 4 bytes, `NULL` with size 0, `%s` of NULL, `LLONG_MIN`,
unknown conversions); `init --selftest` checks a mixed format's length
and text. Gap: `sprintf` has no bound by definition.

**L5. The allocator never returns overlapping or misaligned blocks, and
`free` coalesces with both neighbours.** Blocks carry `size` (with the
in-use and big flags) and `prev_size`; every arena ends in a zero-size
in-use sentinel so `next_hdr` is always valid; big blocks (above 16 KiB)
have their own mapping and are unmapped on `free`; a double `free`
calls `abort`. Check: `test_libc` (64 blocks written and verified,
frees in alternating order, `realloc` growth, a 16000-byte request after
freeing everything needs no new mapping, a 100000-byte request maps and
unmaps exactly once, `calloc` zeroes, `malloc(2^50)` fails); `init
--selftest` (`malloc` 100 000, `realloc` to 200 000 keeps the bytes).
Gap: no randomised stress test; no guard bytes in debug builds.

**L6. `exit` flushes; `_exit` does not.** `exit` runs the `atexit`
handlers in reverse and then every stream's pending output before
`SYS_exit`; `_exit` and `abort` leave buffers unflushed by design.
Check: every utility's output reaches the serial log through `exit`
after `main` returns; the shell calls `fflush(stdout)` before `_exit`
paths it does not have. Gap: a program killed by `kill` loses its
buffered output, as on Unix.

**L7. `spawnvp` never executes a directory or a non-regular file and
never leaves `errno` unset on failure.** Each `PATH` candidate is
accepted only when `stat` says `S_ISREG`; the kernel additionally
requires an execute bit; `errno` ends as the last kernel error or
`ENOENT`. Check: `init --selftest` (`spawnvp("nothere")` is `ENOENT`,
`spawnve("/bin")` is `EACCES`, `spawnve("/etc/rc")` is `EACCES`). Gap:
`PATH` entries longer than 1023 bytes are skipped silently.

**L8. The allocator, stdio, `errno`, the environment and the `atexit`
list are each safe from more than one thread.** User threads arrived with the audit unit "native threads and a
futex", and this invariant used to read "the library is single-threaded and
says so" with a note that on that day `errno` would become thread-local and
the allocator and stdio would take locks *before anything else was done*.
The locks landed with the threads unit; `errno` landed with the unit after
it, because it needed a thread pointer the machine did not have. **This
invariant then said "all three are done" for a year while the library
had five shared things**: `environ` and the `atexit` list were left
unlocked, and the fifth and sixth bullets below are the unit that
closed them (`docs/audit/next-subsystem-libc-shared-tables.md`). The
shape of each is set by its consequence:

- **The allocator takes one lock** (`libc/src/malloc.c`). An unlocked free
  list is the one hazard here that corrupts memory silently, which is
  worse than any other, so it is not left to a rule a caller must know.
  The public functions take the lock once and call an unlocked core,
  because `calloc` and `realloc` are written in terms of `malloc` and
  `free` and the mutex is not recursive.
- **stdio takes one lock** (`libc/src/stdio.c`), held across a whole
  `printf` -- `vfprintf` takes it and the sink writes through the unlocked
  core -- so two threads cannot interleave inside a line or race the
  buffer pointers of a `FILE`.
- **`errno` is per-thread**, and takes no lock at all, because there is no
  longer a shared location for two threads to contend over. Since the
  `__thread` unit a program can do the same for its own state: the
  compiler's `__thread` and C11's `_Thread_local` work, with the image
  placed per thread from the program's own `PT_TLS`
  (`docs/audit/next-subsystem-pt-tls.md`). It lives in a
  128-byte block behind the thread pointer (`libc/include/cosmo/tcb.h`),
  which `SYS_set_tls` sets: `__libc_start` installs a static block for the
  first thread before anything else runs, and `cosmo_thread_start` puts one
  in the page above each thread's stack, so every thread libc knows about
  has a block before its first instruction. `errno` is
  `(*__errno_location())`, one load of the thread pointer and an offset,
  with **no case for a thread that has none** -- on x86-64 there cannot be
  one, because reading `%fs:0` with a zero base dereferences address zero
  and faults before any check could run. The consequence is a contract
  rather than a fallback: **a thread created by a raw `SYS_thread_create`
  with `tls = 0` must not call libc**, and `cosmo_tcb_install` is the way
  for a program that wants such a thread to use libc anyway.

- **The environment takes one lock** (`libc/src/stdlib.c`), shared with
  the `atexit` list because both are cold start-up paths and a second
  lock is a second chance at an ordering bug. `setenv` growing the
  array calls `free(environ)`, so an unlocked `getenv` walking it was
  a use-after-free in the allocator this same invariant locks two
  bullets above. The lock covers the **walk** and not the pointer
  `getenv` returns: that stays valid because `setenv` **leaks** the
  string it replaces rather than freeing it, which is deliberate and
  must not be tidied. `env_count` is an unlocked helper called under
  the mutators' lock — the mutex is not recursive and both mutators
  call it.
- **The `atexit` list takes the same lock**, and `exit` must not hold
  it while running a handler: a handler is arbitrary program code that
  may call `atexit` or `getenv`, so the drain takes the lock, removes
  one handler, releases, and then calls it. Unlocked, the list lost
  handlers (`g_natexit++` is a read-modify-write) and could be written
  **past its end**, because the bound check and the increment were
  separate.

`feof`/`ferror`/`clearerr`/`fileno` read a word without the lock.

**`strerror` and `getcwd(NULL)` are done, and one of them never needed
doing.** `strerror`'s buffer for an unknown code is `_Thread_local` since
the `__thread` unit -- the first use of thread-local storage inside the
library that provides it. `getcwd(NULL)` was already safe: it `malloc`s a
fresh buffer per call and hands it to the caller, so once the allocator
took its lock in the threads unit there was nothing shared left. This
invariant went on naming it for two units afterwards, which is what an
enumeration kept by hand does -- the list is now checked against a
`grep` for writable statics in `libc/src`.

**What that `grep` actually says**, since a first draft of this paragraph
read it too broadly and claimed `strerror`'s was the only writable static
left. It was the only one of a particular kind: a function *returning a
pointer to a static*, which is the kind `errno` and this invariant were
about. The statics that remain fall in three groups. The allocator's
`g_free` and stdio's `g_std`/`g_files` are behind `g_lock` and `g_io`.
`tcb.c`'s `g_tls` is written once in `__libc_start`, before the process
has a second thread, and read-only after. And `stdlib.c`'s `g_atexit`,
`g_natexit`, `environ` and `g_env_owned` are **genuinely unsynchronised**
-- see the gap below.

`cosmo/thread.h` needs none of this: every function there returns `-errno`
rather than setting `errno`, takes no libc lock, and maps its stacks
with `mmap`. A handle is zeroed before the first thing in
`cosmo_thread_start` that can fail, so a refused start leaves a handle
`join` refuses rather than indeterminate memory it would wait on. Check:
review, plus `thrtest` step 11 -- three threads allocating, reallocating,
freeing and printing at once behind a start barrier, each verifying its
own blocks, which an unlocked allocator fails -- step 8 for the refused
handle, and steps 12 to 16 for `errno`: two threads provoking different
failures two thousand times each while the first thread provokes a third,
a thread's `errno` leaving the first thread's alone, a thread made outside
libc installing its own block, and the block sitting above the stack and
freed with it. And step 17 for `__thread`: a `.tdata` variable read back as
its initialiser in every thread, a `.tbss` array zero in a new one, an
over-aligned variable aligned, and `strerror` of an unknown code answering
each thread its own.

**One gap is left, and it is not the one this invariant was about.**
`atexit` does `g_atexit[g_natexit++] = fn`, an unsynchronised
read-modify-write on process-global state, and `setenv`/`unsetenv`
reallocate `environ` with `g_env_owned` tracking ownership -- so two
threads registering handlers, or one setting the environment while
another reads it, race. Nothing in the tree does either from a second
thread: handlers and environment are set before threads start, which is
the normal shape of both. It is named here rather than fixed because it
is process state and not per-thread state, and this invariant is about
the latter; the fix is a lock apiece and belongs to whichever unit needs
it. `cosmo/thread.h` states the same restriction to callers.

**L9. A thread waits on a predicate in a `while`, never on a loop count
and never on a spin.** `cosmo/thread.h` carries a condition variable
(`docs/audit/next-subsystem-condvar.md`), and its contract has two halves
that are each load-bearing. A **waiter** loops: `cosmo_cond_wait` may
return with nothing having happened, so a caller that writes `if` instead
of `while` has written a bug that passes every test on an unloaded
machine. A **signaller** changes the predicate under the waiter's mutex:
that is what orders every signal against the waiter's read of the
sequence number, and it is why a wakeup cannot be lost rather than a
convention that usually works.

The rule against loop counts is the same invariant from the test side. A
wait bounded by iterations measures the host's speed, not the property,
and this tree has been bitten by that substitution repeatedly -- the
threads unit's retry budget, and the family of boot-test flakes that count
N things after a fixed `settle()`. A wait that must give up keeps a
*deadline* and recomputes its relative timeout from it each pass; a wait
that must not give up sleeps rather than yielding, because on a
single-CPU boot the thing being waited for needs the CPU the polling loop
is spinning on.

*Checked by*: `thrtest` steps 18 to 22 -- a signal seen with the mutex
proved retaken by `trylock` returning `-EBUSY`; a signal delivered inside
the sleep window through libc's one-shot probe, since no arrangement of
threads can reach a window a few instructions wide; a broadcast reaching
four of four and one ticket taken by exactly one of four; both timeout
paths; and a waiter broadcast at twenty times whose predicate never
becomes true. The probe's necessity is itself proved: the same
lost-wakeup bug that hangs step 19 **passes** when the test uses a
signaller thread instead.

## Gaps (documented, not invariants)

- No `<math.h>`, locales, wide characters or a wall clock
  (`clock_gettime` is monotonic only). `printf` does format doubles
  (`%f`, `%e`, `%g`) since the FP/SIMD unit; this line said "no floating
  point" until the job-control report swept it.
- No `fork`, `exec*`, `system`, `popen`, `setjmp`, `getpwnam`, dynamic
  linking. (`signal` and `sigaction` arrived with the signals unit; job
  control added `WUNTRACED`/`WIFSTOPPED` and the session calls;
  `<termios.h>` arrived with the terminal-modes unit, carrying the four
  flags this kernel has rather than POSIX's forty -- `c_cflag`, the baud
  rates and the other seventeen control characters are absent, not
  ignored.)
- `sockaddr_in`/`sockaddr_in6` do not exist; the native address shape is
  the only one.
