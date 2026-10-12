# System Calls: Invariants

Rules the system-call layer keeps. Each has a **Check** (what verifies
it today) and, where honest, a **Gap**. Changing a rule means changing
this file and the code together.

Rules kept here only by reference, because another subsystem's document
owns them:

- `docs/kernel/process/invariants.md`: P11 (SWAPGS once per direction),
  P12 (SFMASK), P13 (the entry's kernel stack), P14 (user pointers only
  through `uaccess`), P16 (no kernel pointer reaches user space), P21
  (numbers stable and only appended), P22 (personality lookup
  bounds-checked), P22a (personality selection), P29 (the filter mask in
  the per-process pointer census), P-S1 (no faulting `SYSRET`/`IRETQ`),
  P-S2 (signals delivered only at a return to user mode).
- `docs/kernel/memory/invariants.md` M32: a kernel-mode fault inside a
  user copy is `-EFAULT`, never a panic.
- `docs/kernel/security/invariants.md` S1 (privilege flows down), S8
  (information gates), S14 (system calls only ever get fewer).

**SC1. Every system call from user mode, in either personality and on
either architecture, goes through `syscall_dispatch`.** The x86-64
`SYSCALL` path (`x86_syscall_c`, `kernel/arch/x86_64/user.c`) and the
AArch64 `SVC` path (`handle_syscall`, `kernel/arch/aarch64/trap.c`) are
its only callers, and an `SVC` from EL1 panics. The counters, the
filter, the kill check and signal delivery below all depend on there
being one door. Check: review (the two call sites are the only
references outside `syscall.c`); every boot's user programs reach
handlers only this way. Gap: nothing mechanical stops a third caller.

**SC2. A handler runs with interrupts enabled, `irq_depth == 0`,
`preempt_count == 0`, and a current process.** Asserted on every call
(`KASSERT`, which panics in every build) and `panic` when
`process_current()` is NULL. Because the check runs at the next entry,
a handler that returned to user mode holding a spinlock (which raises
`preempt_count`) panics at the process's next system call. Check: the
assertions run on every system call of every boot (`process-user`,
`syscall-fuzz`, the shell harness). Gap: nothing asserts at the exit
of a call; a leaked lock is caught one call late, or never if the
process makes no further call.

**SC3. An unknown number is `-ENOSYS` and nothing else happens.**
`nr >= pers->count` or a NULL slot returns before the filter, the
argument record, the kill check and the handler; the only effects are
`g_unknown` and a DEBUG line. In particular a filtered process is not
killed for an unknown number. Check: `process-user` (`SYS_COUNT`,
999999 and -1 are `-ENOSYS`); `syscall-fuzz` draws `SYS_COUNT`,
`SYS_COUNT + 1`, 1000 and -1; `lxtest` calls Linux numbers 510 and 9999
(P22). Gap: no test makes an unknown call under a filter.

**SC4. A personality's `count` is the size of its table.**
`native_table[SYS_COUNT]` with `.count = SYS_COUNT`; `g_table[LX_NR_MAX]`
with `.count = LX_NR_MAX`, every slot non-NULL (`lx_unknown` fills the
gaps). The dispatcher indexes `table[nr]` only after `nr < count`.
Check: by construction (the declarations share the constant); review.
Gap: the native table's fullness (105 of 105 today) is not asserted;
a missing row is a silent `-ENOSYS`.

**SC5. The filter is asked only of a number that exists, and a denial
ends the process with `SIGSYS` before the handler runs.**
`syscall_allowed` is called after the lookup; a denied call never
returns to user mode (`process_kill(p, COSMO_SIGSYS)` then
`process_check_kill`, exit status 159). Each personality's
`always_allowed` numbers pass whatever the mask says: native `exit`,
`sigreturn`, `thread_exit`, `set_tls`; Linux `exit`, `exit_group`,
`rt_sigreturn`. Numbers at or above `COSMO_SYSCALL_MASK_WORDS * 64`
(512) are denied. Check: the `proc` section's filter cases
(`init --filter inside`, `outside` → 159, `exit-unnamed`, `widen` → 159,
`inherit`, `inherit-start`), `thrtest` step 10 (a denied
`thread_create` dies with signal 31; `thread_exit` under an all-zero
mask exits with its own status). Gap: `sigreturn` and `rt_sigreturn`
under a mask that omits them are not tested.

**SC6. The filter mask is written only under `p->lock`, word by word
with atomic stores, and only narrows; the dispatcher reads it word by
word with atomic loads and no lock.** `syscall_filter_install` stores
`old & new` for every word (words not supplied count as zero);
`inherit_syscall_mask` decides and copies the child's mask in one hold
of the parent's lock. Check: `widen` (a second, wider mask restores
nothing; confirmed against the bug by replacing the intersection with
an assignment, S14); review of the two lock sites. Gap: no test races
an install against a spawn or against a call in flight.

**SC7. A filter is never carried across numberings.** A filtered
process that tries to `spawn` or `exec` a program of the other
personality gets `-EPERM` (`process_filter_blocks_personality`,
`kernel/process/spawn.c`); a child that is created across them anyway
(an install racing the spawn's check) gets an all-zero mask, so only its
personality's always-allowed calls work. Check: `init --filter
linux-child` (a native process filtered to `spawn` and `wait` cannot
spawn `lxhello`, `EPERM`). Gap: the `exec` refusal and the all-zero
fallback are not tested.

**SC8. The handler's result is in the frame before any signal is
delivered, and only `-EINTR` is restarted.** `syscall_dispatch` stores
the result with `arch_user_regs_set_result_in_frame` before
`signal_deliver(frame, true)` and reads it back after. A restart
happens only for `-EINTR`, only when `syscall_nr` is not
`SIGNAL_NO_RESTART`, and only for a handler with `SA_RESTART` or after
a stop; it restores the recorded number and first argument and steps
the PC back one instruction. Check: `signal-stop-restart` (a console
`read` from the background, stopped and then continued in the
foreground, completes with the line the kernel types afterwards rather
than failing with `EINTR`); `lxtest`'s threads section (a `read`
interrupted by `tgkill` returns `-EINTR` without `SA_RESTART` and
completes after the data arrives with it). Gap: no native-personality test of
`SA_RESTART`; no test that a `sigreturn` result is never restarted.

**SC9. A user-access helper checks the range before it touches user
memory, touches nothing for a zero length, and leaves a string
destination terminated.** `user_range_ok` rejects an address outside
`[USER_LO, USER_HI)` and an `addr + len` that overflows or passes
`USER_HI`; `copy_from_user`/`copy_to_user` return 0 for `len == 0`
before the check; `strncpy_from_user` writes a NUL on every path and
refuses `max == 0` with `-EINVAL`. Check: `uaccess` (seven range cases,
four real faults resumed with `fixups` up by exactly four, the
destination untouched or terminated, zero length is no access);
`process-user` (kernel, below-window, top-of-window, unmapped and
`(size_t)-1` lengths are `-EFAULT`); `process-efault`.

**SC10. User memory is touched only inside
`arch_user_access_begin`/`arch_user_access_end`.** The only such
bracket in `kernel/syscall/` is `raw_copy` in `uaccess.c`. Where the
CPU has the guard (SMAP, PAN), an access outside the bracket faults.
Check: `uaccess-guard` (the bracketed read succeeds; the unbracketed
one faults where the guard exists); `make test-guard` boots a CPU model
with SMAP (x86-64) or PAN (`cortex-a76`) and requires
`uaccess-guard: guard live`. Gap: the default CI CPU models have no
guard, so an unbracketed access passes there.

**SC11. A user-access helper may sleep and is never called with
preemption disabled or in interrupt context.** Each helper starts with
`might_sleep()`, which reports through lockdep (`CONFIG_LOCKDEP`) or
panics otherwise. Check: `might_sleep` on every copy of every boot.
Gap: a copy that is never reached in a test is never checked.

**SC12. An object call never receives more than the bounce holds, and
its count bounds the copy out.** `syscall_obj_read` offers
`min(len, b.cap)`; `syscall_obj_write` offers at most `b.cap` per
round; a returned count larger than offered is a `KASSERT`. A failed
heap bounce falls back to the 1 KiB stack chunk and never returns
`-ENOMEM`. Check: `read-bounce` (the sizing: the stack chunk up to
1 KiB, a 64 KiB heap bounce above it, capped at `IO_BOUNCE_MAX`; an
injected heap refusal takes the stack chunk with
`syscall_bounce_fallback_count` up by one; a ramfs file answers one
64 KiB object call with 64 KiB and a pipe holding 300 bytes answers
300). Gap: no test makes an object over-report, so the `KASSERT` is
checked by review.

**SC13. A native handler's privilege check names a privilege.** Every
privileged decision in `kernel/syscall/native.c` is
`cred_has(cred_current(), PRIV_*)`, which requires the bit and an
effective uid of 0; no handler there compares a uid to 0. Check: the
unprivileged half of the `priv` section of `init --selftest`
(`init --unpriv-test`, uid 1000: `mount`, `umount`, `sethostname` and
`klog` are `EPERM`); host `test_cred` (`cred_has` for each bit, uid 0
without the bit, the bit without uid 0); review (`grep` for `uid == 0`
in `native.c` finds nothing).

**SC14. An error is a negative errno in `[-4095, -1]`, and no success
value lies in that range.** Handlers return `-E*` constants from
`kernel/include/kernel/errno.h`; successes are counts, handles, ids, or
addresses inside the user window. `libc/src/errno.c` relies on the
range. Check: review. Gap: `syscall-fuzz` counts results in the range
as errors but does not assert that the rest are plausible successes.
