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

**P5. The initial stack.** The top page is populated eagerly (the
initial frame is written into it through the direct map); the rest of
the 8 MiB region is lazy with `VM_REGION_GUARD_BELOW`; argv/envp/auxv
strings must fit that page or creation fails with `-EINVAL`. Check:
test `process-user` (64 KiB of stack touched through lazy pages),
review of `build_initial_stack`.

**P6. Refused executables.** `PT_INTERP` and executable
`PT_GNU_STACK` are `-ENOEXEC`; only `ET_EXEC` x86-64 with in-bounds
headers, congruent vaddr/offset, non-overlapping page-granular
segments, and an entry inside an executable segment loads. Check: test
`elf` exercises magic, class, type, W+X, entry, window, bounds, short
file; test `process-reject`.

**P7. Every unmap path shoots down before freeing frames.**
`user_region_teardown` and `vm_user_protect` follow the kernel arena's
protocol: table edit under `vm_space.lock`, unlock, `arch_mmu_shootdown`,
then `pmm_free_page`. Check: review; `smp-shootdown` for the mechanism.

**P8. A user space is destroyed only by the reaper.** Address-space
teardown runs from `process_release`, reached from `thread_put` in the
reaper thread, never on the dead thread's stack or with its CR3 active.
Check: assert `read_cr3() != ctx->root` in `arch_mmu_context_destroy`;
assert `t != thread_current()` in `thread_put`.

**P9. Kernel-half PML4 entries are fixed after `vmm_init`.**
`arch_mmu_context_init_user` copies entries 256–511 from the kernel root
once; nothing creates a new kernel-half PML4 entry afterwards because
the direct map, the arena, and the image all have their PDPTs by the
end of `vmm_init` and `arch_mmu_map` on the kernel space only descends
into existing PML4 entries for those ranges. Check: review. Gap: not
yet asserted at runtime; a `KASSERT` in `descend()` when creating a
PML4 entry above index 255 after init is planned.

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
native syscall calls `user_range_ok` then `copy_*_user` or
`user_range_mapped` before touching a user address; `sys_read`/`sys_write`
bounce through a 512-byte kernel buffer. Check: test `process-user`
(kernel pointer, below-window, top-of-window, unmapped, and overflowing
lengths all `-EFAULT`), review.

**P15. A user-mode fault never panics the kernel.** The fault handler
routes a fault whose frame has `VM_FAULT_USER` and no serviceable region
to `vm_user_hooks.fatal`, which logs and calls `process_exit(139)`
after re-enabling interrupts. Kernel-mode faults on user addresses that
validation did not catch still panic (kernel bug). Check: test
`process-fault` (status `COSMO_EXIT_FAULT` = 139).

**P16. No kernel pointer reaches user space.** Handles are table
indices; syscall results are integers or user addresses; `sys_mmap`
returns addresses inside the user window; `arch_user_enter` zeroes all
general registers. Check: review.

## Objects and handles

**P17. Handle rights are checked at lookup.** `handle_lookup` returns
NULL unless every requested right is held; `sys_write` on the read-only
`stdin` handle is `-EBADF`, `sys_read` on `stdout` likewise. Check: tests
`objects`, `process-user`.

**P18. Handles 0–2 of a new process are the console** with READ,
WRITE, WRITE rights; `close` on any of them works and later use is
`-EBADF`. Check: test `process-user` (`close(2)` then `write(2)`).

**P19. Console `read` returns 0.** There is no input path; the object
never blocks. Check: test `process-user`.

**P20. Process references.** Creator, table, and each thread hold one;
`process_release` runs with `state == EXITED` and `nr_threads == 0`
(asserted) and only after all three are dropped. Check: asserts in
`process_release`; test `process-user` waits for `process_count()` to
return to its baseline.

## ABI

**P21. Syscall numbers are stable and only appended.**
`uapi/cosmo/syscall.h` numbers 0–10 never change meaning; `SYS_COUNT`
grows. Unknown numbers, including values above `SYS_COUNT` and
negative values reinterpreted as large unsigned, return `-ENOSYS`
without side effects. Check: test `process-user` (`SYS_COUNT`, 999999,
-1), review.

**P22. Personality lookup is bounds-checked.** `syscall_dispatch`
checks `nr < pers->count` and a non-NULL entry before calling. Check:
P21's test.

## Gaps (documented, not invariants)

- No `fork`, `exec` from a file, or `wait`; processes are created only
  from an in-memory image by kernel code.
- No signals; a fatal fault is the only asynchronous event a process
  sees, and it is terminal.
- One thread per process; `process_exit` does not yet signal other
  threads.
- One console object shared by every process; no per-process I/O.
- `copy_*_user` relies on validation, not on fault recovery; a
  concurrent `munmap` from another thread of the same process (which
  cannot exist yet) could turn a validated copy into a kernel-mode
  fault that panics.
- SMAP paths (`stac`/`clac`) are compiled but untested: QEMU's `qemu64`
  model has no SMAP.
- P9 is enforced by construction and review only.
