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
| 8 | `munmap` | `void *addr, size_t len` | 0 | `EINVAL` (range, or a page in it is unmapped) |
| 9 | `log` | `const char *s, size_t len` | 0 | `EFAULT`, `EINVAL` (len ≥ 200), `EAGAIN` (an unprivileged caller past 64 lines, refilled at 16 per second) |
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
| 22 | `umount` | `const char *target, unsigned flags` (`COSMO_UMOUNT_FORCE`) | 0 | `EPERM`, `EINVAL`, `EBUSY`, the commit's error |
| 23 | `socket` | `int family, int type, int proto` | handle (READ and WRITE) | `EAFNOSUPPORT`, `EINVAL`, `ENOMEM`, `EMFILE` |
| 24 | `bind` | `int h, const struct cosmo_sockaddr *sa, size_t len` | 0 | `EBADF`, `EFAULT`, `EINVAL`, `EAFNOSUPPORT`, `EPERM`, `EADDRINUSE`, `EADDRNOTAVAIL` |
| 25 | `listen` | `int h, int backlog` | 0 | `EBADF`, `EOPNOTSUPP`, `EINVAL` |
| 26 | `accept` | `int h, struct cosmo_sockaddr *peer, size_t *len` | handle (READ and WRITE) | `EBADF`, `EOPNOTSUPP`, `EINVAL`, `EFAULT`, `EMFILE` |
| 27 | `connect` | `int h, const struct cosmo_sockaddr *sa, size_t len` | 0 | `EBADF`, `EFAULT`, `EINVAL`, `EAFNOSUPPORT`, `EISCONN`, `ECONNREFUSED`, `ETIMEDOUT`, `ENETUNREACH` |
| 28 | `sendto` | `int h, const void *buf, size_t len, const struct cosmo_sockaddr *to, size_t tolen` | bytes sent | `EBADF`, `EFAULT`, `EINVAL`, `EMSGSIZE`, `ENOTCONN`, `EISCONN`, `EPIPE`, `ECONNRESET` |
| 29 | `recvfrom` | `int h, void *buf, size_t len, struct cosmo_sockaddr *from, size_t *fromlen` | bytes, 0 at end of stream | `EBADF`, `EFAULT`, `EINVAL`, `ENOTCONN`, `ECONNRESET` |
| 30 | `shutdown` | `int h, int how` | 0 | `EBADF`, `EINVAL` |
| 31 | `getsockname` | `int h, struct cosmo_sockaddr *sa, size_t *len` | 0 | `EBADF`, `EFAULT` |
| 32 | `spawn` | `const struct cosmo_spawn *req` | the child's pid | `EPERM` (`COSMO_SPAWN_SETCRED` naming ids the caller may not grant), `EAGAIN` (`COSMO_RLIMIT_NPROC`), `EFAULT`, `EINVAL` (unknown flags, NULL path/argv, empty argv, bad map), `E2BIG`, `ENAMETOOLONG`, `EBADF` (map names a free handle), path errors, `ENOTDIR` (cwd), `EACCES` (not a regular executable file), `ENOEXEC`, `ENOMEM` |
| 33 | `wait` | `int pid, int *status, unsigned flags` | the reaped pid; 0 with `COSMO_WNOHANG` when none exited | `EINVAL` (pid 0 or < -1, unknown flag), `ECHILD`, `EINTR`, `EFAULT` |
| 34 | `kill` | `int pid, int sig` | 0 | `EINVAL` (sig outside 1..31, pid <= 0), `ESRCH`, `EPERM` |
| 35 | `pipe` | `int h[2]` | 0; `h[0]` reads, `h[1]` writes | `EFAULT`, `ENOMEM`, `EMFILE` |
| 36 | `dup` | `int h, int target` | the new handle | `EBADF`, `EINVAL` (target < -1 or >= 64), `EMFILE` |
| 37 | `getppid` | none | the parent's pid, 0 for a kernel-created process | none |
| 38 | `chdir` | `const char *path` | 0 | `EFAULT`, `ENAMETOOLONG`, path errors, `ENOTDIR` |
| 39 | `getcwd` | `char *buf, size_t len` | the path length (the NUL is written too) | `ERANGE`, `EFAULT` |
| 40 | `procinfo` | `struct cosmo_procinfo *buf, size_t count` | the number of processes the caller may see (up to `count` records filled): every process for a privileged caller, else those with its real uid | `EFAULT`, `ENOMEM` |
| 41 | `klog` | `char *buf, size_t len` | bytes copied: the newest whole log lines that fit | `EFAULT`, `ENOMEM` |
| 42 | `sysctl` | `const char *name, char *buf, size_t len` | the value's length (NUL written when it fits) | `ENOENT`, `EFAULT` |
| 56 | `getrlimit` | `unsigned resource, uint64_t *value` | 0 | `EINVAL`, `EFAULT` |
| 57 | `setrlimit` | `unsigned resource, uint64_t value` | 0 | `EINVAL` (resource, `NOFILE` > 64), `EPERM` (raising without privilege) |
| 43 | `vm_create` | `int vmm_h` (a handle to `/dev/vmm` open for writing) | a VM handle | `EBADF`, `EPERM`, `ENOTSUP`, `ENOSPC`, `ENOMEM`, `EMFILE` |
| 44 | `vm_mem` | `int vm, uint64_t gpa, uint64_t len` | 0 (a zeroed guest memory region) | `EBADF`, `EINVAL`, `ENOSPC`, `ENOMEM` |
| 45 | `vm_mem_rw` | `int vm, uint64_t gpa, void *buf, size_t len, int write` | bytes copied | `EBADF`, `EINVAL`, `EFAULT`, `ENOMEM` |
| 46 | `vcpu_create` | `int vm, unsigned index` | a vCPU handle | `EBADF`, `ENOTSUP`, `EINVAL`, `EEXIST`, `ENOMEM`, `EMFILE` |
| 47 | `vcpu_regs` | `int vcpu, struct cosmo_vcpu_regs *regs, int set` | 0 | `EBADF`, `EFAULT`, `EINVAL` |
| 48 | `vcpu_run` | `int vcpu, struct cosmo_vm_exit *exit` | 0, `*exit` filled | `EBADF`, `EFAULT`, `ENOTSUP`, `EIO`, `EINTR`, `ENOMEM` |
| 49 | `vcpu_irq` | `int vcpu, unsigned vector` | 0 | `EBADF`, `EINVAL` |

Calls 11–22 (Phase 7) are specified in full, with the `O_*` flags,
`struct cosmo_stat`, `struct cosmo_dirent` and the errno values they add,
in `docs/kernel-services/vfs/api.md`. Calls 23–31 (Phase 8), with
`struct cosmo_sockaddr`, `COSMO_AF_*`, `COSMO_SOCK_*`, `COSMO_SHUT_*`
and their errno values, are in `docs/kernel-services/network/api.md`.
Calls 32–42 (Phase 9) are specified below and in
`docs/kernel/process/api.md`, `docs/kernel/ipc/api.md`,
`docs/kernel/tty/api.md`. Calls 43–49 (Phase 12), with
`struct cosmo_vcpu_regs`, `struct cosmo_vm_exit`, the `COSMO_VM_EXIT_*`
kinds and their errno values, are in
`docs/kernel-services/virtualization/api.md`; a VM handle is an I/O
object (`read` drains the guest's debug console, `fstat` is
`COSMO_DT_CHR`), a vCPU handle only closes. Calls 50–55 are the
credential calls, 56–57 the resource limits (`docs/kernel/security/api.md`);
`SYS_COUNT` is 58. A file opened with `open`
is a `struct file` kobject of a `kobject_io_type`, so `read`, `write`
and `close` operate on it unchanged; the handle carries READ and/or
WRITE rights from the access mode. A socket from `socket` or `accept` is
likewise a `struct socket` kobject with `read`/`write` (`recvfrom`/
`sendto` without an address); a pipe end from `pipe` has `read` or
`write`. `fstat` works on every I/O object with a `stat` operation:
files (the vnode's attributes), the console (`COSMO_DT_CHR`, mode 0620)
and pipe ends (`COSMO_DT_FIFO`, `size` = bytes in the pipe); sockets
have none yet (`EBADF`).

Unknown numbers (including `SYS_COUNT` and above, and negative values
seen as large unsigned) return `-ENOSYS` with no side effects.

Details per call:

- **exit**: sets the process state to EXITING and ends the calling
  thread; the exit status is what `process_wait_exit` reports (the
  kernel logs it for `init`). The wrapper loops on the instruction so
  it is `noreturn` even if a future multi-threaded exit returns.
- **write**: `[buf, buf+len)` must lie inside `[0x400000,
  0x00007FFFFFFFF000)` and be mapped in the caller's space; the copy is
  done in 1024-byte chunks through a kernel bounce buffer, so a fault in
  a later chunk returns the bytes written so far. `len` 0 returns 0
  after the handle check. The console object accepts every byte.
- **read**: at most 1024 bytes per call. The console blocks until a line
  has been typed and returns at most one line (`docs/kernel/tty/api.md`);
  a pipe returns what is buffered or 0 when every write end is gone.
- **sleep_ns**: bounded to 3600 s to catch garbage arguments; the wait
  is a timer sleep, resolution is the 250 Hz tick.
- **mmap**: `len` must be a non-zero page multiple; `flags` must
  include `COSMO_MAP_ANONYMOUS` (file mappings arrive with the VFS);
  `prot` is any subset of READ/WRITE/EXEC except WRITE+EXEC (W^X);
  `PROT_NONE` reserves the range: every access, from user code or from
  a system call given a pointer into it, faults (`-EFAULT` for the
  call, `COSMO_EXIT_FAULT` for the process). Without `COSMO_MAP_FIXED`, the
  page-aligned `hint` at or above 4 MiB is the first-fit search start,
  falling back to `0x0000100000000000`; the result keeps one unmapped
  page between regions. With `COSMO_MAP_FIXED`, `hint` must be page
  aligned and inside the window, and an overlap is `EEXIST` (no silent
  replacement). Pages are demand-zero.
- **munmap**: any page-aligned range inside the window whose every page
  is mapped (by `mmap`, the stack or an ELF segment: regions split as
  needed); a range with an unmapped page is `EINVAL` and nothing
  changes. This strict rule is the native contract; the Linux
  personality's `munmap` skips unmapped pages.
- **EFAULT** everywhere: a user pointer that names an unmapped,
  `PROT_NONE` or wrong-permission page, or one the kernel cannot
  populate for lack of memory, makes the call return `EFAULT`; the
  process is never terminated by a system call's access to its memory
  (`docs/kernel/memory/design.md` §6.1). A call that fails this way may
  have partly written its destination up to the faulting page.
- **log**: copies at most 199 bytes and prints them through `klog` at
  INFO as `pid N: <text>`. Intended for early user diagnostics.
- **close**: releases the handle; the slot can be reused by a later
  install. Closing 0–2 is allowed.
- **spawn**: `req` is `struct cosmo_spawn { path, argv, envp, handles,
  nr_handles, cwd, flags }`; `flags` must be 0; `argv` is required with
  `argv[0]`; `envp` may be NULL; `argv` and `envp` together may hold at
  most `COSMO_ARG_ENTRIES` (128) strings of at most `COSMO_ARG_MAX`
  (2048) bytes including terminators, else `E2BIG`; `handles` is an
  array of `nr_handles` (at most 64) `struct cosmo_spawn_handle {
  child, parent }` pairs, each copying the caller's handle `parent` with
  its rights into the child's slot `child` (slots distinct, in range;
  `handles == NULL` with `nr_handles == 0` copies the caller's 0, 1, 2);
  `cwd` (optional) names the child's working directory relative to the
  caller's; the file must be regular with an execute bit and at most
  16 MiB. The child gets the caller's uid/gid and is the caller's child
  for `wait`. Every pointer is copied before use (`EFAULT`).
- **wait**: `pid` is a child's pid or -1 for any child; `status` may be
  NULL; the status is the child's exit status (`exit(n)` gives `n & 0xff`,
  a kill `128 + sig`, a fault 139); the child is gone once collected.
  Blocks until a matching child exits unless `COSMO_WNOHANG`; `EINTR`
  when the caller is killed while waiting.
- **kill**: `sig` is `1..31` (`COSMO_SIGHUP` 1, `COSMO_SIGINT` 2,
  `COSMO_SIGKILL` 9, `COSMO_SIGSEGV` 11, `COSMO_SIGTERM` 15); every
  signal terminates the target with status `128 + sig` at its next
  system call, return from an interrupt, or killable wait. The caller
  must have the target's uid or uid 0. A zombie is a valid target
  (nothing happens).
- **pipe**: two new handles in the lowest free slots, READ on `h[0]`,
  WRITE on `h[1]`; `docs/kernel/ipc/api.md` for the stream's rules.
- **dup**: `target == -1` takes the lowest free slot; otherwise `target`
  (0..63) is closed first if occupied and the copy installed there;
  `dup(h, h)` returns `h`. Rights are copied.
- **chdir**/**getcwd**: the working directory is a vnode reference plus
  a normalised absolute path (`.`, `..` and repeated slashes resolved);
  `getcwd` needs `len` at least the path length plus one (`ERANGE`).
- **procinfo**: `struct cosmo_procinfo { pid, ppid, uid, gid, state (0
  running, 1 exiting, 2 zombie), nr_threads, syscalls, run_ns, name[32] }`
  in table order; `count` above 4096 is clamped; call again with a larger
  buffer when the result exceeds `count`.
- **klog**: at most 32 KiB (`KLOG_RING_SIZE`); the ring holds every
  emitted line, oldest overwritten first; reading does not consume.
- **sysctl**: names `kernel.name`, `kernel.version`, `kernel.build`,
  `kernel.arch`, `kernel.uptime_ns`, `kernel.nprocs`, `hw.ncpu`,
  `vm.page_size`, `vm.pages_total`, `vm.pages_free`, `vm.cache_pages`
  and `vm.cache_limit` (the page cache's size and its reclaim limit,
  `docs/kernel/security/design.md` §3), since Phase 12
  `hv.backend`, `hv.vms`, `hv.vcpus`, `hv.exits`
  (`docs/kernel-services/virtualization/api.md`), in debug builds
  `debug.faultinject` (the fault-injection rules and counters, one line
  per kind; `docs/verification/api.md`), and `sysctl.names`
  (the list, newline separated); values are strings; read-only.

### Constants

`COSMO_PROT_NONE/READ/WRITE/EXEC` (0, 1, 2, 4); `COSMO_MAP_ANONYMOUS`
(1), `COSMO_MAP_FIXED` (2); `COSMO_RLIMIT_AS/MEM/NOFILE/NPROC/VMEM`
(0–4), `COSMO_RLIM_INFINITY`; `COSMO_SPAWN_SETCRED` (1) with the
`uid`/`gid` fields of `struct cosmo_spawn`; `COSMO_E*` error numbers equal to the
kernel's `errno.h` values (`EBADF` 9, `EFAULT` 14, `EEXIST` 17,
`EINVAL` 22, `EMFILE` 24, `ENOSYS` 38, and others; Phase 9 adds `ESRCH`
3, `EINTR` 4, `E2BIG` 7, `ENOEXEC` 8, `ECHILD` 10, `EACCES` 13,
`ENOTTY` 25, `ESPIPE` 29, `ERANGE` 34); `COSMO_EXIT_FAULT` 139;
`COSMO_STDIN/STDOUT/STDERR` 0/1/2; auxiliary vector tags `COSMO_AT_NULL`
0, `COSMO_AT_PAGESZ` 6, `COSMO_AT_ENTRY` 9; `COSMO_DT_FIFO` 4,
`COSMO_DT_SOCK` 5 (reserved); `COSMO_WNOHANG` 1; `COSMO_SIG*`,
`COSMO_NSIG` 32; `COSMO_ARG_MAX` 2048, `COSMO_ARG_ENTRIES` 128,
`COSMO_PATH_MAX` 1024.

### Initial process state

At entry `rsp` points to `argc`, followed by `argv[0..argc-1]`, NULL,
`envp[...]`, NULL, then `(tag, value)` auxiliary pairs ending with
`AT_NULL`; strings follow. All general registers are zero, `rflags` is
`IF` only, the stack is 16-byte aligned at `argc`. Handles 0, 1, 2 of a
kernel-created process (init) are the console with READ, WRITE, WRITE
rights; a spawned process has exactly the handles its parent mapped.
The working directory is the parent's (the root for init). The stack region is 8 MiB
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
`cosmo_mount`, `cosmo_umount`, and since Phase 8 `cosmo_socket`,
`cosmo_bind`, `cosmo_listen`, `cosmo_accept`, `cosmo_connect`,
`cosmo_sendto`, `cosmo_recvfrom`, `cosmo_shutdown`, `cosmo_getsockname`
(over `cosmo_syscall5`), and since Phase 9 `cosmo_spawn`, `cosmo_wait`,
`cosmo_kill`, `cosmo_pipe`, `cosmo_dup`, `cosmo_getppid`, `cosmo_chdir`,
`cosmo_getcwd`, `cosmo_procinfo`, `cosmo_klog`, `cosmo_sysctl`, and since
Phase 12 `cosmo_vm_create`, `cosmo_vm_mem`, `cosmo_vm_mem_read/write`,
`cosmo_vcpu_create`, `cosmo_vcpu_get/set_regs`, `cosmo_vcpu_run`,
`cosmo_vcpu_irq` (`cosmo/hv.h`), return the raw kernel result as `long`. The C library (`docs/libc/`) translates
them into `errno` and the standard names; programs use the library, the
raw wrappers are internal to it (and to `init --selftest`).

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
  `syscall_unknown_count` and log at DEBUG. Calls `process_check_kill`
  before and after the handler: a process with a pending kill exits
  here instead of returning to user mode.
- Blocking: whatever the handler does.

### `struct syscall_args { uint64_t nr; uint64_t a[6]; void *frame; }`
Built on the dispatcher's stack; lives for one call.

### `struct personality { const char *name; const syscall_fn *table; size_t count; }`
A handler is `int64_t (*)(struct syscall_args *)`. `personality_native`
(`kernel/syscall/native.c`) has `count == SYS_COUNT`; NULL entries are
`-ENOSYS`. `personality_linux` (`compat/linux/syscalls.c`, Phase 11)
has `count == 512` and no NULL entry (unimplemented numbers are a
counting `-ENOSYS` handler); `docs/compat/linux/api.md`. A process's
`pers` is set at creation (by the CosmoOS ELF note) and never changes.

### `int64_t syscall_handle_read(int h, uint64_t ubuf, size_t len)`, `int64_t syscall_handle_write(int h, uint64_t ubuf, size_t len)`, `int syscall_handle_stat(int h, struct cosmo_stat *st)`
The bodies of the native `read`, `write` and `fstat` (handle lookup
with the right, `IO_CHUNK` copies, the object's `read`/`write`/`stat`
operation), exported so the Linux personality's `read`, `write`,
`readv`, `writev`, `fstat` and `newfstatat(AT_EMPTY_PATH)` are the same
code. The native `sys_read`/`sys_write`/`sys_fstat` wrap them; they
take no Linux argument and know nothing of the caller's personality.
May block like the calls they implement.

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
