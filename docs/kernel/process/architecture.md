# Processes and User Mode: Architecture

Phase 4 of the roadmap, extended by Phase 9 (userland). This document
is the specification; `design.md` holds data structures and algorithms;
`api.md`, `invariants.md`, `testing.md` complete the set. The object model and the system-call
layer have their own documents under `docs/kernel/object/` and
`docs/kernel/syscall/`.

## 1. Purpose

Run untrusted code. A process is a kernel object that owns a user
address space, a handle table, credentials, and one or more threads. The
kernel loads a static ELF executable into a fresh address space, enters
user mode, services system calls through a personality-specific table,
delivers faults back to the process as termination, and reclaims
everything when the last thread exits. The first process is `init`,
delivered in the boot archive and started by the kernel; since Phase 9
every other process is created by a running process with `spawn` from
an executable file, waited for with `wait`, and can be terminated with
`kill`.

## 2. Where it sits

```text
   user programs (userland/: init, sh, the utilities)  libc/ (docs/libc/)
        │  SYSCALL instruction, uapi/cosmo/syscall.h numbers and structs
        ▼
   kernel/arch/x86_64/syscall_entry.S   swapgs, kernel stack, frame, sysret
        ▼
   kernel/syscall/     dispatcher, personality table (native), user copy helpers
        ▼
   kernel/process/     process object, ELF loader, user threads, exit/reap
        │
   kernel/object/      kobject refcounts and types, per-process handle table
        │
   kernel/memory/      user vm_space (per-process MMU context), user faults
   kernel/scheduler/   threads (now with a process), CR3 and TSS on switch
```

The scheduler learns only that a thread may belong to a process (for the
address space to activate and the kernel stack to publish). The VMM
learns user spaces and user-mode permissions. Nothing below the process
layer depends on it.

## 3. Responsibilities

**Object model** (`kernel/object/`): `struct kobject` with reference
counting and a type descriptor (name, release); the handle table mapping
small integers to objects with per-handle rights; console object for
standard I/O until the VFS exists.

**Processes** (`kernel/process/`): process table and PIDs; creation from
an ELF image (`process.c`) or from an executable file on behalf of a
calling process (`spawn.c`); user address space lifecycle; the main user
thread; exit with a status; termination on an unrecoverable fault or by
`kill`; parent and children, zombies and `wait`, reparenting of orphans
to init; the working directory; reaping of the address space by the
reaper thread (never on the exiting thread's own stack or CR3).

**ELF loader** (`kernel/process/elf.c`): validation of a static x86-64
ELF64 executable (bounds, W^X, page-granularity overlap, entry inside an
executable segment), mapping of PT_LOAD segments as user regions with
ELF-derived permissions, zero-fill of `.bss`, initial stack with
`argc`/`argv`/`envp`/`auxv` per the System V ABI.

**User mode** (`kernel/arch/x86_64/`): SYSCALL/SYSRET setup (STAR,
LSTAR, SFMASK, EFER.SCE), the syscall entry path, SWAPGS on every
user/kernel transition, TSS `rsp0` and CR3 switch on context switch,
first entry into user mode via IRETQ, SMAP-aware user access windows.

**System calls** (`kernel/syscall/`): the generic dispatcher; the native
personality's table; argument validation; `copy_from_user`/
`copy_to_user` against the process's regions; errno return convention.

**Boot archive** (`boot/`, `kernel/core/bootarchive.c`): protocol
version 3 carries one ustar archive (`\cosmo\boot.tar`) read from the
boot volume; memory type `ARCHIVE` keeps it reserved. `init` is the
archive entry named `init` (`bootarchive_find("init", ...)`). Version 2
carried `init.elf` as a single raw module.

## 4. Non-responsibilities (later)

- `fork` (needs CoW, which needs the VM object layer) and `exec`
  replacing the current image; `spawn` is the creation primitive.
- Signal handlers and masks, job control, sessions, controlling
  terminals; `kill` only terminates.
- Multi-threaded processes from user space (`thread_create` syscall) and
  TLS setup (the fields exist).
- Dynamic linking, `PT_INTERP`, `PT_GNU_STACK` policy beyond refusing
  executable stacks.
- The Linux personality's own behaviour: it lives in `compat/linux/`
  (`docs/compat/linux/`, Phase 11); the process code only selects it
  (by the CosmoOS ELF note), allocates and frees its state through two
  hooks, and builds its auxiliary vector.
- Resource limits enforcement, audit, capabilities beyond a credential
  placeholder (Phase 5+ security work).
- Set-uid, resource limits, argument sizes beyond two stack pages
  (`COSMO_ARG_MAX` 2048 bytes, 128 entries), file-backed `mmap`.

## 5. Interfaces (contracts in api.md)

| Header | Provides |
|---|---|
| `kernel/object.h` | `struct kobject`, `kobject_init/get/put`, `struct kobject_type` |
| `kernel/handle.h` | `struct handle_table`, `handle_install/lookup/close`, rights |
| `kernel/process.h` | `struct process`, `process_create_from_elf` (with `struct process_spawn_attr`), `process_spawn`, `process_exit`, `process_wait_child`, `process_kill`, `process_check_kill`, `process_return_to_user`, `process_chdir`, `process_info`, `process_set_init`, `process_current`, `process_wait_exit` |
| `kernel/elf.h` | `elf_validate`, `elf_load_into` |
| `kernel/syscall.h` | `syscall_dispatch`, `struct syscall_args`, personality |
| `kernel/uaccess.h` | `user_range_ok`, `copy_from_user`, `copy_to_user`, `strncpy_from_user` |
| `arch/user.h` | `arch_user_enter`, `arch_user_access_begin/end`, `arch_syscall_init_cpu` |
| `uapi/cosmo/syscall.h` | syscall numbers and ABI structs shared with user space |

## 6. Data structures (detail in design.md)

`struct kobject`, `struct kobject_type`, `struct handle_table` (fixed
64 entries, spinlock), `struct process` (pid, name, kobject, space,
threads, handles, credentials, personality, state, exit status,
completion, parent and children, `child_wq`, `reaped`, `kill_sig`,
`cwd` and `cwd_path`), `struct vm_space` gains `user` flag and
`user_lo/user_hi`, `struct thread` gains `proc`, `user_entry`,
`user_sp`, `kernel_stack_top`, `struct syscall_args` (nr, six args,
frame pointer).

## 7. Concurrency model

Lock order additions, outermost first:

```text
process_table.lock → process.lock → handle_table.lock
process.lock → vm_space.lock (user space) → pmm_zone.lock
parent.lock → child.lock (never the reverse; reparenting detaches under the parent, then takes each child alone)
```

`process.lock` protects the thread list, state, exit status, children,
`kill_sig` and the working directory;
`handle_table.lock` protects the table. Objects are refcounted; a handle
holds a reference, a lookup returns a referenced object the caller must
put. A process is reaped by the reaper thread after its last thread
exits, so address-space teardown never runs on a stack or CR3 the
exiting thread is still using. Syscalls run on the calling thread's
kernel stack with interrupts enabled and may block.

## 8. Memory ownership

- A process owns its `vm_space` and every frame populated in it; teardown
  frees them and the lower-half page tables.
- The ELF image is borrowed for the duration of loading; segment bytes
  are copied into freshly allocated user frames.
- Handles own object references; closing a handle drops one. The whole
  table is closed when the last thread of the process is gone (before
  the parent collects the status), so a pipe whose writer exited
  delivers end of file at once.
- A zombie owns nothing but its identity and status: no address space is
  kept beyond the reaper, no handles, no working directory.
- The boot archive's memory is `COSMOBOOT_MEM_ARCHIVE`, reserved by the
  PMM for the life of the kernel (the `init` entry is read at each
  `init` start in this phase; a later phase copies it into a ramfs and
  frees it).

## 9. Error handling

System calls return negative errno values in the return register;
invalid handles `-EBADF`, bad pointers `-EFAULT`, bad arguments
`-EINVAL`, unknown numbers `-ENOSYS`, a wait interrupted by a kill
`-EINTR`. A user-mode fault that no region handles terminates the
process with exit status `128 + 11` and a log line naming the fault; a
kill terminates it with `128 + sig`; the kernel never panics on user
behaviour. A
kernel-mode fault on a user address that validation did not catch is a
kernel bug and panics.

## 10. Performance considerations

SYSCALL/SYSRET, not interrupt gates. Every context switch loads CR3 (no
lazy TLB yet); user mappings are non-global so kernel-half entries
survive the switch. Handle lookup is an array index under a spinlock.
Nothing is measured.

## 11. Security considerations

- User and kernel are separated by the MMU: user regions carry the U/S
  bit, kernel regions never do; SMEP/SMAP are enabled where the CPU has
  them and the copy helpers wrap access with STAC/CLAC.
- W^X for user mappings is enforced at load time (a segment that is both
  writable and executable is refused) and `mmap` refuses `PROT_WRITE |
  PROT_EXEC`.
- Every user pointer is range-checked and region-checked before the
  kernel touches it; kernel pointers passed as user pointers are
  `-EFAULT`.
- The user stack is non-executable and guarded.
- SFMASK clears IF, TF, DF, and AC on syscall entry so user state cannot
  leak into kernel execution; SWAPGS is done exactly once per transition.
- The kernel never exposes pointers to user space; handles are indices.
- A child receives exactly the handles its parent maps in `spawn`, with
  the parent's rights on them, and nothing else; the executable must be a
  regular file with an execute bit; `kill` requires the same uid or uid 0.

## 12. Testing strategy

Host tests for the ELF validator with crafted images. Kernel self-tests
for objects and handles, for loading `init` from the module with
`--selftest` and checking its exit status, and for terminating a
deliberately faulting user process with the expected status. The user
program itself exercises every syscall with valid, invalid, boundary,
and hostile-pointer inputs and prints `USERTEST: PASS`. See
`testing.md`.

## 13. Future extensibility

The personality pointer is where the Linux table plugged in (Phase 11:
`personality_linux`, selected when the image lacks the `CosmoOS`
`PT_NOTE`). `struct kobject` is the base for files, sockets, and devices. `vm_space` per
process with region kinds is where CoW (`fork`) and file-backed regions
(`exec`, `mmap` of files) attach. Threads already carry a process
pointer and per-thread kernel stack, so user threads are a syscall away.
AArch64 implements `arch/user.h` with `SVC`, `ERET`, `TTBR0`, and
`PAN`.

## 14. Phase 9: processes as userland sees them

Phase 9 (userland) turns the Phase 4 mechanism into a Unix process
model that a shell can drive. The additions live in `kernel/process/`
(`process.c`, `spawn.c`) and `kernel/syscall/native.c`; their design is
in `design.md` section 10, the calls in `api.md`, the rules in
`invariants.md`.

- **Creation from a file** (`spawn`, system call 32): the parent names
  an executable path, `argv`, `envp`, and exactly which of its handles
  the child receives and at which numbers (a capability-style handle
  map; nothing is inherited implicitly). The kernel reads the file
  through the VFS into a kernel buffer, validates and loads it with the
  Phase 4 ELF loader, gives the child the parent's credentials and
  working directory, records the parent, and starts it. There is no
  `fork`: a handle-based kernel copies exactly what the parent chooses.
- **Termination and reaping** (`wait`, 33): a child that exits becomes
  a zombie until its parent collects the status; `wait` blocks for any
  child or one child, `WNOHANG` polls; orphans are reparented to init
  (the process `kernel_main` registered with `process_set_init`; it is
  pid 1 only when no self-tests created processes first) or, when init
  is gone, reaped by the kernel. Kernel-created processes (parent 0) are
  reaped by the kernel as before. Handles close at exit, not at reaping.
- **Kill** (`kill`, 34): the only asynchronous event a process can
  receive. `SIGKILL`, `SIGTERM`, `SIGINT` (and any number 1..31)
  terminate the target with status `128 + sig`; there are no handlers.
  Permission: same uid or uid 0. Delivery points are the system-call
  boundary, the return from any interrupt or fault to user mode, and
  every killable wait in the kernel (`wait_event_killable`), so a
  process blocked in a `read` on the console or a pipe, or in `wait`,
  dies promptly.
- **Handles**: `pipe` (35, see `docs/kernel/ipc/`), `dup` (36: copy a
  handle to the lowest free slot or to a chosen slot, rights preserved),
  `fstat` on any I/O object (character device for the console, FIFO for
  a pipe end, socket for a socket).
- **Working directory** (`chdir` 38, `getcwd` 39): a per-process
  current directory (vnode reference plus a normalised absolute path)
  from which relative paths in every filesystem system call resolve;
  inherited by `spawn`.
- **Introspection** (`getppid` 37, `procinfo` 40, `klog` 41, `sysctl`
  42): the process table for `ps`, the kernel log ring for `dmesg`, and
  a small read-only set of named values for `sysctl`.

**Non-responsibilities (still)**: `fork`, `exec` replacing the current
image, signal handlers and masks, process groups and sessions, threads
in user programs, resource limits, argument sizes beyond one stack page
(`COSMO_ARG_MAX` 2048 bytes and `COSMO_ARG_ENTRIES` 128 across `argv`
and `envp`), file-backed `mmap`, set-uid.
