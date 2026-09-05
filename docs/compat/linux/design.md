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
new `brk`. Growth merges into the existing heap region and a shrink
splits it (`docs/kernel/memory/design.md` §6.3), so shrink and regrow
in any order keep working; a shrink that fails (it cannot on a
well-formed heap) leaves the break unchanged.

### mmap family

Flags translate: `MAP_ANONYMOUS` (0x20) required (`ENODEV` otherwise:
file mappings are stage 2), `MAP_PRIVATE`/`MAP_SHARED` accepted (all
mappings are private), `MAP_FIXED` (0x10) forces the address (and
unmaps what was there, as Linux does: `vm_user_unmap` then map),
`MAP_NORESERVE`/`MAP_STACK`/`MAP_POPULATE` ignored. `PROT_*` bits equal
the native ones; `PROT_NONE` reserves and traps. A hint that is not page
aligned is ignored, not an error. `munmap` takes any range and skips
unmapped pages (0); `mprotect` takes any range whose pages are all
mapped, splitting and merging regions, and is `-ENOMEM` across a hole,
as on Linux; a `MAP_FIXED` mapping replaces whatever was there.

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
| `close`, `lseek`, `dup`, `dup2`, `dup3`, `pipe`, `pipe2` | direct; `pipe2(O_NONBLOCK)` sets both ends non-blocking, `O_CLOEXEC` is dropped |
| `fstat`, `stat`, `lstat`, `newfstatat` | `struct cosmo_stat` → Linux `struct stat` (144 bytes: `st_dev` 0, `st_ino`, `st_nlink`, `st_mode` = type bits (`S_IFREG` 0100000, `S_IFDIR` 040000, `S_IFCHR` 020000, `S_IFIFO` 010000, `S_IFSOCK` 0140000) or permission bits, uid, gid, `st_size`, `st_blksize` 4096, `st_blocks`, times as `timespec` from `mtime_ns`/`ctime_ns`); `AT_EMPTY_PATH` with an fd is `fstat` |
| `getdents64` | native `getdents` records → `linux_dirent64` (`d_ino`, `d_off`, `d_reclen`, `d_type`, `d_name`), through two kernel buffers; the native name starts at byte 12 of its record (`offsetof(struct cosmo_dirent, name)`, not `sizeof`, which is 16), `d_type` is mapped to Linux's `DT_*` values (`DT_REG` 8, `DT_DIR` 4, `DT_CHR` 2, `DT_FIFO` 1, `DT_SOCK` 12), each output record is the 19-byte header plus the NUL-terminated name padded to 8 bytes, and `d_off` is the offset of the next record in the buffer |
| `mkdir`, `mkdirat`, `rmdir`, `unlink`, `unlinkat` (`AT_REMOVEDIR`), `rename`, `renameat`, `chdir`, `getcwd`, `access`, `faccessat`, `fsync`, `fdatasync`, `sync`, `umask` | direct or trivial (`umask` returns 022; `access` is a `stat`) |
| `fcntl` | `F_GETFD`/`F_SETFD` 0, `F_GETFL` reconstructs the access mode and adds `O_NONBLOCK` when the object is non-blocking, `F_SETFL` sets or clears `O_NONBLOCK` on the object (other status flags dropped; objects that never block accept silently), `F_DUPFD`/`F_DUPFD_CLOEXEC` via `dup`; others `-EINVAL` |
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
`clone3` → `-ENOSYS`. `rseq`, `sched_getaffinity`, `readlink`,
`readlinkat` → `-ENOSYS` (a libc tolerates these).

`getrlimit`, `setrlimit` and `prlimit64` (self only; another pid is
`-EPERM`) map `RLIMIT_AS`, `RLIMIT_RSS` (→ `COSMO_RLIMIT_MEM`),
`RLIMIT_NOFILE` and `RLIMIT_NPROC` onto the native limits
(`docs/kernel/security/design.md` §2). The kernel keeps one value per
resource: a read reports `rlim_cur == rlim_max`; a write stores
`rlim_max` (`cur > max` is `-EINVAL`; raising needs privilege, `-EPERM`).
Every other Linux resource reads as infinity and accepts any value
without effect.

### Time and misc

`clock_gettime(CLOCK_REALTIME | CLOCK_MONOTONIC | *_COARSE | BOOTTIME)`
returns the monotonic clock (there is no wall clock; recorded),
`gettimeofday` and `time` likewise; `nanosleep` and `clock_nanosleep`
(relative; `TIMER_ABSTIME` against the monotonic clock) → the killable
sleep; `getrandom` → `random_get_bytes` (flags ignored); `uname` fills
`struct utsname` (six 65-byte fields: `Linux`, `cosmo`, `6.0.0-cosmo`,
the build id, `x86_64`, `(none)`); `sysinfo` `-ENOSYS`.

### Sockets

`socket` (`AF_INET` 2, `AF_INET6` 10, `SOCK_STREAM` 1 | `SOCK_DGRAM` 2;
`SOCK_NONBLOCK` makes the socket non-blocking, `SOCK_CLOEXEC` is
dropped), `bind`, `connect` (`-EINPROGRESS`/`-EALREADY` on a
non-blocking socket), `listen`, `accept`, `accept4` (`SOCK_NONBLOCK` on
the accepted socket; `-EAGAIN` from a non-blocking listener), `sendto`,
`recvfrom` (`-EAGAIN` when a non-blocking socket would wait), `shutdown`,
`getsockname`, `getpeername` translate Linux `sockaddr_in` (family,
port in network order, 4-byte address) and `sockaddr_in6` (family, port,
flowinfo, 16-byte address, scope) to `struct netaddr` and back (output
length honoured, full size reported). `setsockopt` returns 0 for
`SOL_SOCKET` options (`SO_REUSEADDR`, `SO_KEEPALIVE`, `SO_BROADCAST`,
...) and `-ENOPROTOOPT` otherwise; `getsockopt` `-ENOPROTOOPT`;
`sendmsg`/`recvmsg`/`poll`/`select`/`epoll_*` `-ENOSYS`.

## Stage 2 (audit milestone 10)

Finding #30 and §12.3–12.6 of the audit. The kernel side (threads, the
signal core, `arch_user_regs`, the full-restore exit, the wall clock) is
`docs/kernel/process/design.md` §11; this section is the personality's
part, and the AArch64 table.

### Signals

`rt_sigaction` stores into the shared `struct sigaction_k` table (Linux's
`struct k_sigaction` has the same four words); `rt_sigprocmask`,
`sigaltstack`, `sigpending`, `rt_sigsuspend` and `pause` work on the
calling thread's sets; `kill(pid, sig)` is `signal_send(process)` after
the credential check, `tgkill(tgid, tid, sig)` is `signal_send_thread`
on the thread whose Linux tid matches (`-ESRCH` otherwise, `-EINVAL` when
`tgid` is not the process). Signal 0 probes. `SIGKILL` and `SIGSTOP`
refuse `rt_sigaction` as before.

`signal_setup_frame` builds Linux's `rt_sigframe` on the interrupted
stack (or the alternate stack with `SA_ONSTACK`), 16-byte aligned:

- **x86-64**: `pretcode` (the `SA_RESTORER` address; a handler without
  `SA_RESTORER` gets `SIGSEGV`, as on Linux), `struct ucontext` (`uc_flags`,
  `uc_link` 0, `uc_stack`, `mcontext` with the 23 `gregs` in Linux's
  order, `fpstate` pointing at the frame's 512-byte FXSAVE image,
  `uc_sigmask` = the mask before the handler), `siginfo` (128 bytes:
  `si_signo`, `si_errno` 0, `si_code` `SI_USER`/`SI_TKILL`/`SEGV_MAPERR`,
  `si_pid`/`si_uid` of the sender or `si_addr` of a fault), the FXSAVE
  image. Entry: `rdi` = sig, `rsi` = &siginfo, `rdx` = &ucontext, `rip` =
  handler, `rsp` = &pretcode, `rax` = 0 (an interrupted call's restart is
  arranged before the frame is pushed: `rax` = nr, `rdi` = the first
  argument, `rip` -= 2). The FXSAVE image is the thread's live vector
  state saved into the frame; `rt_sigreturn` loads it back through a
  kernel-built XSAVE header that names only the x87 and SSE components
  (a value the CPU cannot refuse), so AVX registers above the SSE
  halves do not survive a handler (a recorded gap: Linux saves the
  extended area too).
- **AArch64**: `siginfo`, then `struct ucontext` (`uc_flags`, `uc_link`,
  `uc_stack`, `uc_sigmask`, `mcontext`: `fault_address`, `regs[31]`, `sp`,
  `pc`, `pstate`, a reserved area holding an `esr_context` (syndrome 0:
  the fault's ESR is not carried yet) and the terminator; no
  `fpsimd_context`, FP/SIMD is off at EL0). Entry: `x0`,
  `x1`, `x2` as above, `pc` = handler, `sp` = the frame, `lr` = the
  restorer: `SA_RESTORER` when set, else the kernel's trampoline (below).
  Restart: `x8` = nr, `x0` = the first argument, `pc` -= 4.

`rt_sigreturn` reads the `ucontext` back (x86-64: `rsp` points at it
after the restorer's `ret` popped `pretcode`; AArch64: `sp` points at the
frame record below the `rt_sigframe`), loads the FXSAVE image when the
frame names one, and hands the register set to the core's
`signal_return`: `cs` and `ss` are the user selectors whatever the frame
says, `rflags` keeps only the user-changeable bits, a non-canonical `rip`
becomes 0 (the SYSRET guard, `docs/kernel/process/design.md` §11), the
mask is set and the full-restore exit requested. A frame that cannot be
read is a `SIGSEGV` on the thread, delivered at this call's exit. The
handler entry clears `DF`, `TF` and `RF`; `uc_stack` describes the
*interrupted* context's relation to the alternate stack (Linux's
`sas_ss_flags(sp)`: `SS_DISABLE` with none configured, 0 when the
handler was moved onto it, `SS_ONSTACK` when already on it).

**The signal trampoline.** Every Linux process gets one read-only,
executable page at `LX_SIGTRAMP` (`0x7FFFFFFF1000`, the page above the
stack's top with one unmapped page between) holding `mov $15, %eax; syscall` / `mov x8, #139; svc #0` and
`ud2`/`brk`. AArch64 handlers return through it (the arm64 kernel has no
`SA_RESTORER` in common use); on x86-64 it is present for symmetry and
used when a handler has no restorer *and* the process asked for it
through `SA_RESTORER` = 0 — which Linux refuses; so does this kernel
(`SIGSEGV`). The page is a `VM_REGION_ANON` region named `sigtramp`,
populated at process creation.

Default dispositions follow Linux except the stop signals (ignored: no
job control), and `SIGKILL`/`SIGTERM`/`SIGINT` and the rest terminate
with `128 + sig`, the status `wait4` encodes as a termination by signal.

### Threads: `clone`

`clone(flags, stack, ptid, ctid, tls)` (x86-64 argument order; AArch64
swaps `tls` and `ctid`) is accepted only with `CLONE_VM | CLONE_THREAD |
CLONE_SIGHAND` (musl's `pthread_create` set: plus `CLONE_FS`,
`CLONE_FILES`, `CLONE_SYSVSEM`, `CLONE_SETTLS`, `CLONE_PARENT_SETTID`,
`CLONE_CHILD_CLEARTID`, `CLONE_DETACHED`); anything else, `fork`,
`vfork` and `clone3` are `-ENOSYS`. The child is `process_add_thread`
with the caller's frame (result 0, `rsp`/`sp` = `stack`, `fs`/`tpidr_el0`
= `tls`), `*ptid` and `*ctid` written as the flags ask, `clear_child_tid`
recorded on the thread. `set_tid_address` stores it too and returns the
Linux tid; `gettid` returns it; `exit` ends the calling thread only
unless it is the last (`exit_group` ends the process). `futex` gains
`FUTEX_REQUEUE` and `FUTEX_CMP_REQUEUE` (waiters move from one word to
another under both buckets' locks, lower address first with the second
annotated nested for lockdep; a waiter leaves whichever list it is on
when it wakes; `CMP` checks the first word first, `-EAGAIN`, and the
check is atomic against the bucket's other operations: the bucket's
`queue_seq` is noted, the word compared unlocked, and the act happens
under the locks only if the sequence is unchanged, else the compare is
redone — the same shape as `futex_wait`'s compare-then-enqueue; both
operations return woken + requeued as the Linux kernel does),
`FUTEX_WAIT_BITSET` and
`FUTEX_WAKE_BITSET` with the all-ones set only (`-ENOSYS` for a real
bitset; an absolute timeout on the named clock, already-past deadlines
answer `-ETIMEDOUT` after the value check), and honours
`FUTEX_CLOCK_REALTIME` (`-ENOSYS` with plain `WAIT`, as Linux).
`sched_getaffinity` reports the online CPUs in 8 bytes (`-EINVAL` for a
shorter set, `-ESRCH` for an unknown pid); `sched_setaffinity` accepts
and ignores. `tkill(tid, sig)` finds the thread in the caller's process
(or another process's main thread by pid). A clone with `CLONE_THREAD`
but without `CLONE_SIGHAND` or `CLONE_VM`, or with a flag outside the
set, is `-EINVAL`; without `CLONE_THREAD` (a fork) `-ENOSYS`. The
child's FPU state is the reset state (a recorded deviation).

### Dynamic executables

`elf_validate` accepts `ET_DYN` (`info->is_dyn`; segment addresses are
relative and the caller chooses the load bias) and records `PT_INTERP`
(`info->interp`, a path of at most 255 bytes inside the file). `spawn`
loads a PIE at `USER_PIE_BASE` (`0x555500000000`) and, when the image
names an interpreter, reads that file too (through the caller's path
lookup and `VFS_MAY_EXEC` check, `ET_DYN` or `ET_EXEC`), loads it at the
first free range at or above `USER_INTERP_BASE` (`0x7F0000000000`) and
starts the process at the interpreter's entry with `AT_BASE` = its bias,
`AT_ENTRY` = the program's entry, `AT_PHDR`/`AT_PHNUM` = the program's
table, `AT_EXECFN` = the path and `AT_PLATFORM` = the machine string.
`mmap` with a file (`MAP_PRIVATE`, or `MAP_SHARED` without `PROT_WRITE`,
which is the same thing for a file nobody else writes) maps an anonymous
region and fills it from the file (`file_pread` through a bounce buffer
into the caller's own new mapping, bytes past the end zero), then
applies the requested protection: a private file mapping is a snapshot,
which is what a dynamic linker needs for text and data and what a
`MAP_PRIVATE` mapping is allowed to be. `MAP_SHARED | PROT_WRITE` on a
file is `-EOPNOTSUPP` (no page-cache-backed regions yet). Offsets must
be page aligned (`-EINVAL`).

### `poll` and `ppoll`

Both translate `struct pollfd` to the kernel's `io_poll`
(`docs/kernel/io/design.md`, "Polling"): `POLLIN` ↔ `COSMO_IO_READABLE`,
`POLLOUT` ↔ `WRITABLE`, `POLLHUP` ↔ `HANGUP`, `POLLERR` ↔ `ERROR`;
`POLLNVAL` for a handle that is not an I/O object, a negative `fd` is
skipped, the timeout is milliseconds (`-1` for ever) or a timespec.
`ppoll`'s temporary mask is applied around the wait. `select` and
`epoll` stay stage 3.

### Wall clock

`CLOCK_REALTIME` and `CLOCK_REALTIME_COARSE` read `clock_realtime_ns()`;
`gettimeofday` and `time` too; `MONOTONIC`, `MONOTONIC_RAW`,
`MONOTONIC_COARSE` and `BOOTTIME` stay the monotonic clock.
`clock_nanosleep(TIMER_ABSTIME)` subtracts the named clock's now.

### The AArch64 table

The system-call numbers move out of `linux_abi.h` into
`nr_x86_64.h` and `nr_aarch64.h` (the generic table: `openat` 56,
`read` 63, `write` 64, `exit` 93, `exit_group` 94, `futex` 98, `clone`
220, ...), selected by the architecture; calls that exist only on x86-64
(`open`, `stat`, `pipe`, `dup2`, `fork`, `poll`, `select`, `time`,
`access`, `readlink`, `rename`, `mkdir`, `rmdir`, `unlink`, `creat`,
`getpgrp`, `arch_prctl`) are defined only there and their table entries
are guarded. `struct lx_stat` has the AArch64 layout (128 bytes) under
that architecture, `uname` reports `aarch64`, the thread pointer is
`tpidr_el0` which user code sets itself, `clone` takes `tls` before
`ctid`, and signal frames follow the arm64 layout above. The one
personality source compiles for both; the empty table is gone.
`tests/linux` builds for AArch64 as well (`lxabi.h` carries an `svc`
wrapper and an arm64 `_start`), and the boot test requires `LINUXTEST:
PASS` on both machines.

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
- Stage 3: `epoll`/`poll` (over the object readiness operation that
  milestone 8 added), `sendmsg`, `/proc`.
- Another architecture's Linux ABI (AArch64) reuses everything but the
  numbers table and `arch_prctl`. Phase 13 put the x86-64 table under
  `#if defined(ARCH_X86_64)` and left an empty table elsewhere;
  `linux_process_init/release/auxv` and the conversions are already
  generic, and the AArch64 test programs (`tests/linux`) are excluded
  from that build until the table exists.
