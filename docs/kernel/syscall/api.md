# System Calls: API

Two audiences: user programs (the native ABI, stable) and kernel code
(the dispatcher and personality contract, internal). Each entry follows
constitution section 52.

## Native ABI (`kernel/include/uapi/cosmo/syscall.h`)

**ABI stability: stable.** Numbers are never renumbered; new calls are
appended and `SYS_COUNT` grows. The header is shared verbatim with user
space (`libc/include/cosmo/syscall.h` includes it).

### Calling convention (x86-64)

`SYSCALL` with the number in `rax` and arguments in `rdi`, `rsi`,
`rdx`, `r10`, `r8`, `r9`; the result returns in `rax`. `rcx` and `r11`
are destroyed by the instruction; every other register is preserved.
A negative result in `[-4095, -1]` is `-errno`; anything else is a
success value (a count, a pid, an address). The kernel never returns
a kernel pointer.

Every call may block (sleep, demand-zero fault during a copy), may be
preempted, and runs with interrupts enabled on the calling thread's
kernel stack.

### Calls

| Nr | Name | Arguments | Result | Errors |
|---|---|---|---|---|
| 0 | `exit` | `int status` | never returns | none |
| 1 | `write` | `int h, const void *buf, size_t len` | bytes written (may be short) | `EFAULT` (range), `EBADF` (no handle, no WRITE right, object has no `write`) |
| 2 | `read` | `int h, void *buf, size_t len` | bytes read, 0 at end of file | `EFAULT`, `EBADF` |
| 3 | `getpid` | none | pid (> 0) | none |
| 4 | `yield` | none | 0 | none |
| 5 | `sleep_ns` | `uint64_t ns` | 0 after at least `ns` | `EINVAL` (> 1 hour) |
| 6 | `clock_ns` | none | monotonic nanoseconds since boot | none |
| 7 | `mmap` | `void *hint, size_t len, int prot, int flags` | address | `EINVAL`, `ENOMEM`, `EEXIST` |
| 8 | `munmap` | `void *addr, size_t len` | 0 | `EINVAL` |
| 9 | `log` | `const char *s, size_t len` | 0 | `EFAULT`, `EINVAL` (len ≥ 200) |
| 10 | `close` | `int h` | 0 | `EBADF` |
| 11 | `open` | `const char *path, int flags, uint32_t mode` | handle | path errors, `EEXIST`, `EISDIR`, `EROFS`, `EMFILE` |
| 12 | `stat` | `const char *path, struct cosmo_stat *st` | 0 | path errors, `EFAULT` |
| 13 | `fstat` | `int h, struct cosmo_stat *st` | 0 | `EBADF`, `EFAULT` |
| 14 | `lseek` | `int h, int64_t off, int whence` | new position | `EBADF`, `EINVAL`, `ESPIPE` |
| 15 | `mkdir` | `const char *path, uint32_t mode` | 0 | path errors, `EEXIST`, `EROFS` |
| 16 | `unlink` | `const char *path` | 0 | path errors, `EISDIR`, `EBUSY` |
| 17 | `rmdir` | `const char *path` | 0 | path errors, `ENOTDIR`, `ENOTEMPTY`, `EBUSY` |
| 18 | `rename` | `const char *old, const char *new` | 0 | path errors, `EXDEV`, `ENOTEMPTY`, `EBUSY` |
| 19 | `getdents` | `int h, void *buf, size_t len` | bytes, 0 at end | `EBADF`, `EFAULT`, `ENOTDIR`, `EINVAL` |
| 20 | `sync` | none | 0 | filesystem error |
| 21 | `mount` | `source, target, fstype, flags` | 0 | `EPERM`, `ENODEV`, `EBUSY`, `EIO` |
| 22 | `umount` | `const char *target` | 0 | `EPERM`, `EINVAL`, `EBUSY` |

Calls 11–22 (Phase 7) are specified in full, with the `O_*` flags,
`struct cosmo_stat`, `struct cosmo_dirent` and the errno values they add,
in `docs/kernel-services/vfs/api.md`. `SYS_COUNT` is 23. A file opened
with `open` is a `struct file` kobject of a `kobject_io_type`, so `read`,
`write` and `close` operate on it unchanged; the handle carries READ
and/or WRITE rights from the access mode.

Unknown numbers (including `SYS_COUNT` and above, and negative values
seen as large unsigned) return `-ENOSYS` with no side effects.

Details per call:

- **exit**: sets the process state to EXITING and ends the calling
  thread; the exit status is what `process_wait_exit` reports (the
  kernel logs it for `init`). The wrapper loops on the instruction so
  it is `noreturn` even if a future multi-threaded exit returns.
- **write**: `[buf, buf+len)` must lie inside `[0x400000,
  0x00007FFFFFFFF000)` and be mapped in the caller's space; the copy is
  done in 512-byte chunks through a kernel bounce buffer, so a fault in
  a later chunk returns the bytes written so far. `len` 0 returns 0
  after the handle check. The console object accepts every byte.
- **read**: at most 512 bytes per call. The console returns 0 (no input
  path yet).
- **sleep_ns**: bounded to 3600 s to catch garbage arguments; the wait
  is a timer sleep, resolution is the 250 Hz tick.
- **mmap**: `len` must be a non-zero page multiple; `flags` must
  include `COSMO_MAP_ANONYMOUS` (file mappings arrive with the VFS);
  `prot` is any subset of READ/WRITE/EXEC except WRITE+EXEC (W^X);
  `PROT_NONE` currently maps readable. Without `COSMO_MAP_FIXED`, the
  page-aligned `hint` at or above 4 MiB is the first-fit search start,
  falling back to `0x0000100000000000`; the result keeps one unmapped
  page between regions. With `COSMO_MAP_FIXED`, `hint` must be page
  aligned and inside the window, and an overlap is `EEXIST` (no silent
  replacement). Pages are demand-zero.
- **munmap**: `addr`/`len` must name exactly one region created by
  `mmap` (the stack and ELF segments are regions too, and can be
  unmapped whole); partial or non-matching ranges are `EINVAL`.
- **log**: copies at most 199 bytes and prints them through `klog` at
  INFO as `pid N: <text>`. Intended for early user diagnostics.
- **close**: releases the handle; the slot can be reused by a later
  install. Closing 0–2 is allowed.

### Constants

`COSMO_PROT_NONE/READ/WRITE/EXEC` (0, 1, 2, 4); `COSMO_MAP_ANONYMOUS`
(1), `COSMO_MAP_FIXED` (2); `COSMO_E*` error numbers equal to the
kernel's `errno.h` values (`EBADF` 9, `EFAULT` 14, `EEXIST` 17,
`EINVAL` 22, `EMFILE` 24, `ENOSYS` 38, and others); `COSMO_EXIT_FAULT`
139; `COSMO_STDIN/STDOUT/STDERR` 0/1/2; auxiliary vector tags
`COSMO_AT_NULL` 0, `COSMO_AT_PAGESZ` 6, `COSMO_AT_ENTRY` 9.

### Initial process state

At entry `rsp` points to `argc`, followed by `argv[0..argc-1]`, NULL,
`envp[...]`, NULL, then `(tag, value)` auxiliary pairs ending with
`AT_NULL`; strings follow. All general registers are zero, `rflags` is
`IF` only, the stack is 16-byte aligned at `argc`. Handles 0, 1, 2 are
the console with READ, WRITE, WRITE rights. The stack region is 8 MiB
below `0x00007FFFFFFF0000` with a guard page beneath it; only its top
page is populated.

## User-side wrappers (`libc/include/cosmo/syscall.h`)

`cosmo_syscall6(nr, a1..a6)` is the inline-asm primitive;
`cosmo_syscall0..4` are macros over it. Typed wrappers `cosmo_exit`
(noreturn), `cosmo_write`, `cosmo_read`, `cosmo_getpid`,
`cosmo_yield`, `cosmo_sleep_ns`, `cosmo_clock_ns`, `cosmo_mmap`,
`cosmo_munmap`, `cosmo_log`, `cosmo_close`, and since Phase 7 `cosmo_open`,
`cosmo_stat`, `cosmo_fstat`, `cosmo_lseek`, `cosmo_mkdir`, `cosmo_unlink`,
`cosmo_rmdir`, `cosmo_rename`, `cosmo_getdents`, `cosmo_sync`,
`cosmo_mount`, `cosmo_umount` return the raw kernel result
as `long`; there is no `errno` variable yet. ABI stability: the wrapper
names are the start of the native libc and are meant to stay.

## Kernel dispatcher (`kernel/include/kernel/syscall.h`)

**ABI stability: internal.**

### `int64_t syscall_dispatch(uint64_t nr, const uint64_t args[6], void *frame)`
- Purpose: route one call through the current process's personality.
- Inputs: number, six raw arguments, the opaque arch frame.
- Outputs: the value for the user's result register.
- Concurrency: called on the current thread's kernel stack with
  interrupts enabled, `irq_depth == 0`, `preempt_count == 0` (asserted).
  Panics if the current thread has no process. Increments the global
  and per-process call counters; unknown numbers also bump
  `syscall_unknown_count` and log at DEBUG.
- Blocking: whatever the handler does.

### `struct syscall_args { uint64_t nr; uint64_t a[6]; void *frame; }`
Built on the dispatcher's stack; lives for one call.

### `struct personality { const char *name; const syscall_fn *table; size_t count; }`
A handler is `int64_t (*)(struct syscall_args *)`. `personality_native`
(`kernel/syscall/native.c`) has `count == SYS_COUNT`; NULL entries are
`-ENOSYS`. A process's `pers` is set at creation and never changes in
this phase.

### `uint64_t syscall_count(void)`, `uint64_t syscall_unknown_count(void)`
Relaxed atomic reads; interrupt-safe; for diagnostics and tests.

## x86-64 entry (`kernel/arch/x86_64/syscall_entry.S`, `user.c`)

**ABI stability: internal (but `percpu` offsets 8 and 16 are asm ABI).**

`x86_syscall_entry`: `swapgs`; save the user `rsp` at `%gs:16`; load
the kernel stack from `%gs:8`; push `ss`, user `rsp`, `r11` (rflags),
`cs`, `rcx` (rip), `rax`, then `rdi rsi rdx r10 r8 r9`, then the
callee-saved `rbx rbp r12–r15`; `sti`; `call x86_syscall_c`; `cli`; pop
everything; `swapgs`; `sysretq`. The push order defines
`struct x86_syscall_frame` (`x86/trapframe.h`, 18 words):

| Offset | Field |
|---|---|
| 0–40 | `r15 r14 r13 r12 rbp rbx` |
| 48–88 | `r9 r8 r10 rdx rsi rdi` (arguments 6..1) |
| 96 | `rax` (number in, result out) |
| 104–136 | `rip cs rflags rsp ss` |

`x86_syscall_c(frame)` calls `syscall_dispatch(frame->rax, {rdi, rsi,
rdx, r10, r8, r9}, frame)` and stores the result in `frame->rax`.
Per-CPU setup is `arch_syscall_init_cpu` (`docs/kernel/process/api.md`).
