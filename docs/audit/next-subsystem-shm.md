# NEXT SUBSYSTEM — the Linux personality has no System V shared memory

> Constitution §68 report. This PR adds the report and the probe
> (`tools/shm-probe.py`); the `LX_shmget`/`shmat`/`shmdt`/`shmctl` syscall
> numbers, the `kernel/ipc/shm.c` segment registry, the four Linux doors and
> the `lxtest` checks described under "Design" and listed in "Affected files"
> are planned work that lands in the implementation PR that follows, gated on
> CI. As the report was committed, the four calls were unlisted in
> `linux_table`, so the dispatcher's `lx_unknown` returned `-ENOSYS`.

## Problem

System V shared memory — `shmget(2)`, `shmat(2)`, `shmdt(2)`, `shmctl(2)`
(x86-64 29 / 30 / 67 / 31, AArch64 194 / 196 / 197 / 195) — has no syscall
number defined in `compat/linux/nr_*.h` and no `linux_table` entry, so every
one of the four reaches the dispatcher's `lx_unknown` and returns `-ENOSYS`. A
Linux program that shares memory through the SysV API — the classic
`shmget`/`shmat` pattern, and the one glibc's `/dev/shm`-free fallback and many
older servers, databases and `ipcs`-aware tools use — cannot run.

The native model has no SysV IPC at all: `kernel/ipc/` holds `fifo`, `futex`,
`pipe` and `unix`, and `kernel/include/uapi/cosmo/syscall.h` defines no `shm*`
native call. The Linux personality is the only door SysV shm would arrive at.

The mechanism it needs already exists and is proven. A SysV segment is exactly
an anonymous, memory-backed, page-cache file that two processes map
`MAP_SHARED` — which is what the **memfd** unit shipped: `ramfs_anon_reg`
creates an anonymous ramfs regular file, `vfs_ftruncate` gives it a size, and
`vm_user_map_file`/`vm_user_map_file_free` map its page cache shared. The one
piece SysV adds over memfd is a **key→segment namespace** (an integer `shmid`
instead of a file descriptor) and the SysV lifecycle (`IPC_RMID` marks a
segment, which is freed only when the last attach detaches). That lifecycle is
*also* already modelled: the memfd anon file is "born unlinked, freed on the
last reference", which is precisely `IPC_RMID` semantics.

## Probe

`tools/shm-probe.py` adds the four `LX_shm*` numbers to `compat/linux/nr_x86_64.h`
and `compat/linux/nr_aarch64.h` (so a raw call compiles on both arches) and one
check to `tests/linux/lxtest.c` (which runs in the standard boot):

```
LXSHM: shmget unimplemented -> -ENOSYS; no System V shared memory
```

The check calls `shmget(IPC_PRIVATE, 4096, IPC_CREAT | 0600)` and asserts it
returned `-ENOSYS` at the report commit, before the implementation. The marker
prints only when the syscall really returned `-ENOSYS`, so grepping it cannot
show a false result after a failed check.

## Why it matters

- **A whole IPC family is missing.** `shmget`/`shmat` is the oldest and still
  common way C programs share memory; its absence is a hard `-ENOSYS`, not a
  degraded path. `ipcs`/`ipcrm` tooling and programs that probe for SysV IPC at
  startup fail outright.
- **It is in pattern and the mechanism exists.** A segment is a `MAP_SHARED`
  anonymous page-cache file (the memfd backing) plus a keyed namespace; the
  `IPC_RMID` lifecycle is the anon file's free-on-last-reference. The
  implementation adds a small registry and the four doors, not a new memory or
  VM mechanism.

## The implementation before this unit

| piece | where | what it does |
|---|---|---|
| anonymous backing | `ramfs_anon_reg(mode, &vn)` (`vfs.h`) | an unnamed ramfs regular file, born unlinked (frees on the last reference) — the memfd backing |
| sizing | `vfs_ftruncate(vn, size)` (`vfs.h`) | sets the file length, as `ftruncate` |
| shared map | `vm_user_map_file_free(space, from, size, prot, maxprot, VM_MAP_SHARED, vn, off, name, &base)` (`vmm.h`) | maps a vnode's page cache shared, placing it in a free gap; `vm_user_map_file` maps at an exact base |
| detach | `vm_user_unmap(space, base, size, flags)` (`vmm.h`) | unmaps a range, dropping the mapping's reference to the vnode |
| open | `vfs_open_vnode(vn, mode, &f)` (`vfs.h`) | wraps a vnode in a `struct file`, as memfd does |
| per-process state | `struct linux_state` (`compat/linux/syscalls.c`), reached via `lx()` | the place to record a process's attaches |
| dispatch | `linux_table[LX_NR_MAX]`; unlisted → `lx_unknown` | the `shm*` calls are unlisted → `-ENOSYS` |

## Design

### 1. A global segment registry (`kernel/ipc/shm.c`)

SysV keys are system-wide, so the registry is global (one lock), not
per-process. Each live segment records:

```c
struct shm_segment {
    int           id;        /* the shmid handed back by shmget */
    int32_t       key;       /* IPC_PRIVATE segments carry key 0 and are keyless */
    struct vnode *vn;        /* the anonymous ramfs file (the segment's backing) */
    size_t        size;      /* page-rounded */
    uint32_t      mode;      /* low 9 bits of shmflg */
    uint32_t      cuid, cgid;/* creator */
    uint32_t      nattch;    /* attaches, for IPC_STAT */
    bool          removed;   /* IPC_RMID seen: no new attach, drop on last detach */
    uint32_t      refs;      /* registry (while !removed) + one per live attach */
};
```

The **segment record itself is reference-counted**: the registry's lookup
tables hold one reference while the segment is not `removed`, and every live
attach holds one. A reference also pins the backing `vn`. This matters for the
lifecycle (§3): `IPC_RMID` can drop the registry's reference and unlink the
segment from the lookup tables, yet a `shmdt` from a process still attached must
find the record to decrement `nattch` and release its reference — so the record
is freed on the *last* reference, not at removal, and an attach reaches it
through a direct pointer (not a re-lookup by `id`). Lookups by `id`
(`shmat`/`shmctl`) and `key` (`shmget`) see only segments still in the tables.

### 2. The four Linux doors

- **`shmget(key, size, shmflg)`** — `IPC_PRIVATE` always makes a new keyless
  segment; a real `key` looks one up, creating it under `IPC_CREAT` (and
  `-EEXIST` under `IPC_CREAT | IPC_EXCL` if it already exists), else `-ENOENT`.
  A new segment rounds `size` up to pages (bounded against the user window),
  `ramfs_anon_reg(mode & 0777, &vn)` + `vfs_ftruncate(vn, size)`, and is
  inserted into the `id`/`key` tables holding the registry reference. Returns
  the `shmid`.
- **`shmat(shmid, shmaddr, shmflg)`** — looks up the segment and takes a
  reference on its record. Maps its vnode `MAP_SHARED`, read-only under
  `SHM_RDONLY` and read/write otherwise: when `shmaddr` is `NULL` the kernel
  chooses a gap (`vm_user_map_file_free`); when it is non-`NULL` it must be
  page-aligned and the mapping is placed **at that exact address**
  (`vm_user_map_file`), returning `-ENOMEM`/`-EINVAL` if that address is not
  free rather than silently relocating (`SHM_RND` rounding is deferred). Records
  the attach `{addr, size, struct shm_segment *seg}` in `linux_state`, bumps
  `nattch`, and returns the address.
- **`shmdt(shmaddr)`** — finds the per-process attach at `shmaddr` (`-EINVAL` if
  none), `vm_user_unmap`s its range, decrements the segment's `nattch` through
  the record pointer the attach holds, drops the attach's reference on the
  record (freeing it if it was the last and the segment was `removed`), and
  drops the attach record.
- **`shmctl(shmid, cmd, buf)`** — `IPC_STAT` fills a `struct shmid_ds` (the
  `ipc_perm`, `shm_segsz`, `shm_nattch`, creator ids, mode); `IPC_RMID` unlinks
  the segment from the `id`/`key` tables, marks it `removed`, and drops the
  registry's reference (the record and its backing survive while live attaches
  hold references). After removal, `shmat`/`IPC_STAT` of that `shmid` are
  `-EINVAL` — the id is gone — while existing attaches keep working until they
  detach. `IPC_SET`, `SHM_LOCK`/`UNLOCK` and `SHM_INFO` are deferred `-EINVAL`.

### 3. The `IPC_RMID` lifecycle rides on the anon file's refcount

This is the crux, and it is why no new memory lifecycle is needed. The segment
backing is the memfd anon file: born unlinked, its pages freed when its last
reference drops. The segment *record* (§1) is reference-counted in lockstep —
each reference on the record also pins the `vn`:

- the **registry reference** held while the segment is not removed, and
- an **attach reference** each live `shmat` holds (alongside the VM file
  region's own reference on `vn`).

`IPC_RMID` unlinks the segment from the `id`/`key` tables and drops the registry
reference; the record and its pages are freed only when the last attach
`shmdt`s (or the process exits and its address space is torn down, which unmaps
the file regions and, via `linux_process_release` walking the leftover attach
records, drops their references). Because an attach holds a direct pointer to
the record, a detach after `IPC_RMID` still reaches it to decrement `nattch` and
release its reference. That is exactly SysV's "marked for destruction,
destroyed on the last detach" — obtained from reference counting, with no
deferred-free bookkeeping of our own.

### 4. Per-process attaches and exit

`shmat` records each attach `{addr, size, seg}` in `linux_state`, where `seg`
is the reference-counted segment record (not a bare `shmid` to be looked up
again — the id may be gone after `IPC_RMID`). `shmdt` consumes one by address,
reaching its segment through `seg`. `linux_process_release` walks any left at
exit, decrements each segment's `nattch` and drops its reference (the mappings
themselves are freed by the address-space teardown, so the pages go without a
detach call). `nattch` is thus accurate for `IPC_STAT` across normal detach and
exit; it is advisory only — the memory lifecycle in §3 depends on references,
not on this counter.

## Affected files

| file | change |
|---|---|
| `compat/linux/nr_x86_64.h`, `compat/linux/nr_aarch64.h` | `LX_shmget`/`shmat`/`shmdt`/`shmctl` numbers |
| `compat/linux/linux_abi.h` | `IPC_PRIVATE`/`IPC_CREAT`/`IPC_EXCL`/`IPC_RMID`/`IPC_STAT`/`SHM_RDONLY`, `struct lx_shmid_ds`, `struct lx_ipc_perm` |
| `kernel/ipc/shm.c`, `kernel/include/kernel/shm.h` | the segment registry and its operations |
| `compat/linux/syscalls.c` | `lx_shmget`/`shmat`/`shmdt`/`shmctl` + the four table entries |
| `tests/linux/lxtest.c` | the shm checks |
| `README.md` | Status entry |

## APIs

Planned for the implementation. The four Linux `shm*(2)` calls, over a new
native segment registry (`kernel/ipc/shm.c`) that composes the existing
`ramfs_anon_reg` / `vfs_ftruncate` / `vm_user_map_file` machinery; no on-disk
or native user ABI change beyond the registry's own kernel-internal interface.

## Tests

Planned for the implementation (`tests/linux/lxtest.c`, which runs in the
standard boot).

| test | proves |
|---|---|
| create + attach + share | `shmget(IPC_PRIVATE, 8192, IPC_CREAT\|0600)` ≥ 0; `shmat` returns an address; a write through it is visible through a **second** `shmat` of the same `shmid` (one segment, two mappings, shared) |
| attach at a fixed address | `shmat(id, addr, 0)` with a free page-aligned `addr` returns exactly `addr` (not a relocated one), and the segment is readable there; `shmat` at an occupied `addr` is `-ENOMEM` (no silent relocation) |
| shmdt | `shmdt` of an attached address returns 0 and a later `mprotect` of it is `-ENOMEM` (really unmapped); `shmdt` of an unattached address is `-EINVAL` |
| IPC_STAT | `shmctl(id, IPC_STAT, &ds)` reports `shm_segsz == 8192` and `shm_nattch` equal to the live attach count (e.g. 1, then 2 after a second `shmat`, then back down after `shmdt`) |
| IPC_RMID lifecycle | with one attach still live, `IPC_RMID` returns 0, the live mapping **still reads its sentinel** (not freed at removal), and `shmat`/`shmctl(IPC_STAT)` of that `shmid` are now `-EINVAL` (the id is gone, no new attach); after the last `shmdt` the `shmid` stays `-EINVAL`. This distinguishes freed-on-last-detach from freed-at-removal (the live read would fault) and from never-removed (`IPC_STAT` would still succeed) — a page-reuse check could not. |
| errors | `shmat` of a bad `shmid` is `-EINVAL`; `shmget` with `IPC_CREAT\|IPC_EXCL` of an existing key is `-EEXIST`; an unaligned `shmaddr` without `SHM_RND` is `-EINVAL` |

**Planned mutations** (each alone, boot confirmed):
- map the segment `MAP_PRIVATE` instead of `MAP_SHARED`: the two-mapping
  share test fails (the second attach does not see the first's write).
- `shmat` always choosing a free gap (`vm_user_map_file_free`) even for a
  non-`NULL` `shmaddr`: the fixed-address test fails (the returned address is
  not the requested one).
- `shmdt` returning 0 without unmapping: the shmdt test's `mprotect` `-ENOMEM`
  check fails.
- `IPC_RMID` freeing the record / unmapping live attaches instead of only
  dropping the registry reference: the `IPC_RMID`-with-live-attach read faults
  (freed at removal, not on the last detach).

## Benchmarks

None.

## Risks

- **Bounded SysV surface.** v1 implements `shmget` (private and keyed,
  `IPC_CREAT`/`IPC_EXCL`), `shmat` (`SHM_RDONLY`, `NULL` or aligned address),
  `shmdt`, and `shmctl` `IPC_STAT`/`IPC_RMID`. Deferred: `SHM_RND` address
  rounding, `SHM_REMAP`, `SHM_HUGETLB`, segment resize, `IPC_SET`,
  `SHM_LOCK`/`UNLOCK`, `SHM_INFO`/`IPC_INFO`, and fine-grained permission
  checks beyond the creator mode. A deferred flag is `-EINVAL`, never a wrong
  result.
- **`nattch` accuracy vs. the free lifecycle.** The segment is freed by
  reference counting (§3), not by the `nattch` counter, so even if the counter
  were ever wrong the memory lifecycle is still correct. The counter is
  maintained at `shmat`/`shmdt` and at exit for `IPC_STAT` reporting only.
- **Global namespace, no per-container isolation.** SysV IPC is system-wide;
  there are no IPC namespaces yet (none in the kernel), so all processes share
  one key space. This matches a single-namespace Linux and is noted, not a
  defect for v1.
- **Key permissions.** v1 stores the creator uid/gid and mode and enforces the
  creator mode on attach; a full `ipc_perm` ownership/permission check (and
  `IPC_SET` to change it) is deferred.

## Alternatives considered

- **POSIX `shm_open` instead of SysV.** POSIX shared memory is already
  reachable: `memfd_create` makes an anonymous page-cache file and `mmap`
  `MAP_SHARED` shares it, and a descriptor passes between processes over an
  `AF_UNIX` socket with `SCM_RIGHTS` (both built). The genuinely missing API is
  the *System V* one — integer keys, not descriptors — so this unit builds that.
- **`setsockopt`/`getsockopt` expansion.** The socket stub has almost no
  per-socket backing to honour: `SO_TYPE` and `SO_ERROR` are reportable, but
  `TCP_NODELAY`, `SO_REUSEADDR`, `SO_RCVBUF`/`SO_SNDBUF` and `SO_LINGER` have no
  settable field on the socket or TCP PCB today, so an honest unit would mostly
  still answer `-ENOPROTOOPT`. Low payoff next to a whole working IPC family.
- **`rseq` (restartable sequences).** No support exists anywhere; it needs a
  new per-thread registration ABI and critical-section abort logic in the
  return-to-user path — a greenfield subsystem, not a composition of existing
  mechanism. Deferred.
