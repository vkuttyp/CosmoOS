# Linux compatibility: design

## Personality selection

`elf_validate` gains a scan of `PT_NOTE` segments: a note with name
`CosmoOS\0` (namesz 8) and type 1 sets `info->cosmo_note = true`. The
loader also records `info->phdr_vaddr` (the virtual address of the
program header table when it lies inside a `PT_LOAD`, else 0),
`info->phnum` and `info->phent` for the Linux auxiliary vector.
`process_create_from_elf` sets `p->pers = (info.cosmo_note || parent ==
NULL) ? &personality_native : &personality_linux`: a process without a
parent was created by the kernel (`init`, the self-test children) and is
native whatever its image. The note is emitted by `crt0.S`:

```asm
    .section .note.cosmo, "a", @note
    .balign 4
    .long 8, 4, 1            /* namesz, descsz, type */
    .asciz "CosmoOS"         /* 8 bytes with the NUL */
    .long 1                  /* desc: ABI version */
```

and `user.ld` places `.note.cosmo` in a `PT_NOTE` program header that
overlaps the read-only load segment. A binary that lacks it is treated
as Linux; nothing else is inspected (a Linux static binary has no
distinguishing mark either; `EI_OSABI` is 0 in both).

`user.ld`'s text segment is declared `PT_LOAD FILEHDR PHDRS` and the
sections start at `0x400000 + SIZEOF_HEADERS`, so the ELF header and
the program header table are part of the loaded image (headers at
`0x400000`, the table at `0x400040`) and `info->phdr_vaddr` is non-zero
for native and test programs alike; a program's entry is therefore no
longer exactly `0x400000`. A Linux libc walks `AT_PHDR` at start-up
(musl's `__init_tls` looks for `PT_TLS`), so a zero there would fault
before `main`.

## The Linux process

Everything is shared with a native process: `struct process`, handles,
the address space, exit and kill. Personality-specific state lives in a
small block hung off the process:

```c
struct linux_state {                 /* process->linux, allocated by linux_process_init for Linux processes */
    uint64_t brk_start, brk;         /* heap: page above info.hi; the mapping moves in pages */
    uint64_t clear_child_tid;        /* set_tid_address (stored; no threads exit yet) */
    uint64_t sigmask;                /* rt_sigprocmask */
    struct lx_sigaction act[64];     /* rt_sigaction: handler, flags, restorer, mask (32 bytes each) */
    struct lx_stack_t altstack;      /* sigaltstack: ss_sp, ss_flags, ss_size */
    unsigned unknown_syscalls;       /* diagnostics */
};
```

`process` also records `image_end` (`info.hi`) for every process; the
Linux state's `brk_start` is its page-rounded value.

The initial stack for a Linux process is built by the same
`build_initial_stack` with a personality flag: the auxiliary vector
becomes the Linux one (`AT_PHDR`, `AT_PHENT` 56, `AT_PHNUM`, `AT_PAGESZ`
4096, `AT_ENTRY`, `AT_RANDOM` pointing at 16 random bytes placed among
the strings, `AT_UID/EUID/GID/EGID` from the credentials, `AT_SECURE` 0,
`AT_HWCAP` 0, `AT_CLKTCK` 100, `AT_NULL`). The string and pointer area
now spans two populated pages (`INITIAL_STACK_PAGES` 2, written byte by
byte through the direct map since the two frames need not be adjacent)
so that `COSMO_ARG_MAX` arguments fit with the larger vector; the 16
random bytes are placed for every process, native ones simply do not
point at them.

### Thread pointer

`arch_prctl(ARCH_SET_FS, addr)` stores `addr` in `thread->tls_base`
(canonical user address required) and writes `MSR_FS_BASE` at once;
`arch_thread_switch_prepare` writes `MSR_FS_BASE` from `next->tls_base`
whenever `next` belongs to a process (kernel threads never touch `%fs`,
so a stale base is harmless there and the write is skipped). `ARCH_GET_FS`
copies it out. `%gs` is not offered (`ARCH_SET_GS` returns `-EINVAL`):
the kernel owns `GS_BASE` through `swapgs`.

### brk

`brk_start` is the page after the highest loaded segment; `brk` starts
equal to it. `brk(0)` returns the current value; `brk(addr)` maps
`[page_up(brk), page_up(addr))` anonymous read-write when growing (fails
with the old value returned, as Linux does, when the range is taken or
memory runs out; the region is capped at 1 GiB above `brk_start`) and
unmaps `[page_up(addr), page_up(brk))` when shrinking, then returns the
new `brk`.

### mmap family

Flags translate: `MAP_ANONYMOUS` (0x20) required (`ENODEV` otherwise:
file mappings are stage 2), `MAP_PRIVATE`/`MAP_SHARED` accepted (all
mappings are private), `MAP_FIXED` (0x10) forces the address (and
unmaps what was there, as Linux does: `vm_user_unmap` then map),
`MAP_NORESERVE`/`MAP_STACK`/`MAP_POPULATE` ignored. `PROT_*` bits equal
the native ones. A hint that is not page aligned is ignored, not an
error. `munmap` and `mprotect` map to `vm_user_unmap` and
`vm_user_protect`; a `mprotect` that does not cover exactly one region
returns `-EINVAL` (a recorded limit of the VMM's region granularity).

### futex (`kernel/ipc/futex.c`)

A native primitive: `futex_wait(space, uaddr, val, timeout_ns)` and
`futex_wake(space, uaddr, n)`. One table of 64 buckets hashed by
`(space, uaddr)`; each waiter is a `struct futex_waiter { space, uaddr,
struct waitqueue wq (one waiter), woken }` on the bucket list; each
bucket carries a `wake_seq`. `wait`: lock the bucket, read `wake_seq`,
unlock; `copy_from_user` the word with no lock held (a demand fault may
allocate, a fatal fault kills the process: neither may happen under a
spinlock, which `might_sleep` in `copy_from_user` enforces); compare with
`val` (`-EAGAIN` on mismatch); lock, and if `wake_seq` changed return 0
(a wake ran between the compare and the enqueue; a spurious return the
futex contract permits and musl retries), else enqueue; unlock;
`wait_event_killable` with an optional timer (`-ETIMEDOUT`; a kill gives
`-EINTR`); dequeue. `wake`: lock, bump `wake_seq`, mark and wake up to `n`
waiters of that `(space, uaddr)`, return the count. No wake is lost: a
wake that ran after the waiter's read of the word bumped the sequence the
waiter checks before sleeping (`docs/kernel/lockdep/design.md`, "futex"). The Linux call accepts `FUTEX_WAIT`
(0), `FUTEX_WAKE` (1), with or without `FUTEX_PRIVATE_FLAG` (128) and
`FUTEX_CLOCK_REALTIME`; other operations return `-ENOSYS`.

### Signals, stage 1

`rt_sigaction(sig, act, oldact, sigsetsize 8)` stores and returns
32-byte `struct k_sigaction` records (`handler`, `flags`, `restorer`,
`mask`) for signals 1..63 (`EINVAL` for `SIGKILL`/`SIGSTOP` with a
non-NULL `act`); `rt_sigprocmask` keeps a mask; `sigaltstack` keeps the
record. None of it influences delivery: there is none. `kill(pid, sig)`
and `tgkill` translate to `process_kill` (Linux and native signal
numbers coincide for 1..31; a real-time signal is delivered as
`SIGKILL`), and the target's exit status becomes `128 + sig` natively,
which the Linux `wait4` encodes as "terminated by signal `sig`".
`kill(pid, 0)` is an existence probe; a target with another uid is
`-EPERM` unless the caller is uid 0.

### Files

| Linux | Translation |
|---|---|
| `read`, `write`, `pread64`, `pwrite64` | the native handle path (`file_read`/`file_pread` or the object's `read`/`write` for pipes, console, sockets); Linux and native share the handle table |
| `readv`, `writev` | loop over up to 1024 iovecs (each validated), stop at the first short transfer |
| `open`, `openat(AT_FDCWD, ...)`, `creat` | Linux `O_*` (octal Linux values) mapped to `COSMO_O_*`; `O_CLOEXEC`, `O_NONBLOCK`, `O_NOCTTY`, `O_LARGEFILE` accepted and dropped; a `dirfd` other than `AT_FDCWD` is `-ENOSYS` |
| `close`, `lseek`, `dup`, `dup2`, `dup3`, `pipe`, `pipe2` | direct (`pipe2` flags dropped) |
| `fstat`, `stat`, `lstat`, `newfstatat` | `struct cosmo_stat` → Linux `struct stat` (144 bytes: `st_dev` 0, `st_ino`, `st_nlink`, `st_mode` = type bits (`S_IFREG` 0100000, `S_IFDIR` 040000, `S_IFCHR` 020000, `S_IFIFO` 010000, `S_IFSOCK` 0140000) or permission bits, uid, gid, `st_size`, `st_blksize` 4096, `st_blocks`, times as `timespec` from `mtime_ns`/`ctime_ns`); `AT_EMPTY_PATH` with an fd is `fstat` |
| `getdents64` | native `getdents` records → `linux_dirent64` (`d_ino`, `d_off`, `d_reclen`, `d_type`, `d_name`), through two kernel buffers; the native name starts at byte 12 of its record (`offsetof(struct cosmo_dirent, name)`, not `sizeof`, which is 16), `d_type` is mapped to Linux's `DT_*` values (`DT_REG` 8, `DT_DIR` 4, `DT_CHR` 2, `DT_FIFO` 1, `DT_SOCK` 12), each output record is the 19-byte header plus the NUL-terminated name padded to 8 bytes, and `d_off` is the offset of the next record in the buffer |
| `mkdir`, `mkdirat`, `rmdir`, `unlink`, `unlinkat` (`AT_REMOVEDIR`), `rename`, `renameat`, `chdir`, `getcwd`, `access`, `faccessat`, `fsync`, `fdatasync`, `sync`, `umask` | direct or trivial (`umask` returns 022; `access` is a `stat`) |
| `fcntl` | `F_GETFD`/`F_SETFD` 0, `F_GETFL` reconstructs the access mode, `F_SETFL` accepted, `F_DUPFD`/`F_DUPFD_CLOEXEC` via `dup`; others `-EINVAL` |
| `ioctl` | `-ENOTTY` for every request (so libcs treat the console as a non-terminal and fully buffer; recorded) |

### Processes and identity

`getpid`, `getppid`, `gettid` (= pid), `getuid`, `geteuid`, `getgid`,
`getegid` (credentials), `getpgrp`/`getpgid`/`setpgid`/`setsid` (pid or
0: no groups), `exit`, `exit_group` (identical: one thread),
`set_tid_address` (stores, returns pid), `set_robust_list` (0),
`wait4(pid, status, options, rusage)` → `process_wait_child` with
`WNOHANG`; the status is encoded: exit `n` → `n << 8`; a kill by `sig`
(native `128 + sig`) → `sig`; a fault (native 139) → `SIGSEGV` (11);
`rusage` is zeroed when given. `execve`, `fork`, `vfork`, `clone`,
`clone3` → `-ENOSYS`. `rseq`, `prlimit64`, `getrlimit`, `setrlimit`,
`sched_getaffinity`, `readlink`, `readlinkat` → `-ENOSYS` (a libc
tolerates these).

### Time and misc

`clock_gettime(CLOCK_REALTIME | CLOCK_MONOTONIC | *_COARSE | BOOTTIME)`
returns the monotonic clock (there is no wall clock; recorded),
`gettimeofday` and `time` likewise; `nanosleep` and `clock_nanosleep`
(relative; `TIMER_ABSTIME` against the monotonic clock) → the killable
sleep; `getrandom` → `random_get_bytes` (flags ignored); `uname` fills
`struct utsname` (six 65-byte fields: `Linux`, `cosmo`, `6.0.0-cosmo`,
the build id, `x86_64`, `(none)`); `sysinfo` `-ENOSYS`.

### Sockets

`socket` (`AF_INET` 2, `AF_INET6` 10, `SOCK_STREAM` 1 | `SOCK_DGRAM` 2
with `SOCK_CLOEXEC`/`SOCK_NONBLOCK` dropped), `bind`, `connect`,
`listen`, `accept`, `accept4`, `sendto`, `recvfrom`, `shutdown`,
`getsockname`, `getpeername` translate Linux `sockaddr_in` (family,
port in network order, 4-byte address) and `sockaddr_in6` (family, port,
flowinfo, 16-byte address, scope) to `struct netaddr` and back (output
length honoured, full size reported). `setsockopt` returns 0 for
`SOL_SOCKET` options (`SO_REUSEADDR`, `SO_KEEPALIVE`, `SO_BROADCAST`,
...) and `-ENOPROTOOPT` otherwise; `getsockopt` `-ENOPROTOOPT`;
`sendmsg`/`recvmsg`/`poll`/`select`/`epoll_*` `-ENOSYS`.

## Errors

Linux and native errno numbers coincide for every value the kernel
produces (the native table was laid out on the Linux numbers; this
phase added `ENOPROTOOPT` 92), so results pass through unchanged.
`personality_linux.count` is `LX_NR_MAX` (512) and every slot without an
implementation holds `lx_unknown` (filled in once, from
`linux_process_init`, before the first Linux process can make a call):
`-ENOSYS`, `unknown_syscalls++`, the first eight per process logged at
DEBUG (`linux: pid N: unimplemented system call NR`), so porting work
sees what a program wanted. A number at or above 512 never reaches the
table: the dispatcher's bounds check returns `-ENOSYS` and logs
`syscall: pid N unknown number NR (linux)`. Thirteen numbers the
personality knows but refuses (`fork`, `execve`, `clone`, `readlink`,
the rlimit calls, ...) are `lx_nosys`: `-ENOSYS` without the count.

## Ownership, concurrency, memory

`struct linux_state` is allocated by `linux_process_init` after the
image is loaded and freed by `linux_process_release` from
`process_release` and from the creation fail path. Futex waiters live on
the waiting thread's stack. The personality holds no locks of its own
beyond the futex bucket locks; every translated call takes the native
subsystem's locks through the native API. Kernel buffers for structure
translation are on the stack (`struct stat` 144 bytes, `utsname` 390,
`iovec` arrays walked one entry at a time, sockaddr 28) or bounded heap
buffers (`pread64`/`pwrite64`/stream `sendto` 4 KiB chunks;
`getdents64` two buffers of the caller's length, at most 64 KiB;
`recvfrom` at most 64 KiB).

## Security

A Linux process is as untrusted as a native one and goes through the
same validation: every user pointer through `uaccess`, every path
through the VFS, every handle through the table with rights. Nothing in
the personality grants what the native ABI would refuse (invariant 7
read the other way). `uname` lying about the kernel is a presentation
choice, not a privilege.

## Testing strategy

`tests/linux/lxtest.c`: a freestanding x86-64 program with its own
`_start`, raw `syscall` wrappers with Linux numbers, and the Linux
structure layouts written out by hand; it checks every area above and
prints `LINUXTEST: PASS` or `LINUXTEST: FAIL <what>`. `lxhello` prints
one line. Both are built by `tests/linux/linux.mk` with the user flags
and `user.ld` (no crt0, no libc, therefore no CosmoOS note) and carried
in the boot archive under `tests/linux/` (mode 0755 for `tests/`
entries). `/etc/rc.test` runs them; the harness requires their markers
in self-test builds. When `musl-gcc` exists on the build host,
`hello_musl.c` is compiled `-static` with it and run too (`hello from
musl`), and the harness requires that line when the build reported musl
(`HAVE_MUSL=1`); without `musl-gcc`, `MUSL_GCC=` may name any command
with the same interface, such as a wrapper compiling in an Alpine
container. Host tests: `test_linux` compiles the pure conversion
helpers (`compat/linux/convert.c`: flags, stat, dirent, wait status,
sockaddr) under ASan/UBSan. Kernel self-test `linux-elf`: the loader
sees the note in `init` (and a program header address), and its absence
in `lxhello` (whose table is at `0x400040`). Details in `testing.md`.

## Future extensibility

- Stage 2: `PT_INTERP` and file-backed `mmap` for the dynamic linker;
  `clone` with `CLONE_THREAD` once the kernel has user threads; signal
  frames and `rt_sigreturn`; `execve` once the native side has it.
- Stage 3: `epoll`/`poll`, `sendmsg`, `/proc`.
- Another architecture's Linux ABI (AArch64) reuses everything but the
  numbers table and `arch_prctl`. Phase 13 put the x86-64 table under
  `#if defined(ARCH_X86_64)` and left an empty table elsewhere;
  `linux_process_init/release/auxv` and the conversions are already
  generic, and the AArch64 test programs (`tests/linux`) are excluded
  from that build until the table exists.
