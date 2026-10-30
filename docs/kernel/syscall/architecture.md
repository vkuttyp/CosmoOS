# System Calls: Architecture and Design

## Purpose

The single controlled entry from user mode into the kernel. Two layers,
as the constitution requires: an architecture-specific entry that
captures registers and switches stacks, and a generic dispatcher that
maps a number to a function through the calling process's personality.

## Position

```text
   user code ── SYSCALL ──► kernel/arch/x86_64/syscall_entry.S
                                  │ struct syscall_frame on the kernel stack
                                  ▼
                            kernel/syscall/syscall.c   syscall_dispatch()
                                  │ personality->table[nr]
                                  ▼
                            kernel/syscall/native.c    sys_write, sys_mmap, ...
                                  │ handles, uaccess, process, vmm, sched
```

On AArch64 the entry is `SVC #0`, taken by the `el0_sync` vector
(`kernel/arch/aarch64/vectors.S`) and routed by `handle_sync` in
`kernel/arch/aarch64/trap.c` to `handle_syscall`, which calls the same
`syscall_dispatch`.

Invariant 7 (Linux compatibility must not contaminate the native ABI)
is met structurally: a personality is a table plus conventions; the
native functions never see Linux numbers, and the Linux table (Phase
11, `compat/linux/syscalls.c`, `docs/compat/linux/`) calls the same
kernel subsystems through its own translation functions. The only
thing it shares with `native.c` is the exported bodies of `read`,
`write` and `fstat` (`syscall_handle_read/write/stat`), which take
native arguments.

## Responsibilities

- Entry/exit with exactly one SWAPGS each way, kernel stack from the
  per-CPU block, interrupts enabled during the call, SFMASK clearing IF,
  TF, DF, AC, NT.
- Argument marshalling into `struct syscall_args` (number plus six
  64-bit arguments; the x86-64 convention puts the fourth in `r10`
  because `rcx` is clobbered by SYSCALL).
- Bounds check on the number; `-ENOSYS` otherwise.
- The native table (`uapi/cosmo/syscall.h`); numbers are stable from
  the first release of this document.
- User memory access only through `uaccess.h`: range check against
  `[USER_LO, USER_HI)`, then a copy inside a user-access window (SMAP
  on x86-64, PAN on AArch64) by `arch_copy_user_raw`, whose faulting
  instructions carry exception fixups; there is no walk of the region
  list (`docs/kernel/memory/design.md` §6.1).
- The syscall filter: a known number outside the process's mask kills
  it with `SIGSYS` (`syscall_allowed`, `docs/kernel/security/design.md`
  §1f).
- Signal delivery and restart on the system-call frame after the
  handler returns (`signal_deliver(frame, true)`).
- Errno convention: negative values in the return register; user
  wrappers negate into `errno` if they wish.

## Non-responsibilities

- The signal core itself: queues, actions, handler frames and the
  restart rule live in `kernel/process/signal.c` and the personalities'
  frame builders (`docs/kernel/process/design.md` §11); the dispatcher
  only calls `signal_deliver`.
- vDSO/fast paths.
- Compatibility numbering of any other operating system (the Linux
  numbering is the Linux personality's, `compat/linux/`).
- Audit logging and argument-level (seccomp-BPF-style) filtering; the
  filter decides by number only.

## Data structures

```c
struct x86_syscall_frame {   /* pushed by syscall_entry.S, x86-64 (x86/trapframe.h) */
    uint64_t flags;                          /* X86_SYSCALL_FULL_RESTORE: return by IRETQ */
    uint64_t rcx, r11;                       /* restored on the IRETQ path */
    uint64_t r15, r14, r13, r12, rbp, rbx;   /* callee-saved, for diagnostics */
    uint64_t r9, r8, r10, rdx, rsi, rdi;     /* arguments 6..1 */
    uint64_t rax;                            /* number in, result out */
    uint64_t rip, cs, rflags, rsp, ss;       /* user return state (rcx/r11 origin) */
};

struct syscall_args { uint64_t nr; uint64_t a[6]; void *frame; };
typedef int64_t (*syscall_fn)(struct syscall_args *a);
struct personality { const char *name; const syscall_fn *table; unsigned count;
                     const uint16_t *always_allowed; unsigned nr_always_allowed;
                     /* signal, lifecycle and auxv hooks: kernel/process.h */ };
```

AArch64 uses its ordinary `struct arch_trap_frame` as the system-call
frame. `docs/kernel/syscall/design.md` describes the rest.

## Concurrency

Syscalls run on the calling thread's kernel stack with interrupts
enabled and may block (sleep, mutex). They hold no lock across a
blocking call. Handle lookups return referenced objects; the reference
is dropped before returning to user mode.

## Error handling

Every argument is validated before use; the first failing check
determines the errno. A pointer inside the user window that faults
during a copy (unmapped, `PROT_NONE`, wrong permission, or a demand
fault that finds no memory) makes the copy return `-EFAULT` through the
exception fixup; demand-zero faults on the process's own regions are
serviced during the copy (`docs/kernel/memory/invariants.md` M32).

## Security

No kernel pointer ever reaches user space. The kernel never dereferences
a user pointer outside `uaccess.h`. `SFMASK` guarantees kernel code runs
with a clean flags register. Returning to user mode restores exactly
the user's `rip`/`rflags` from the frame; the kernel stack pointer is
never exposed.

## Testing

`userland/init --selftest` calls every syscall with valid, invalid,
boundary, hostile-pointer, and unknown-number inputs and checks each
result; the kernel self-test runs it and requires status 0. See
`docs/kernel/process/testing.md`.

## Future

Per-thread syscall accounting and tracing hooks. (Fault recovery by
exception tables, once listed here, exists since audit milestone 5,
`docs/kernel/memory/design.md` §6.1.) The Linux personality table arrived in Phase 11
(`docs/compat/linux/`); the virtualization calls in Phase 12
(`docs/kernel-services/virtualization/`).

## Files (Phase 7)

The filesystem calls (`open`, `stat`, `fstat`, `lseek`, `mkdir`, `unlink`,
`rmdir`, `rename`, `getdents`, `sync`, `mount`, `umount`; numbers 11–22)
live in `kernel/syscall/native.c` beside the Phase 4 calls and translate
to `kernel-services/vfs/` entry points: paths are copied with
`strncpy_from_user` (1024 bytes), handles resolve to `struct file`
through `file_from_kobject`, results are copied out. `struct file` is a
`kobject_io_type`, so the dispatcher's `read`/`write`/`close` need no
knowledge of files. `mount`/`umount` require `PRIV_MOUNT`
(`cred_has`, since roadmap M5; `cred.uid == 0` before it). See
`docs/kernel-services/vfs/api.md`.

## The bounce

Every user copy in `read` and `write` goes through a kernel buffer,
because the I/O object interface takes kernel memory. Its size was a
console line, `IO_CHUNK` = 1 KiB, so a `read` returned at most a
kilobyte whatever was asked (the audit's 4.2 MEDIUM; the unit
`docs/audit/next-subsystem-file-path.md`). Now `syscall_bounce_get`
sizes it to the request: the caller's stack chunk for a kilobyte and
less, the heap above it up to `IO_BOUNCE_MAX` = 64 KiB (the page path of
`kmalloc`), and a heap allocation that fails degrades to the stack chunk
-- a read under memory pressure is slow, never `-ENOMEM`. One object
call per `read`: what the object returns is what the read returns, so a
pipe or a tty keeps its semantics and a file fills up to 64 KiB. `write`
loops the bounce. Both personalities use it (`native.c`, the Linux
`read`/`write`/`pread`/`pwrite`, the private file mapping's fill), and
the socket path keeps its own. Measured (`read-bench`, `USERBENCH`): on
x86-64 under TCG the object path reads a ramfs file at 44 MiB/s with
1 KiB requests and 317 with 64 KiB; the whole syscall from user mode at
38 and 110.

## Sockets (Phase 8) and processes (Phase 9)

Calls 23–31 translate to `kernel-services/network/socket.c` (`ksock_*`,
`docs/kernel-services/network/api.md`). Calls 32–42 (`spawn`, `wait`,
`kill`, `pipe`, `dup`, `getppid`, `chdir`, `getcwd`, `procinfo`, `klog`,
`sysctl`) translate to `kernel/process/` (`process_spawn`,
`process_wait_child`, `process_kill`, `process_chdir`, `process_info`),
`kernel/ipc/pipe.nova` (`pipe_create`), the handle table (`handle_get`,
`handle_install`, `handle_install_at`, `handle_close` for `dup`),
`kernel/core/log.c` (`klog_copy`) and a static table of read-only values
in `native.c` (`sysctl`). `sys_spawn` copies the whole request (path,
`argv`, `envp`, handle map, optional `cwd`) into one kernel allocation
before anything else runs; `sys_fstat` accepts every I/O object with a
`stat` operation; every path system call resolves relative paths from
`process_current()->cwd`; blocking calls (`read` on the console or a
pipe, `write` on a full pipe, `wait`, `sleep_ns`, the socket waits) use
`wait_event_killable` and return `-EINTR` when the process is killed,
after which the dispatcher's `process_check_kill` ends the process.
`SYS_COUNT` was 58 at the end of audit milestone 6, 97 as of the
file-regions unit (`SYS_msync` 96), and is 105 as of roadmap M5
(`SYS_sysctl_set` 104) — the
header is the authority and `docs/kernel/syscall/api.md` lists every
number; what follows is how it grew: Phase 12 added the seven virtualization calls 43–49,
the Prompt #3 fix pass the six credential calls 50–55, audit milestone 6
`getrlimit`/`setrlimit` 56–57 (`docs/kernel/security/api.md`)
(`kernel-services/virtualization/hvsys.c`, listed in the native table
like every other handler; `docs/kernel-services/virtualization/`).
