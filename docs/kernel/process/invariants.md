# Processes and User Mode: Invariants

Each invariant names how it is checked: build (compile-time or link),
assert (KASSERT/panic at runtime), test (a self-test name), or review.

## Address spaces

**P1. User regions carry U/S; kernel regions never do.**
`vm_user_map_anon` and the user fault path map with `ARCH_MMU_MAP_USER`;
`vm_kernel_alloc`, `vm_map_phys`, and the kernel fault path map with
`ARCH_MMU_MAP_GLOBAL` and never `ARCH_MMU_MAP_USER`. `descend()` sets
U/S on intermediate entries only when creating them for a user mapping.
Check: test `process-user` (a kernel pointer passed to `write` is
`-EFAULT` before any access; user code runs at all), review.

**P2. The user window is `[VM_USER_LO, VM_USER_HI)`.** Every user
region, every `mmap` result, and every pointer accepted by `uaccess`
lies inside it; the null page and the first 4 MiB are never mappable.
Check: assert in `vm_user_map_anon` (`user_range_valid`), test
`process-user` (writes from `0x10` and `VM_USER_HI` are `-EFAULT`,
`MAP_FIXED` at `0x10` is `-EINVAL`).

**P3. W^X for user mappings.** `elf_validate` refuses a W+X segment
and an executable `PT_GNU_STACK`; `vm_user_map_anon` and
`vm_user_protect` refuse W+X; `sys_mmap` refuses
`PROT_WRITE|PROT_EXEC`. The stack region is RW. Check: tests `elf`
(W^X rule string), `process-user` (`mmap` W+X `-EINVAL`).

**P4. ELF bytes are copied through the direct map.** `elf_load_into`
resolves each destination page with `arch_mmu_query` and writes via
`phys_to_virt`; it never dereferences a user address and needs neither
the user CR3 nor STAC. Check: review (the function has no
`arch_user_access_begin`).

**P5. The initial stack.** The top two pages (`INITIAL_STACK_PAGES`)
are populated eagerly (the initial frame is written into them byte by
byte through the direct map); the rest of the 8 MiB region is lazy with
`VM_REGION_GUARD_BELOW`; argv/envp/auxv strings (at most
`INITIAL_STRINGS_MAX` 300) and the 16 `AT_RANDOM` bytes must fit those
pages or creation fails with `-EINVAL`. Check: test `process-user` (64
KiB of stack touched through lazy pages), `linux-elf` and the Linux
programs (which read the larger vector), review of
`build_initial_stack`.

**P6. Refused executables.** `PT_INTERP` and executable
`PT_GNU_STACK` are `-ENOEXEC`; only `ET_EXEC` x86-64 with in-bounds
headers, congruent vaddr/offset, non-overlapping page-granular
segments, and an entry inside an executable segment loads. Check: test
`elf` exercises magic, class, type, W+X, entry, window, bounds, short
file; test `process-reject`.

**P7. Every unmap path shoots down before freeing frames.**
`user_range_teardown` and `vm_user_protect` follow the kernel arena's
protocol: table edit under `vm_space.lock`, unlock,
`arch_mmu_shootdown_cpus` on the CPUs running the space, then
`pmm_free_page`; the regions are unlinked before the teardown so no
fault can repopulate the range (`docs/kernel/memory/invariants.md` M35).
Check: review; `smp-shootdown` for the mechanism; `user-vmm`.

**P8. A user space is destroyed only by the reaper.** Address-space
teardown runs from `process_release`, reached from `thread_put` in the
reaper thread, never on the dead thread's stack or with its CR3 active.
Check: assert `read_cr3() != ctx->root` in `arch_mmu_context_destroy`;
assert `t != thread_current()` in `thread_put`.

**P9. Kernel-half PML4 entries are fixed after `vmm_init`.**
`arch_mmu_context_init_user` copies entries 256–511 from the kernel root
once; `vmm_init` pre-populates the arena's 64 PML4 slots with
`arch_mmu_prepopulate` (the direct map and the image have theirs from
the mapping itself), so nothing creates a kernel-half PML4 entry
afterwards. Check: in debug builds `descend()` panics if it would create
one after the first user root exists (`docs/kernel/memory/invariants.md`
M36); review.

**P10. CR3 is switched on every switch to a different space.**
`arch_thread_switch_prepare` activates `next->proc->space` or
`kernel_space` whenever the root differs; there is no lazy TLB, so a
space is active only on CPUs running its threads. Check: review;
P8's assert depends on it.

## Transitions

**P11. SWAPGS exactly once per direction.** `x86_syscall_entry` swaps
on entry and before `sysretq`; `isr_common` swaps on entry and before
`iretq` only when the saved CS at frame offset 144 has RPL 3;
`arch_user_enter` swaps once before `iretq`. `KERNEL_GS_BASE` is 0 while
the kernel runs, so a double swap would leave GS at 0 and the next
per-CPU access would fault at address 0. Check: test `process-user`
(43 syscalls plus a tick-driven preemption of user code, all returning
with a valid GS), review.

**P12. `SFMASK` clears IF, TF, DF, AC, NT.** Kernel code after
`SYSCALL` runs with interrupts off until it has switched stacks, no
single-step trap, forward string direction, SMAP enforced, and no
nested-task flag. Check: review of `arch_syscall_init_cpu`.

**P13. The syscall entry's kernel stack is always the current thread's.**
`percpu.kernel_stack_top` (offset 8) and TSS `rsp0` are written by
`arch_thread_switch_prepare` before every context switch and by
`arch_user_enter`; `syscall_entry.S` reads offset 8 and parks the user
rsp at offset 16. Check: build (`STATIC_ASSERT` on the offsets), test
`process-user`.

**P14. User pointers reach kernel code only through `uaccess`.** Every
native syscall calls `copy_*_user` or `strncpy_from_user` for a user
address, and those touch user memory only through `arch_copy_user_raw`,
whose accesses carry exception fixups; `sys_read`/`sys_write` bounce
through a 1024-byte kernel buffer. Check: test `process-user` (kernel
pointer, below-window, top-of-window, unmapped, and overflowing lengths
all `-EFAULT`), `process-efault` (PROT_NONE, read-only, torn ranges),
`uaccess`; grep for casts of user addresses outside `uaccess.c` finds
none (review).

**P15. A user-mode fault never panics the kernel, and a kernel-mode
fault at a user address never panics inside a user copy.** The fault
handler routes a fault whose frame has `VM_FAULT_USER` and no
serviceable region (or no memory to service it) to
`vm_user_hooks.fatal`, which raises `SIGSEGV` on the thread (milestone
10: a handler frame is built on the trap frame and the hook returns, or
the process ends with 139 after interrupts are re-enabled); every other
CPU exception from user mode, registered or not, is a signal on the
thread (`arch_trap_unhandled` on x86-64; the AArch64 classifier maps
every EL0 syndrome to a registered kind) — `lxsig badstack` (a `push`
on a non-canonical `rsp`, `#SS`) and `lxsig ill` are the checks; a kernel-mode fault at a user address resumes at
the exception fixup of the copy and the system call returns `-EFAULT`
(`docs/kernel/memory/invariants.md` M32). A kernel-mode fault at a user
address with no fixup is a kernel bug and panics. Check: tests
`process-fault` (status `COSMO_EXIT_FAULT` = 139), `process-protnone`,
`process-efault`, `process-oom`.

**P16. No kernel pointer reaches user space.** Handles are table
indices; syscall results are integers or user addresses; `sys_mmap`
returns addresses inside the user window; `arch_user_enter` zeroes all
general registers. Check: review.

## Objects and handles

**P17. Handle rights are checked at lookup.** `handle_lookup` returns
NULL unless every requested right is held; `sys_write` on the read-only
`stdin` handle is `-EBADF`, `sys_read` on `stdout` likewise. Check: tests
`objects`, `process-user`.

**P18. Handles 0–2 of a kernel-created process are the console** with
READ, WRITE, WRITE rights; a spawned child holds exactly the handles its
parent mapped (`struct cosmo_spawn_handle` pairs, the parent's rights
copied), or its parent's 0–2 when the map is empty; nothing else is
inherited. `close` on any handle works and later use is `-EBADF`. Check:
test `process-user` (`close(2)` then `write(2)`; a child spawned with
only handles 1 and 2 mapped writes into the pipe it was given; a map
naming a free parent handle is `-EBADF`, a duplicate child slot
`-EINVAL`).

**P19. Console `read` is a tty read.** It blocks until a complete line
exists, returns at most one line, 0 for `^D` on an empty line, `-EINTR`
when the process is killed, and 0 at once for a zero-length request
(`docs/kernel/tty/invariants.md`). Check: test `process-user` (zero
length), `process-spawn` (kill of a blocked reader), the interactive
harness.

**P20. Process references.** Creator (or `process_spawn`, which drops it
at once), table, each thread, each child (`parent`), and `process_set_init`
hold one; `process_release` runs with `state == EXITED` and
`nr_threads == 0` (asserted) and only after all are dropped. A zombie's
table reference is dropped by the parent's `wait`, by reparenting when
no parent remains, or at once when the process had no parent. Check:
asserts in `process_release`; test `process-user` waits for
`process_count()` to return to its baseline; `init --selftest` reaps
every child it creates and sees `-ECHILD` afterwards.

## ABI

**P21. Syscall numbers are stable and only appended.**
`uapi/cosmo/syscall.h` numbers 0–42 never change meaning; `SYS_COUNT`
(43) grows. Unknown numbers, including values above `SYS_COUNT` and
negative values reinterpreted as large unsigned, return `-ENOSYS`
without side effects. Check: test `process-user` (`SYS_COUNT`, 999999,
-1), review.

**P22. Personality lookup is bounds-checked.** `syscall_dispatch`
checks `nr < pers->count` and a non-NULL entry before calling, for both
personalities. Check: P21's test; `lxtest` calls Linux numbers 510 and
9999.

**P22a. Personality selection (Phase 11).** `pers` is
`&personality_native` when the image carries the `CosmoOS` `PT_NOTE`
(type 1) or the process has no parent (kernel-created), else
`&personality_linux`; it is set once in `process_create_from_elf` and
never changes. `p->linux` is non-NULL exactly for Linux processes and is
freed with the process. Check: self-test `linux-elf`, the Linux
programs in `/etc/rc.test`; `docs/compat/linux/invariants.md` L1, L3.

## Phase 9: processes, kill, working directory

**P23. A process is never freed, and its status never lost, while
someone can still ask for it.** An exited child stays in the table as a
zombie (state EXITED, `reaped == false`) until its parent's
`process_wait_child` collects it; a parent that exits first hands its
children to init (which reaps them) or, when init is gone, to the
kernel, which drops exited ones at once and lets live ones self-reap.
Check: `init --selftest` (`waitpid` returns the status once and
`-ECHILD` the second time); `process-spawn` and `process-user` leave
`process_count()` at its baseline. Gap: no test creates an orphan under
the real init.

**P24. Handles and the working directory close when the last thread is
gone, not when the zombie is reaped.** `process_last_thread_gone` calls
`handle_table_destroy` and drops `cwd`; `process_release` finds them
already gone. Consequence: a pipe whose writer exited delivers end of
file even while the parent still has to `wait`. Check: `init --selftest`
reads EOF from a pipe whose only remaining writer was a child that has
exited but not been reaped. Gap: none.

**P25. Kill is delivered only on the target's own thread, at a
boundary, and exactly once.** `process_kill` sets `kill_sig` and the
status under `p->lock` and wakes the thread; the process exits at the
next `process_check_kill` (system-call entry and exit),
`process_return_to_user` (an interrupt or fault returning to ring 3), or
`wait_event_killable`/`thread_sleep_ns_killable` returning `-EINTR`. A
second kill, or a kill of an exiting or exited process, changes nothing.
The status is `128 + sig`. Check: `process-spawn` (`init --block` in a
console read dies with 143 within 2 s; `init --spin` dies with 137 at
a timer tick); `init --selftest` (a `cat` blocked on a pipe dies with
137). Gap: no test kills a process blocked in a socket wait or a sleep.

**P26. `kill` honours credentials and validates its arguments.** Signal
numbers outside `1..31` and pids `<= 0` are `-EINVAL`, an unknown pid
`-ESRCH`, and `-EPERM` unless `cred_may_signal` (privileged, or the
sender's real/effective uid equals the target's real/saved uid). Check:
`init --selftest` (`-ESRCH`, `-EINVAL`) and `init --unpriv-test` (a
uid-1000 child's `kill(getppid(), SIGTERM)` is `-EPERM` and the root
parent survives).

**P26a. Privilege is `cred_privileged` and nothing else.** Every
privileged operation (mount, umount, klog, reserved ports, setgroups,
setres* beyond the caller's own ids) asks the one predicate in
`kernel/cred.h`; an unprivileged process cannot regain privilege
(`setresuid` refuses ids it does not hold, all or nothing). Check:
`tests/host/test_cred.c` (the rules), `init --unpriv-test` (every
privileged call and every root-owned object refused). Gap: no
capability set yet; privilege is all-or-nothing.

**P26b. Privilege flows down and limits bind.** The rules S1–S5 and S8
of `docs/kernel/security/invariants.md`: a new process's ids come only
from its parent (copy or a permitted `SETCRED`), its limits are its
parent's, lowered freely and raised only with privilege, and every
limit is enforced where the resource is granted. Check: there.

**P27. Relative paths resolve from the process's working directory,
whose string and vnode agree.** Every path system call passes
`process_current()->cwd`; `chdir` verifies the target is a directory
before swapping vnode and normalised string together under
`process.lock`; a child inherits both (or the `cwd` named in the spawn
request). Check: `init --selftest` (`mkdir` relative to `/tmp`,
`chdir("cwdtest/../cwdtest/.")` gives `/tmp/cwdtest`, `..` gives `/tmp`,
`ENOTDIR`, `ENOENT`, `ERANGE`; a child's `cd` leaves the parent's cwd);
`process-spawn` (the `path_normalize` table). Gap: a renamed ancestor is
not noticed by `getcwd` (the string is authoritative for display, the
vnode for resolution).

**P28. `spawn` executes only a regular file with an execute bit, reads
it through the VFS, and copies every argument before touching it.**
`process_spawn` checks `COSMO_DT_REG` and `mode & 0111` (`-EACCES`),
bounds the image at 16 MiB (`-ENOEXEC`), reads it into a kernel-arena
buffer and runs the Phase 4 validator on that copy; `sys_spawn` copies
the request, path, argv, envp (`COSMO_ARG_MAX`, `COSMO_ARG_ENTRIES`,
`-E2BIG`) and the handle map into kernel memory first. Check: `init
--selftest` (`/etc/rc` and `/bin` are `-EACCES`, a missing file
`-ENOENT`, an empty `argv` `-EINVAL`); `process-reject`. Gap: no test
exceeds `COSMO_ARG_MAX`.

## Milestone 10: threads, signals, the return paths

**P-S1. The kernel never executes `SYSRET` or `IRETQ` with a value the
instruction could fault on.** `x86_syscall_return_check` forces the
`iretq` exit when `rip` is not canonical or `rflags` has a bit outside
the user mask; `arch_user_regs_sanitize` — applied to every register set
built from user memory (`rt_sigreturn`, a handler frame, a clone's first
entry) — keeps only the user-changeable `rflags` bits, sets the fixed
ones (`IF`, bit 1) and replaces a `rip` at or above
`0x0000800000000000` with 0, so `iretq` itself never faults and the
process takes a user-mode `SIGSEGV` at 0 instead. `rsp` is loaded
unchecked by design (`iretq` does not fault on it); its first use faults
in user mode as `#SS`, a signal. Check: `lxsig badret` (status 139 from
`rt_sigreturn` to `0x8000000000000000`), `lxsig badstack` (139), review
of `x86_syscall_return_check` and `sanitize`. Gap: no test forces the
`TF`-in-`rflags` path through the full restore.

**P-S2. A signal is delivered only at a return to user mode, on the
frame that return uses, and each return runs at most one handler.**
`signal_deliver` is called from `syscall_dispatch` after the handler
(system-call frame) and from `process_return_to_user` (trap frame, only
for user frames with `irq_depth == 0`); exceptions that arrive on other
stacks (`#DB` on its IST) only *queue* (`signal_send_thread`) and the
next return delivers. The handler's mask and the signal itself are
blocked before the frame is written back, so a second instance waits
for `rt_sigreturn`. Check: `lxtest` (a handler sees its own signal
blocked and the mask restored afterwards; the SIGSEGV handler steps over
the store and the program continues), `init --selftest` trap tests (each
exception's own signal number), review.

**P-S3. A thread's signal state is freed with the thread and a process's
with the process, and a signal to an exited thread is refused.**
`thread_put` frees `sig_info` and `init_regs`; `process_release` frees
`sigactions` and `sig_shared_info`; `process_find_thread` skips
`THREAD_EXITED` threads (`tgkill` to a gone tid is `-ESRCH`). Check:
`lxtest` (`tgkill(pid, tid, 0)` after the join is `-ESRCH`); the
`released` log lines; review. Gap: no leak counter.

**P-S4. The process ends when its last live thread does, and an exiting
process takes every thread with it.** `nr_live` counts threads that have
not exited; `process_thread_exit` of the last one, or `process_exit` from
any, sets EXITING (an earlier status wins) and wakes every thread; each
sees `signal_pending()` true and exits at its next return to user mode
or killable wait; `process_last_thread_gone` then reaps. Check: `lxsig
group` (a second thread's `exit_group(7)` ends a spinning main thread:
status 7), `lxsig lastthread` (the main thread exits, the other's
`exit_group(5)` gives 5), `lxtest` joins through `CHILD_CLEARTID`.
Gap: no test kills a process with a thread blocked in `futex_wait`.

**P-P1. A reaped pid is not findable.** Once a process's status has been
collected -- by its parent's `waitpid`, or by the exit path when it has
no parent -- `process_lookup` refuses it, even though the object stays
in the table until its last reference drops. Without that rule
`kill(pid, 0)` answers 0 for a child whose `waitpid` has already
returned, which is the one thing signal 0 exists to answer correctly,
and POSIX says the pid may be reused by then. The window is between the
reap and the release, so from user mode it is a race: CI lost it once
and 200 consecutive tries never lost it on the development machine.
Check: `process-reaped`, which holds the object alive on purpose and
looks the pid up, and fails every time when the rule is removed.

## Signals a program can catch, sessions, and the terminal

**P-S6. A handler returns to exactly the registers it interrupted.** The
native frame carries the general registers, the blocked mask and the
architecture's FP/SIMD image, and `sigreturn` puts all three back --
because a handler compiled by an ordinary toolchain uses the caller-saved
general registers and the vector registers freely. The frame records the
length of the FP image it carries rather than assuming it, so a mismatch
between the frame and the architecture cannot silently carry nothing.
Check: `signal-native` (a handler that tramples four scratch registers
and all sixteen vector registers, at a system call), `signal-async` (the
same at an interrupt return, with a spin loop that reports how many
times it went round so that a signal arriving too early fails the probe
instead of passing it vacuously), `signal-fault` (at a fault, with the
handler mapping the page and the store retried).

**P-S7. `sigreturn` trusts nothing the program can write.** The frame is
found from the stack pointer, so a program can point it anywhere; the
magic is checked (a mismatch is `SIGSEGV` on the thread) and the
registers go through `signal_return`'s sanitiser and the full-restore
exit. `sigreturn` is in the personality's `always_allowed` list, so no
syscall filter can turn a caught signal into a kill. Check: review, and
the syscall fuzzer, which reaches `sigreturn` with arbitrary stacks.

**P-S8. A process group is a closed subset of one session.** `setpgid`
moves only the caller or a child of it, only within the caller's
session, never a session leader, and only into a group that already has
a member in that session or is named by the target's own pid;
`COSMO_SPAWN_SETPGID` applies the same rules where the child cannot yet
ask for itself. `setsid` is refused to a process that already leads a
group. This is what makes "the foreground group of this terminal" a
group the terminal's session can reason about. Check: `signal-group`,
`signal-setsid`.

**P-S9. `^C` reaches the terminal's foreground group and nothing else.**
The line discipline raises `SIGINT` (and `SIGQUIT` for `^\`) on every
process of `fg_pgid` and throws the line under edit away; with no
foreground group the byte is dropped as before. Only the session that
holds the terminal may name a foreground group, and only a group of that
session; a terminal is released when its session leader exits (`SIGHUP`
to the foreground group), so a dead session cannot keep the keyboard.
Check: `tty-intr` (claim, a second session refused, `^C` delivered, the
terminal released), and the interactive boot test, which types `^C` at a
running `sleep` and sees it exit 130 with the next prompt immediately
after.

**P-J1. A process stops only at a return to user mode, and only while
its own state says so.** The per-thread `sig_must_stop` gets a thread to
that point (through `signal_pending`, so a killable wait returns
`-EINTR`); `p->stopped`, re-read under the lock at the park and again in
the wait it blocks on, decides whether it stays there. A flag that
outlived its stop parks nothing. Check: `signal-stop`, `signal-stop-late`
(a stop and a continue back to back, before the target has run, six
times, bounded so the failure is a failure rather than a hang).

**P-J2. A call cut short by a stop is restarted, not failed.**
Unconditionally, because a stop carries no `SA_RESTART`. Check:
`signal-stop-restart`, in which a background reader is stopped *by its
own read* -- the only way to be sure the stop lands inside the call
under test -- then given the terminal and continued, and must return the
line rather than `-EINTR`. Two earlier versions of this test aimed a
stop at a sleeping child from its parent and passed with the restart
deliberately broken, because the stop kept landing between calls.

**P-J3. A stopped process is still killable, and `SIGSTOP` is still
uncatchable.** The park's wait ends on `kill_sig`; `UNBLOCKABLE` and
both `sigaction` gates refuse `SIGSTOP`. Check: `signal-stop-kill`,
`signal-stop-mask`.

**P-J4. A parent is told a process stopped only once every thread has
parked.** `nr_stopped` counts them and the last one to park sets
`stop_reportable`, so a shell cannot take the terminal back while a
thread of the job is still running. Reporting is edge-triggered in both
directions. A thread cloned while the process is stopped inherits the
flag, or it would count towards `nr_live` and never towards
`nr_stopped` and the parent would wait for ever. Check: `signal-stop`
(the edge) and `signal-stop-threads`, which clones a worker into a
sleep and stops the process from the main thread -- with the sibling
wake removed, the worker never parks and no stop is ever reported.

**P-J5a. A signal that will not stop anything is never reported as
though it would.** `signal_raise_stop_self` tests the action and sends
the signal under one acquisition of `p->lock` and returns whether either
a stop or a handler will follow; the terminal turns a `false` into
`-EIO`. Splitting the two -- ask, then send -- is a race another thread
of the same process can win, and the read would then return `-EINTR` for
a stop that never comes, which a retrying program retries for ever.
Check: `tty-ttin`'s blocked-`SIGTTIN` reader, which fails with the ask
removed. The multi-threaded race itself is not reproducible here for the
usual reason.

**P-J5. An orphaned process group is never stopped.** Nothing is left in
its session that could continue it, so `^Z` at the terminal skips it and
a background read from it is `-EIO` rather than `SIGTTIN`. Check:
`tty-ttin`, which builds a real orphan (a grandchild whose parent exits)
and requires `-EIO`, and whose absence wedges the boot rather than
failing it -- which is what the rule exists to prevent.

## Gaps (documented, not invariants)

- No `fork` or `exec` replacing the current image; `spawn` is the only
  creation primitive; `clone` creates threads only.
- **One multi-threaded case remains unaimable**: a thread cloned
  *during* a stop, between the stop being posted and the last thread
  parking. It is handled -- the new thread inherits `sig_must_stop` --
  but no test can reach it, because the main thread cannot clone while
  it is itself stopped. `signal-stop-threads` covers the ordinary
  multi-threaded stop, which is what two rounds of review were about.
- **Two guards are not distinguishable by any test here**,
  and both are kept because they are plainly right rather than because
  anything proves them. Setting `stop_reportable` only when the *last*
  thread parks (rather than when the stop is posted) matters when a
  parent's `waitpid` can win a race against a thread still on its way to
  parking -- which needs more than one thread. So does the guard below.
- **One guard in `process_stop_park` is not distinguishable by any
  test.** The re-read of `p->stopped` at the top of the function guards
  the bookkeeping (`nr_stopped`, `stop_reportable`) against a stale
  flag; the *hang* it would otherwise allow is already prevented by the
  wait condition below it, which reads the same field. With the
  continue's sweep intact the stale-flag state is unreachable at all.
  It is kept because it is the guard for a phantom stop report, and
  recorded here because nothing proves it. (The equivalent belt-and-
  braces in the `SIGKILL` path *was* deleted, for the same reason.)
- A clone's FPU state is the reset state, not the caller's; AVX state
  above the SSE halves does not survive a handler on x86-64 (the frame
  carries the FXSAVE image only).
- The native signal ABI has no alternate signal stack, no real-time
  signals, no queue of siginfo per signal, and no `sigsuspend`; a
  handler that overflows the stack it was called on is not caught.
- A background process *writing* to the terminal is not sent `SIGTTOU`:
  writes go through, as they do on Linux by default (`TOSTOP` is not
  built). Reads are stopped, and `tcsetpgrp` from the background does
  raise `SIGTTOU`.
- One console object shared by every process that inherited it; no
  device nodes, so a process that closed handle 0 cannot reopen the
  console.
- `copy_*_user` relies on validation, not on fault recovery; a
  concurrent `munmap` from another thread of the same process (which
  cannot exist yet) could turn a validated copy into a kernel-mode
  fault that panics.
- SMAP paths (`stac`/`clac`) are compiled but untested: QEMU's `qemu64`
  model has no SMAP.
- P9 is enforced by construction and review only.
