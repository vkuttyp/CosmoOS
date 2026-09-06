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

### `int process_create_from_images(const struct process_image *exe, const struct process_image *interp, const char *name, ...)` (milestone 10)
- Purpose: the same, from an executable image plus, when it names a
  program interpreter (`PT_INTERP`), the interpreter's image;
  `struct process_image { const void *data; size_t size; const char
  *path; }`, `path` being what `AT_EXECFN` reports.
  `process_create_from_elf` is this with `interp == NULL`.
- An `ET_DYN` executable is rebased to `USER_PIE_BASE`
  (`0x555500000000`); an `ET_DYN` interpreter to the first free range at
  or above `USER_INTERP_BASE` (`0x7F0000000000`) after the executable is
  mapped; an `ET_EXEC` interpreter loads where it was linked. With an
  interpreter the main thread starts at the interpreter's entry;
  `p->interp_base` (0 without one) and `p->exec_entry` feed `AT_BASE`
  and `AT_ENTRY`. An executable with `PT_INTERP` and no interpreter
  image, an interpreter that itself has `PT_INTERP`, or a PIE that does
  not fit at its base is `-ENOEXEC` (logged); no free range for the
  interpreter is `-ENOMEM`.

### `void process_exit(int status)` (noreturn)
- Purpose: terminate the calling process with `status`.
- Inputs: `status` as reported by `process_wait_exit`.
- Concurrency: runs the personality's `thread_exit` hook, sets state
  EXITING under `process.lock` (keeping an earlier status), wakes every
  other thread of the process (each leaves at its next return to user
  mode or killable wait: `signal_pending` is true for an exiting
  process), then calls `thread_exit`; never returns. Requires a
  process-owning thread and a preemptible context (the fault hook and
  the trap tail re-enable interrupts first).

### `void process_thread_exit(int status)` (noreturn, milestone 10)
- Ends the calling thread only (the hook, `nr_live--`); when it was the
  last live thread the process exits with `status`. Linux `exit`.

### `int process_add_thread(struct process *p, const struct arch_user_regs *regs, uintptr_t tls, struct thread **out)`, `void process_thread_start(struct thread *t)`, `void process_thread_abandon(struct thread *t)` (milestone 10)
- Create a thread of `p` that enters user mode with the register set
  `regs` (`arch_user_enter_regs`) and the thread pointer `tls`, with the
  caller's blocked-signal set inherited and a fresh (reset) FPU state.
  After `add` the thread is linked (`process_find_thread` sees it, it
  counts in `nr_threads` and `nr_live`) but not runnable, and the caller
  holds the creator's reference; `start` hands the reference to the
  process and enqueues it; `abandon` starts it so that it exits at once
  without running user code (a tid word could not be written). Errors:
  `-ENOMEM`; `-EAGAIN` when the process is exiting or has
  `PROCESS_MAX_THREADS` (256) live threads.

### `struct thread *process_find_thread(struct process *p, uint32_t lx_tid)`
- The live thread of `p` with that Linux tid (the pid for the main
  thread, `0x10000 + kernel tid` otherwise), or NULL. A borrowed
  pointer, used only to queue a signal under the process's lock, which
  is safe while the process exists (`sig_info` lives until
  `thread_put`).

### `int process_wait_exit(struct process *p)`
- Purpose: block until `p` has exited; return its status (kernel
  callers: `kernel_main`, the self-tests).
- Concurrency: `wait_for_completion(&p->exited)`; may block. `p` must be
  referenced by the caller. Returns immediately if already exited.

### `int process_spawn(const char *path, const char *const argv[], const char *const envp[], const struct process_handle_map *handles, unsigned nr_handles, const char *cwd, const struct process_spawn_cred *cred, pid_t *pid_out)` (`spawn.c`)

`process_spawn_attr.rlim` (NULL: the parent's limits or the defaults)
lets a kernel creator name a child's limits. `cred` is NULL to inherit, or the child's uid and gid
(`COSMO_SPAWN_SETCRED`): `-EPERM` unless the caller is privileged or
holds both ids (`docs/kernel/security/design.md` §1). The child is also
refused with `-EAGAIN` when its real uid already has
`COSMO_RLIMIT_NPROC` processes.
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

### `void process_check_kill(void)`, `void process_return_to_user(struct arch_trap_frame *frame)`, `bool process_kill_pending(void)`
- The delivery points (milestone 10: signals ride the same ones).
  `process_check_kill` exits the current process when `kill_sig` is set
  or the process is exiting (called by `syscall_dispatch` before the
  handler; after it the dispatcher calls `signal_deliver(frame, true)`
  when `signal_pending()`). `process_return_to_user(frame)` enables
  interrupts, runs `signal_deliver(frame, false)` on the trap frame and
  disables them again (called by the arch trap tail for user frames with
  `irq_depth == 0` and `preempt_count == 0`); a handler frame set up
  there is what the trap returns into. `process_kill_pending` is now
  `signal_pending()`: a kill, an exiting process, or a deliverable
  signal, so every killable wait returns `-EINTR` for all three
  (declared in `wait.h`).

### `kernel/signal.h` (milestone 10)
The signal core, `kernel/process/signal.c`; `docs/kernel/process/design.md`
§11. Linux numbers (`SIGHUP` 1 … `SIGSYS` 31, 64 signals), `SIGMASK(sig)`,
`struct sigaction_k { handler, flags, restorer, mask }` (Linux's
`k_sigaction`), `SA_*`, `struct sigaltstack_k { sp, size, flags }`,
`struct signal_info { sig, source (SIGSRC_USER/TKILL/FAULT/KERNEL),
fault_addr, code (1 unmapped, 2 protection), sender_pid, sender_uid }`.
- `int signal_send(struct process *p, int sig, const struct signal_info *info)`,
  `int signal_send_thread(struct thread *t, int sig, ...)`: queue (any
  context; `-EINVAL` for a number outside 1..64, `-ESRCH` for a thread
  without a process). Under `p->lock`: `SIGKILL` sets the kill flag and
  wakes every thread; an ignored signal (action `SIG_IGN`, or `SIG_DFL`
  with an ignore default: `SIGCHLD`, `SIGURG`, `SIGWINCH`, `SIGCONT` and
  the stop signals) is discarded even when blocked; a default-terminate
  signal sets the kill flag unless every candidate thread blocks it
  (then it stays pending until one unblocks, `signal_set_blocked`
  rechecks); anything else is queued on the thread or the process and a
  thread that can take it is woken.
- `void signal_fault(int sig, uint64_t addr, struct arch_trap_frame *frame)`,
  `void signal_fault_info(const struct signal_info *, frame)`: a fault on
  the calling thread; with a handler installed and unblocked the frame is
  built now and the call returns (the trap returns into the handler),
  otherwise the process terminates with `128 + sig` (logged) and the
  call does not return.
- `signal_set_action`/`signal_get_action` (per process),
  `signal_blocked`/`signal_set_blocked` (the calling thread; `SIGKILL`
  and `SIGSTOP` never block), `signal_pending_set`,
  `signal_set_blocked_saved(mask)` (the mask to restore after a
  temporary one: `rt_sigsuspend`, `ppoll`).
- `bool signal_pending(void)`; `void signal_deliver(void *frame, bool
  is_syscall)`: the delivery loop — terminate on a kill or an exiting
  process; dequeue the lowest deliverable signal (synchronous faults
  first); ignore, terminate, or build one handler frame through the
  personality's `signal_frame(regs, act, info, blocked_before)` (with
  `SA_RESTART` and a `-EINTR` result the call is re-armed first:
  `arch_user_regs_restart_syscall` with `thread.syscall_nr`/`syscall_arg0`
  unless the number is `SIGNAL_NO_RESTART`), then block the handler's
  mask plus the signal (unless `SA_NODEFER`), reset the action under
  `SA_RESETHAND`, sanitise the registers and write them back. A frame
  the personality cannot build (`-EFAULT`: an unmapped stack) ends the
  process with `128 + SIGSEGV`. One handler per return; the next runs
  when that one returns. Interrupts are enabled around the frame build.
- `void signal_return(void *syscall_frame, const struct arch_user_regs *regs, uint64_t blocked)`:
  `rt_sigreturn`'s tail — sanitise, set the mask, write the frame (full
  restore).
- `int signal_wait(void)`: sleep until `signal_pending()`; returns
  `-EINTR` (`pause`, `rt_sigsuspend`).
- `int signal_process_init/void signal_process_release(struct process *)`:
  the 64-entry action table (`kzalloc`).

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

### `unsigned process_info(struct cosmo_procinfo *buf, unsigned count, const struct credentials *viewer)`

Records for every process when `viewer` is privileged, else only for
processes whose real uid is the viewer's; the return value counts the
qualifying ones. See also `process_getrlimit`, `process_setrlimit`,
`process_count_uid` and `process_log_permitted` in
`docs/kernel/security/api.md`.
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
`image_end` (the page after the highest loaded segment); milestone 10:
`sigactions` (64 `struct sigaction_k`, owned), `sig_shared_pending` and
`sig_shared_info` (process-directed signals not yet taken), `nr_live`
(threads that have not exited; the process ends when it reaches 0),
`main_thread`, `interp_base`, `exec_entry`, `exec_path[128]`. `struct
personality` gained two optional hooks: `signal_frame` (build a handler
frame on a register set; NULL means handlers cannot run) and
`thread_exit` (a thread of this personality is leaving). `struct
process_handle_map { int child, parent; }`
(the shape of `struct cosmo_spawn_handle`, asserted) and
`struct process_spawn_attr` are the spawn inputs; `PROCESS_WAIT_NOHANG`
is the wait flag.

Constants: `USER_LO` = `VM_USER_LO` (4 MiB), `USER_HI` = `VM_USER_HI`,
`USER_STACK_TOP` = `0x00007FFFFFFF0000`, `USER_STACK_SIZE` = 8 MiB,
`USER_MMAP_BASE` = `0x0000100000000000`, `USER_PIE_BASE` =
`0x0000555500000000`, `USER_INTERP_BASE` = `0x00007F0000000000`,
`PROCESS_MAX_THREADS` = 256.

## kernel/elf.h

### `int elf_validate(const void *image, size_t size, uint64_t user_lo, uint64_t user_hi, struct elf_info *info, const char **why)`
- Purpose: decide whether `image` is a loadable executable (`ET_EXEC`,
  or since milestone 10 `ET_DYN`) for this machine and describe its
  segments. An `ET_DYN` image is validated relative to address 0
  (`is_dyn` set; its segments must fit the window's span) and the caller
  rebases it with `elf_rebase(info, base)`, which adds `base` to every
  segment, the entry, `lo`, `hi` and `phdr_vaddr`. A `PT_INTERP` is
  recorded (`has_interp`, `interp[256]`: 1..255 bytes inside the file,
  NUL-terminated), not refused.
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
  - "not ET_EXEC or ET_DYN"
  - "not x86-64" (or "not AArch64")
  - "bad program header table"
  - "program header table outside the file"
  - "PT_NOTE outside the file"
  - "two PT_INTERP entries"
  - "PT_INTERP path is empty, too long or outside the file"
  - "PT_INTERP path is not NUL-terminated"
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

All helpers may block (a demand-zero fault during the copy allocates).
They check the range, then copy through `arch_copy_user_raw`, whose
faulting instructions carry exception fixups: any fault inside the copy
is `-EFAULT` (`docs/kernel/memory/design.md` §6.1). From a thread with
no process every user address faults and is `-EFAULT`.

- `bool user_range_ok(uint64_t addr, size_t len)`: inside
  `[USER_LO, USER_HI)` without overflow; `len` 0 accepts `addr == USER_HI`.
- `int copy_from_user(void *dst, uint64_t src, size_t len)` /
  `int copy_to_user(uint64_t dst, const void *src, size_t len)`: 0 or
  `-EFAULT`; copy inside `arch_user_access_begin/end`. On `-EFAULT` the
  destination may be partly written up to the faulting page.
- `int strncpy_from_user(char *dst, uint64_t src, size_t max)`: length,
  `-EFAULT`, `-ENAMETOOLONG`, `-EINVAL` (max 0); always terminates `dst`;
  copies page by page so a string ending before an inaccessible page is
  accepted and one running into it is `-EFAULT`.

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
  no native call sets it yet. On AArch64 the register (`tpidr_el0`) is
  user-writable, so the switch hook also *saves* the outgoing thread's
  value (milestone 10).
- Milestone 10, the register file (`struct arch_user_regs`, defined per
  architecture in this header so generic code and modules see it:
  x86-64 the 16 general registers plus `rip`, `rflags`; AArch64 `x[31]`,
  `sp`, `pc`, `pstate`): `arch_user_regs_from_syscall/to_syscall` (the
  system-call frame; `to_syscall` sets the user selectors and requests
  the full-restore exit on x86-64), `from_trap/to_trap`, `pc`/`sp`/
  `set_pc`/`set_sp`/`set_result`/`result`, `restart_syscall(r, nr,
  arg0)` (`rip -= 2` / `pc -= 4`), `sanitize` (user-changeable flag
  bits only; x86-64 also replaces a non-canonical or kernel-half `rip`
  with 0), `arch_user_enter_regs(r)` (noreturn: the first entry of a
  clone), `set_result_in_frame`/`result_in_frame`, and the FPU image
  hooks `arch_user_fpu_image_size/save/restore` (x86-64: the 512-byte
  FXSAVE image; AArch64: none).
- `x86_syscall_return_check(frame)` (`user.c`): sets
  `X86_SYSCALL_FULL_RESTORE` when `rip` is not canonical or `rflags`
  carries a bit `SYSRET` may not load; the exit takes `iretq` then
  (`syscall_entry.S`: the frame now carries `flags`, `rcx`, `r11` below
  the saved registers).

## kernel/vmm.h additions

- `VM_USER_LO` (`0x400000`), `VM_USER_HI` (`0x00007FFFFFFFF000`),
  `VM_REGION_USER` flag, `struct vm_space.user`, `.anon_pages`.
- `int vm_space_create_user(struct vm_space **out)`: `-ENOMEM`; the
  new root mirrors kernel PML4 entries 256–511.
- `void vm_space_destroy(struct vm_space *)`: tears down every region
  (frames freed after shootdown), frees lower-half tables, frees the
  struct. Must not be the active space on the calling CPU. May block.
- `int vm_user_map_anon(space, base, size, prot, flags, name)`: exact
  range inside the window, page aligned, no W+X (`VM_PROT_NONE` reserves),
  `-EEXIST` on overlap; `VM_REGION_POPULATED` for eager zeroed frames,
  `VM_REGION_GUARD_BELOW` for a guard page; unwinds fully on `-ENOMEM`;
  merges with an equal adjacent region.
- `int vm_user_unmap(space, base, size, flags)`: any page range in the
  window; regions are split at the ends. `VM_UNMAP_STRICT`: every page
  must be mapped or `-EINVAL` with nothing changed; without it unmapped
  pages are skipped. `-ENOMEM` if a split record cannot be allocated
  (nothing changed). Shoots down on the CPUs running the space before
  freeing frames.
- `int vm_user_protect(space, base, size, prot)`: any page range;
  `-EINVAL` for W+X or a bad range; `-ENOMEM` if a page is unmapped or
  a split cannot be allocated (nothing changed); splits at the ends,
  merges equal neighbours afterwards; `VM_PROT_NONE` keeps the frames;
  shoots down.
- `unsigned vm_user_region_count(space)`: for tests.
- `uint64_t vm_user_find_free(space, from, size)`: first fit at or
  above `from` keeping one unmapped page between regions; 0 if none.
- `bool vm_user_range_mapped(space, addr, len, prot)`: contiguous
  coverage by regions all carrying `prot`.
- `void vm_set_user_hooks(const struct vm_user_hooks *)`: the process
  layer supplies `current_space()` and `fatal()` (noreturn) for the
  fault handler.

## arch/user.h additions (milestone 5)

- `size_t arch_copy_user_raw(void *dst, const void *src, size_t n)`:
  copy with every access in the exception table; returns the bytes not
  copied (0 on success). x86-64: `rep movsb`, one entry; AArch64: an
  aligned 8-byte loop and a byte loop, four entries. Call inside the
  access window.
- `bool arch_trap_fixup(struct arch_trap_frame *)`: if the frame's PC
  has an exception-table entry, move the PC to the fixup and return
  true.

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
last, then drops the reference. Milestone 10: `init_regs` (a clone's
first register set, freed at its first user entry), `sig_pending`,
`sig_blocked`, `sig_saved_blocked`/`sig_restore_blocked`, `sig_info`
(64 entries, freed by `thread_put`), `altstack`, `syscall_nr`/
`syscall_arg0` (the call in progress, for `SA_RESTART`),
`clear_child_tid`, `lx_tid`.

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
