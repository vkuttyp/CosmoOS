# Processes and User Mode: API

Every entry follows constitution section 52: purpose, inputs, outputs,
ownership, lifetime, concurrency, blocking, interrupt context, failure
modes, ABI stability. All interfaces here are kernel-internal (no
stability promise) except `uapi/cosmo/syscall.h`, which is user ABI and
documented in `docs/kernel/syscall/api.md`.

## Shared contracts

- **Lock order** (outermost first):
  `process_table.lock → process.lock → handle_table.lock` and
  `process.lock → vm_space.lock → pmm_zone.lock`. All are spinlocks
  taken with interrupts saved and disabled.
- **References**: a `struct process` is a `kobject`. Three parties hold
  references while a process runs: the creator (returned by
  `process_create_from_elf`), the process table (dropped by
  `process_last_thread_gone`), and each thread (dropped by `thread_put`
  in the reaper). The object is released when all are gone.
- **Blocking**: functions marked *may block* run only from thread
  context with preemption enabled; nothing here is interrupt-safe except
  `process_current`, `process_get`, `process_put` (when not the last
  reference), and the diagnostics.

## kernel/process.h

### `void process_init(void)`
- Purpose: create the process slab cache and register the VMM user
  fault hooks (`vm_set_user_hooks`).
- Inputs/outputs: none. Requires `sched_init` and `vmm_init`.
- Concurrency: boot CPU, single-threaded. Panics on allocation failure.

### `int process_create_from_elf(const void *image, size_t size, const char *name, const char *const argv[], const char *const envp[], struct process **out)`
- Purpose: build a process from a static ELF image in kernel memory and
  make its main thread runnable.
- Inputs: `image`/`size` borrowed for the call; `name` copied
  (truncated to `PROCESS_NAME_MAX`); `argv`/`envp` NULL-terminated,
  may be NULL, copied onto the initial user stack.
- Outputs: 0 and `*out` referenced; `-ENOEXEC` (validation failed,
  rule logged), `-ENOMEM`, `-EINVAL` (strings exceed the first stack
  page), `-EEXIST` (segment overlap at mapping time).
- Ownership: the caller owns one reference on `*out` and must
  `process_put` it (after `process_wait_exit` or immediately).
- Lifetime: the process outlives the call until its last thread is
  reaped and every reference is dropped.
- Concurrency: takes `process_table.lock` briefly; the main thread is
  enqueued on the least-loaded CPU. May block (allocations, page
  population, TLB shootdown on failure unwind).
- Failure modes: on failure nothing is registered, the partial address
  space is destroyed, and `*out` is untouched.

### `void process_exit(int status)` (noreturn)
- Purpose: terminate the calling process with `status`.
- Inputs: `status` as reported by `process_wait_exit`.
- Concurrency: sets state EXITING under `process.lock` then calls
  `thread_exit`; never returns. Requires a process-owning thread and a
  preemptible context (the fault hook re-enables interrupts first).
- Only one thread per process exists in this phase; the multi-thread
  signalling path is a documented gap.

### `int process_wait_exit(struct process *p)`
- Purpose: block until `p` has exited; return its status.
- Concurrency: `wait_for_completion(&p->exited)`; may block. `p` must be
  referenced by the caller. Returns immediately if already exited.

### `struct process *process_current(void)`
- Purpose: the process of the calling thread, NULL for kernel threads.
- Ownership: not referenced; valid while the caller runs on that
  thread. Interrupt-safe (reads `this_cpu()->current->proc`).

### `struct process *process_lookup(pid_t pid)`
- Purpose: find a live process by pid.
- Outputs: referenced pointer or NULL. Takes `process_table.lock`.

### `process_get` / `process_put`
- Atomic reference operations on `p->obj`. The last `put` runs
  `process_release`: destroys the handle table and the address space,
  unlinks from the table, frees the struct. May block (shootdowns);
  never call the last put with a spinlock held.

### `void process_last_thread_gone(struct process *p)`
- Purpose: called by `thread_put` when the reaped thread was the last of
  its process. Sets EXITED, logs the exit line, completes `exited`,
  drops the table's reference.
- Concurrency: reaper thread context, interrupts enabled.

### `unsigned process_count(void)`, `void process_dump_all(void)`
- Diagnostics. `process_dump_all` prints pid, name, state, thread count,
  and syscall count under `process_table.lock`.

### `struct process` fields
`obj` (kobject), `pid`, `parent_pid` (creator's pid or 0), `name`,
`space` (user `vm_space`, owned), `handles`, `threads` + `nr_threads`
(under `process.lock`), `cred` (uid/gid placeholders), `pers`
(`&personality_native`), `state` (RUNNING → EXITING → EXITED),
`exit_status`, `exited` (completion), `lock`, `all_link`, `syscalls`.

Constants: `USER_LO` = `VM_USER_LO` (4 MiB), `USER_HI` = `VM_USER_HI`,
`USER_STACK_TOP` = `0x00007FFFFFFF0000`, `USER_STACK_SIZE` = 8 MiB,
`USER_MMAP_BASE` = `0x0000100000000000`.

## kernel/elf.h

### `int elf_validate(const void *image, size_t size, uint64_t user_lo, uint64_t user_hi, struct elf_info *info, const char **why)`
- Purpose: decide whether `image` is a loadable static x86-64
  executable and describe its segments.
- Pure function (string.h only; compiled on the host with
  `ELF_HOST_TEST`). Never blocks, never allocates, interrupt-safe.
- Outputs: 0 and `*info` (entry, lo, hi, page-rounded segments with
  file offset/size and the unaligned file vaddr); `-ENOEXEC` with
  `*why` set to one of these immortal strings, in check order:
  - "file shorter than the ELF header"
  - "bad ELF magic"
  - "not ELF64 little-endian v1"
  - "not ET_EXEC (static executables only)"
  - "not x86-64"
  - "bad program header table"
  - "program header table outside the file"
  - "PT_INTERP: dynamic executables are not supported"
  - "PT_GNU_STACK requests an executable stack"
  - "PT_LOAD memsz smaller than filesz"
  - "PT_LOAD file bytes outside the file"
  - "PT_LOAD address range overflows"
  - "PT_LOAD is writable and executable (W^X)"
  - "PT_LOAD vaddr and offset are not congruent modulo the page size"
  - "PT_LOAD outside the user address range"
  - "PT_LOAD segments share a page"
  - "too many PT_LOAD segments" (`ELF_MAX_SEGMENTS` = 16)
  - "no PT_LOAD segments"
  - "entry point is not inside an executable segment"

### `int elf_load_into(struct vm_space *space, const void *image, const struct elf_info *info)`
- Purpose: map each segment as a populated user region (RW while
  copying), copy file bytes frame by frame through the direct map, then
  set the ELF permissions with `vm_user_protect`.
- Outputs: 0, `-ENOMEM`, `-EEXIST`, `-EFAULT` (cannot happen for a
  populated region). May block. On failure the caller destroys the
  space.

## kernel/uaccess.h

All helpers require a process-owning thread and may block (a demand-zero
fault during the copy allocates). They never touch user memory before
both checks pass.

- `bool user_range_ok(uint64_t addr, size_t len)`: inside
  `[USER_LO, USER_HI)` without overflow; `len` 0 accepts `addr == USER_HI`.
- `bool user_range_mapped(uint64_t addr, size_t len, vm_prot_t prot)`:
  every page in a region of the current process carrying `prot`.
- `int copy_from_user(void *dst, uint64_t src, size_t len)` /
  `int copy_to_user(uint64_t dst, const void *src, size_t len)`: 0 or
  `-EFAULT`; copy inside `arch_user_access_begin/end`.
- `int strncpy_from_user(char *dst, uint64_t src, size_t max)`: length,
  `-EFAULT`, `-ENAMETOOLONG`, `-EINVAL` (max 0); always terminates `dst`;
  checks page by page so a string ending before an unmapped page is
  accepted.

## arch/user.h

- `void arch_syscall_init_cpu(void)`: per CPU; sets EFER.SCE, STAR,
  LSTAR, SFMASK, KERNEL_GS_BASE = 0. Called from `x86_start` and
  `x86_ap_entry`.
- `void arch_user_enter(uintptr_t entry, uintptr_t sp)` (noreturn):
  publishes the current thread's kernel stack, zeroes all general
  registers, SWAPGS, IRETQ to ring 3 with `rflags = IF`.
- `arch_user_access_begin/end`: STAC/CLAC when the CPU has SMAP.
- `bool arch_trap_frame_is_user(const struct arch_trap_frame *)`:
  `(cs & 3) != 0`.

## kernel/vmm.h additions

- `VM_USER_LO` (`0x400000`), `VM_USER_HI` (`0x00007FFFFFFFF000`),
  `VM_REGION_USER` flag, `struct vm_space.user`, `.anon_pages`.
- `int vm_space_create_user(struct vm_space **out)`: `-ENOMEM`; the
  new root mirrors kernel PML4 entries 256–511.
- `void vm_space_destroy(struct vm_space *)`: tears down every region
  (frames freed after shootdown), frees lower-half tables, frees the
  struct. Must not be the active space on the calling CPU. May block.
- `int vm_user_map_anon(space, base, size, prot, flags, name)`: exact
  range inside the window, page aligned, no W+X, `-EEXIST` on overlap;
  `VM_REGION_POPULATED` for eager zeroed frames, `VM_REGION_GUARD_BELOW`
  for a guard page; unwinds fully on `-ENOMEM`.
- `int vm_user_unmap(space, base, size)`: exact region only, `-EINVAL`
  otherwise; shoots down before freeing frames.
- `int vm_user_protect(space, base, size, prot)`: whole region only;
  `-EINVAL` for W+X, NONE, or partial ranges; shoots down.
- `uint64_t vm_user_find_free(space, from, size)`: first fit at or
  above `from` keeping one unmapped page between regions; 0 if none.
- `bool vm_user_range_mapped(space, addr, len, prot)`: contiguous
  coverage by regions all carrying `prot`.
- `void vm_set_user_hooks(const struct vm_user_hooks *)`: the process
  layer supplies `current_space()` and `fatal()` (noreturn) for the
  fault handler.

## arch/mmu.h additions

- `ARCH_MMU_MAP_USER`: U/S on the leaf and on intermediate entries
  created for it. `VM_PROT_USER` reported by `arch_mmu_query` when U/S
  is set.
- `int arch_mmu_context_init_user(ctx, kernel)`: root below 4 GiB with
  the kernel half copied.
- `void arch_mmu_context_destroy(ctx)`: frees every lower-half table
  and the root; asserts `CR3 != root` and every freed page carries
  `PG_PAGETABLE`.

## kernel/thread.h additions

`proc` (holds a process reference; NULL for kernel threads),
`proc_link` (under `process.lock`), `user_entry`, `user_sp`,
`tls_base` (unused). `thread_put` of a process thread leaves the
process's list and calls `process_last_thread_gone` when it was the
last, then drops the reference.

## kernel/percpu.h fixed offsets

`self` at 0, `kernel_stack_top` at 8, `user_rsp_scratch` at 16;
asserted with `STATIC_ASSERT` and relied on by `syscall_entry.S`.

## arch/context.h

`void arch_thread_switch_prepare(struct thread *next)`: called by the
scheduler with the run-queue lock held before `arch_context_switch`;
publishes `next`'s kernel stack top (per-CPU block and TSS `rsp0`) and
activates `next->proc->space` or `kernel_space` if CR3 differs.
