# Processes and User Mode: Architecture

Phase 4 of the roadmap. This document is the specification; `design.md`
holds data structures and algorithms; `api.md`, `invariants.md`,
`testing.md` complete the set. The object model and the system-call
layer have their own documents under `docs/kernel/object/` and
`docs/kernel/syscall/`.

## 1. Purpose

Run untrusted code. A process is a kernel object that owns a user
address space, a handle table, credentials, and one or more threads. The
kernel loads a static ELF executable into a fresh address space, enters
user mode, services system calls through a personality-specific table,
delivers faults back to the process as termination, and reclaims
everything when the last thread exits. The first process is `init`,
delivered by the bootloader as a module because no filesystem exists
yet.

## 2. Where it sits

```text
   user programs (userland/init, later the shell)      libc/include/cosmo/
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
an ELF image; user address space lifecycle; the main user thread; exit
with a status; termination on an unrecoverable fault; reaping of the
address space by the reaper thread (never on the exiting thread's own
stack or CR3).

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

- `fork`, `exec` from a file, `wait`, process trees with reparenting
  (needs a VFS for exec; fork needs CoW which needs the VM object layer).
- Signals, job control, sessions, controlling terminals.
- Multi-threaded processes from user space (`thread_create` syscall) and
  TLS setup (the fields exist).
- Dynamic linking, `PT_INTERP`, `PT_GNU_STACK` policy beyond refusing
  executable stacks.
- Linux personality (Phase 11); only the native table exists.
- Resource limits enforcement, audit, capabilities beyond a credential
  placeholder (Phase 5+ security work).
- Console input (`read` on the console returns 0 until a serial receive
  path exists).

## 5. Interfaces (contracts in api.md)

| Header | Provides |
|---|---|
| `kernel/object.h` | `struct kobject`, `kobject_init/get/put`, `struct kobject_type` |
| `kernel/handle.h` | `struct handle_table`, `handle_install/lookup/close`, rights |
| `kernel/process.h` | `struct process`, `process_create_from_elf`, `process_exit`, `process_current`, `process_wait_exit` |
| `kernel/elf.h` | `elf_validate`, `elf_load_into` |
| `kernel/syscall.h` | `syscall_dispatch`, `struct syscall_args`, personality |
| `kernel/uaccess.h` | `user_range_ok`, `copy_from_user`, `copy_to_user`, `strncpy_from_user` |
| `arch/user.h` | `arch_user_enter`, `arch_user_access_begin/end`, `arch_syscall_init_cpu` |
| `uapi/cosmo/syscall.h` | syscall numbers and ABI structs shared with user space |

## 6. Data structures (detail in design.md)

`struct kobject`, `struct kobject_type`, `struct handle_table` (fixed
64 entries, spinlock), `struct process` (pid, name, kobject, space,
threads, handles, credentials placeholder, personality, state, exit
status, completion, parent pid), `struct vm_space` gains `user` flag and
`user_lo/user_hi`, `struct thread` gains `proc`, `user_entry`,
`user_sp`, `kernel_stack_top`, `struct syscall_args` (nr, six args,
frame pointer).

## 7. Concurrency model

Lock order additions, outermost first:

```text
process_table.lock → process.lock → handle_table.lock
process.lock → vm_space.lock (user space) → pmm_zone.lock
```

`process.lock` protects the thread list, state, and exit status;
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
- Handles own object references; closing a handle drops one.
- The boot archive's memory is `COSMOBOOT_MEM_ARCHIVE`, reserved by the
  PMM for the life of the kernel (the `init` entry is read at each
  `init` start in this phase; a later phase copies it into a ramfs and
  frees it).

## 9. Error handling

System calls return negative errno values in the return register;
invalid handles `-EBADF`, bad pointers `-EFAULT`, bad arguments
`-EINVAL`, unknown numbers `-ENOSYS`. A user-mode fault that no region
handles terminates the process with exit status `128 + 11` and a log
line naming the fault; the kernel never panics on user behaviour. A
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

## 12. Testing strategy

Host tests for the ELF validator with crafted images. Kernel self-tests
for objects and handles, for loading `init` from the module with
`--selftest` and checking its exit status, and for terminating a
deliberately faulting user process with the expected status. The user
program itself exercises every syscall with valid, invalid, boundary,
and hostile-pointer inputs and prints `USERTEST: PASS`. See
`testing.md`.

## 13. Future extensibility

The personality pointer is where the Linux table plugs in. `struct
kobject` is the base for files, sockets, and devices. `vm_space` per
process with region kinds is where CoW (`fork`) and file-backed regions
(`exec`, `mmap` of files) attach. Threads already carry a process
pointer and per-thread kernel stack, so user threads are a syscall away.
AArch64 implements `arch/user.h` with `SVC`, `ERET`, `TTBR0`, and
`PAN`.
