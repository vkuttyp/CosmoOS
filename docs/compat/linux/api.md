# Linux compatibility: API

Every interface of the Linux personality: the ELF note that selects a
personality, the Linux system calls the table translates (with the
deviations a program can observe), the pure conversion functions, the
process hooks, the futex primitive and the thread-pointer hook.
Stability (constitution section 52): the Linux ABI is Linux's and is
therefore frozen from the outside; what this document fixes is *which
subset* is honoured and *how* each call deviates. The kernel-internal
interfaces (`linux_process_init`, `futex_wait`, `arch_set_tls_base`) are
internal and may change with the code and this document together.
Nothing here is a native interface: the native ABI
(`docs/kernel/syscall/api.md`, 43 calls) is unchanged by this phase
(invariant 7).

## The CosmoOS ELF note (`libc/src/crt0.S`, `userland/user.ld`, `kernel/process/elf.c`)

**ABI stability: stable** (a program built today must keep being native).

| Field | Value |
|---|---|
| Section | `.note.cosmo`, flags `"a"`, type `@note`, 4-byte aligned |
| `namesz` | 8 |
| `descsz` | 4 |
| `type` | 1 |
| `name` | `CosmoOS\0` (8 bytes) |
| `desc` | `uint32_t` ABI version, currently 1 (not interpreted yet) |
| Program header | `PT_NOTE` (`note` in `user.ld`'s `PHDRS`), overlapping the read-only `PT_LOAD` (`:rodata :note`) |

`elf_validate` walks every `PT_NOTE` segment (bounds-checked against the
file; `namesz` at most 256, `descsz` at most 4096, a malformed note ends
the walk without failing the image) and sets `info->cosmo_note` when a
note with `namesz` 8, type 1 and name `CosmoOS` is found. The note is
emitted by `crt0.S`, so every program linked with the native libc has
it; a program without `crt0.o` (the `tests/linux/` programs) or built by
a foreign toolchain has none. `user.ld` also puts the ELF header and
program header table inside the text segment (`FILEHDR PHDRS`; sections
start at `0x400000 + SIZEOF_HEADERS`), which is what lets the loader
publish `AT_PHDR`: a native program's headers are at `0x400000`, its
program header table at `0x400040`, and its entry is no longer exactly
`0x400000`.

`struct elf_info` (`kernel/include/kernel/elf.h`) gained `bool
cosmo_note`, `uint64_t phdr_vaddr` (the program header table's virtual
address when it lies inside a `PT_LOAD`'s file bytes, else 0),
`uint16_t phnum`, `uint16_t phent`.

## Personality selection (`kernel/process/process.c`)

```c
p->pers = (info.cosmo_note || parent == NULL) ? &personality_native : &personality_linux;
```

A process created by the kernel (`parent == NULL`: `init`, the self-test
children of the kernel) is always native regardless of its image. A
`spawn`ed image without the note runs under `personality_linux`. The
choice is made once and never changes. After `elf_load_into`, `p->
image_end = info.hi` (the page after the highest loaded segment) and,
for a Linux process, `linux_process_init` allocates `p->linux`.

## Process hooks (`kernel/include/kernel/process.h`, `compat/linux/syscalls.c`)

**ABI stability: internal.**

### `int linux_process_init(struct process *p, const struct elf_info *info)`
- Purpose: allocate the personality's per-process state
  (`struct linux_state`: `brk_start`, `brk`, `clear_child_tid`,
  `sigmask`, `act[64]`, `altstack`, `unknown_syscalls`) and set
  `brk_start = brk = page_align_up(info->hi)`. Also completes the
  personality table the first time (every empty slot becomes
  `lx_unknown`), before any Linux process can make a call.
- Called from `process_create_from_elf` after the image is loaded and
  before the stack is built. May allocate (`kzalloc`); returns `-ENOMEM`
  (creation then fails and `linux_process_release` runs on the fail
  path).

### `void linux_process_release(struct process *p)`
- Frees `p->linux` and clears the pointer. Called from
  `process_release` (the last reference) and from the creation fail
  path; both guard on `p->linux != NULL`, so it never runs for a native
  process. Does not block.

### `unsigned linux_auxv(struct process *p, const struct elf_info *info, uint64_t random_addr, uint64_t *w, unsigned max)`
- Purpose: write the Linux auxiliary vector into the word array
  `build_initial_stack` is assembling; returns the number of words
  written (at most `max`, 28 today). Order and values:

| Tag | Value |
|---|---|
| `AT_PHDR` (3) | `info->phdr_vaddr` |
| `AT_PHENT` (4) | `info->phent` (56) |
| `AT_PHNUM` (5) | `info->phnum` |
| `AT_PAGESZ` (6) | 4096 |
| `AT_ENTRY` (9) | `info->entry` |
| `AT_RANDOM` (25) | `random_addr`: 16 bytes from `random_get_bytes`, 16-byte aligned, placed below the strings |
| `AT_UID` (11), `AT_EUID` (12) | `p->cred.uid` |
| `AT_GID` (13), `AT_EGID` (14) | `p->cred.gid` |
| `AT_SECURE` (23) | 0 |
| `AT_HWCAP` (16) | 0 |
| `AT_CLKTCK` (17) | 100 |
| `AT_NULL` (0) | 0 |

The native vector is unchanged (`AT_PAGESZ`, `AT_ENTRY`, `AT_NULL`);
the 16 random bytes are placed for every process but only the Linux
vector points at them. The initial frame spans two eagerly populated
stack pages (`INITIAL_STACK_PAGES` 2) and up to 300 argument and
environment strings (`INITIAL_STRINGS_MAX`).

### `extern const struct personality personality_linux`
`{ .name = "linux", .table = g_table, .count = LX_NR_MAX (512) }`.
`g_table` is `linux_table` with `lx_unknown` in every empty slot; a
number at or above 512 never reaches the table (the dispatcher's own
bounds check returns `-ENOSYS` and logs `unknown number ... (linux)`).

## Linux system calls (`compat/linux/syscalls.c`)

**ABI stability: Linux's.** Numbers are the x86-64 table
(`compat/linux/linux_abi.h`, `LX_*`). Arguments arrive in `rdi rsi rdx
r10 r8 r9`, the result in `rax`, `-errno` negative; errno values are the
native ones, which coincide with Linux's for every value the kernel
produces. Every call runs on the calling thread's kernel stack with
interrupts enabled and may block; every user pointer passes through
`uaccess` (`copy_from_user`, `copy_to_user`, `strncpy_from_user`,
`user_range_ok`); every handle passes through the process's handle table
with rights (a Linux fd *is* a native handle: 0/1/2 are the console or
whatever `spawn` mapped). 100 numbers have an entry: 87 are translated,
13 return `-ENOSYS` explicitly (listed at the end); everything else
returns `-ENOSYS` through `lx_unknown`, which increments
`linux_state.unknown_syscalls` and logs the first eight per process at
DEBUG (`linux: pid N: unimplemented system call NR`).

### Files and directories

| Nr | Call | Translation | Deviations |
|---|---|---|---|
| 0, 1 | `read`, `write` | `syscall_handle_read`/`syscall_handle_write` (the native `read`/`write` bodies: files, pipes, console, sockets, `IO_CHUNK` 1024) | none |
| 17, 18 | `pread64`, `pwrite64` | `file_pread`/`file_pwrite` in 4 KiB chunks through a kernel buffer | `-EINVAL` for a negative offset; regular files only (`-EBADF` for other objects) |
| 19, 20 | `readv`, `writev` | up to `IOV_MAX` 1024 iovecs, each through the `read`/`write` path; stops at the first short transfer; returns the partial count if an error follows progress | zero-length iovecs skipped |
| 2 | `open` | `lx_open_flags` → `vfs_open(cwd, path, flags, mode & 07777)`; handle rights from the access mode | `O_CLOEXEC O_NONBLOCK O_NOCTTY O_LARGEFILE O_NOFOLLOW` accepted and dropped; any other unknown flag `-EINVAL` |
| 85 | `creat` | `open(O_WRONLY|O_CREAT|O_TRUNC)` | |
| 257 | `openat` | as `open` after `check_dirfd` | `dirfd` must be `AT_FDCWD` (-100) unless the path is absolute; otherwise `-ENOSYS` |
| 3 | `close` | `handle_close` | |
| 8 | `lseek` | `file_seek` (`SEEK_*` coincide) | `-ESPIPE` for a handle that is not a file (pipe, socket, console) |
| 4, 6, 5 | `stat`, `lstat`, `fstat` | `vfs_stat` / `syscall_handle_stat` → `lx_stat_from_native` (144 bytes) | `lstat` is `stat` (no symlinks); `st_dev`, `st_rdev` 0; `st_atime` = `st_mtime`; `fstat` works on every I/O object (pipes report `S_IFIFO`, sockets `S_IFSOCK`, the console `S_IFCHR`) |
| 262 | `newfstatat` | empty path with `AT_EMPTY_PATH` (0x1000) → `fstat(dirfd)`; else `check_dirfd` then `stat` | other flags ignored |
| 217 | `getdents64` | `file_readdir` into a kernel buffer of `len - len/4` bytes, `lx_dirents_from_native` into a second buffer of `len`, copied out | `len` clamped to 64 KiB, `-EINVAL` below 32; `d_off` is the offset of the next record in *this* buffer, not a seekable cookie |
| 83, 258 | `mkdir`, `mkdirat` | `vfs_mkdir(cwd, path, mode & 07777)` | `mkdirat`: `check_dirfd` |
| 84 | `rmdir` | `vfs_rmdir` | |
| 87, 263 | `unlink`, `unlinkat` | `vfs_unlink`; `unlinkat` with `AT_REMOVEDIR` (0x200) → `vfs_rmdir` | `check_dirfd` |
| 82, 264 | `rename`, `renameat` | `vfs_rename` | `check_dirfd` on both dirfds |
| 80 | `chdir` | `process_chdir` | |
| 79 | `getcwd` | copies `cwd_path` with its NUL; returns the length **including** the NUL (Linux's raw syscall behaviour) | `-ERANGE` when it does not fit |
| 21, 269 | `access`, `faccessat` | existence only (`vfs_stat`) | mode ignored (no permission enforcement yet); `check_dirfd` |
| 74, 75 | `fsync`, `fdatasync` | `file_sync` | identical |
| 162 | `sync` | `vfs_sync` | |
| 95 | `umask` | returns 022 | nothing stored |
| 32 | `dup` | `handle_install` of the same object with the same rights (lowest free slot) | |
| 33 | `dup2` | closes the target, `handle_install_at`; `dup2(fd, fd)` returns `fd` | target must be `0..63` (`-EBADF`) |
| 292 | `dup3` | as `dup2` | `-EINVAL` when both are equal; flags dropped |
| 22, 293 | `pipe`, `pipe2` | `pipe_create`; read end with READ, write end with WRITE | `pipe2` flags (`O_CLOEXEC`, `O_NONBLOCK`) dropped; `-EMFILE` installs nothing |
| 72 | `fcntl` | `F_GETFD F_SETFD F_SETFL` → 0; `F_GETFL` → `O_RDONLY`/`O_WRONLY`/`O_RDWR` reconstructed from the handle's rights; `F_DUPFD`, `F_DUPFD_CLOEXEC` (1030) → first free slot at or above `arg` | other commands `-EINVAL` |
| 16 | `ioctl` | `-ENOTTY` for every request on a valid handle | libcs then treat the console as a non-terminal (full buffering; `isatty` false) |

### Memory

| Nr | Call | Translation | Deviations |
|---|---|---|---|
| 12 | `brk` | `brk(0)` returns the break; growing maps `[page_up(brk), page_up(addr))` anonymous RW (`vm_user_map_anon`, region name `brk`); shrinking unmaps `[page_up(addr), page_up(brk))`; the break is stored exactly as requested | on any failure (below `brk_start`, more than 1 GiB above it, range taken, out of memory) the **unchanged** break is returned, as Linux does |
| 9 | `mmap` | `MAP_ANONYMOUS` required; `lx_prot`; `MAP_FIXED` → `vm_user_unmap` then map at `addr` (must be page aligned and in range, else `-EINVAL`); otherwise `vm_user_find_free` from a page-aligned hint, falling back to `USER_MMAP_BASE` | file mappings `-ENODEV`; `PROT_WRITE|PROT_EXEC` `-EINVAL` (W^X); `PROT_NONE` maps read-only; `MAP_SHARED` accepted (mappings are private); `MAP_NORESERVE`, `MAP_STACK`, `MAP_POPULATE` ignored; `len` 0 or larger than the user window `-EINVAL`; no free range `-ENOMEM` |
| 11 | `munmap` | `vm_user_unmap` of the page-rounded range | `-EINVAL` unless `addr` is page aligned, `len` non-zero and the range is a user range |
| 10 | `mprotect` | `vm_user_protect` | same W^X and alignment rules; `vm_user_protect` requires the range to be exactly one region (`-EINVAL` otherwise: a recorded VMM limit) |
| 28 | `madvise` | 0 | advice ignored |

### Process, identity, signals

| Nr | Call | Translation | Deviations |
|---|---|---|---|
| 60, 231 | `exit`, `exit_group` | `process_exit(status & 0xff)` | identical (one thread per process) |
| 39, 186 | `getpid`, `gettid` | `pid` | `gettid == getpid` |
| 110 | `getppid` | `parent_pid` | |
| 102, 107 | `getuid`, `geteuid` | `cred.uid` | |
| 104, 108 | `getgid`, `getegid` | `cred.gid` | |
| 111, 112 | `getpgrp`, `setsid` | return `pid` | no process groups or sessions |
| 109 | `setpgid` | 0 | |
| 218 | `set_tid_address` | stores `clear_child_tid`, returns `pid` | never written (no thread exit yet) |
| 273 | `set_robust_list` | 0 | |
| 158 | `arch_prctl` | `ARCH_SET_FS` (0x1002): `arch_set_tls_base(addr)`; `ARCH_GET_FS` (0x1003): copies `thread->tls_base` out | `SET_FS` with a non-zero address that is not 8 readable user bytes `-EPERM`; `ARCH_SET_GS`/`ARCH_GET_GS` and anything else `-EINVAL` |
| 61 | `wait4` | `process_wait_child(pid, WNOHANG ? PROCESS_WAIT_NOHANG : 0)`; status through `lx_wait_status`; `rusage` zeroed (144 bytes) when given | `pid == 0` or `pid < -1` `-ECHILD` (no groups); `-ECHILD` with no children |
| 62 | `kill` | `sig == 0`: existence probe (`process_lookup`); else `process_kill(target, sig < 32 ? sig : SIGKILL)` | `pid <= 0` `-ESRCH`; `sig` outside `1..63` `-EINVAL`; another uid's process `-EPERM` unless uid 0; the target terminates with native status `128 + sig` |
| 234 | `tgkill` | `kill(tid, sig)` (`tgid` ignored) | |
| 13 | `rt_sigaction` | stores and returns 32-byte `struct k_sigaction` records (`handler flags restorer mask`) in `linux_state.act[sig]` | `sigsetsize` must be 8; `sig` in `1..63`; a non-NULL `act` for `SIGKILL`/`SIGSTOP` `-EINVAL`; **nothing is ever delivered** |
| 14 | `rt_sigprocmask` | `SIG_BLOCK`/`SIG_UNBLOCK`/`SIG_SETMASK` on `linux_state.sigmask`; old mask out | `sigsetsize` must be 8; the mask influences nothing |
| 131 | `sigaltstack` | stores and returns `linux_state.altstack` | never used |
| 24 | `sched_yield` | `sched_yield` | |

### Time, random, system

| Nr | Call | Translation | Deviations |
|---|---|---|---|
| 228 | `clock_gettime` | `clock_now_ns` as `timespec` for clock ids `0..7` | **every clock is the monotonic clock** (no wall clock yet); id above 7 `-EINVAL` |
| 96 | `gettimeofday` | monotonic time as `timeval` | timezone argument ignored |
| 201 | `time` | monotonic seconds | |
| 35 | `nanosleep` | `thread_sleep_ns_killable` | `tv_sec` above one year or a bad `tv_nsec` `-EINVAL`; on `-EINTR` the remainder is written as 0 |
| 230 | `clock_nanosleep` | as `nanosleep`; `TIMER_ABSTIME` (1) is taken relative to the monotonic clock | clock ids `0..7` |
| 318 | `getrandom` | `random_get_bytes` in 256-byte pieces | flags ignored; at most 256 KiB per call |
| 63 | `uname` | `sysname "Linux"`, `nodename "cosmo"`, `release "6.0.0-cosmo"`, `version "<KERNEL_NAME> <KERNEL_VERSION> <COSMO_BUILD_ID>"`, `machine "x86_64"`, `domainname "(none)"` (six 65-byte fields, 390 bytes) | a presentation decision so libcs' version checks pass |
| 202 | `futex` | `FUTEX_WAIT` (0) → `futex_wait(space, uaddr, val, timeout)` (a `timespec` timeout of zero becomes 1 ns so it still times out); `FUTEX_WAKE` (1) → `futex_wake(space, uaddr, val)`; `FUTEX_PRIVATE_FLAG` (128) and `FUTEX_CLOCK_REALTIME` (256) masked off | other operations `-ENOSYS`; `uaddr` must be 4 readable user bytes (`-EFAULT`) and 4-byte aligned (`-EINVAL`) |

### Sockets

Addresses are Linux `sockaddr_in` (16 bytes) or `sockaddr_in6` (28
bytes) converted by `lx_sockaddr_to_netaddr`/`lx_sockaddr_from_netaddr`;
output addresses honour the caller's length and report the full size,
as Linux does.

| Nr | Call | Translation | Deviations |
|---|---|---|---|
| 41 | `socket` | `ksock_create` for `AF_INET` (2)/`AF_INET6` (10), `SOCK_STREAM` (1)/`SOCK_DGRAM` (2); handle with READ and WRITE | `SOCK_NONBLOCK`/`SOCK_CLOEXEC` dropped; other families `-EAFNOSUPPORT` (`AF_UNIX` included); other types `-EINVAL`; protocol ignored |
| 49 | `bind` | `ksock_bind` | address length `2..28` |
| 42 | `connect` | `ksock_connect` | |
| 50 | `listen` | `ksock_listen` | |
| 43, 288 | `accept`, `accept4` | `ksock_accept`; peer address out; new handle with READ and WRITE | `accept4` flags dropped |
| 44 | `sendto` | stream: `ksock_sendto` in 4 KiB chunks, stops at a short send; datagram: one send of the whole buffer | datagram above 64 KiB `-EMSGSIZE`; flags ignored |
| 45 | `recvfrom` | `ksock_recvfrom` into a buffer of at most 64 KiB; source address out when asked | flags ignored |
| 48 | `shutdown` | `ksock_shutdown` (`SHUT_*` coincide) | |
| 51, 52 | `getsockname`, `getpeername` | `ksock_getsockname`/`ksock_getpeername` | |
| 54 | `setsockopt` | 0 for `SOL_SOCKET` (1), `-ENOPROTOOPT` otherwise | nothing stored |
| 55 | `getsockopt` | `-ENOPROTOOPT` | |

### Explicit `-ENOSYS` (13 entries)

`clone` 56, `fork` 57, `vfork` 58, `execve` 59, `readlink` 89,
`getrlimit` 97, `sysinfo` 99, `setrlimit` 160, `sched_getaffinity` 204,
`readlinkat` 267, `prlimit64` 302, `rseq` 334, `clone3` 435. These are
`lx_nosys`, not `lx_unknown`: they are known and refused, so they are
not counted as unknown. `poll` 7, `select` 23, `rt_sigreturn` 15,
`mremap` 25, `msync` 26, `pause` 34, `sendmsg` 46, `recvmsg` 47 have
numbers in `linux_abi.h` but no entry: they go through `lx_unknown`.

## Conversions (`compat/linux/convert.h`, `convert.c`)

**ABI stability: internal.** Pure functions: no kernel state, no
locks, no allocation, interrupt-safe; compiled unchanged by
`tests/host/test_linux.c`.

| Function | Purpose | Result |
|---|---|---|
| `int lx_open_flags(unsigned lx, unsigned *native)` | Linux `O_*` (octal) → `COSMO_O_*` | 0; -1 for an access mode of 3 or a flag outside the known set |
| `void lx_stat_from_native(const struct cosmo_stat *, struct lx_stat *)` | native stat → 144-byte Linux `stat` | type bits from `COSMO_DT_*` (`S_IFDIR S_IFCHR S_IFIFO S_IFSOCK`, else `S_IFREG`), permission bits `& 07777`, `st_blksize` 4096, `st_blocks` = size/512 rounded up, `mtime`/`ctime` split into seconds and nanoseconds, `atime` = `mtime` |
| `int lx_wait_status(int native)` | native exit status → Linux wait word | `COSMO_EXIT_FAULT` (139) → 11 (`SIGSEGV`); `128 < n < 192` → `n - 128` (killed by that signal); else `(n & 0xff) << 8` (exited) |
| `int lx_sockaddr_to_netaddr(const void *sa, size_t len, struct netaddr *out)` | Linux sockaddr → `netaddr` | 0; `-EINVAL` when `len` is below 2 or below the family's size; `-EAFNOSUPPORT` for any family but `AF_INET`/`AF_INET6` |
| `size_t lx_sockaddr_from_netaddr(const struct netaddr *in, void *out, size_t cap)` | `netaddr` → Linux sockaddr, at most `cap` bytes written | the full size (16 or 28) regardless of `cap` |
| `uint8_t lx_dirent_type(uint8_t native)` | `COSMO_DT_*` → Linux `DT_*` | `REG` 1→8, `DIR` 2→4, `CHR` 3→2, `FIFO` 4→1, `SOCK` 5→12, else 0 |
| `size_t lx_dirents_from_native(const uint8_t *in, size_t inlen, uint8_t *out, size_t outcap)` | native `getdents` records → `linux_dirent64` records | bytes written; the native name starts at byte 12 (`offsetof(struct cosmo_dirent, name)`, not `sizeof`); each output record is the 19-byte header, the name, a NUL, padded to 8; stops before a record that would not fit and at the first malformed input record (`reclen` below 12, past the input, or `namelen` past `reclen`) |
| `int lx_prot(unsigned lx, int *native)` | `PROT_*` → `COSMO_PROT_*` | 0; -1 for a bit outside `READ|WRITE|EXEC` |

Structure layouts (`linux_abi.h`), asserted by the host test: `struct
lx_stat` 144, `struct lx_utsname` 390, `struct lx_sockaddr_in` 16,
`struct lx_sockaddr_in6` 28, `struct lx_sigaction` 32; `struct
lx_dirent64` is a 19-byte header (`d_ino d_off d_reclen d_type`)
followed by `d_name`.

## Futex (`kernel/include/kernel/futex.h`, `kernel/ipc/futex.c`)

**ABI stability: internal.** A native primitive keyed by `(struct
vm_space *, user address)`; 64 buckets, each a spinlock and a waiter
list. Waiters live on the waiting thread's stack.

### `int futex_wait(struct vm_space *space, uint64_t uaddr, uint32_t val, uint64_t timeout_ns)`
- Purpose: block while the 32-bit user word at `uaddr` equals `val`.
- Under the bucket lock: `copy_from_user` the word (`-EFAULT`), compare
  (`-EAGAIN` when it differs), enqueue. Then `wait_event_killable` on a
  private wait queue with an optional one-shot timer; the waker and the
  timer wake the thread directly (`sched_wake`).
- Outputs: 0 when woken by `futex_wake` (even if the timer also fired),
  `-ETIMEDOUT` after `timeout_ns` (0 means no timeout), `-EINTR` when the
  process is killed, `-EINVAL` for a misaligned address.
- Thread context only; may block; takes the bucket lock with interrupts
  off for the compare and the dequeue.

### `int futex_wake(struct vm_space *space, uint64_t uaddr, unsigned n)`
- Wakes up to `n` waiters of exactly that `(space, uaddr)`, in FIFO
  order, marking each `woken` before `sched_wake`; returns how many.
  `-EINVAL` for a misaligned address. Any thread context; does not
  block; takes the bucket lock with interrupts off.

## Thread pointer (`kernel/include/arch/user.h`, `kernel/arch/x86_64/user.c`, `context.c`)

**ABI stability: internal (arch interface).**

### `void arch_set_tls_base(uintptr_t base)`
Stores `base` in the current thread's `tls_base` and writes
`MSR_FS_BASE` (0xC0000100) immediately, so `%fs:` accesses see it before
the call returns. `arch_thread_switch_prepare(next)` writes
`MSR_FS_BASE` from `next->tls_base` whenever `next->proc != NULL`, so
the value follows the thread across every switch; kernel threads never
use `%fs` and skip the write. `%gs` stays the kernel's (`swapgs`).

## Files and make targets

| Path | Content |
|---|---|
| `compat/linux/linux_abi.h` | the Linux ABI written out: numbers, flags, `AT_*`, structures; shared with `tests/linux/lxabi.h` |
| `compat/linux/convert.h`, `convert.c` | the pure conversions above |
| `compat/linux/syscalls.c` | `struct linux_state`, the hooks, every `lx_*` handler, the table, `personality_linux` |
| `kernel/ipc/futex.c`, `kernel/include/kernel/futex.h` | the futex primitive |
| `tests/linux/linux.mk` | `make linux-tests`: `lxhello.elf`, `lxtest.elf`, and `hello_musl` when `MUSL_GCC` is set or `musl-gcc` is found; `HAVE_MUSL` (0/1) is passed to the boot harness by `make test` |
