# Processes and User Mode: API

Every entry follows constitution section 52: purpose, inputs, outputs,
ownership, lifetime, concurrency, blocking, interrupt context, failure
modes, ABI stability. All interfaces here are kernel-internal (no
stability promise) except `uapi/cosmo/syscall.h`, which is user ABI and
documented in `docs/kernel/syscall/api.md`.

## Shared contracts

- **Lock order** (outermost first):
  `process_table.lock → process.lock → handle_table.lock`,
  `process.lock → vm_space.lock → pmm_zone.lock`, and
  `parent.lock → child.lock` (never the reverse). All are spinlocks
  taken with interrupts saved and disabled.
- **References**: a `struct process` is a `kobject`. Parties holding
  references while a process runs: the creator (returned by
  `process_create_from_elf`; `process_spawn` drops it at once), the
  process table (dropped by `process_last_thread_gone` when nobody will
  wait, otherwise by `process_wait_child` when the zombie is collected),
  each thread (dropped by `thread_put` in the reaper), each child (its
  `parent` pointer), and `process_set_init` for init. The object is
  released when all are gone.
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

### `int process_create_from_elf(const void *image, size_t size, const char *name, const char *const argv[], const char *const envp[], const struct process_spawn_attr *attr, struct process **out)`
- Purpose: build a process from a static ELF image in kernel memory and
  make its main thread runnable.
- Inputs: `image`/`size` borrowed for the call; `name` copied
  (truncated to `PROCESS_NAME_MAX`); `argv`/`envp` NULL-terminated,
  may be NULL, copied onto the initial user stack; `attr` NULL for a
  kernel creator (console handles 0, 1, 2; root working directory; no
  parent; uid 0) or a `struct process_spawn_attr { parent, handles,
  nr_handles, cwd, cwd_path }` naming the calling process, its validated
  handle map (`handles == NULL` with `nr_handles == 0` copies the
  parent's 0, 1, 2 as they are), and an optional working directory
  (NULL: the parent's). The child inherits the parent's credentials.
- Outputs: 0 and `*out` referenced; `-ENOEXEC` (validation failed,
  rule logged), `-ENOMEM`, `-EINVAL` (strings exceed the first stack
  page), `-EEXIST` (segment overlap at mapping time), `-EBADF`/`-EBUSY`
  from installing the handle map.
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
- Purpose: block until `p` has exited; return its status (kernel
  callers: `kernel_main`, the self-tests).
- Concurrency: `wait_for_completion(&p->exited)`; may block. `p` must be
  referenced by the caller. Returns immediately if already exited.

### `int process_spawn(const char *path, const char *const argv[], const char *const envp[], const struct process_handle_map *handles, unsigned nr_handles, const char *cwd, pid_t *pid_out)` (`spawn.c`)
- Purpose: create a child of the calling process from an executable
  file (the `spawn` system call's engine).
- Inputs: kernel copies of the path, `argv` (`argv[0]` required),
  `envp` (may be NULL), the handle map (`{child, parent}` pairs, at most
  `HANDLE_TABLE_SIZE`; child slots in range and distinct, parent handles
  present in the caller's table) and an optional working directory,
  both resolved relative to the caller's cwd.
- Outputs: 0 and the child's pid; `-EINVAL`, `-EBADF` (map), `-ENOENT`
  and the other path errors, `-ENOTDIR` (cwd), `-EACCES` (not a regular
  file or no execute bit), `-ENOEXEC` (empty, over 16 MiB, or rejected
  by `elf_validate`), `-ENOMEM`, `-EIO`.
- Ownership: the child is linked under the caller (`children`) and holds
  a reference on it; the creator's reference is dropped before return.
- Concurrency: may block (VFS reads, allocations). Asserts a current
  process. The image is read into a populated kernel-arena buffer and
  freed after loading.

### `int process_wait_child(int pid, unsigned flags, pid_t *pid_out, int *status_out)`
- Purpose: collect an exited child. `pid > 0` names one child, `-1` any.
- Outputs: 0 with `*pid_out` the reaped pid and `*status_out` its exit
  status (the table reference is dropped: the zombie is gone); 0 with
  `*pid_out == 0` when `PROCESS_WAIT_NOHANG` found nothing; `-ECHILD`
  when no matching child exists; `-EINTR` when the caller is killed
  while waiting.
- Blocking: `wait_event_killable(&cur->child_wq, ...)` unless NOHANG.

### `void process_kill(struct process *p, int sig)`
- Purpose: terminate `p` asynchronously with status `128 + sig`.
- Effects: under `p->lock`, if RUNNING and not already being killed,
  records `kill_sig` and the status and `sched_wake`s every thread of
  `p`. Delivery happens on the target's own thread at the next boundary:
  `process_check_kill` (system-call entry and exit), `process_return_to_user`
  (any interrupt or fault returning to ring 3), or a `wait_event_killable`
  that returns `-EINTR`. A zombie or exiting process is untouched.
- Concurrency: interrupt-safe (spinlock and `sched_wake` only); the
  caller holds a reference (`process_lookup`).

### `void process_check_kill(void)`, `void process_return_to_user(void)`, `bool process_kill_pending(void)`
- The delivery points. `process_check_kill` exits the current process
  when `kill_sig` is set (called by `syscall_dispatch` before and after
  the handler). `process_return_to_user` does the same after enabling
  interrupts (called by the arch trap tail for user frames with
  `irq_depth == 0` and `preempt_count == 0`). `process_kill_pending`
  is the read used by `wait_event_killable` (declared in `wait.h`).

### `int process_chdir(const char *path)`
- Purpose: change the calling process's working directory.
- Effects: `path_normalize(cwd_path, path)` computes the new absolute
  string (`-ENAMETOOLONG`), `vfs_lookup(cur->cwd, path)` must yield a
  directory (`-ENOENT`, `-ENOTDIR`, ...); the vnode reference and the
  string are swapped under `process.lock`. Every path system call passes
  `process_current()->cwd` as the VFS start vnode.

### `int path_normalize(const char *base, const char *rel, char *out, size_t n)`
- Pure string function: joins `rel` to the absolute `base` (or takes
  `rel` alone when absolute), drops `.` and empty components, resolves
  `..` (never above the root), produces `/x/y` or `/`. `-ENAMETOOLONG`
  when `n` is too small. Tested by `process-spawn`.

### `unsigned process_info(struct cosmo_procinfo *buf, unsigned count)`
- Fills up to `count` records (pid, ppid, uid, gid, state, threads,
  syscalls, summed thread run time, name) in table order under the table
  lock (each process's lock for its thread list) and returns the total
  number of processes, zombies included.

### `void process_set_init(struct process *p)`
- Records init (a reference is taken; a previous init is released) so
  `process_last_thread_gone` can reparent orphans to it while it is
  RUNNING. `kernel_main` calls it after creating init and with NULL
  after init exits.

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
  its process. Sets EXITED; reparents the children to init (or makes
  them kernel-owned, dropping exited unreaped ones); closes the handle
  table and drops `cwd`; logs the exit line; completes `exited`; then
  wakes the parent's `child_wq` (the zombie keeps the table's reference
  for `process_wait_child`) or, with no parent, drops the table's
  reference.
- Concurrency: reaper thread context, interrupts enabled; `handle_table_destroy`
  may block (last puts of files, pipe ends, sockets).

### `unsigned process_count(void)`, `void process_dump_all(void)`
- Diagnostics. `process_dump_all` prints pid, name, state, thread count,
  and syscall count under `process_table.lock`.

### `struct process` fields
`obj` (kobject), `pid`, `parent_pid` (the parent's pid, init's after
reparenting, 0 for kernel-created processes), `name`, `space` (user
`vm_space`, owned), `handles`, `threads` + `nr_threads` (under
`process.lock`), `cred` (uid/gid; inherited by spawn), `pers`
(`&personality_native`), `state` (RUNNING → EXITING → EXITED),
`exit_status`, `exited` (completion), `lock`, `all_link`, `syscalls`;
Phase 9: `parent` (referenced or NULL), `children`/`sibling`,
`child_wq`, `reaped`, `kill_sig`, `cwd` (referenced vnode),
`cwd_path[1024]`; Phase 11: `linux` (`struct linux_state *`, the Linux
personality's state, NULL for native processes; `docs/compat/linux/`),
`image_end` (the page after the highest loaded segment). `struct
process_handle_map { int child, parent; }`
(the shape of `struct cosmo_spawn_handle`, asserted) and
`struct process_spawn_attr` are the spawn inputs; `PROCESS_WAIT_NOHANG`
is the wait flag.

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
  file offset/size and the unaligned file vaddr; Phase 11: `cosmo_note`
  when a `PT_NOTE` holds a note named `CosmoOS` of type 1, `phdr_vaddr`
  when the program header table lies inside a `PT_LOAD`'s file bytes
  (else 0), `phnum`, `phent`); a `PT_NOTE` outside the file is
  "PT_NOTE outside the file"; `-ENOEXEC` with
  `*why` set to one of these immortal strings, in check order:
  - "file shorter than the ELF header"
  - "bad ELF magic"
  - "not ELF64 little-endian v1"
  - "not ET_EXEC (static executables only)"
  - "not x86-64"
  - "bad program header table"
  - "program header table outside the file"
  - "PT_NOTE outside the file"
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
- `void arch_set_tls_base(uintptr_t base)` (Phase 11): stores `base` in
  the current thread's `tls_base` and writes `MSR_FS_BASE` at once;
  `arch_thread_switch_prepare` reloads `MSR_FS_BASE` from
  `next->tls_base` for every thread with a process, so the user `%fs`
  base follows the thread. Used by the Linux `arch_prctl(ARCH_SET_FS)`;
  no native call sets it yet.

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
`tls_base` (the user `%fs` base, `arch_set_tls_base`; loaded on every
switch to a thread with a process). `thread_put` of a process thread leaves the
process's list and calls `process_last_thread_gone` when it was the
last, then drops the reference.

## kernel/wait.h additions (Phase 9)

`wait_event_killable(wq, cond)` evaluates to 0 or `-EINTR`: the
`wait_event` loop with `process_kill_pending()` checked after the thread
is queued and BLOCKED and before it blocks; `thread_sleep_ns_killable(ns)`
is the sleep with the same contract (cancels its timer when woken
early). `sched_wake(t)` on a thread is what `process_kill` uses to end a
wait. See `docs/kernel/scheduler/api.md`.

## kernel/percpu.h fixed offsets

`self` at 0, `kernel_stack_top` at 8, `user_rsp_scratch` at 16;
asserted with `STATIC_ASSERT` and relied on by `syscall_entry.S`.

## arch/context.h

`void arch_thread_switch_prepare(struct thread *prev, struct thread *next)`:
called by the scheduler with the run-queue lock held and interrupts
disabled before `arch_context_switch`; saves `prev`'s vector/x87 state
and loads `next`'s when they own state (`arch/fpu.h`; `prev` is NULL
when a CPU abandons its bootstrap context), publishes `next`'s kernel
stack top (per-CPU block and TSS `rsp0`) and activates
`next->proc->space` or `kernel_space` if CR3 differs.

## kernel/cred.h (Prompt #3 fix pass)

`struct credentials { ruid, euid, suid, rgid, egid, sgid, ngroups, groups[16] }`,
`cred_privileged(c)` (`euid == 0`; the single privilege predicate),
`cred_current()` (the process's, or `cred_kernel` on a kernel thread),
`cred_in_group`, `cred_setresuid`/`cred_setresgid` (POSIX rules, `-1`
keeps, all or nothing, `-EPERM`/`-EINVAL`), `cred_setgroups`
(privileged), `cred_may_signal`. Process wrappers taking `process->lock`:
`process_setresuid`, `process_setresgid`, `process_setgroups`. Native
system calls 50-55: `setresuid`, `setresgid`, `getresuid`, `getresgid`,
`setgroups`, `getgroups`. Details: `design.md`, "Credentials".
