# Linux compatibility: invariants

Rules that must not be broken without changing this document and the
code together. Each names how it is checked today and what is not yet
covered.

**L1. Personality is chosen by the CosmoOS note and by nothing else,
and kernel-created processes are native.**
`process_create_from_elf` sets `p->pers = (info.cosmo_note || parent ==
NULL) ? &personality_native : &personality_linux`; no header field,
path, name or flag takes part, and the choice never changes for the life
of the process. Check: kernel self-test `linux-elf` (`init` carries the
note, `tests/linux/lxhello` does not); `/etc/rc.test` runs `lxhello`,
`lxtest` and (when built) `hello_musl` from the shell and their output
proves they ran as Linux; `init` itself (kernel-created) is native.
Gap: no test spawns a note-less image from the kernel to see it run
native; a native program linked without `crt0.o` silently becomes a
Linux process (by design, documented).

**L2. The native ABI is unchanged by the Linux personality**
(constitution invariant 7). `SYS_COUNT` is still 43; no native call,
number, structure or errno meaning changed; `kernel/syscall/native.c`
only *exported* the bodies of `read`, `write` and `fstat` as
`syscall_handle_read/write/stat` for the translation to call; the one
header addition (`ENOPROTOOPT` 92 in `kernel/errno.h`) is a new value
nothing native returns. Nothing under `kernel/`, `kernel-services/` or
`libc/` includes `compat/linux/*.h`; the only kernel references to the
personality are the `extern personality_linux`, the three hook
prototypes in `kernel/process.h` and the `struct linux_state *linux`
pointer. Check: `init --selftest` (`USERTEST: PASS`, every native call)
and the shell test pass unchanged; `grep -rn "compat/linux\|linux_abi"
kernel kernel-services libc` finds only comments and the hook
prototypes. Gap: review only; no build barrier stops a future native
file from including `linux_abi.h`.

**L3. Linux state lives and dies with the process.** `p->linux` is
allocated only in `linux_process_init` (called only for
`personality_linux`), freed in `process_release` and on the creation
fail path, both guarded by `p->linux != NULL`; nothing else holds a
pointer to it; every `lx_*` handler reaches it through the current
process. Check: review; the boot test's `[DEBUG] process: ... released`
lines for `lxtest` and `hello_musl`. Gap: no leak counter compares
allocations with releases.

**L4. A futex wake between the compare and the sleep is never lost.**
`futex_wait` reads the user word, compares it and enqueues the waiter
under the bucket spinlock; `futex_wake` takes the same lock to find and
mark waiters; a waiter marked `woken` returns 0 even if its timer also
fired; the waiter dequeues itself under the lock before returning, so a
stack-resident waiter never outlives its frame on a list. Check:
`lxtest` (`FUTEX_WAIT` with the wrong value `-EAGAIN`, with a 20 ms
timeout `-ETIMEDOUT`, `FUTEX_WAKE` with no waiter returns 0); review of
`futex.c`. Gap: no two-thread test (a single-threaded process cannot
wake itself); the primitive's contention behaviour is untested until
user threads exist.

**L5. Signals are recorded, never delivered.** `rt_sigaction`,
`rt_sigprocmask` and `sigaltstack` store into `struct linux_state` and
read back exactly what was stored; no code path invokes a user handler,
builds a signal frame or consults `sigmask`; `kill` and `tgkill`
translate to `process_kill`, which terminates the target exactly as the
native `kill` does (status `128 + sig`, which `lx_wait_status` reports
as "killed by `sig`"). Check: `lxtest` stores a handler for signal 2
and reads it back, is refused for `SIGKILL`, sees its mask round-trip;
`hello_musl` runs musl's start-up (which installs nothing but reads
the mask). Gap: a Linux program that relies on a handler running
(`SIGCHLD`, `SIGALRM`, `SIGPIPE` ignored) misbehaves; recorded as stage
2 work in `design.md`.

**L6. No Linux system call number crashes or panics the kernel.** Every
slot of the 512-entry table is a function (`lx_unknown` where nothing is
implemented: `-ENOSYS`, counted, first eight logged); numbers at or
above 512 fail the dispatcher's bounds check (`-ENOSYS`, logged); every
implemented handler validates its arguments before touching kernel
state. Check: `lxtest` calls 510 (in range, unimplemented) and 9999 (out
of range) and expects `-ENOSYS` from both; the boot log shows both
diagnostics. Gap: no fuzzing of argument values across the table
(constitution section 60 names the method; the host cannot run the
handlers, so it would be a target-side program).

**L7. Linux structure layouts are fixed, byte for byte.** `struct
lx_stat` 144 bytes, `struct lx_utsname` 390, `struct lx_sockaddr_in` 16,
`struct lx_sockaddr_in6` 28, `struct lx_sigaction` 32, `struct
lx_dirent64` a 19-byte header; `linux_dirent64` records are 8-byte
aligned with a NUL after the name; `getcwd` returns the length including
the NUL. Check: `tests/host/test_linux.c` asserts every size and the
dirent layout; `lxtest` reads `st_size`, `st_mode`, `d_type`,
`d_reclen`, `getsockname`'s 16 with a canary after a 4-byte buffer;
`hello_musl` (a real libc) parses `uname`'s result and prints the pid.
Gap: `struct stat`'s time fields and `rusage`'s 144 zero bytes are
checked by review only.

**L8. The translation calls native services by their C interfaces,
never by number.** No `lx_*` handler indexes `personality_native.table`
or issues a native system call; files go through `vfs_*`/`file_*`,
sockets through `ksock_*`, processes through `process_*`, memory through
`vm_user_*`, and the three shared bodies through
`syscall_handle_read/write/stat`. Check: `grep -n "personality_native\|
sys_" compat/linux/syscalls.c` finds nothing. Gap: review only.

**L9. The heap stays where the program expects it.** `brk_start` is
`page_align_up(info->hi)` (the page after the highest loaded segment);
`brk` never moves below `brk_start` nor more than 1 GiB (`LX_BRK_MAX`)
above it; growth maps only `[page_up(old), page_up(new))`, shrinking
unmaps only `[page_up(new), page_up(old))`; a failed `brk` returns the
unchanged break and changes no mapping. `mmap` without `MAP_FIXED`
allocates from `USER_MMAP_BASE` (`0x1000_0000_0000`, far above any
break) or from the caller's aligned hint, so the two never collide by
default. Check: `lxtest` grows the break by 8 KiB, writes the pages,
shrinks back, and sees a request below `brk_start` refused with the old
value; `hello_musl`'s `malloc` (musl uses `brk` first, then `mmap`).
Gap: no test of the 1 GiB cap or of a `MAP_FIXED` mapping placed over
the heap (allowed, as on Linux).

**L10. The thread pointer follows the thread.** `arch_prctl(ARCH_SET_FS)`
stores into `thread->tls_base` and writes `MSR_FS_BASE` at once;
`arch_thread_switch_prepare` writes `MSR_FS_BASE` from
`next->tls_base` on every switch to a thread with a process, so a
Linux process resumes with its own `%fs` base after any preemption or
sleep, and a native process resumes with 0. Check: `lxtest` sets the
base, reads it back with `ARCH_GET_FS`, reads a word through `%fs:0`,
sleeps 10 ms (a switch away and back) and reads through `%fs:0` again;
`hello_musl` runs musl's TLS set-up and `errno` accesses through `%fs`.
Gap: no test with two Linux processes alternating on one CPU; `%gs`
cannot be set (`-EINVAL`), by design.

**L11. Every Linux call validates like a native call.** User pointers go
through `uaccess` (`copy_from_user`, `copy_to_user`,
`strncpy_from_user`, `user_range_ok`); paths are copied with
`strncpy_from_user` into `VFS_PATH_MAX` and resolved by the VFS from the
process's cwd; handles resolve through the process's table with the
rights the operation needs (READ for `read`/`getdents64`/`accept`/
`recvfrom`, WRITE for `write`/`sendto`, none for metadata); structure
copies use bounded kernel buffers (`readv` one iovec at a time,
`pread64`/`sendto` in 4 KiB chunks, `getdents64`/`recvfrom` at most 64
KiB). A Linux process can obtain nothing the native ABI would refuse.
Check: `lxtest` (`close` twice `-EBADF`, `stat` of a missing path
`-ENOENT`, `ioctl` `-ENOTTY`, `F_GETFL` reflects the pipe end's rights);
review. Gap: no hostile-pointer sweep over the Linux table like `init
--selftest`'s over the native one.

**L12. W^X holds for Linux mappings.** `mmap` and `mprotect` refuse
`PROT_WRITE|PROT_EXEC` with `-EINVAL` before reaching the VMM; the
loader's rules (W^X segments, no executable stack, `PT_INTERP`
refused) apply to Linux images unchanged. Check: `lxtest` requests an
RWX anonymous mapping and gets `-EINVAL`; the loader tests in
`docs/kernel/process/testing.md`. Gap: a program that maps RW, writes
code and `mprotect`s to RX (a JIT) works, as on Linux; nothing prevents
it, by design.

## Gaps (documented, not invariants)

- Stage 2 of constitution section 40 is not attempted: no `PT_INTERP`,
  no file-backed `mmap`, no `clone`/`fork`/`execve`, no signal frames,
  no `rt_sigreturn`, no `poll`/`select`/`epoll`, no `sendmsg`/`recvmsg`.
- `dirfd` arguments other than `AT_FDCWD` are refused (`-ENOSYS`)
  unless the path is absolute; the VFS has no `openat` semantics yet.
- Every clock is monotonic; `uname` reports a kernel release that is not
  the kernel's; `ioctl` is `-ENOTTY` for every request, so Linux libcs
  never see the console as a terminal.
- `getdents64`'s `d_off` is not a seekable directory cookie.
- Linux errno values pass through unchanged because the native numbers
  were chosen to match; a future native errno that Linux lacks would need
  a mapping here.
