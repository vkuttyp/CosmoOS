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
the rights; `handle_close` drops the table's reference; `handle_table_
destroy` closes everything. Handles 0, 1, 2 of a new process are the
console object with read, write, write rights.

The console object (`kernel/object/console_obj.c`) is a `kobject` whose
type carries `read`/`write` function pointers used by `sys_read`/
`sys_write`; `write` goes to `console_write`, `read` returns 0 (no input
path yet). This is the seed of `struct file`; the VFS replaces it.

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
    struct credentials cred;           /* uid/gid placeholders */
    const struct personality *pers;    /* &personality_native */
    enum process_state state;
    int exit_status;
    struct completion exited;
    spinlock_t lock;
    struct list_node all_link;         /* process table */
    pid_t parent_pid;
};
```

Process table: global list + spinlock + next pid (1 is the first
process created, which is `init`).

### Creation from an ELF image

`process_create_from_elf(image, size, name, argv[], envp[])`:

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
   populated except the top page, which receives the initial frame:

   ```text
   USER_STACK_TOP-ish (16-byte aligned at the final rsp):
     argc
     argv[0..argc-1], NULL
     envp[0..], NULL
     auxv: AT_PAGESZ, AT_ENTRY, AT_NULL
     strings (argv, envp bytes)
   ```
5. Handles 0/1/2 → console. Create the main thread with `thread_prepare`
   plus `t->proc = process`, `t->user_entry = elf.entry`, `t->user_sp =
   initial rsp`, entry function `user_thread_start`. Enqueue.

`user_thread_start()` (kernel side of a user thread's first run): the
usual trampoline has released the run-queue lock and enabled
interrupts; it calls `arch_user_enter(entry, sp)` which never returns.

### Exit

`process_exit(status)` (from `sys_exit` or from a fatal fault): set
state EXITING and exit status under `process.lock`, then `thread_exit()`
on the calling thread. `thread_put` of the last thread of a process
(reaper context) calls `process_release_last_thread()` → state EXITED,
`complete(&exited)`, log `process N ('name') exited with status S`,
then `kobject_put(&process->obj)`. The process type's release
destroys the handle table and the address space (`vm_space_destroy`:
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
user_sp`, `uintptr_t kernel_stack_top`, `uintptr_t tls_base` (unused
yet).

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
frame's `rax`. Unknown numbers return `-ENOSYS` and are counted.

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

`userland/init/crt0.S`: `_start` reads `argc`/`argv` from the initial
stack per the ABI, aligns the stack, calls `main`, then `exit`.
`userland/init/init.c`: banner via `write(1)`, `--selftest` mode runs
the syscall checks and prints `USERTEST: PASS`/`FAIL`, `--crash` mode
dereferences address 0. `libc/include/cosmo/syscall.h`: inline
`syscall0..6` wrappers around the `SYSCALL` instruction using the
`uapi` numbers. Built with the kernel's freestanding flags for
`x86_64-unknown-none-elf`, linked at `0x400000` with a user linker
script (three W^X segments, non-executable stack), and packed into the
boot archive as the entry `init` (`scripts/mkbootarchive.py`, see
`docs/kernel/module/design.md`).

## 9. Failure modes

| Condition | Behaviour |
|---|---|
| archive missing on the boot volume, or no `init` entry in it | loader logs, kernel skips `init` with a warning; self-tests that need it report skipped |
| ELF rejected | `process_create_from_elf` returns `-ENOEXEC` with a log line naming the rule |
| out of memory during load | `-ENOMEM`, partial space destroyed |
| user fault without region | process terminated with status 139, logged |
| user W+X mmap | `-EINVAL` |
| syscall with kernel pointer | `-EFAULT` |
| syscall on closed handle | `-EBADF` |
| handle table full | `-EMFILE` |
| kernel fault on unvalidated user pointer | panic (kernel bug) |
| SYSCALL from a CPU without SCE | impossible: every CPU runs `arch_syscall_init_cpu` |
