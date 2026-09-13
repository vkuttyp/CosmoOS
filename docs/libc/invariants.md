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

**L8. The allocator, stdio and `errno` are each safe from more than one
thread.** User threads arrived with the audit unit "native threads and a
futex", and this invariant used to read "the library is single-threaded and
says so" with a note that on that day `errno` would become thread-local and
the allocator and stdio would take locks *before anything else was done*.
The locks landed with the threads unit; `errno` landed with the unit after
it, because it needed a thread pointer the machine did not have. All three
are done, and the shape of each is set by its consequence:

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
  longer a shared location for two threads to contend over. It lives in a
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

`strerror` and `getcwd(NULL)` still use static or heap storage without
synchronisation of their own, and `feof`/`ferror`/`clearerr`/`fileno` read
a word without the lock.

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
freed with it. Gap: `strerror` and `getcwd(NULL)`, which are now
*fixable* -- the block is where they would go -- and are their own unit.

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
