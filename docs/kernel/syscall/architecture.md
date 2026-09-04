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

Invariant 7 (Linux compatibility must not contaminate the native ABI)
is met structurally: a personality is a table plus conventions; the
native functions never see Linux numbers, and a Linux table (Phase 11)
will call the same kernel subsystems through its own translation
functions.

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
  `[USER_LO, USER_HI)`, mapping check against the process's regions with
  the required protection, then a copy inside an SMAP window.
- Errno convention: negative values in the return register; user
  wrappers negate into `errno` if they wish.

## Non-responsibilities

- Signals, restartable calls, `SA_RESTART` semantics.
- vDSO/fast paths.
- Compatibility numbering of any other operating system.
- Audit and seccomp-style filtering (the dispatcher has the hook point).

## Data structures

```c
struct syscall_frame {       /* pushed by syscall_entry.S, x86-64 */
    uint64_t r15, r14, r13, r12, rbp, rbx;   /* callee-saved, for diagnostics */
    uint64_t r9, r8, r10, rdx, rsi, rdi;     /* arguments 6..1 */
    uint64_t rax;                            /* number in, result out */
    uint64_t rip, cs, rflags, rsp, ss;       /* user return state (rcx/r11 origin) */
};

struct syscall_args { uint64_t nr; uint64_t a[6]; void *frame; };
typedef int64_t (*syscall_fn)(struct syscall_args *a);
struct personality { const char *name; const syscall_fn *table; unsigned count; };
```

## Concurrency

Syscalls run on the calling thread's kernel stack with interrupts
enabled and may block (sleep, mutex). They hold no lock across a
blocking call. Handle lookups return referenced objects; the reference
is dropped before returning to user mode.

## Error handling

Every argument is validated before use; the first failing check
determines the errno. A pointer that passes validation but faults is a
kernel bug (panic) except for demand-zero faults on the process's own
anonymous regions, which the fault handler services.

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

Per-thread syscall accounting, tracing hooks, Linux personality table,
`copy_from_user` with fault recovery (exception tables) so validation
can be relaxed for performance.
