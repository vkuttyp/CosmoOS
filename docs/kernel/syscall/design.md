# System Calls: Design

How the system-call layer is built: the entry paths, the dispatcher, the
personality tables, the filter, user-memory access and the bounce. The
calls themselves are specified in `api.md` and in the documents of the
subsystems they translate to; the rules are in `invariants.md`.

## Files

| File | Contents |
|---|---|
| `kernel/arch/x86_64/syscall_entry.S` | `x86_syscall_entry`: SWAPGS, stack switch, frame push, `SYSRET` or `IRETQ` exit |
| `kernel/arch/x86_64/user.c` | `arch_syscall_init_cpu` (STAR, LSTAR, SFMASK), `x86_syscall_c`, `x86_syscall_return_check`, `arch_user_access_begin/end` (STAC/CLAC), `arch_user_regs_restart_syscall` |
| `kernel/arch/aarch64/vectors.S`, `trap.c`, `user.c` | `el0_sync` vector, `handle_sync` → `handle_syscall`; PAN window; `arch_user_regs_restart_syscall` |
| `kernel/arch/*/uaccess.S` | `arch_copy_user_raw`, the copy whose user-touching instructions carry exception-table entries |
| `kernel/syscall/syscall.c` | `syscall_dispatch`, `syscall_allowed`, `syscall_filter_install`, the global counters |
| `kernel/syscall/native.c` | the native table `native_table[SYS_COUNT]`, `personality_native`, 91 of its 105 handlers, the bounce, the shared `syscall_handle_*`/`syscall_obj_*` I/O bodies |
| `kernel/syscall/personality.c` | `personality_for_elf`: the list of personalities and the selection rule |
| `kernel/syscall/uaccess.c` | `user_range_ok`, `copy_from_user`, `copy_to_user`, `strncpy_from_user` |
| `kernel/syscall/uaccesstest.c` | self-tests `uaccess`, `uaccess-guard` |
| `kernel-services/virtualization/hvsys.c` | the ten native virtualization handlers (`sys_vm_*`, `sys_vcpu_*`) |
| `kernel/process/native_signal.c` | the four native signal handlers and `native_signal_frame` |
| `compat/linux/syscalls.c` | `personality_linux` and its table (`docs/compat/linux/`) |

The native table has an entry for every number below `SYS_COUNT` (105
designated initialisers for 105 numbers as of roadmap M5).

## Data structures

**`struct syscall_args`** (`kernel/include/kernel/syscall.h`): the
number, six 64-bit arguments and an opaque pointer to the arch frame.
Built on `syscall_dispatch`'s stack for one call; every handler takes
it.

**`struct personality`** (`kernel/include/kernel/process.h`): `name`,
`table`, `count`, `always_allowed`/`nr_always_allowed` (the numbers a
filter may never deny), `signal_frame`, `thread_exit`, and the roadmap
M3 lifecycle and initial-stack hooks (`claims_elf`, `init`, `release`,
`platform`, `auxv`, `fork`, `exec`). Two exist:

| | `personality_native` | `personality_linux` |
|---|---|---|
| Table | `static const native_table[SYS_COUNT]`, compile-time | `static g_table[LX_NR_MAX]` (512), filled from `linux_table` on the first Linux process, empty slots `lx_unknown` |
| `count` | `SYS_COUNT` (105) | `LX_NR_MAX` (512) |
| Always allowed | `SYS_exit`, `SYS_sigreturn`, `SYS_thread_exit`, `SYS_set_tls` | `LX_exit`, `LX_exit_group`, `LX_rt_sigreturn` |
| Selected by | the CosmoOS ELF note, or a kernel-created process | every other ELF |

**The arch frame.** On x86-64, `struct x86_syscall_frame`
(`x86/trapframe.h`, 21 words, `_Static_assert`ed): a `flags` word,
`rcx`/`r11` for the `IRETQ` path, the callee-saved registers, the six
argument registers, `rax`, and an `IRETQ` frame (`api.md`, "x86-64
entry"). On AArch64 the ordinary `struct arch_trap_frame`
(`aarch64/trapframe.h`, `0x130` bytes). Generic code reaches either only
through `arch_user_regs_*` (`arch_user_regs_set_result_in_frame`,
`arch_user_regs_from_syscall`, `arch_user_regs_restart_syscall`).

**Per-thread call record**: `struct thread.syscall_nr` and
`syscall_arg0` (`kernel/include/kernel/thread.h`), written by the
dispatcher before the handler, read by `signal_deliver` to restart the
call. `SIGNAL_NO_RESTART` (`~0`) in `syscall_nr` marks a call whose
result register is not a result (`sigreturn`, `rt_sigreturn`, set in
`kernel/process/native_signal.c` and `compat/linux/signal.c`).

**Per-process filter**: `struct process.syscall_mask[
COSMO_SYSCALL_MASK_WORDS]` (8 words, 512 bits), bit N allowing call N in
the process's personality; all ones means no filter.

**Per-process count**: `struct process.syscalls`, incremented by the
dispatcher, reported by `procinfo` and the process dump.

**The bounce**: `struct io_bounce { buf, cap, heap }` (`syscall.h`),
described under "Memory".

**Global counters** (`syscall.c`, `native.c`): `g_calls`, `g_unknown`,
`g_filtered`, `g_bounce_heap`, `g_bounce_fallback`, read by
`syscall_count`, `syscall_unknown_count`, `syscall_filtered_count`,
`syscall_bounce_heap_count`, `syscall_bounce_fallback_count`. Only the
fallback count has a reader in the tree (`read-bounce`,
`kernel-services/vfs/vfstest.c`).

## The path of one call

1. **Entry.** x86-64: `SYSCALL` lands in `x86_syscall_entry` with `IF`,
   `TF`, `DF`, `AC`, `NT` cleared by SFMASK; it swaps GS, parks the
   user `rsp` in the per-CPU block (offset 16), loads the kernel stack
   (offset 8), pushes the frame, `cld`, `sti`, and calls
   `x86_syscall_c`, which reads the arguments from the frame. AArch64:
   `SVC #0` is a synchronous exception on `el0_sync`; `handle_sync`
   panics on an SVC from EL1, otherwise `handle_syscall` copies `x0`–`x5`
   and enables interrupts.
2. **Preconditions.** `syscall_dispatch` asserts interrupts enabled and
   `irq_depth == 0 && preempt_count == 0` (`KASSERT`, every build) and
   panics if the thread has no process.
3. **Counting.** `g_calls` (relaxed atomic) and `p->syscalls` (a plain
   increment).
4. **Lookup.** `nr >= pers->count` or a NULL slot: `g_unknown`, a DEBUG
   log line, `-ENOSYS`. Nothing else runs.
5. **Filter.** `syscall_allowed(p, nr)`; a denial bumps `g_filtered`,
   logs a warning, `process_kill(p, COSMO_SIGSYS)` and
   `process_check_kill()`, which does not return.
6. **Record.** `syscall_args` is built; `t->syscall_nr`,
   `t->syscall_arg0` are set.
7. **Kill check.** `process_check_kill()` ends a process already marked
   for death before its handler runs.
8. **Handler.** `pers->table[nr](&a)`.
9. **Signals.** If `signal_pending()`, the result is stored in the frame
   (`arch_user_regs_set_result_in_frame`), `signal_deliver(frame, true)`
   runs, and the result is read back from the frame, since delivery may
   have rewritten it (a handler frame, a restart) or not returned (a
   termination).
10. **Exit.** x86-64: `x86_syscall_c` stores the result in `rax` and
    `x86_syscall_return_check` chooses `IRETQ` over `SYSRET` for a
    non-canonical `rip` or `rflags` outside the user mask
    (`docs/kernel/process/invariants.md` P-S1). AArch64: `handle_syscall`
    stores `x0` and disables interrupts; `return_to_user_check` calls
    `process_return_to_user`, which delivers anything raised since step
    9.

### Restart

`signal_deliver` (`kernel/process/signal.c`) restarts a call when its
result is `-EINTR` and `syscall_nr` is not `SIGNAL_NO_RESTART`, in two
cases: the handler about to run has `SA_RESTART`, or the thread parked
for a stop (unconditional, so `^Z` then `fg` does not turn a blocked
`read` into `-EINTR`). `arch_user_regs_restart_syscall` puts the number
back in `rax`/`x8`, the first argument back in `rdi`/`x0` and steps the
PC back over the instruction (2 bytes for `SYSCALL`, 4 for `SVC`). The
first argument is recorded because on AArch64 the result overwrites
`x0`; the other argument registers are unchanged by a call on either
architecture. There is no `-ERESTART*` family: any call that returns
`-EINTR` is restarted under these two conditions.

## Ownership and lifetime

- `struct syscall_args` and the arch frame live on the calling thread's
  kernel stack for the duration of the call; the frame is the thread's
  user register state until return.
- The personalities and the native table are static. The Linux table
  is written once under `g_table_lock` the first time a Linux process
  is created (`linux_table_get`), before that process can run.
- `p->pers` is set when the process is built (`personality_for_elf`),
  copied by `fork`, and changed only by `exec` when the new image
  selects the other personality (`exec_personality`,
  `kernel/process/process.c`). The dispatcher reads it once per call.
- The filter mask is part of `struct process` and dies with it. A child
  inherits it at creation (`inherit_syscall_mask`); a child of the other
  personality cannot be started by a filtered process (`spawn` and
  `exec` return `-EPERM`, `process_filter_blocks_personality`).
- Kernel objects reached through a handle are referenced by
  `handle_lookup`/`handle_get` and released by the handler before it
  returns (`syscall_handle_read`, `syscall_handle_write`).
- A heap bounce is allocated and freed within one handler
  (`syscall_bounce_get`/`syscall_bounce_put`).

## Concurrency model

- A system call runs on the calling thread's kernel stack with
  interrupts enabled and preemption enabled, and may sleep. Threads of
  one process enter concurrently on different CPUs; nothing in the
  dispatcher serialises them.
- Blocking waits that a kill must end use `wait_event_killable` and
  return `-EINTR` (`architecture.md`, "Sockets (Phase 8) and processes
  (Phase 9)").
- The filter mask has one lock and one lock-free reader. Every writer
  (`syscall_filter_install`) and the copy to a child
  (`inherit_syscall_mask`) take `p->lock`; the writer stores each word
  with a relaxed atomic store; `syscall_allowed` reads each word with a
  relaxed atomic load and no lock. Because the mask only narrows and a
  word is read whole, a reader sees either the old or the new word, and
  a call already past the check when a filter lands may complete. The
  child's copy is decided and taken in one hold, so it is never a mix of
  before and after an install (`docs/kernel/process/invariants.md` P29
  lists the field).
- `g_calls`, `g_unknown`, `g_filtered` and the bounce counters are
  relaxed atomics; `p->syscalls` is a plain increment from every thread
  of the process, so with several threads it can lose counts. It is a
  diagnostic.
- The user-access helpers call `might_sleep()`: a demand-zero fault
  during a copy allocates, so they are never called with a spinlock held
  or in interrupt context.

## Memory

- The dispatcher allocates nothing.
- `syscall_bounce_get` uses the caller's 1 KiB stack chunk (`IO_CHUNK`)
  for requests up to 1 KiB, and `kmalloc(min(len, IO_BOUNCE_MAX), 0)`
  (64 KiB, the page path of `kmalloc`) above it; a failed allocation
  falls back to the stack chunk and counts `g_bounce_fallback`, so a
  large `read` under memory pressure is shorter, not `-ENOMEM`.
  `syscall_obj_read` makes one object call; `syscall_obj_write` loops
  the bounce until the request is done, a short write, or an error.
- `strncpy_from_user` copies in page-bounded pieces, so a string that
  ends before an inaccessible page is accepted and one that runs into it
  is `-EFAULT`; it always terminates the destination.
- `sys_syscall_filter` copies the whole mask (at most 64 bytes) to the
  stack before installing anything.
- Paths are copied into kernel buffers of `VFS_PATH_MAX` (`get_path` in
  `native.c`); `spawn` and `exec` copy their whole request into one
  kernel allocation before acting (`api.md`).

## Error handling

- A handler returns a non-negative result or `-errno`. User space treats
  `[-4095, -1]` as an error (`libc/src/errno.c`: `r < 0 && r > -4096`);
  no success value falls in that range, since results are counts,
  handles, ids and user-window addresses below `VM_USER_HI`.
- An unknown number is `-ENOSYS`, with or without a filter.
- A denied known number is not an error return: the process is killed
  with `SIGSYS` (exit status 159). The `-EPERM` after
  `process_check_kill()` in `syscall_dispatch` is unreachable.
- Arguments are validated before side effects; an undefined bit in a
  flags word is `-EINVAL` (`api.md`, "Unknown flag bits").
- A user pointer outside the window, or one whose copy faults, is
  `-EFAULT` (`copy_*_user`, `strncpy_from_user`), never a panic and never
  a signal: a fault inside `arch_copy_user_raw` resumes at its fixup
  (`docs/kernel/memory/design.md` §6.1, M32). A copy that fails may have
  written its destination up to the faulting page. A `read` whose
  copy-out faults after the object consumed the data calls the object's
  `read_undo`, when it has one, and returns `-EFAULT`.
- `strncpy_from_user` returns `-ENAMETOOLONG` for a string that does not
  fit and `-EINVAL` for a zero-size buffer.
- An object that reports more bytes than it was offered trips a
  `KASSERT` in `syscall_obj_read`/`syscall_obj_write` (the `-EIO`
  branch behind it is unreachable while `KASSERT` panics).

## Performance

- Fixed per-call work: two assertions, two counter increments, a bounds
  check, one relaxed load of a mask word (plus a scan of the
  always-allowed list, at most four entries, only when the bit is
  clear), two thread-field stores, `process_check_kill` (two acquire
  loads) and `signal_pending()` after the handler.
- On x86-64 the exit is `SYSRET` unless the frame needs `IRETQ`.
- The bounce sizes itself to the request: measured with `read-bench` on
  x86-64 under TCG, a ramfs file reads at 44 MiB/s with 1 KiB requests
  and 317 MiB/s with 64 KiB through the object path, 38 and 110 MiB/s for
  the whole system call from user mode (`architecture.md`, "The
  bounce").
- There is no vDSO; `clock_ns` and `getpid` are full system calls.

## Security

- **User pointers.** Every handler reaches user memory through
  `uaccess.h` (`docs/kernel/process/invariants.md` P14). Each helper
  checks `[USER_LO, USER_HI)` = `[0x400000, 0x00007FFFFFFFF000)` without
  overflow before touching memory, and copies inside
  `arch_user_access_begin/end`: STAC/CLAC when the CPU has SMAP, PAN
  cleared and set when it has PAN. Outside that window an access to a
  user page faults where the guard exists (`uaccess-guard`).
- **Entry state.** SFMASK clears `IF`, `TF`, `DF`, `AC`, `NT`; the entry
  also executes `cld`. On return, a frame that `SYSRET` could fault on
  goes through `IRETQ`, and register sets built from user memory are
  sanitised (`arch_user_regs_sanitize`; P-S1); on AArch64 the same
  function keeps only the user `PSTATE` bits and forces EL0t.
- **The filter.** Number-only, intersect-only, inherited, unprivileged
  to install (`docs/kernel/security/design.md` §1f, S14). It is asked
  only of numbers that exist, so installing a filter never turns an
  `-ENOSYS` into a death. Numbers at or above 512 are denied by
  `syscall_allowed`; the lookup rejects them first in both
  personalities.
- **Privileges.** A native handler that needs privilege asks
  `cred_has(cred_current(), PRIV_*)` (`kernel/include/kernel/cred.h`):
  the bit must be held and the effective uid must be 0. In `native.c`:
  `mount`, `umount` (`PRIV_MOUNT`), `switch_root` (`PRIV_MOUNT` and
  init), `klog`, `sethostname`, `sysctl_set` and, in
  `CONFIG_SELFTEST` builds, the `debug.preempt_probe` sysctl
  (`PRIV_SYS_ADMIN`); `log` is rate-limited
  for callers without `PRIV_SYS_ADMIN` (`process_log_permitted`). The
  other privileged operations are checked in the subsystem the handler
  calls (`kill` through `cred_may_signal`, `procinfo` and `setrlimit`
  with `PRIV_PROC`, `spawn` flags; `docs/kernel/security/design.md` §1).
- **No kernel pointers out.** Results are integers or user addresses;
  `klog`, which can carry kernel addresses, needs `PRIV_SYS_ADMIN`
  (`docs/kernel/security/invariants.md` S8).
- **Personality isolation.** The native handlers see only native
  arguments; the Linux personality shares only `syscall_handle_read`,
  `syscall_handle_write`, `syscall_handle_stat`, `syscall_obj_*` and the
  bounce, which take native arguments.

## Future extensibility

- New native calls are appended: a number in `uapi/cosmo/syscall.h`
  below a larger `SYS_COUNT`, a row in `native_table`, a row in
  `api.md`. The filter has room for 512 numbers in either personality
  (`COSMO_SYSCALL_MASK_WORDS`); growing past that changes the mask ABI.
- A third personality is an entry in `g_personalities`
  (`personality.c`) with its own `claims_elf`, table and always-allowed
  list.
- Not implemented: argument-level filtering, audit logging, tracing
  hooks, per-thread accounting, a vDSO, and readers for the
  `syscall_count`/`syscall_unknown_count`/`syscall_filtered_count`
  counters (no sysctl exposes them).
