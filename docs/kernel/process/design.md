# Processes and User Mode: Design

## 1. Kernel objects and handles

```c
struct kobject_type {
    const char *name;
    void (*release)(struct kobject *obj);   /* refcount reached zero */
};

struct kobject {
    const struct kobject_type *type;
    uint32_t refcount;                      /* atomic */
};
```

`kobject_init(obj, type)` sets refcount 1. `kobject_get/put` are atomic;
`put` calls `type->release` at zero. Objects embed a `kobject` as their
first-or-any member and recover themselves with `container_of`.

```c
#define HANDLE_TABLE_SIZE 64

struct handle_entry { struct kobject *obj; unsigned rights; };
struct handle_table { spinlock_t lock; struct handle_entry entries[HANDLE_TABLE_SIZE]; };

#define HANDLE_RIGHT_READ  (1u << 0)
#define HANDLE_RIGHT_WRITE (1u << 1)
```

`handle_install` takes a reference and returns the lowest free index or
`-EMFILE`; `handle_lookup(table, h, rights_needed)` returns a referenced
object (caller `kobject_put`s) or NULL when the index is empty or lacks
the rights; `handle_get` returns a referenced object with its rights
(for `dup` and `spawn`); `handle_close` drops the table's reference;
`handle_table_destroy` closes everything. Handles 0, 1, 2 of a
kernel-created process are the console object with read, write, write
rights; a spawned child gets exactly what its parent maps (section 10).

The console object (`kernel/object/console_obj.c`) is a `kobject` whose
type (`struct kobject_io_type`) carries `read`/`write`/`stat` function
pointers used by `sys_read`/`sys_write`/`sys_fstat`; `write` goes to
`console_write`, `read` to the console tty (`docs/kernel/tty/`), `stat`
reports a character device. `struct file`, `struct socket` and the pipe
ends are the other I/O kobjects.

## 2. Process

```c
enum process_state { PROCESS_RUNNING, PROCESS_EXITING, PROCESS_EXITED };

struct process {
    struct kobject obj;
    pid_t pid;
    char name[PROCESS_NAME_MAX];
    struct vm_space *space;
    struct handle_table handles;
    struct list_node threads;          /* struct thread.proc_link */
    unsigned nr_threads;
    struct credentials cred;           /* kernel/cred.h: real/effective/saved uid and gid, groups */
    const struct personality *pers;    /* &personality_native, or &personality_linux (Phase 11) */
    enum process_state state;
    int exit_status;
    struct completion exited;
    spinlock_t lock;
    struct list_node all_link;         /* process table */
    pid_t parent_pid;
    /* Phase 9, section 10 */
    struct process *parent;            /* referenced; NULL for kernel-created processes */
    struct list_node children, sibling;
    struct waitqueue child_wq;
    bool reaped;
    int kill_sig;
    struct vnode *cwd;
    char cwd_path[1024];
};
```

Process table: global list + spinlock + next pid. Pid 1 is the first
process created; in self-test builds that is a self-test's process, so
`kernel_main` registers the real init with `process_set_init` instead
of assuming pid 1.

### Creation from an ELF image

`process_create_from_elf(image, size, name, argv[], envp[], attr)`
(`attr` NULL for kernel creators; section 10 describes the spawn
attributes):

1. `elf_validate(image, size, &info)`: ELF64 LE, `ET_EXEC`, `EM_X86_64`,
   program headers in bounds, each `PT_LOAD` in bounds, no W+X segment,
   no two segments sharing a page, entry inside an executable segment,
   all segment vaddrs inside `[USER_LO, USER_HI)`; `PT_GNU_STACK` with X
   refused; `PT_INTERP` refused (static only).
2. Allocate `struct process`, `vm_space_create_user(&space)` (fresh MMU
   context whose kernel half copies the kernel root's PML4 entries
   256–511, see §5).
3. For each `PT_LOAD`: `vm_user_map_anon(space, vaddr_page, size_pages,
   prot, "elf-seg")` with `VM_REGION_POPULATED`, then copy `p_filesz`
   bytes from the image through the direct map into the new frames and
   leave the rest zero. Copying happens through the frames' direct-map
   addresses, never through the user mapping, so it needs neither the
   user CR3 nor STAC.
4. Stack: `vm_user_map_anon(space, USER_STACK_TOP - USER_STACK_SIZE,
   USER_STACK_SIZE, RW, "stack")` with a guard page below, lazily
   populated except the top two pages (`INITIAL_STACK_PAGES`), which
   receive the initial frame (written byte by byte through the direct
   map, since the two frames need not be adjacent):

   ```text
   USER_STACK_TOP-ish (16-byte aligned at the final rsp):
     argc
     argv[0..argc-1], NULL
     envp[0..], NULL
     auxv: AT_PAGESZ, AT_ENTRY, AT_NULL          (native)
           or the Linux vector: AT_PHDR AT_PHENT AT_PHNUM AT_PAGESZ AT_ENTRY
           AT_RANDOM AT_UID AT_EUID AT_GID AT_EGID AT_SECURE AT_HWCAP AT_CLKTCK AT_NULL
     16 random bytes (AT_RANDOM), 16-byte aligned
     strings (argv, envp bytes; at most INITIAL_STRINGS_MAX 300 of them)
   ```

   Phase 11: before the stack, `elf_load_into` is followed by
   `p->image_end = info.hi` and, for a Linux process (`p->pers ==
   &personality_linux`: the image lacks the CosmoOS note and the
   process has a parent), `linux_process_init` (`docs/compat/linux/`).
5. Handles: 0/1/2 → console for a kernel creator, else the parent's map.
   Create the main thread with `thread_prepare`
   plus `t->proc = process`, `t->user_entry = elf.entry`, `t->user_sp =
   initial rsp`, entry function `user_thread_start`. Enqueue.

`user_thread_start()` (kernel side of a user thread's first run): the
usual trampoline has released the run-queue lock and enabled
interrupts; it calls `arch_user_enter(entry, sp)` which never returns.

### Exit

`process_exit(status)` (from `sys_exit`, from a fatal fault, or from a
kill delivery point): set state EXITING and exit status under
`process.lock`, then `thread_exit()` on the calling thread. `thread_put`
of the last thread of a process (reaper context) calls
`process_last_thread_gone()` → reparent the children, state EXITED,
close the handle table and drop the working directory (so pipes deliver
EOF now, not at reaping), `complete(&exited)`, log `process N ('name')
exited with status S`; then either keep the table's reference as a
zombie for the parent's `wait` or, with no parent, `kobject_put`. The
process type's release destroys the address space (`vm_space_destroy`:
unmap all regions with frames freed and shootdown, then
`arch_mmu_context_destroy` frees the lower-half tables) and frees the
struct. The reaper never runs on the dead process's CR3 because every
switch activates the next thread's own space (kernel space for kernel
threads).

`process_wait_exit(process)` waits on `exited` and returns the status
(kernel-internal; used by `kernel_main` for `init` and by the tests).

### Fatal user faults

`vm_fault_handler` with a user-mode frame and no region or a permission
violation: log `process N ('name') fault: <kind> at <addr>`, then
`process_exit(128 + 11)` from the faulting thread's context (the fault
handler runs on that thread's kernel stack with interrupts disabled;
`process_exit` → `thread_exit` requires preemptible context, so the
handler enables interrupts first — a user-mode trap frame always has IF
set — and never returns to user mode).

## 3. Threads with a process

`struct thread` gains: `struct process *proc` (NULL for kernel
threads), `struct list_node proc_link`, `uintptr_t user_entry,
user_sp`, `uintptr_t kernel_stack_top`, `uintptr_t tls_base` (the
user `%fs` base; set by `arch_set_tls_base` from the Linux
`arch_prctl(ARCH_SET_FS)` and loaded into `MSR_FS_BASE` by
`arch_thread_switch_prepare` for every thread with a process).

`arch_context_switch` callers (`schedule_internal`) call
`arch_thread_switch_prepare(next)` before switching: it writes
`percpu->kernel_stack_top = next->stack_base + next->stack_size`, sets
TSS `rsp0` to the same, and activates `next->proc ? next->proc->space :
kernel_space` (skipped when the root is unchanged).

## 4. User-mode entry and exit (x86-64)

MSRs at `arch_syscall_init_cpu()` (every CPU):
- `EFER.SCE = 1`
- `STAR = (0x10 << 48) | (0x08 << 32)`: SYSCALL loads CS 0x08/SS 0x10;
  SYSRET loads CS 0x10+16 = 0x20, SS 0x10+8 = 0x18 (matches the GDT).
- `LSTAR = x86_syscall_entry`
- `SFMASK = IF | TF | DF | AC | NT`
- `KERNEL_GS_BASE = 0` (the user value); `GS_BASE` = percpu while in
  the kernel. SWAPGS on every transition.

`x86_syscall_entry` (`syscall_entry.S`):

```text
swapgs
mov %rsp, %gs:PERCPU_USER_RSP_SCRATCH
mov %gs:PERCPU_KERNEL_STACK_TOP, %rsp
push user ss (0x1b), user rsp, r11 (rflags), cs (0x23), rcx (rip)  -- iret-like frame
push rax(nr) and the six argument registers rdi rsi rdx r10 r8 r9, plus callee-saved for diagnostics
sti
mov %rsp, %rdi ; call syscall_dispatch (returns value in rax, stored into frame)
cli
pop registers ; rcx = rip, r11 = rflags ; rsp = user rsp
swapgs
sysretq
```

`isr.S` gains: on entry, if `frame.cs & 3` then `swapgs`; on exit the
same test before `iretq`. The double-fault IST path uses the same test.

`arch_user_enter(entry, sp)`: `cli`, set `%gs:kernel_stack_top` and TSS
rsp0 for the current thread (already done at switch), push an IRETQ
frame `ss=0x1b, rsp, rflags=0x202, cs=0x23, rip=entry`, zero every
general register, `swapgs`, `iretq`.

User-access windows: `arch_user_access_begin()` = `stac` when SMAP is
enabled, `arch_user_access_end()` = `clac`.

## 5. User address spaces (VMM additions)

```c
#define USER_LO       0x0000000000400000ULL
#define USER_HI       0x00007FFFFFFFF000ULL
#define USER_STACK_TOP 0x00007FFFFFFF0000ULL
#define USER_STACK_SIZE (8u << 20)
```

`vm_space_create_user(struct vm_space **out)`: `kzalloc`, lock, list,
`arch_mmu_context_init_user(&mmu)` which allocates a root below 4 GiB
and copies PML4 entries 256–511 from the kernel root. For this to be
complete for all time, `vmm_init` pre-creates every kernel-half PML4
entry it will ever use (direct-map range, arena range, image slot), and
`arch_mmu_map` on the kernel space asserts it never creates a new PML4
entry afterwards (invariant P9).

`vm_user_map_anon(space, base, size, prot, name, flags)`: ANON region
with `VM_REGION_USER`; page-table leaves get the U/S bit; populated or
lazy. `vm_user_unmap(space, base, size)`: exact-region unmap for `munmap`
(partial unmaps are `-EINVAL` in this phase). `vm_space_destroy(space)`:
teardown every region (frames + shootdown), destroy the MMU context
(free lower-half tables), free the struct.

Fault handling for user addresses: `space = process_current()->space`
when the current thread has a process (the fault came either from user
mode or from a `copy_*_user` in kernel mode); demand-zero for ANON
regions with matching access; otherwise a user-mode frame terminates
the process and a kernel-mode frame panics (validation should have
refused the pointer).

`arch_mmu_query` for user spaces reports `VM_PROT_USER` in the prot
bits so tests can verify U/S.

## 6. System calls

```c
struct syscall_args { uint64_t nr; uint64_t a[6]; struct syscall_frame *frame; };
typedef int64_t (*syscall_fn)(struct syscall_args *a);

struct personality {
    const char *name;
    const syscall_fn *table;
    unsigned count;
};
```

`syscall_dispatch(frame)` (called with interrupts enabled on the kernel
stack): builds `struct syscall_args`, picks `process_current()->pers`,
range-checks `nr`, calls the function, stores the result into the
frame's `rax`. Unknown numbers return `-ENOSYS` and are counted. Two
personalities exist: `personality_native` (below) and
`personality_linux` (`compat/linux/syscalls.c`, 512 entries,
`docs/compat/linux/`), chosen at creation by the CosmoOS ELF note.

Native table (`uapi/cosmo/syscall.h`, numbers stable from now on):

| nr | name | signature |
|---|---|---|
| 0 | `exit` | `(int status)` never returns |
| 1 | `write` | `(int h, const void *buf, size_t len)` → bytes or -errno |
| 2 | `read` | `(int h, void *buf, size_t len)` → bytes (0 for console) |
| 3 | `getpid` | `()` → pid |
| 4 | `yield` | `()` → 0 |
| 5 | `sleep_ns` | `(uint64_t ns)` → 0 |
| 6 | `clock_ns` | `()` → monotonic ns |
| 7 | `mmap` | `(void *hint, size_t len, int prot, int flags)` → addr or -errno; anonymous only |
| 8 | `munmap` | `(void *addr, size_t len)` → 0 or -errno (exact region) |
| 9 | `log` | `(const char *s, size_t len)` → 0; kernel log line prefixed with the pid |
| 10 | `close` | `(int h)` → 0 or -EBADF |

User copy helpers (`kernel/syscall/uaccess.c`): `user_range_ok(addr,
len)` (inside `[USER_LO, USER_HI)`, no overflow); `user_range_mapped
(space, addr, len, prot)` (every page inside a region with the required
prot); `copy_from_user`/`copy_to_user` do both checks, then copy inside
an access window. Demand-zero faults taken during the copy are handled
by the fault handler as kernel-mode faults on a user ANON region.

## 7. Boot archive and protocol v3

`cosmoboot_info` version 3 carries `archive_phys` and `archive_size`
(version 2 used the same words for one raw ELF, `module_phys` and
`module_size`); memory type `COSMOBOOT_MEM_ARCHIVE` (13). The loader
reads `\cosmo\boot.tar` (optional: absent → fields 0) into low memory
of that type. The kernel's PMM reserves it, `bootarchive_init` parses
it, and `kernel_main` after the boot modules and the self-tests looks up
the entry named `init` (`bootarchive_find`) and creates `init` from it
with `argv = {"init"}`,
waits for it to exit, logs the status, and shuts down (this phase has
nothing else to run). The self-test creates it with `argv = {"init",
"--selftest"}` and requires status 0, then with `--crash` and requires
status 139.

## 8. Userland

Since Phase 9 the userland is a C library and a set of programs
(`docs/libc/`, `docs/userland/`). `libc/src/crt0.S`: `_start` reads
`argc`/`argv`/`envp` from the initial stack per the ABI, aligns the
stack, calls `__libc_start`, which calls `main` and `exit`.
`userland/init/init.c`: runs `/etc/rc` and the console shell (`--selftest`
runs the system-call checks and prints `USERTEST: PASS`/`FAIL`, `--crash`
dereferences address 0, `--block` reads the console, `--spin` loops).
`libc/include/cosmo/syscall.h`: inline `cosmo_syscall0..6` wrappers around
the `SYSCALL` instruction using the `uapi` numbers. Programs are built
with the kernel's freestanding flags for `x86_64-unknown-none-elf`
without `-mcmodel=kernel`, linked at `0x400000` with `userland/user.ld`
(three W^X segments, the ELF and program headers inside the text
segment so `AT_PHDR` can be published, a `PT_NOTE` carrying the CosmoOS
ABI note from `crt0.S`, non-executable stack), and packed into the boot
archive as `init`, `bin/<name>`, `sbin/<name>` and `etc/<name>`
(`scripts/mkbootarchive.py`, see `docs/kernel/module/design.md`).

## 9. Failure modes

| Condition | Behaviour |
|---|---|
| archive missing on the boot volume, or no `init` entry in it | loader logs, kernel skips `init` with a warning; self-tests that need it report skipped |
| `spawn` of a missing, non-regular or non-executable file | `-ENOENT`, `-EACCES` |
| `spawn` with a bad handle map | `-EBADF` (parent handle free), `-EINVAL` (child slot out of range or duplicate) |
| `wait` with nothing to wait for | `-ECHILD`; `-EINTR` when killed while waiting |
| `kill` of an unknown pid, a foreign uid, or signal 0 | `-ESRCH`, `-EPERM`, `-EINVAL` |
| ELF rejected | `process_create_from_elf` returns `-ENOEXEC` with a log line naming the rule |
| out of memory during load | `-ENOMEM`, partial space destroyed |
| user fault without region | process terminated with status 139, logged |
| user W+X mmap | `-EINVAL` |
| syscall with kernel pointer | `-EFAULT` |
| syscall on closed handle | `-EBADF` |
| handle table full | `-EMFILE` |
| kernel fault on unvalidated user pointer | panic (kernel bug) |
| SYSCALL from a CPU without SCE | impossible: every CPU runs `arch_syscall_init_cpu` |

## 10. Phase 9: spawn, wait, kill, the working directory

### Process fields

```c
struct process {
    ...
    struct process *parent;            /* referenced; NULL for kernel-created processes and orphans of a dead init */
    struct list_node children;         /* struct process.sibling, under parent->lock */
    struct list_node sibling;
    struct waitqueue child_wq;         /* parents block here in wait() */
    bool reaped;                       /* status collected (or no one will) */
    int kill_sig;                      /* 0, or the signal that is terminating the process */
    struct vnode *cwd;                 /* referenced; dropped when the last thread is gone */
    char cwd_path[1024];               /* VFS_PATH_MAX: normalised absolute path of cwd */
};
```

`process_set_init(p)` records the process `kernel_main` started as init
(a referenced global); orphans go to it while it is RUNNING.

Lock order stays `process_table.lock → process.lock → handle_table.lock`;
a parent's lock is taken before a child's (`parent.lock → child.lock`)
and never the reverse, which the reparenting code respects by taking the
table lock first and then each child's lock alone.

### spawn

```text
sys_spawn(user struct cosmo_spawn *):
  copy the request; strncpy path (VFS_PATH_MAX); copy argv/envp pointer arrays and strings
    (COSMO_ARG_MAX 2048 string bytes and COSMO_ARG_ENTRIES 128 entries in all, else -E2BIG; the
    first-stack-page limit) into one kzalloc'd struct spawn_copy
  copy the handle map (≤ HANDLE_TABLE_SIZE entries); each parent handle must exist
    (rights copied as-is), child numbers must be distinct and < HANDLE_TABLE_SIZE
  open the path relative to cwd: regular file, mode & 0111 else -EACCES, size ≤ 16 MiB
  read it fully into a kernel arena buffer (vm_kalloc); file_put
  process_create_from_elf(image, size, basename, argv, envp, &attr, &p) with
    `struct process_spawn_attr { struct process *parent; const struct process_handle_map *handles;
    unsigned nr_handles; struct vnode *cwd; const char *cwd_path; }` (kernel creators pass NULL: console
    handles 0-2, root cwd, no parent); the child's credentials are the parent's
  free the buffer (vm_kernel_free); return pid (the creator's reference is dropped)
```

Handles are installed (from the parent's table with `handle_get`, same
rights) before the main thread is enqueued, so the child never runs with
a half-built table; an install failure fails the creation. `argv[0]` is
required (the program name). The child's `name` is the last path
component (truncated to `PROCESS_NAME_MAX`). `process_spawn` asserts a
current process: only system calls reach it.

### exit, zombies, wait, reparenting

`process_last_thread_gone` (reaper context) now does, under the table
lock: set EXITED; detach every child under `p->lock`, then for each
child alone under its own lock hand it to init (`process_set_init`'s
process, if RUNNING and not the exiting process; `parent_pid` becomes
init's pid) or make it kernel-owned (`parent = NULL`, and an exited
unreaped child is dropped at once); push the children onto init's list
under init's lock and wake init's `child_wq`. Then, outside the table
lock, close the handle table and drop `cwd` (handles close at exit, so
a pipe whose writer exited delivers EOF before the parent waits; the
zombie keeps only identity and status). Finally `complete(&p->exited)`
(for `process_wait_exit`, kernel callers), and either wake
`parent->child_wq` (zombie: the table reference stays until `wait`) or,
with no parent, drop the table reference (`reaped = true`).

```text
sys_wait(pid, user status*, flags):
  for (;;)
    lock parent; scan children for state == EXITED && !reaped (pid == -1: any; else that pid, which must be a child else -ECHILD)
    if found: reaped = true, unlink from children, unlock; copy status; process_put (the table ref); return its pid
    if no children at all (or the named pid is not a child): unlock; return -ECHILD
    unlock
    if flags & WNOHANG: return 0
    wait_event_killable(&cur->child_wq, a child is reapable)   -> -EINTR when killed
```

The wait status is the process's `exit_status`: `exit(n)` gives `n & 0xff`,
a fatal fault gives 139 (`COSMO_EXIT_FAULT`, as before), a kill gives
`128 + sig`. No encoding macros are needed; the shell prints it.

### kill

```text
sys_kill(pid, sig):
  1 ≤ sig ≤ 31 else -EINVAL; pid ≤ 0 → -EINVAL (no groups); target = process_lookup(pid) else -ESRCH
  permission: cred_may_signal(&cur->cred, &target->cred) else -EPERM
              (privileged, or the sender's real/effective uid equals the target's real/saved uid)
  process_kill(target, sig)
process_kill(p, sig):
  lock p; if state != RUNNING or kill_sig already set: unlock, return
  kill_sig = sig; exit_status = 128 + sig
  for each thread t of p: sched_wake(t)      (a BLOCKED thread becomes READY; anything else is untouched)
  unlock
```

Delivery: `process_check_kill()` (`if (cur->proc && cur->proc->kill_sig) process_exit(cur->proc->exit_status)`)
is called by `syscall_dispatch` before and after the handler, by the
arch trap dispatcher when it is about to return to ring 3 (any
interrupt, so a CPU-bound loop dies at the next timer tick), and by
`wait_event_killable`, which returns `-EINTR` instead of sleeping when
the flag is set. The kick is `sched_wake(t)` on the thread itself, not
on its wait queue: a BLOCKED thread becomes READY and its
`wait_event_killable` loop re-checks the condition and then the flag; a
thread that is running or ready is untouched and meets the flag at its
next boundary. `process_exit` keeps its single-thread shape (one thread
per process is still the rule); killing a process that is already
exiting, or twice, is a no-op.

`kill(pid, 0)` is not special: 0 is `-EINVAL` (no existence probe yet).

### wait_event_killable

```c
#define wait_event_killable(wq, cond) ({                                     \
    int __rc = 0; struct wait_entry __we; wait_entry_init(&__we);          \
    for (;;) {                                                             \
        waitqueue_prepare((wq), &__we);                                    \
        if (cond) break;                                                   \
        if (process_kill_pending()) { __rc = -EINTR; break; }              \
        sched_block_current();                                             \
    }                                                                      \
    waitqueue_finish((wq), &__we); __rc; })
```

The flag check sits after `prepare` (the thread is queued and BLOCKED,
`waiting_on` set) so a kill that lands between the check and the block
finds the thread on the queue and wakes it. Used by the tty, pipes,
`wait`, `thread_sleep_ns_killable` (behind `sys_sleep_ns`), and the
socket layer's blocking paths (`accept`, `connect`, `recv`, `send`) so
that a killed network client dies too.

### Working directory

`cwd` starts as the root (kernel-created processes) or the parent's
(`spawn`; the request may name another directory, resolved relative to
the parent's cwd). Every path system call passes `cur->cwd` as the
`start` vnode for relative paths (`vfs_*` already take a start vnode;
absolute paths ignore it). `chdir(path)`: resolve, must be a directory,
compute the new normalised path (`normalize_path(cwd_path, path)`:
split on `/`, drop `.` and empty components, pop on `..`, bounded by
`VFS_PATH_MAX`), swap the reference under `process.lock`. `getcwd`
copies `cwd_path`. The string is authoritative for display; the vnode
for resolution (so a renamed ancestor is not noticed by `getcwd`, as on
Unix systems that cache the path; recorded).

### dup

`dup(h, target)`: `target == -1` → lowest free slot; otherwise `target`
must be `< HANDLE_TABLE_SIZE`, an existing object there is closed first
(after the source has been looked up, so `dup(h, h)` returns `h`
unchanged). Rights are copied. `-EMFILE` when the table is full.

### Introspection

- `getppid`: `parent_pid` (0 for kernel-created processes and for
  orphans of a dead init; init's pid after reparenting).
- `procinfo(buf, count)`: fills up to `count` entries of
  `struct cosmo_procinfo { uint32_t pid, ppid, uid, gid, state, nr_threads; uint64_t syscalls, run_ns; char name[32]; }`
  in pid order under the table lock (copied out after unlocking), returns
  the total number of processes (zombies included, state 2) so the caller
  can size its buffer. `run_ns` sums the threads' `run_time_ns`.
- `klog(buf, len)`: the kernel log gains a 32 KiB ring of formatted
  lines (`kernel/core/log.c`, filled under the log lock at emit time, in
  all builds); the call copies the newest complete lines that fit in
  `len`, oldest first, and returns the byte count. Reading does not
  consume.
- `sysctl(name, buf, len)`: a static table of read-only values rendered
  as strings: `kernel.name`, `kernel.version`, `kernel.build`,
  `kernel.arch`, `kernel.uptime_ns`, `kernel.nprocs`, `hw.ncpu`,
  `vm.page_size`, `vm.pages_total`, `vm.pages_free`, and `sysctl.names`
  (the list, newline-separated). Returns the length of the value (not
  counting the NUL that is written when it fits); `-ENOENT` for an
  unknown name. Writable values arrive when there is policy to set.

### Credentials (Prompt #3 fix pass)

`struct credentials` (`kernel/cred.h`) is POSIX-shaped: `ruid, euid,
suid`, `rgid, egid, sgid`, `ngroups` and `groups[16]`. The security
boundary is one predicate, `cred_privileged(c)` (`euid == 0`), consulted
by every privileged operation: `mount`, `umount`, `klog`, binding a port
below 1024, `setgroups`, and any `setres*` change beyond shuffling ids
the caller already holds. Discretionary file access uses the effective
ids and the supplementary groups (`vfs_permission`). `kill` uses
`cred_may_signal`. Kernel threads and boot-time work run as
`cred_kernel` (root); `cred_current()` returns the current process's
credentials or that.

A child inherits its parent's credentials by copy at spawn; the kernel
creates init with all ids 0. A process changes only its own credentials,
under `process->lock` (it is single-threaded today; the lock is for the
readers on other CPUs once threads exist): `setresuid`/`setresgid`
follow POSIX (`-1` keeps an id; an unprivileged caller may set each id
only to one of its current real, effective or saved ids; all or
nothing, `-EPERM`), `setgroups` is privileged, `getres*`/`getgroups`
read. Native system calls 50-55 (`SYS_setresuid` ... `SYS_getgroups`);
the Linux personality maps `setuid`, `setgid`, `setreuid`, `setregid`,
`setresuid`, `setresgid`, `getresuid`, `getresgid`, `getgroups`,
`setgroups` onto them and its `get*id` calls return the matching id.
The rules are pure functions in `kernel/process/cred.c`, tested on the
host (`tests/host/test_cred.c`); the boundary is tested by
`init --unpriv-test` (`docs/kernel-services/vfs/invariants.md` V14).

### The return-to-user hook

`x86_trap_dispatch` ends by checking the saved CS
(`arch_trap_frame_is_user`) with `irq_depth` and `preempt_count` at 0:
when returning to ring 3 and the current process has `kill_sig` set,
`process_return_to_user` enables interrupts (the trap tail runs with
them off) and calls `process_exit`, which never returns; the thread's kernel stack is the trap stack, which
is the same stack a system call uses, so nothing special is needed. The
hook is `process_return_to_user()` in generic code; the arch code only
decides "this frame returns to user mode".

### Error codes added

`EINTR 4`, `ESRCH 3`, `ECHILD 10`, `EACCES 13`, `E2BIG 7`, `ENOEXEC 8`
(existed in the kernel, now in the UAPI), `ENOTTY 25`, `ESPIPE 29`
(UAPI), `ERANGE 34` (UAPI).
