# CosmoOS Post-Roadmap Architecture Audit

Date: 2026-09-05. Tree: `main` at merge commit b8b61e6 (PR #14, Phase 13). Governing input: `prompts/CosmoOS Prompt #2 — Post-Roadmap Architecture Audit & Next-Generation Engineering.md`.

This is a read-only audit. Nothing in the tree was changed; no builds or QEMU runs were needed for the findings below. Every finding cites `file:function:lines` in the tree as of the commit above. Severity scale: CRITICAL = exploitable or kernel-wide corruption reachable today; HIGH = reachable defect with bounded blast radius, or a latent CRITICAL that a planned feature will trigger; MEDIUM = correctness or robustness defect needing unusual conditions; LOW = hygiene, documentation drift, or minor inconsistency. Where a claim could not be confirmed in source it is marked "plausible".

Terminology used deliberately: "crash consistency" (not "corruption immunity"), "detected / undetected corruption", "grace period", "quiescent state".

---

## 1. Executive Summary

CosmoOS has completed the thirteen roadmap phases as designed: a C11 higher-half hybrid kernel with a clean `kernel/include/arch/*.h` seam, two working architectures (x86-64 and AArch64 stage 1), signed loadable modules, a copy-on-write filesystem with two-slot superblock commit, an mbuf network stack with RFC 6298 TCP, an AMD-V hypervisor exposed through kobjects, a Linux x86-64 personality that runs static musl binaries, and a boot harness plus 70 kernel self-tests and 10 ASan/UBSan host tests running on both architectures in CI. Documentation is unusually complete (133 files, 27 `api.md`, 24 of them with zero stale function references).

The audit found the design sound and the implementation careful at the unit level, but with a consistent gap at the *boundaries between units*: object lifetimes across interrupt handlers, timers, workers and syscalls; state that lives in device-shared memory; and arch entry paths that assume conditions the hardware does not guarantee. Concretely:

- **7 CRITICAL findings.** (1) NMI and #MC have no IST and the SYSCALL entry/exit windows can run an NMI on the user stack or with user GS at CPL0 (`idt.c:50-63`, `syscall_entry.S:25-28,80-83`). (2) The LAPIC ICR write pair is not atomic against local interrupts, so `smp_call_function_single` can deliver a cross-call to the wrong CPU and then panic (`lapic.c:148-154`). (3) No FPU/SIMD state is saved anywhere; a VM guest inherits and clobbers the owner thread's XMM state, and two SSE-using Linux processes corrupt each other; SSE enablement is also inconsistent across CPUs (`context.c:40-55`, `svm_run.S:44-80`, `trampoline.S:55-57`). (4) The TCP MSS helper takes a sleeping mutex while holding the global TCP spinlock with IRQs off, remotely triggerable on every SYN (`tcp.c:259-264` → `netif.c:194-226`). (5) The module-signing private key is tracked in git and is the default signing key (`tools/keys/cosmo-dev.key`, `module.mk:17`). (6) No permission is enforced on any file operation and every process is uid 0 (`vfs.c:568-651`, `process.c:252,273`). (7) `virtq_add` indexes the descriptor table with a `next` value it reads back from device-writable memory, a device-triggered out-of-bounds kernel write (`virtqueue.c:126-137`).
- **~35 HIGH findings**, clustering into four root causes: (a) refcounts without a release path and no grace-period primitive (device, blkdev, netif, tcp_pcb, interrupt slots, timers, module text); (b) region-granular user VM operations and a single-thread assumption in uaccess and handle composites; (c) filesystem semantics that hold only on a lightly-used disk (no fsync durability, no metadata reserve, 264-extent cap, sparse-write exhaustion); (d) missing baseline network hardening (SYN flood, blind RST, kernel-stack leak in ICMP quoting, UAFs on close-vs-RX).
- **Scalability ceiling is 64 CPUs** by type (`cpumask_t`), and practically ~16 by contention: global shootdown and cross-call slots, one TCP lock, one RX worker, one cosmofs lock, all device IRQs on CPU 0, AArch64 full-TLB flush on every user switch.
- **Verification depth** is the weakest pillar relative to the constitution's "correctness first": zero fuzzing, zero fault injection, no crash-consistency harness, no concurrency tests, no benchmarks, statistics structures with no transport to userland, unsymbolized panics.

**Recommended next subsystem (section 20): Kernel Object Lifetime and Quiescence Hardening.** It is the root cause behind the largest share of HIGH findings across five subsystems, it is prerequisite to safe hot-unplug, module unload, multithreading, async I/O and NVMe, and it matches the constitution's priority order (correctness, cleanliness, observability, security). A short critical-fix pass (section 19, milestone 1) precedes it.

---

## 2. Current Architecture

### 2.1 Repository forensics

| Directory | Files | Lines | Content |
|---|---|---|---|
| `kernel/` | 245 | 27,890 | arch/x86_64 4,092; arch/aarch64 2,719; include/kernel 4,175; memory 2,430; core 2,118; scheduler 1,926; process 1,488; module 1,324; syscall 1,150; interrupt, timer, object, ipc, tty, device, block, security |
| `kernel-services/` | 39 | 10,451 | network 4,605; filesystem/cosmofs 2,179; vfs 2,004; virtualization 1,517; storage 52 |
| `drivers/` | 17 | 2,899 | virtio 1,565; pci 584; acpi 368; include 335; `network/`, `nvme/`, `storage/` are empty directories |
| `compat/` | 6 | 1,921 | linux only (x86-64 table) |
| `libc/` | 44 | 2,895 | native C library, `-mgeneral-regs-only` |
| `userland/` | 34 | 2,400 | init, sh, 12 coreutils, 7 system tools |
| `pkg/` | 9 | 2,211 | package format, manager, host builder |
| `boot/` | 17 | 2,167 | UEFI loader, protocol v4, x86_64 + aarch64 |
| `tests/` | 37 | 3,719 | boot harness (3 py), 10 host tests, 3 module fixtures, 3 linux, 6 hv guests |
| `docs/` | 133 | 23,220 | per-subsystem architecture/design/api/invariants/testing |
| `scripts/`, `build/`, `tools/` | 15/7/5 | 919/293/316 | |
| Git | 629 tracked files, 61 commits, 14 PRs, no tags | | |

### 2.2 Layering as built

```
 userland (init, sh, coreutils, vmctl, pkg)      Linux static binaries (musl)
 libc (native ABI, SYS 0..49)                    compat/linux (x86-64 table, 99 entries)
 ───────────────────────── syscall_dispatch(pers->table[nr]) ─────────────────────────
 kernel-services: vfs+pagecache+ramfs | cosmofs | storage pool | network | virtualization
 kernel: process/elf/spawn | object/handle | ipc (pipe, futex) | tty | module+security
         scheduler (per-CPU RR) | timer | interrupt/irq/ipi | memory (bootmem/pmm/buddy/slab/vmm) | device/dma/block
 ───────────────────────── kernel/include/arch/*.h (cpu, mmu, trap, irq, irqc, smp, timer, user, module, pci, hv, ...) ─────
 arch/x86_64 (SYSCALL, LAPIC/IOAPIC, SVM+NPT)     arch/aarch64 (SVC, GICv2/GICv2m, PSCI, EL1/EL0)
 drivers (signed .ko): virtio, virtio_blk, virtio_net, virtio_rng, virtio_console; built-in: acpi, pci, serial/pl011
 boot/uefi (protocol v4) — QEMU q35 / virt
```

Cross-cutting facts that the rest of this report depends on:

- **Every process has exactly one thread** (`kernel/process/process.c:420-422`; `clone`/`fork`/`clone3` are `lx_nosys`, `compat/linux/syscalls.c:1279,1338`). Many correct-today designs are correct only for this reason and are flagged as latent.
- **Threads never migrate between CPUs** (`sched.c:pick_cpu:141-155` runs once at creation; `sched_wake:299-315` re-enqueues on `t->cpu`). Timers, `thread_sleep_ns`, `futex_wait` and run-time accounting silently depend on it.
- **All locks are spinlocks except six sleeping mutexes** (`g_mounts_lock`, `g_device_mutex`, `g_blk_lock`, `g_netif_lock`, module `g_lock`, hv `g_lock`) plus per-object mutexes in VFS, cosmofs, sockets and hv. There is no lock-order checker and no grace-period primitive of any kind.
- **The kernel, loader, modules and native userland are all compiled `-mgeneral-regs-only`** (`build/arch/x86_64.mk:10,18`, `build/arch/aarch64.mk:14,22`, `libc/libc.mk:11`). Only Linux-personality binaries and VM guests can execute FPU/SIMD instructions.

### 2.3 Concurrency model summary

| Context | May sleep | Locks it takes | Notes |
|---|---|---|---|
| Syscall (thread) | yes | any | 1 KiB stack bounce for read/write (`native.c:36,100-101`) |
| Page fault (kernel or user) | no | `vm_space.lock` → `kmem_cache.lock` → `zone.lock` | panics if space lock already held (`vmm.c:518-519`) |
| Timer callback (tick ISR) | no | `runqueue.lock`, `waitqueue.lock`, `g_work_lock` | runs on the arming CPU only (`timer.c:100`) |
| Device IRQ (MSI-X) | no | `virtqueue.lock`, `mbufq.lock`, slab locks (alloc in IRQ) | all routed to CPU 0 (`virtio_pci.c:177,339`) |
| netrx worker (one thread) | yes | `tcp.c g_lock`, `udp.c g_lock`, ARP/ND, then `g_netif_lock` (mutex, see 9) | single 512-entry queue for all NICs |
| Reaper thread | yes | `g_reap_lock`, `kernel_space.lock` | frees stacks (needs shootdown) |
| vCPU run loop | yes (between entries) | `run_lock` → `vm->lock` | host tick exits guest; kill checked per iteration |

---

## 3. Implemented Subsystems

### 3.1 Implementation matrix

| Subsystem | Status | Implementation | Tests | Limitations |
|---|---|---|---|---|
| Boot, UEFI x86_64 | Implemented | `boot/uefi/{main,elf,memory,console}.c`, `arch/x86_64/` | harness banner, `test-crash` | No image verification |
| Boot, UEFI aarch64 | Implemented | `boot/uefi/arch/aarch64/{cpu,paging,serial}.c` | CI aarch64 | `SCTLR.WXN` cleared; EL1 only |
| bootmem / PMM / buddy | Implemented | `kernel/memory/{bootmem,pmm,buddy}.c` | `pmm`, host `test_buddy` | one node; racy `pmm_page_put`; 32 B/frame flat array |
| VMM kernel arena | Implemented | `kernel/memory/vmm.c`, arch `mmu.c` | `vmm` | linear region list; tables never freed |
| VMM user spaces | Implemented | `vmm.c:572-818` | none direct (via process tests) | exact-region unmap/protect only; PROT_NONE = READ; no ASLR |
| slab / kmalloc | Implemented | `slab.c`, `kmalloc.c` | `kmalloc`, host `test_slab` | per-cache global lock, no per-CPU magazines |
| DMA API | Partial | `kernel/device/dma.c` | `dma` | bus == phys; unmap no-op; 32-bit default mask never widened |
| Interrupts / IRQ / IPI | Implemented | `kernel/interrupt/*`, `x86/{idt,ioapic,lapic}`, `aarch64/gic.c` | `irq-route`, `smp-call`, `smp-shootdown` | no IST for NMI/#MC; no unregister grace period; single call slot |
| Timers | Implemented | `kernel/timer/timer.c`, arch timers | `timer`, `smp-ticks` | 250 Hz periodic; no `timer_cancel_sync`; TSC per-CPU unsynchronised |
| SMP | Implemented | `kernel/core/smp.c`, `arch/*/smp.c`, trampolines | 8 `smp-*` | `CONFIG_MAX_CPUS=64`, xAPIC only, no hotplug |
| Scheduler | Implemented | `kernel/scheduler/*` | 12 sched tests | no migration/balancing, no PI, no syscall-return preemption |
| Processes / ELF / spawn | Implemented | `kernel/process/{process,spawn,elf}.c` | `process-*`, `elf`, USERTEST | one thread per process; unbounded `p_memsz`; all uid 0 |
| Kobjects / handles | Implemented | `kernel/object/*` | `objects` | 64 slots; rights READ/WRITE only; io-type assumed by cast |
| Native syscalls | Implemented | `kernel/syscall/{syscall,native,uaccess}.c`, 50 numbers | USERTEST, shelltest | check-then-copy uaccess; `read` ≤ 1 KiB/call |
| IPC | Partial | `ipc/pipe.c`, `ipc/futex.c` | `ipc-pipe`, lxtest futex | WAIT/WAKE only; copy under spinlock; no channels |
| tty | Implemented | `kernel/tty/tty.c` | `tty-ldisc` | no ^C, no sessions |
| VFS / page cache / ramfs | Implemented | `kernel-services/vfs/*` | `vfs-ramfs`, `pagecache` | no permission checks; no symlinks; no reclaim; rename lock order |
| cosmofs | Implemented | `kernel-services/filesystem/cosmofs/*` | `cosmofs-*`, host `test_cosmofs` | commit only at sync/umount; 264 extents; no data checksums |
| Storage pool | Stub | `kernel-services/storage/pool.c` (52 lines) | `pool` | one member; no DVA, no TXG |
| Block layer | Implemented | `kernel/block/blk.c` | `blk` | single-buffer bio; no FUA; no timeout; `-EAGAIN` surfaces |
| virtio blk/net/rng/console | Implemented (modules) | `drivers/virtio/*` | `blk`, `virtio-console`, `random`, nettest | split ring only; free list in shared memory |
| Network stack | Implemented | `kernel-services/network/*` | 7 `net-*`, harness echo | one TCP lock, one worker, no SYN cookies/PMTUD/reassembly |
| Sockets | Implemented | `network/socket.c`, SYS 23–31 | USERTEST | blocking only; no poll/options |
| Module loader / signing | Implemented | `kernel/module/*`, `kernel/security/*` | `module-*`, host `test_modelf`, `test_crypto`, `test_reloc_aarch64` | dev key in repo; unload unsafe; no x86 reloc host test |
| ACPI | Partial | `drivers/acpi/acpi.c` | `acpi` | MADT only |
| PCI | Implemented | `drivers/pci/*` | `pci` | 64-device cap; no INTx for drivers; no rescan |
| libc | Implemented (subset) | `libc/` | host `test_libc` | |
| Userland | Implemented | init, sh, 12 coreutils, 7 tools | shelltest | |
| Package system | Implemented | `pkg/`, `tools/pkgbuild.py`, `ports/` | host `test_pkg`, shelltest | |
| Service manager / init | Stub | `init` runs `/etc/rc` then shell | — | no supervision |
| /proc, /sys | Missing | — | — | only `/dev/vmm` exists |
| Linux compat | Partial | `compat/linux/` 99 entries, 78 handlers | `linux-elf`, LINUXTEST, musl hello, host `test_linux` | x86 only; signals stored not delivered; no dynamic linking |
| Virtualization | Partial | `kernel-services/virtualization/`, `x86/svm*.c` | 8 `hv-*`, HVTEST, host `test_hv` | AMD-V only; no FPU save; XSETBV/WBINVD not intercepted |
| AArch64 | Implemented (stage 1) | `kernel/arch/aarch64/` | CI matrix | no Linux table, no hv, no FP/SIMD, ASID 0 |
| Tests | Partial | see section 16 | — | no fuzzing, fault injection, coverage, benchmarks |
| Docs | Implemented | 133 files | api.md sync ~98 % | drift listed in 3.2 |

### 3.2 README and documentation versus code

| Claim | Location | Reality | Evidence |
|---|---|---|---|
| Native ABI has "43 calls" | `docs/README.md:28` | `SYS_COUNT` is 50 (43–49 are hv) | `uapi/cosmo/syscall.h:73` |
| Linux personality "translating 87 Linux calls" | `README.md` Phase 11 | 99 table entries, 78 distinct handlers | `compat/linux/syscalls.c:1239-1339` |
| "property and fuzz tests" exist | `tests/README.md:2` | none; four `testing.md` files list fuzzing as a gap | `find tests` |
| Kernel W^X on aarch64 | `docs/kernel/memory/invariants.md` M13 | leaf PXN/UXN only; `SCTLR.WXN` cleared by loader, never set | `boot/uefi/arch/aarch64/cpu.c:77`, `kernel/arch/aarch64/cpu.c:53-59` |
| "SMAP is not enabled" | `docs/kernel/module/invariants.md:156` | SMAP enabled and used | `cpu.c:104-105`, `user.c:102-108` |
| Panic prints context | `docs/kernel/diagnostics/*` | always `context: boot (no threads yet)` | `kernel/core/panic.c:84` |
| Lock order `g_mounts_lock → mount->lock → vnode->lock` | `docs/kernel-services/vfs/design.md:131-133` (V7) | `mount->lock` is taken under `vnode->lock` | `vfs.c:40,89`, `cosmofs.c:208` |
| Scheduler: `runqueue.lock` is a leaf; only `waitqueue.lock` precedes it | `docs/kernel/scheduler/invariants.md` S2/S4 | predecessors `process.lock`, `futex.bucket`, `tty.lock`; successor `gic.g_lock` | `process.c:602-603`, `futex.c:121`, `tty.c:67`, `gic.c:375-381` |
| "nothing holds a spinlock across a blocking wait" | `docs/kernel-services/network/design.md:189-196,301-307` | `seg_mss` takes a mutex under `g_lock` | `tcp.c:259-264` |
| "Everything the device writes (used ring) is validated" | `drivers/virtio/virtqueue.c:7` | descriptor `next` chain is device-writable and unchecked | `virtqueue.c:126-137,196` |
| `dma_alloc` "Sleeps (PMM)" | `kernel/include/kernel/dma.h:32` | PMM never sleeps | `pmm.c` |
| Memory invariants M21 "user faults panic", M24 "no user spaces" | `docs/kernel/memory/invariants.md` | stale since Phase 4 | `vmm.c:572-818` |
| `compile_commands.json` "is git-ignored" | `Makefile:66` | 170 KB file present at the tree root | `ls` |
| `kernel/diagnostics/`, `kernel/smp/` directories | `docs/README.md` layout | no such code directories | `ls kernel` |
| QEMU `kvm`/`hvf` are drop-in faster options | `docs/development.md:260` | HVF on Apple Silicon needs `QEMU_CPU=host`, aarch64 only | `scripts/qemu-run.sh` |
| Memory API complete | `docs/kernel/memory/api.md` | 8 user-space VMM functions undocumented | header diff |
| Scheduler / process API complete | `docs/kernel/{scheduler,process}/api.md` | `sched_watchdog_*`, `thread_create_on`, `linux_auxv`, `linux_process_init/release` undocumented | header diff |
| "70 self-tests", "1312 vectors", coreutils list, sysctl/hv syscall ranges | `README.md` | correct | `selftest.c`, `platform.h`, `userland/` |

---

## 4. Correctness Audit

This section covers the kernel core: objects, handles, processes, ELF loading, uaccess, IPC, and the arch entry paths. Subsystem-specific correctness is in sections 5 to 13.

### 4.1 Verified correct

- Handle-table primitives are SMP-safe: every operation is one critical section under the per-table irqsave spinlock, lookup takes the object reference under that lock, and the table's own reference is dropped only after the slot is cleared (`kernel/object/handle.c:32-116`). `kobject_get` on zero panics rather than resurrecting (`object.c:18-19`). No error-path reference leak was found in any native or hv handler (`native.c:52-55,77,95-98,110,240,296,426,509,549,585,606,758,809`; `hvsys.c:26-27,37-38,50,58,120,124,158,164`).
- Process exit and reap ordering is correct: thread's process reference dropped after `process_last_thread_gone` (`thread.c:233`), parent held by counted reference (`process.c:374`), `find_reapable_locked` re-checks after wake (`process.c:538-550`).
- Kill delivery is checked at syscall entry/exit (`syscall.c:42,44`), on every trap return to user (`x86_64/trap.c:86-89`, `aarch64/trap.c:64-69`) and in every killable wait (`wait.h:75-92`, flag checked after `waitqueue_prepare`).
- The `wait_event` Mesa protocol is correct for wakers that take the wait-queue lock (`wait.c:20-52`, `sched.c:207-235,317-330`).
- ELF validation is strong: ET_EXEC native only, bounded program-header table, `PT_INTERP` refused, executable stack refused, `p_memsz < p_filesz` refused, overflow checks, W^X per segment, overlap check, entry must lie in an executable segment, PT_NOTE walk with 64-bit math and `next <= off` guard (`kernel/process/elf.c:34-165`).
- SYSRET cannot see a non-canonical RIP today because `VM_USER_HI = 0x7FFFFFFFF000` keeps user addresses one page below the canonical hole (`vmm.h:66`), R11 is CPU-saved RFLAGS, and no handler rewrites the frame.
- Every direct user dereference goes through `uaccess.c` (grep across `native.c`, `syscalls.c`, `convert.c`, `hvsys.c`, `socket.c`, `spawn.c`, `process.c`, `futex.c` finds casts only at `uaccess.c:41,54,79`).

### 4.2 Findings

| Sev | Finding | Evidence |
|---|---|---|
| CRITICAL | NMI and #MC have no IST; an NMI in the SYSCALL entry window before `swapgs` runs with the user's GS base (the ISR decides SWAPGS from CS alone), and an NMI after `popq %rsp` on exit pushes a kernel frame on the user stack at CPL0. Latent under QEMU (no NMIs injected) but it is the classic paranoid-entry class. | `kernel/arch/x86_64/idt.c:idt_init:50-63` (IST only for #DF); `syscall_entry.S:25-28,80-83`; `isr.S:56-58` |
| CRITICAL | LAPIC ICR_HI/ICR_LO write pair is not atomic against local interrupts. `smp_call_function_single` deliberately sends with IRQs enabled; a tick handler that wakes a thread issues its own IPI between the two writes, redirecting the `IPI_CALL` to the wrong CPU. `fn(arg)` then runs on an unintended CPU and the caller panics after 1 s. `ipi_broadcast_others` is immune (shorthand). | `kernel/arch/x86_64/lapic.c:icr_send:148-154`; `kernel/interrupt/ipi.c:125-129` |
| CRITICAL | No FPU/SIMD state save/restore exists anywhere: context switch handles CR3 and FS_BASE only; `svm_run.S` spills GPRs only; no `fxsave`/`xsave` in `kernel/arch/x86_64/`. Two SSE-using Linux processes, or a VM guest and its owner, corrupt each other's registers and leak state across the boundary. Additionally CR4.OSFXSR is inherited from firmware on the BSP and never set on APs (trampoline sets only PAE), so SSE availability differs per CPU. | `context.c:arch_thread_switch_prepare:40-55`; `svm_run.S:44-80`; `trampoline.S:55-57`; `cpu.c:x86_cpu_enable_features:99-108` |
| HIGH | Store-buffer lost wakeup for wakers that bypass the wait-queue lock (x86 TSO). `waitqueue_prepare` stores `BLOCKED` under `wq->lock` only; `futex_wake` and `process_kill` store their flag and call `sched_wake` under a different lock, never touching the waiter's private wait queue. The waiter's `BLOCKED` store and its flag load are separated only by a release store; the load may complete first. Outcome: futex wait without timeout hangs; kill is delayed. AArch64 `STLR;LDAR` ordering happens to save it. (Plausible; ordering argument verified, not reproduced.) | `wait.c:39`; `futex.c:118-121`; `process.c:596-603`; `wait.h:84` |
| HIGH | `smp_call_function_single` and `arch_mmu_shootdown` assert only "IRQs enabled", not "no spinlock held". A caller holding a plain `spin_lock` while a target spins irqsave on the same lock never gets its IPI acknowledged and panics after 1 s. | `ipi.c:123-135`; `x86_64/mmu.c:264-286` |
| HIGH | Kernel-mode demand-zero fault under memory exhaustion panics instead of returning `-EFAULT`: any `copy_to_user` into a fresh lazily-mapped page after physical memory is exhausted reaches `panic_frame`. User-triggerable. | `vmm.c:vm_fault_handler:526-531`, reached from `uaccess.c:41,54` |
| HIGH | ELF loader bounds each segment but not total `p_memsz`; `elf_load_into` maps segments `POPULATED`, allocating every page under `space->lock` with IRQs disabled. A tiny executable can demand the whole machine's memory and hold IRQs off for seconds on one CPU (breaking shootdown acks, see section 6). | `elf.c:elf_validate:102-140`; `elf.c:elf_load_into:194`; `vmm.c:vm_user_map_anon:690-710` |
| HIGH (latent) | uaccess is check-then-copy with no exception fixup table. Correct only while processes are single-threaded and mappings unshared; a sibling `munmap` between check and `memcpy` becomes a kernel-mode fault at a user address, which is fatal for the process at best and a panic when it happens under a lock. | `uaccess.c:33-57`; `vmm.c:555-565` |
| HIGH | `futex_wait` calls `copy_from_user` while holding the bucket spinlock with IRQs off. A fatal fault inside the copy terminates the process without releasing the bucket lock, deadlocking that bucket for every process; a demand fault allocates under a raw spinlock. | `kernel/ipc/futex.c:futex_wait:65-70`; `process.c:hook_fatal:76-90` |
| MEDIUM | No preemption point on syscall return on either arch, and `spin_unlock_irqrestore` restores IRQs after `preempt_enable` has already tested them, so a same-CPU wake of a higher-priority thread waits for the next tick (≤ 4 ms). | `syscall_entry.S:60-83`; `aarch64/trap.c:71-77`; `spinlock.c:72-76`; `percpu.c:69` |
| MEDIUM | `sched_wake` KASSERT window: `process_create` publishes the pid and links the thread with `cpu == -1` before enqueueing; a `process_kill` in between panics. | `process.c:349-353,368,384`; `sched.c:301` |
| MEDIUM | `pipe_read` can return a false EOF to a second reader: the wait condition is evaluated unlocked and a competing reader can drain the ring between wake and lock. | `ipc/pipe.c:pipe_read:86-103` |
| MEDIUM | Composite handle operations (dup-to-target, spawn validate/install) are non-atomic; TOCTOU once threads exist. | `native.c:806-807`; `syscalls.c:524-525`; `spawn.c:33-50` + `process.c:228-237` |
| MEDIUM | `read` returns at most 1 KiB per call for every object type (stack bounce buffer); the Linux personality inherits it. | `native.c:36,100-101`; `syscalls.c:176` |
| MEDIUM | Interrupt slot unregistration has no grace period; `interrupt_register` is check-then-set with only local IRQ disable. | `interrupt.c:50-60,68-89,121-126` |
| MEDIUM | `timer_cancel` does not wait for a running callback on another CPU; safe today only because timers always run on the arming CPU and threads never migrate. Undocumented load-bearing invariant. | `timer.c:100,119-134`; `wait.c:124-139`; `futex.c:96-97` |
| LOW | `WNOHANG` can observe a zombie before its handles are closed; `sys_exit` racing `process_kill` overwrites the kill status; handles truncated to `int`; refcount has no saturation; `mmap`/`mount`/`umount` ignore unknown flag bits; `socket.proto` ignored; no `_Static_assert` on UAPI struct sizes; `accept` drops an established connection if the peer-address copy faults. | `process.c:460,506,414-417`; `native.c:83,156,394,413,464,507-511`; `object.c:17`; `uapi/cosmo/syscall.h:155,261,274` |

### 4.3 Kernel object model assessment

The kobject/handle model is small and sound but thin: two rights (READ, WRITE), no CONTROL/DUP/TRANSFER right (so a read-only socket handle may `connect`/`shutdown`, `native.c:478,488,523,620,630`), no way to reduce rights on dup, no generation counter, and `kobject_io_type` is assumed by cast for every installed object (`native.c:51,94,290`). Processes are kobjects but never installed. Refcount semantics are consistent everywhere the audit looked, with one systemic exception: several objects have a refcount but no owning release (see section 10) and several have neither (netif, tcp_pcb, see section 9).

---

## 5. Memory Audit

### 5.1 Inventory and locks

Allocators: bootmem (top-down, one-shot, `kernel/memory/bootmem.c`), PMM with three zones DMA/DMA32/NORMAL in one node (`pmm.c`, `buddy.c` orders 0..10), bitmap slabs plus 15 kmalloc classes ≤ 8 KiB with a page path ≤ 4 MiB (`slab.c`, `kmalloc.c`), a sorted linked-list VMM for the kernel arena `0xFFFFC000…` plus a near arena for modules and for user spaces (`vmm.c`). `struct page` is 32 B with 6 spare bytes (`page.h:27-40`); `struct vm_region` has no backing-object pointer and per-page state lives only in page tables (`vmm.h:35-45`; `vmm.c:340,616`).

| Lock | Protects | Taken from IRQ/trap | Order |
|---|---|---|---|
| `pmm_zone.lock` ×3 | free lists, counters | yes (`vmm.c:526` from #PF) | innermost |
| `kmem_cache.lock` (per cache) | slab lists | possible | → zone (`slab.c:96`) |
| `g_caches_lock`, `g_large_lock`, dma `g_stats_lock` | registry / counters | – | leaves |
| `kernel_space.lock` / `vm_space.lock` | region list, page tables | yes (#PF, `vmm.c:521`) | → cache → zone |
| `g_shootdown_lock` (x86) | broadcast slot | no; spins with IRQs on up to 1 s | no other lock held |

Observed order `vm_space → kmem_cache → zone` matches `invariants.md` M22; no reverse nesting found.

### 5.2 Findings

| Sev | Finding | Evidence |
|---|---|---|
| HIGH | `pmm_page_put` is not atomic: load, compare, then a separate `fetch_sub`. Two concurrent puts from refcount 2 both observe 2, both decrement, and the frame is leaked at refcount 0; a later `pmm_page_get` panics. Latent (only `memtest.c:91-96` uses it) but it is the primitive shared memory and COW would be built on. | `pmm.c:pmm_page_put:243-254` |
| HIGH (latent) | Kernel-half PML4 entries are snapshotted once per user space; `descend` creates new PML4-level PDPTs lazily and nothing pre-populates the 64 arena entries at `vmm_init`. If the arena ever crosses a 512 GiB boundary while processes exist, a kernel access from process context faults, `arch_mmu_map` returns `-EEXIST`, and `vm_fault_handler` panics. Invariant P9 is asserted by comment only. AArch64 is immune (TTBR1). | `x86_64/mmu.c:152-159,192-195`; `vmm.c:188-245,534-538` |
| HIGH | A user-mode not-present fault on a lazily-mapped kernel ANON region (modules use lazy `aflags`) selects `kernel_space` without checking `VM_FAULT_USER`, allocates and maps a kernel frame, and only then kills the process on the retry. Unprivileged code can force allocation of every page of every lazy kernel region. | `vmm.c:vm_fault_handler:507-547`; `module.c:325-329` |
| MEDIUM | Teardown window: `region_teardown` drops the space lock with the region still listed; a fault on the just-unmapped chunk re-populates it and the frame is leaked with a live mapping to a freed VA. | `vmm.c:350-352,371-374,626-628,729-732` |
| MEDIUM | `PROT_NONE` is mapped as readable; guard pages and `mprotect(PROT_NONE)` never trap. | `native.c:170-171`; `syscalls.c:678-679,723-724`; `vmm.c:741` |
| MEDIUM | `vm_user_unmap`/`vm_user_protect` require an exact region match; `lx_brk` shrink ignores the failure and leaves memory mapped, after which the next growth fails with `-EEXIST` for the life of the process. | `vmm.c:721,741`; `syscalls.c:635-653,684` |
| MEDIUM | POPULATE paths hold `space->lock` irqsave across all page allocations and memsets (ELF segments, boot module images). A CPU in such a section cannot acknowledge a shootdown; the initiator panics after 1 s. | `vmm.c:274-313,682-711`; `elf.c:194`; `spawn.c:100`; `mmu.c:279-286` |
| MEDIUM | Page array (`pmm_max_pfn × 32 B`) must fit under the bootstrap direct map (4 GiB on aarch64); sparse maps or ~1 TiB RAM overflow it and `bootmem_alloc` panics. `phys_in_direct_map` is a pure upper-bound test. | `pmm.c:149-150`; `bootmem.c:67-68,111`; `boot/uefi/arch/aarch64/paging.c:6`; `page.h:98-101` |
| MEDIUM | Intermediate page tables are never reclaimed; AArch64 forces every table page into ZONE_DMA32 forever. | `x86_64/mmu.c:12`; `aarch64/mmu.c:13,68` |
| LOW | `vm_user_protect` updates `r->prot` before `arch_mmu_protect` with no rollback; `pmm_free_pages` misuse checks read flags unlocked; `dma_alloc` maps any mask < 32 bits to the 16 MiB DMA zone and never retries a lower zone; `dma_free` hard-codes bus == phys against `dma.h:9-10`; `kmalloc` is not zeroed by default. | `vmm.c:750-751`; `pmm.c:216-227`; `dma.c:23-30,52-55,72`; `kmalloc.h:9` |

Verified OK: `sys_mmap`/`lx_mmap`/`lx_munmap`/`lx_mprotect` size and wrap checks (`native.c:154`; `syscalls.c:661-663,702,712`); `user_range_valid` overflow (`vmm.c:661-665`); all user frames zeroed (`vmm.c:526,692`; `process.c:315`); W^X enforced at every entry (`native.c:160`; `syscalls.c:669,714`; `vmm.c:380,673,739`); NX required on x86 (`mmu.c:66`); user-exec pages PXN on aarch64 (`mmu.c:104`); PG_DEFERRED release order (`vmm.c:215,230`; `pmm.c:270`).

### 5.3 Scalability and future-proofing

| CPUs | Serialization points |
|---|---|
| 4 | three zone locks on every page alloc/free; one lock per kmalloc class; `kernel_space.lock` for every kernel fault |
| 16 | zone lock hot; every 32-page unmap chunk sends 15 IPIs and waits (`vmm.c:317`; `mmu.c:276-288`); `g_shootdown_lock` serializes all shootdowns |
| 64 | hard cap: `CONFIG_MAX_CPUS=64`, `cpumask_t` is `uint64_t` (`percpu.h:22,28`); AArch64 `tlbi vmalle1is` on every user switch flushes all CPUs (`aarch64/mmu.c:377-380`); `vm_space_destroy` still shoots down per chunk though no CPU has the space active |
| 256 | unsupported by type; linear region list O(n) per `copy_from_user` (`vmm.c:803`) |

No per-CPU cache exists at any layer. Required for growth: per-CPU frame caches in front of zones; per-CPU slab magazines; per-space "CPUs that ran this space" mask on x86 and ASIDs with `tlbi aside1is` on AArch64; skip shootdown in `vm_space_destroy`; a region tree; a bit-array cpumask. The buddy algorithm, `struct page` layout and `arch_mmu` API need not change.

Future features against current abstractions: ASLR needs only a randomized `from` in `vm_user_find_free` (`vmm.c:758-788`) plus a PIE load bias in `elf_load_into`; file-backed and shared mappings need `object*` + `offset` + kind on `vm_region`, a protection-fault path (`vmm.c:524` rejects `VM_FAULT_PRESENT`) and, at the UAPI level, an `fd`/`offset` pair that the 4-argument `SYS_mmap` cannot carry (`uapi/cosmo/syscall.h:27,190-191`); huge pages need a page-size field and split code (unmap/protect refuse splits, `x86_64/mmu.c:351-363`, `aarch64/mmu.c:251-262`); COW/fork needs a correct frame refcount and a reverse map (8 spare bytes in `struct page`); swap needs LRU and a page→mapping link, none present.

### 5.4 NUMA readiness

Single `g_node` (`pmm.c:24`); zone selection by physical address only (`pmm.c:35-42`); `struct page` carries a zone but no node (`page.h:31`); `pmm_alloc_pages(order, flags)` has no node or CPU parameter (`pmm.h:72`); bootmem always allocates from the highest range (`bootmem.c:85-86`); one linear direct-map window (`page.h:78-86`); DMA has no device-to-node mapping (`dma.c:41-63`); no topology structure (`percpu.h` knows `cpu_id`/`hw_id` only); no SRAT parsing (`drivers/acpi/acpi.c` decodes MADT only). The additions are mechanical (`pmm_node[]`, node ranges in `zone_for_pfn`, `page.reserved0` as node id, a policy argument to `pmm_alloc_pages`).

### 5.5 Tests

Covered: PMM alloc/free/zones/refcount/coalesce (`memtest.c:36-135`), kernel VMM (`:139-252`), kmalloc (`:256-344`), host buddy and slab (7 tests each). Untested: every user-space VMM function, user fault → fatal, kernel-mode fault on a user address, populate-failure unwind paths (`vmm.c:299-307,696-707`), `pmm_page_put` concurrency, SMP stale-TLB correctness (shootdowns are counted, never verified), concurrent alloc/free, allocation from IRQ context, `-EEXIST` partial-map behaviour, mmap/munmap/brk/mprotect semantics from user mode, DMA mask edges.

---

## 6. SMP / Scheduler Audit

### 6.1 Algorithm as implemented

- **Placement** happens once: `pick_cpu` (`sched.c:141-155`) picks the online CPU in the affinity mask with the smallest `nr_running`, read unlocked, `current` not counted (so a CPU running a spinner with an empty queue ties with an idle CPU). `sched_wake` (`:299-315`) always re-enqueues on `t->cpu`. **No migration, no idle stealing, no periodic balancing, no `setaffinity`.**
- **Policy**: 64-level bitmap, FIFO per level, `ctz` pick (`policy_rr.c:37-43`); 10 ms slice charged in whole 4 ms ticks regardless of switch-in time (`rr_tick:50-64`), so effective slices are 8–12 ms; preempted threads with slice left re-enqueue at head.
- **Preemption points**: IRQ return (`x86_64/trap.c:81-83`, `aarch64/trap.c:109-110`), `preempt_enable` (only if IRQs currently enabled, `percpu.c:63-71`), idle loop. None on syscall or exception return.
- **Switch**: `rq->lock` held across `arch_context_switch`, released by the next thread (`sched_finish_switch:185-195`); exited threads handed to the reaper via `rq->prev_exited` because freeing a stack needs a shootdown (`thread.c:66-88,208-236`).
- **Tick**: 250 Hz periodic on every CPU including idle ones; no dynticks. TSC per CPU with a single boot base and no synchronisation check; `has_invariant_tsc` detected but unused (`timer.c:32`; `x86_64/timer.c:115-118`; `cpu.c:83`).

### 6.2 Findings (beyond the CRITICAL/HIGH entries already listed in 4.2)

| Sev | Finding | Evidence |
|---|---|---|
| HIGH | Every thread exit (via the reaper's `vm_kernel_free`) broadcasts a TLB shootdown to all online CPUs and waits for all acknowledgements under one global lock; combined with IRQ-off populate loops (5.2) this is a 1 s panic waiting for load. | `mmu.c:255-291`; `vmm.c:352`; `thread.c:219` |
| MEDIUM | TSC skew across CPUs breaks cross-CPU comparisons: `smptest.c:264-266`, the watchdog (`sched.c:345-358`, kicked from any CPU, checked on CPU 0), and any future migration. | `timer.c:32`; `x86_64/timer.c:115-118` |
| MEDIUM | `sched_enqueue_new` does not IPI an idle remote CPU for the lowest-priority level (compares against idle's priority 63); `sched_wake` has the explicit idle test. | `sched.c:178,309` |
| LOW | No priority inheritance; `mutex_unlock` uses `wake_one` and a woken waiter can lose to a barging `mutex_trylock` indefinitely. | `mutex.h:4`; `mutex.c:38-42` |
| LOW | Watchdog fires once, prints from CPU 0's tick, reads `g_watchdog_last_kick` unsynchronised; no hard/soft-lockup detection, no NMI path. | `sched.c:355-364` |
| LOW | Sequential AP bring-up with `udelay(10000)` per CPU (~0.7 s at 64 CPUs). | `x86_64/smp.c:112-125` |

Verified correct: exit/reap refcount protocol (refcount 2, last put after switch-out); `irq_depth`/`preempt_count` handling in both trap tails; the idle `hlt`/`wfi` race is closed by the IRQ-return preemption point; `sched_tick` and `run_expired` take plain `spin_lock` only where IRQs are already off.

### 6.3 Scalability

| Concern | Evidence | 16 CPUs | 64 CPUs | 256 CPUs |
|---|---|---|---|---|
| Global `g_shootdown_lock`, broadcast + wait-all per exit | `mmu.c:255-291` | noticeable on thread churn | serial bottleneck | infeasible |
| Single `smp_call` slot | `ipi.c:24-28` | serialises | bottleneck | – |
| `gic.g_lock` taken on every IPI, under `rq.lock` (aarch64) | `gic.c:364-381` | contended | hot | – |
| Shared `g_slots[v].count` atomic RMW for tick/IPI vectors | `interrupt.c:119` | ~4k RMW/s | ~16k/s | bad |
| False sharing: `g_counts[cpu][4]`, `g_shootdown_stats[cpu]`, `g_next_cval[cpu]`, `g_cur_intid[cpu]`, unaligned `g_rqs[]` (1096 B) and kmalloc'd `percpu` | `ipi.c:21`; `mmu.c:242`; `aarch64/timer.c:35`; `gic.c:63`; `sched.c:25`; `smp.c:52` | measurable | per-IRQ line bouncing | – |
| All device IRQs on CPU 0 | `serial.c:123`; `pl011.c:140`; `virtio_pci.c:177,339` | CPU 0 hot | saturates | – |
| xAPIC physical destinations only, 8-bit IDs | `lapic.c:5`; `irqc.c:110-111` | – | ok ≤ 255 | cannot address |
| `cpumask_t = uint64_t`, `CONFIG_MAX_CPUS=64` | `percpu.h:22-28` | – | hard cap | – |

Required for 64+: bit-array cpumask; x2APIC with logical/cluster mode; per-CPU call queues (`call_many`); shootdown by address-space CPU set with per-space generations; cache-aligned `struct runqueue`/`struct percpu`/counters; idle stealing or periodic balancing; IRQ affinity spreading; ticket or MCS spinlocks.

### 6.4 CPU feature framework

x86 gathers vendor/family/model, NX, SMEP, SMAP, UMIP, PGE, APIC, x2APIC, FSGSBASE, invariant TSC (`cpu.c:39-88`) and enables WP/NXE/PGE/SMEP/SMAP/UMIP per CPU (`:90-109`); detection runs on the BSP only. Not gathered or unused: XSAVE and state size, PCID/INVPCID, LA57, TSC-deadline, 1 GiB pages (detected privately in `mmu.c:45,62`, duplicating NX detection), SSE4.2 (CRC32C is table-driven), IBRS/IBPB/SSBD, MD_CLEAR; FSGSBASE detected but CR4 bit never set (good); x2APIC never enabled. AArch64 reads MIDR/MPIDR/PAN/PARange/GIC (`aarch64/cpu.c:36-61`), enables PAN + SPAN; no LSE gating, no errata table, GICv3 panics (`gic.c:106-107`). There is no generic `arch_cpu_has()` capability API, no alternatives patching, no microcode or errata handling; generic code reads arch structs directly.

### 6.5 Tests

Covered (`selftest.c:240-306`): thread create/join, yield, tick preemption, sleep bounds, mutex/semaphore/completion/waitqueue semantics, timer rearm/order/cancel/rate, IRQ routing, SMP online/affinity/parallel/call/shootdown/wake latency/ticks/mutex. Untested: multi-bit affinity and balancing; wake racing block (S22 window); `process_kill` vs `wait_event_killable`; futex races and timeouts; cross-CPU `timer_cancel`; concurrent `smp_call_function_single` from two CPUs; shootdown while targets hold spinlocks; concurrent `interrupt_register`/`unregister`; `smp_stop_others`; AP start failure; watchdog firing; reaper backlog; priority preemption on wake; TSC skew. No benchmarks exist for context switch, wakeup latency, IPI round trip, spinlock acquire or shootdown cost.

---

## 7. Synchronization Audit

### 7.1 Lock inventory

Legend: type S = spinlock, M = sleeping mutex; IRQ = taken in interrupt context; Local = per-CPU instance. All spinlocks disable preemption (`spinlock.c:31-45`); the owner check turns same-CPU self-deadlock into a panic (`spinlock.c:39-40`).

| Lock | File | Type | Protects | IRQ | Local | Contention | Notes |
|---|---|---|---|---|---|---|---|
| `runqueue.lock` | scheduler/sched.c | S | rq lists/bitmap, thread state/cpu/slice, owner's `need_resched` | yes | per-CPU, taken remotely by every cross-CPU wake | hot on wake-heavy loads at 64 CPUs | not a leaf (→ `gic.g_lock`, LAPIC ICR spin) |
| `waitqueue.lock` | scheduler/wait.c | S | waiter list, `waiting_on`, sets BLOCKED | yes | no | per object | → `runqueue.lock` |
| `mutex/semaphore/completion.lock` | scheduler/*.c | S | owner/count/done | up/complete | no | per object | leaf, released before wake |
| `g_thread_list_lock` | scheduler/thread.c:22 | S | all-threads list, tid | yes (watchdog dump) | no | every create/exit | → `klog-ring` → `console` via kprintf |
| `g_reap_lock` | thread.c:55 | S | reap list | yes | no | every exit | leaf |
| `timer_queue.lock` | timer/timer.c | S | per-CPU pending list | yes | per-CPU, remote via cancel | low | leaf; callbacks run unlocked |
| `g_irq_lock` | interrupt/irq.c:24 | S | `g_irqs[1024]` | enable/disable | no | config | → `g_vector_lock`/`gic.g_lock`, `ioapic.g_lock` |
| `g_vector_lock` | x86_64/irqc.c:25 | S | vector bitmap | no | no | config | leaf |
| `ioapic.g_lock` | x86_64/ioapic.c:42 | S | IOREGSEL/IOWIN | via irq_disable | no | config | leaf |
| `gic.g_lock` | aarch64/gic.c:55 | S | vector maps, SGI map, GICD regs | **yes, every `arch_ipi_send`** | no | global per IPI | leaf |
| `g_call_lock` | interrupt/ipi.c:25 | S | single cross-call slot | no (IRQs must be on) | no | serialises all cross-calls | → `gic.g_lock` |
| `g_shootdown_lock` | x86_64/mmu.c:238 | S | va/len/acks | no (IRQs on) | no | serialises all shootdowns | → `gic.g_lock` equiv. |
| `kernel_space.lock` / `vm_space.lock` | memory/vmm.c | S | regions, page tables | yes (#PF) | no | global for kernel VA | → `kmem_cache.lock` → `zone.lock` |
| `kmem_cache.lock`, `g_caches_lock`, `g_large_lock`, `zone.lock` | memory/* | S | allocator state | yes | no | every alloc/free | cache released before `slab_release` → pmm |
| `process.lock` | process.h:69 | S | threads, children, kill_sig, cwd | no | no | per process | → `runqueue.lock` (`process_kill`); `g_process_table_lock` → `process.lock` |
| `g_process_table_lock` | process.c:31 | S | pid table | no | no | spawn/exit | → `process.lock` |
| `handle_table.lock` | object/handle.c | S | slots, count | no | no | per process | leaf; never held across `kobject_put` |
| `futex bucket.lock` | ipc/futex.c:25 | S | waiter lists | timer cb wakes without it | no | 1 of 64 | → `vm_space.lock` (copy_from_user!), → `runqueue.lock` |
| `pipe.lock` | ipc/pipe.c | S | ring, end counts | no | no | per pipe | leaf; conditions read unlocked |
| `tty.lock` | tty/tty.h:29 | S | ring, edit line | yes (serial RX) | no | console | → `waitqueue.lock` → `runqueue.lock` (`tty.c:67`) |
| `klog-ring`, `console` | core/log.c:20, console.c:14 | S | ring, sinks | yes | no | every kprintf | sequential |
| `g_mounts_lock` | vfs/vfs.c:19 | M | fs registry, mount list | – | no | held across `fs->sync` | outermost VFS |
| `mount->lock` | vfs | M | vnode hash | – | no | per mount | taken **under** `vnode->lock` (leaf in practice) |
| `vnode->lock` | vfs | M | size, links, dir contents, flags | – | no | per vnode | parent → child; rename parents by address |
| `file->lock` | vfs | M | pos | – | no | per file | before `vnode->lock` |
| `pagecache.lock` | vfs/pagecache.c | M | page hash | – | no | per vnode | under `vnode->lock` |
| `cfs->lock` | cosmofs | M | all metadata, bitmap, buffers, commit I/O | – | no | **one per filesystem** | under vnode/pagecache locks |
| `g_blk_lock` | block/blk.c | M | blkdev list | – | no | registry | |
| `g_device_mutex` | device/device.c:24 | M (recursive) | buses, devices, drivers, probe/remove | – | no | all device ops | modules → device → `kernel_space.lock` |
| module `g_lock` | module/module.c | M | module list, load/unload incl. `init()` | – | no | outermost | |
| `virtqueue.lock`, `vpci.lock`, `vblk.lock` | drivers/virtio | S | ring free list/avail; MMIO select; inflight[] | yes / yes / thread only | no | per device | `vblk.lock` bypassed by IRQ path |
| `mbufq.lock`, mbuf `g_stats_lock` | network/mbuf.c | S | queues, counters | yes | no | per queue | leaf |
| `g_netif_lock` | network/netif.c | **M** | netif list | – | no | thread only | **taken under `tcp.c g_lock`** (CRITICAL, 9.2) |
| `netif->lock`, `g_work_lock` | network/netif.c | S | addr/flags; deferred work | – / timer IRQ | no | | leaf |
| `tcp.c g_lock` | network/tcp.c | S | pcb table **and every pcb** | worker + syscall | no | **one for all connections** | → mutex (bug) |
| `udp.c g_lock`, `arp.c g_lock`, `ipv6.c g_nd_lock` | network | S | pcb list / tables | worker + syscall | no | global | leaf |
| `sock->lock` | network/socket.c | M | socket state | – | no | per socket | documented outermost of net |
| hv `g_lock` | virtualization/vmm.c | M | VM list | – | no | | → `vm->lock` |
| `vm->lock`, `vcpu->run_lock` | virtualization | M | regions, devices, vcpus; run loop + regs | – | no | per VM / vCPU | `run_lock` → `vm->lock` |
| `vcpu->irq_lock`, `vm->console.lock`, `g_asid_lock` | virtualization, svm.c | S | pending vectors; ring; ASID bitmap | yes / – / – | no | | leaves |
| `pci-legacy`, `fwcfg`, `random`, `linux-table`, `g_blk` stats | various | S | registers / counters | – | no | | leaves |

### 7.2 Lock-order graph

```
 tty.lock ──┐
 process.lock ──┤
 futex.bucket ──┼──→ waitqueue.lock ──→ runqueue.lock ──→ gic.g_lock (aarch64) / LAPIC ICR busy-spin (x86)
 futex.bucket ──→ vm_space.lock ──→ kmem_cache.lock ──→ zone.lock          (copy_from_user under bucket lock)
 g_process_table_lock ──→ process.lock ──→ handle_table.lock
 g_irq_lock ──→ { g_vector_lock | gic.g_lock | ioapic.g_lock }
 g_call_lock ──→ gic.g_lock ;  g_shootdown_lock ──→ gic.g_lock
 g_thread_list_lock ──→ klog-ring ──→ console
 module g_lock ──→ g_device_mutex ──→ kernel_space.lock
 g_mounts_lock ──→ vnode->lock (dir/mountpoint) ──→ pagecache.lock ──→ cfs->lock ──→ [sync block I/O]
 vnode->lock (parent) ──→ vnode->lock (child) ──→ mount->lock          (docs say mount before vnode: inverted)
 vnode->lock (rename: lower address) ──→ vnode->lock (higher address)  (conflicts with parent→child: ABBA, 8.2)
 sock->lock ──→ tcp.c g_lock ──→ g_netif_lock [MUTEX]                  (sleeping lock under spinlock: CRITICAL, 9.2)
 tcp.c g_lock ──→ arp/nd g_lock ──→ netif->lock ──→ driver virtqueue.lock
 hv g_lock ──→ vm->lock ;  vcpu->run_lock ──→ vm->lock
```

### 7.3 Deadlock and lock-safety report

| Sev | Issue | Evidence |
|---|---|---|
| CRITICAL | Sleeping mutex acquired under an IRQ-off spinlock: `seg_mss` → `netif_owns_ipv4/6` → `mutex_lock(&g_netif_lock)` while `tcp.c g_lock` is held. `mutex_lock` only panics when `irq_depth != 0`; in thread context (netrx worker or syscall) a contended mutex calls `sched_block_current` with a spinlock held and preemption disabled. Triggered by every SYN, connect and retransmit. | `tcp.c:seg_mss:259-264`; `netif.c:194-226`; callers `tcp_connect:637-641`, `tcp_input:960,996`, `pcb_work:513-516` |
| HIGH | ABBA in VFS rename: parents locked in address order, everything else parent-before-child. `rename("/a/x","/a/b/y")` with `&b < &a` vs concurrent `rmdir("/a/b")`. | `vfs.c:943-946` vs `883-890` |
| HIGH | `copy_from_user` under `futex bucket.lock` introduces `bucket → vm_space → zone` with IRQs off and a blocking helper in atomic context. | `futex.c:65-70` |
| HIGH | `smp_call_function_single`/`arch_mmu_shootdown` 1 s panic when the caller holds any spinlock a target is spinning on irqsave. | `ipi.c:123-135`; `mmu.c:264-286` |
| MEDIUM | `vfs_sync` holds `g_mounts_lock` across a full cosmofs commit; every mount/umount blocks behind it. | `vfs.c:348-357` |
| MEDIUM | Documented orders are stale in three subsystems (scheduler S2/S4, VFS V7, network design.md) and there is no runtime lock-order checker to catch drift. | see 3.2 |
| OK | No cycle found among spinlocks; every spinlock chain terminates in `gic.g_lock`, `zone.lock`, `console` or `handle_table.lock`. `spin_lock` without irqsave is used only where IRQs are already off or where the lock is never taken from IRQ context (`sched_tick:375`, `run_expired:138,150`, `ipi.c:125`, `mmu.c:270`). | audit of ~90 declarations |

### 7.4 Where epoch-based reclamation (EBR) or RCU applies

| Structure | Read pattern | Write pattern | Benefit |
|---|---|---|---|
| Interrupt slot table `g_slots` | every interrupt, lock-free acquire load already | rare register/unregister | **highest**: gives `interrupt_unregister` the grace period `interrupt.h:15-18` promises; unblocks safe unplug and module unload. Quiescent state = every CPU passing `irq_depth==0` or `schedule()`; latency ≤ one tick |
| Timer callbacks | per-CPU | cancel from same CPU only today | `timer_cancel_sync` needs "callback not running" = the same quiescence signal |
| percpu registry / online mask | every IPI/shootdown | boot only | O(1) reads with a bitmask; low payoff beyond that |
| netif list, ARP/ND tables, TCP pcb table | every packet | rare | remove `g_netif_lock` from the hot path (fixes 9.2 CRITICAL structurally); per-pcb locks become possible |
| device/blkdev/module lists | per I/O submit (blkdev) | hotplug | pairs with refcounted release (section 10) |
| Run queues, timer queues, wait queues, page tables | write-heavy | – | not candidates |

---

## 8. VFS / Filesystem Audit

### 8.1 Structure

VFS (`kernel-services/vfs/vfs.c`, 1074 lines): mount table, per-component locked path walk (`VFS_MAX_COMPONENTS` 40), vnodes with kobject refcounts, `vnode_ops`, `struct file`, `fs_type.mount(blkdev, flags)`. Vnode types REG/DIR/CHR only (`vfs.h:61-78`). Page cache (`pagecache.c`, 252 lines): 32 fixed buckets per vnode, write-back only on sync, no reclaim. ramfs (`ramfs.c`, 475 lines): root, boot archive, `/dev` char nodes. cosmofs (2,179 lines): two superblock slots with generation + CRC32C, two-level imap (3.87 M inodes), 256 B inodes with 10 direct extents plus one indirect block of 254 (`CFS_MAX_EXTENTS` 264), 64 B directory entries with names ≤ 47, one flat bitmap (63 GiB device ceiling, `format.h:55`), in-memory authoritative bitmap, deferred frees, 64-entry metadata buffer LRU, one mutex, `failed` poison. Pool (`pool.c`, 52 lines): one blkdev, 4 KiB blocks. Block (`blk.c`, 227 lines): `bio{sector,nsectors,dir READ/WRITE/FLUSH,buf,done}`, synchronous helpers with no timeout.

### 8.2 Findings

| Sev | Finding | Evidence |
|---|---|---|
| CRITICAL | No permission enforcement on any file operation; `mode` is stored, never checked; `uid`/`gid` never set from the creator; every process is uid 0. Only mount/umount are uid-gated (and that gate is inert). Acknowledged as a phase gap (V14) but it is a multi-user OS with no access control. | `vfs.c:vfs_open:568-651`; `cosmofs.c:409-415`; `ramfs.c:43-63`; `docs/kernel-services/vfs/design.md:15` |
| HIGH | Rename ABBA (7.3) and an unsynchronised ancestor-loop check: the "directory under itself" walk holds only the two parents; a concurrent rename of an ancestor can create a detached cycle. The walk also calls `lookup(p, "..")` without `p->lock`, and ramfs reads `n->parent` unlocked against `ramfs_rename`. | `vfs.c:943-946,968-989,978`; `ramfs.c:201` |
| HIGH | `vnode_lookup_cached` is check-then-get on the refcount: a vnode at refcount 0 awaiting `vnode_release` (which needs `mnt->lock`, held here) is skipped, so a second vnode is created for the same inode; two divergent `cfs_vnode.inode` copies then race on `inode_sync`. | `vfs.c:91-96`; `cosmofs.c:208` |
| HIGH | `fsync` gives no durability. Commit happens only on `vfs_sync`/umount (V17); `cfs_vnode_sync` writes the inode into the buffer cache only. A panic loses everything since the last explicit sync; dirty buffers are never evicted. | `cosmofs.c:753-762`; `cosmofs_core.c:88-102` |
| HIGH | Full-disk deadlock: deletion needs allocations (CoW dir block, inode/imap blocks, bitmap chunks) and freed blocks become allocatable only after a commit. At zero free blocks nothing can be deleted or committed; there is no metadata reserve. | `cosmofs_core.c:512-519,222-243` |
| HIGH | Fragmentation cap: each rewrite of one logical block allocates at `alloc_hint` and splits the run; only physically adjacent runs merge. Random page-wise rewrite reaches 264 extents (~1 MiB of 1-block runs) and every further write fails `-EFBIG`. Directories are capped identically at 264 blocks (≤ 16,896 entries). | `cosmofs.c:cfs_set_block:133-156,53-54,151-152`; `dir_write_block:262-281` |
| HIGH | Sparse write at a large offset allocates and writes zero blocks from the current end to the target: `pwrite(fd, "x", 1, 1<<40)` consumes all free space with I/O, then fails ENOSPC with the in-memory inode already pointing at the zero blocks. | `cosmofs.c:cfs_writepage:701-715` |
| HIGH | Unbounded memory: page cache has no reclaim; ramfs enforces `RAMFS_MAX_FILE` only in truncate and `RAMFS_MAX_PAGES` is unused, so any process can exhaust RAM via `/tmp`; reading a large cosmofs file pins every page until the last close. | `pagecache.c` (no shrinker); `ramfs.c:19,37,256-259` |
| HIGH | Block layer returns `-EAGAIN` when virtio slots are exhausted; `sync_io` reports it as an I/O error; cosmofs may `cfs_fail` (poison the mount) on it. Any burst larger than `nr_slots` concurrent bios can poison the filesystem. | `virtio_blk.c:84-87`; `blk.c:151-153`; `cosmofs.c:578,608` |
| HIGH | Directory blocks and data blocks carry no checksum; a torn or silently corrupted directory block is undetected (entries are only range-checked). | `cosmofs.c:268,646-648`; `format.h:33-40` |
| MEDIUM | Commit retry after a post-superblock failure can overwrite a possibly-durable root: if the second flush fails, `gen`/`sb_slot` do not advance, later mutations dirty the same generation's buffers in place, and the retry rewrites block numbers that root N may reference, unordered before its flush. Any error after `super_write` begins must poison or treat the slot as committed. | `cosmofs_core.c:cfs_commit:460-525,496-501`; `cfs_buf_cow:193-196` |
| MEDIUM | No fallback to the older superblock slot when the newer root's tree is unreadable; mount returns `-EIO` although the older root is intact by construction. | `cosmofs_core.c:764-793` |
| MEDIUM | FLUSH is issued even when `VIRTIO_BLK_F_FLUSH` was not negotiated (`vb->flush` recorded, never consulted) → `-ENOTSUP` → every commit fails on such a device. | `virtio_blk.c:184,101-105` |
| MEDIUM | `pagecache_truncate` zeroes the tail page but does not mark it dirty; truncate → close → reopen → write within the old block exposes the previously truncated bytes. | `pagecache.c:191-195,62-63` |
| MEDIUM | `close()` cannot report write-back errors: `vnode_release`/`file_release` ignore `pagecache_sync` failures and drop dirty pages silently. | `vfs.c:48-50,526-530` |
| MEDIUM | Stale metadata buffer aliasing: a freed block's clean buffer is never invalidated; `cfs_buf_new` on a reused block pushes a second buffer with the same `blkno`; `buf_find` has no generation check. Correct today only because eviction is from the LRU tail. | `cosmofs_core.c:78-86,93,177` |
| MEDIUM | `VFS_MAX_COMPONENTS` 40 is returned as `-ELOOP`; a 41-deep tree is unreachable. cosmofs names ≤ 47 vs `VFS_NAME_MAX` 255. No symlinks at all. | `vfs.c:479-482`; `format.h:84-90` |
| MEDIUM | `blk_submit` performs a throw-away `dma_map` as a validity predicate and never unmaps; the driver maps again. | `blk.c:102`; `virtio_blk.c:108` |
| LOW | `cfs_buf_get` kind-mismatch leaks a buffer reference; `commit_bitmap` leaks `reserved[]` on error (free space shrinks until remount); `cfs_free_block_deferred` leaks a block on OOM; `-ENOSPC` from `dir_add` is unreachable; `vnode_release` writes back dirty pages of an `nlink==0` file before evicting them. | `cosmofs_core.c:132-133,381-435,253`; `cosmofs.c:334`; `vfs.c:47-53` |

Verified correct: lookup vs unlink/rmdir (`VNODE_DEAD` set under both locks, checked under the parent lock: `vfs.c:890-902,422,598,857`); unlink-vs-open; mount lifetime (`follow_mount` refuses while `unmounting`, refcount scan is final: `vfs.c:367-393,275-313`); the "publish destination first, roll back" rename ordering is sound for in-memory pre-commit state (`cosmofs.c:524-583`); inodes are CoWed into the buffer cache (not written through to disk), so the committed root always references only its own blocks; the commit protocol (write CoW blocks → FLUSH → write other superblock slot → FLUSH → advance) is correct on a device that honours FLUSH; `super_read` validation; inode numbers are 64-bit and never reused; integer handling in `pagecache_write`, `file_seek`, `extents_load`, `blk_submit`.

### 8.3 Portability of the VFS to other filesystems

Blockers, in order of cost: `vnode_ops` lacks getattr/setattr, link, symlink/readlink, mknod, statfs, ioctl, mmap, permission and revalidate; there is no dentry/name cache (every component calls the FS, and for cosmofs that is a linear directory read from the pool under `dir->lock`, `cosmofs.c:289-335`); readdir cookies are bare indices (ramfs positions shift on removal, `ramfs.c:227-236`); no `(ino, generation)` identity; `fs_type.mount(blkdev, flags)` has no options string, source path or multiple devices; no mount stacking (`vfs.c:231`) so no bind/overlay; `vfs_root()` is global (`vfs.c:179-184`) so no chroot or per-process namespace; `struct vnode` embeds a fixed-size page cache. A pseudo-filesystem (procfs, devfs) is feasible today (ramfs already demonstrates NULL-bdev mounts and synthesised vnodes). Network filesystems and overlay need the ops, identity and caching redesign.

### 8.4 Storage pool and transaction-engine readiness

What exists is a single-member pool of raw block numbers. The commit already has the right skeleton (dirty state → CoW allocation → metadata update → checksum seal → flush → root write → flush) but as singletons inside `struct cfs`: `gen`, `bufs`, `bitmap_dirty`, `pending_free` (`cosmofs_internal.h:27-51`). To become transaction groups it needs: a `struct txg` with open/quiescing/syncing states; a writeback thread with dirty thresholds (also the fix for the fsync finding); dependency ordering rather than MRU order (`cosmofs_core.c:477-485`); FUA/barrier flags on `struct bio` (`blk.h:22-26,28-39` has neither); and a `struct dva {vdev, offset, size}` replacing every raw `blkno` in extents, imap pointers and superblock roots (`format.h:37,63,79`) with a member table in `cfs_super` (currently a constant 1) and per-vdev allocation groups. That is an on-disk format change; the reserved `csum_root`/`snap_root`/`members` fields make it a versioned extension rather than a rewrite.

### 8.5 Storage corruption model

| Fault | Detected | Recovered | Repaired | Reported |
|---|---|---|---|---|
| Power loss | torn superblock via CRC; uncommitted work absent | previous root | n/a | generation at mount |
| Software crash / panic | same; loses all writes since last `vfs_sync` (unbounded) | same | n/a | none |
| Torn metadata write | CRC + self-`blkno` + kind (`mhdr_check`, `cosmofs_core.c:47-55`) → `-EIO` | none (no redundancy, no older-root walk) | none | `kerror` per block |
| Torn data or directory block | **undetected** | – | – | – |
| Bad sector, read | driver `-EIO` propagates | none, no retry/remap | none | `blkdev.errors` |
| Bad sector, write | op fails; after rename publication → poison | forced umount discards transaction | none | `kerror` |
| Silent corruption | metadata yes; data/dir no; `free_blocks` cross-check (`load_bitmap`, `:723-725`) | bitmap authoritative | none | `kwarn` |
| Malicious modification | no (CRC32C is recomputable; no MAC) | – | – | – |
| Firmware lying about flush | newer valid superblock over incomplete children → `-EIO` later, no fallback | none | none | – |

No scrub, fsck, redundancy or error injection exists.

### 8.6 Checksum policy

CRC32C, table-driven software (`kernel/core/crc32c.c`, no SSE4.2/ARMv8-CRC dispatch), covers both superblocks and every `cfs_mhdr` metadata kind; not data blocks, not directory blocks. Cost is one 4 KiB pass per metadata read/write, negligible against synchronous I/O. There is no checksum-algorithm abstraction: `block_crc` is hard-coded and the 4-byte header field has no algorithm id (`format.h:38`). Recommendation: keep CRC32C for metadata (adequate for random corruption of 4 KiB units), add an algorithm id, and store 32-byte data checksums in the *parent* pointer (extent entry) as a versioned format extension; `csum_root` is reserved for exactly this.

### 8.7 Tests

`vfstest.c` covers page-cache semantics, ramfs namespace errors, open-unlink survival, second mount/umount, vnode leak check. `cosmofstest.c` covers pool bounds, format/remount generation, a 45-block file with mid rewrite (extent split/merge), replacing rename, rmdir, persistence, free-space return, `ENAMETOOLONG`, and "crash" = a software discard on unmount plus one torn-superblock byte flip. Host `test_cosmofs.c` covers struct sizes and index arithmetic. Untested: any concurrency; ENOSPC and full-disk deletion; fragmentation/`EFBIG`; directories > 64 entries (multi-block directories are never exercised); multi-chunk bitmaps (disk > 127 MiB); `cfs_fail`/`VFS_UMOUNT_FORCE`; rename rollback; I/O error injection; power-cut at arbitrary commit points; readdir cookie stability under mutation; unlink of an open cosmofs file; truncate-then-extend. **There is no automated crash-consistency harness** (no block-level record/replay, no prefix-of-writes simulation, no image fuzzing).

---

## 9. Networking Audit

### 9.1 Structure

`kernel-services/network/`: mbuf (128 B inline or 2048 B refcounted cluster, 64 B headroom), netif (no refcount), single `netrx` worker draining one 512-entry queue for all NICs with a 64-packet budget (`netif.c:321-333`), ether/ARP (64 entries)/IPv4/ICMP/IPv6+ND (32 entries)/UDP/TCP (1253 lines)/sockets as kobjects. TCP keeps data in two fixed 64 KiB byte rings per connection, allocated at pcb creation (`tcp.c:tcp_pcb_new:151-177`). One spinlock `g_lock` protects the pcb table and every pcb. Syscall surface: socket/bind/listen/accept/connect/sendto/recvfrom/shutdown/getsockname (`native.c:417-637`), all blocking via `wait_event_killable`; no `O_NONBLOCK`, poll/select/epoll, `setsockopt` or `getpeername` in the native ABI.

### 9.2 Findings

| Sev | Finding | Evidence |
|---|---|---|
| CRITICAL | Sleeping mutex under the global TCP spinlock with IRQs off (7.3). | `tcp.c:259-264`; `netif.c:194-226` |
| HIGH | Kernel stack disclosure to the network: the ICMP Port Unreachable path copies `IPV4_HDR_LEN(ip4)` (up to 60) bytes from the 20-byte stack copy of the IP header made in `ipv4_input`. A UDP datagram with IP options to a closed port leaks up to 40 bytes of `ipv4_input`'s frame. | `udp.c:udp_input:255-263`; `ipv4.c:250-256` |
| HIGH | Use-after-free in `ksock_accept`: the child pcb returned by `tcp_accept` has `sock == NULL`; a RST or timeout before `tcp_attach_socket` runs `pcb_end_locked` → `pcb_free_locked`, then attach writes freed memory; the `-ENOMEM` path double-closes. | `socket.c:171-189`; `tcp.c:223-226,1065-1067` |
| HIGH | Use-after-free on UDP close vs RX: `udp_input` captures `pcb->sock` under the lock, drops it, and calls `sock_wake` without a reference; `socket_release` can free the socket in between. TCP takes a reference (`tcp.c:446-460`); UDP does not. | `udp.c:245,269-270`; `socket.c:21-29` |
| HIGH | Timer callback vs pcb free: `timer_cancel` returns without waiting for a running callback on another CPU; `pcb_free_locked` then frees while `rexmit_timer` → `net_work_queue(&pcb->work)` writes the freed pcb and links it into `g_work`. | `timer.c:119-148`; `tcp.c:127-149,180-203`; `netif.c:269-278` |
| HIGH | Listener lockout by SYN flood: `nr_queued` counts SYN_RCVD and ESTABLISHED children together against a backlog of 16; each spoofed SYN pins a full pcb with 128 KiB of rings for the SYN-ACK retransmission lifetime (~4 min). No SYN cache or cookies. | `tcp.c:944-945,151-177` |
| MEDIUM | Blind in-window RST/SYN aborts (no RFC 5961 challenge ACK); TIME_WAIT is pinned forever by any segment on the 4-tuple; orphaned FIN_WAIT_2 pcbs never time out (no keepalive, no FIN_WAIT_2 timer): a remote peer holds 128 KiB per closed connection indefinitely. | `tcp.c:1059-1077,1020-1030` |
| MEDIUM | NIC hot-unplug UAF: `vnet_remove` frees the `struct vnet` embedding the netif while queued mbufs still carry `pkt.rcvif` and `netif_default()` returns unreferenced pointers. | `virtio_net.c:180-197`; `netif.c:232,305-319` |
| MEDIUM | ICMP reflection with no rate limit (unreachables on every unknown port/protocol, unlimited echo replies); DF set with no PMTUD (black-hole risk). | `ipv4.c:68,102-126,260-267`; `udp.c:257-263` |
| LOW | Sequential ephemeral ports from a random base; 1500-byte segment buffer on the stack under an IRQ-off spinlock; `ipv4.c:262-267` prepends `ihl` bytes but rewrites 20, quoting uninitialised bytes after an `m_pullup`; ND accepts any NS-with-SLLAO as reachable (on-link cache poisoning). | `tcp.c:246-255,297`; `udp.c:49-58`; `ipv4.c:262-267`; `ipv6.c:209-219` |

Verified correct: every header parse site pulls up before reading and checks length fields against the mbuf length (`ether_input:15-20`, `ipv4_input:199-222`, `ipv6_input:388-399`, `icmp_input:131-144`, `udp_input:193-210`, `tcp_input:867-884`, `arp_input:165`, ND `200,227`); IPv4/ICMP/ICMPv6/UDP/TCP checksums verified; sequence arithmetic wrap-safe (`tcp.c:29-32,1033-1039`); RTO per RFC 6298 with Karn's rule (`tcp.c:834-852,505-507`); wait/wake protocol has no lost wakeup for sockets (`wait.h:57-69`); used-ring ids validated by the driver.

### 9.3 TCP feature status

Present: all 11 states with simultaneous open and half-close; MSS option (clamped 1460/1440/16384-loopback); slow start, congestion avoidance, fast retransmit on 3 dupacks; delayed ACK (40 ms / every second segment); zero-window probe (but counted against `TCP_MAX_REXMIT`, so a stalled peer kills the connection after ~4 min); TIME_WAIT 2 s. Absent: fast recovery, SACK, out-of-order reassembly (dropped and ACKed, `tcp.c:1170-1176`), window scaling (advertised window ≤ 65535), Nagle, keepalive, FIN_WAIT_2 timeout, PMTUD, SYN cookies, RFC 5961, checksum offload (`M_CSUM_OK` defined, never produced), IP fragmentation/reassembly (fragments dropped, `ipv4.c:223-227`), routing beyond one default interface, IPv6 global routing.

### 9.4 Buffer path, copies and zero-copy readiness

| Path | Copies | Sites |
|---|---|---|
| TCP RX | 3 | cluster → rcvbuf ring (`tcp.c:1157`) → syscall bounce (`tcp.c:679`) → user (`native.c:607`) |
| TCP TX | 4 | user → bounce (`native.c:570`) → sndbuf ring (`tcp.c:665`) → 1500 B stack temp (`tcp.c:297-301`) → cluster (`tcp.c:302`) |
| UDP RX / TX | 2 / 2 | |

Zero-copy blockers: TCP data lives in byte rings, not mbuf chains; the syscall layer always bounces through `SOCK_IO_CHUNK`; clusters are 2048 B slab objects, neither page-granular nor user-mappable; virtio posts one whole cluster per frame with no mergeable buffers (`virtio_net.c:37-57`); `m_pullup` leaves zero headroom (`mbuf.c:160`) so every later prepend allocates; 64 B headroom is too small for vnet(12)+eth(14)+IPv6(40)+TCP(20) so IPv6 TX chains always start with an extra buffer.

### 9.5 Multiqueue and async I/O readiness

Nothing is per-CPU: `g_rxq` and `g_work` are global, virtio-net allocates one RX and one TX queue (`virtio_net.c:148-151`), all vectors go to CPU 0, and timers are pinned to the arming CPU so a connection's timers scatter across CPUs. Under 4+ CPUs all protocol input serialises on one CPU and TX contends on `g_lock` with it. For an io_uring-style ring, the socket layer needs non-blocking `ksock_*` variants returning `-EAGAIN`, a per-socket readiness/completion hook at the `sock_wake` sites, mbuf-based TCP receive queues (rings force a copy), and user-pinned buffers registered with the cluster allocator. The readiness predicates already exist piecemeal (`tcp_accept_ready`, `tcp_recv_avail`, `tcp_send_space`).

### 9.6 Tests

`nettest.c` covers mbuf ops and queues, checksums over odd chain boundaries, ARP ageing and admission, UDP v4/v6 round trip and ICMP unreachable, TCP refused connect, 1 MiB v4 and 256 KiB v6 transfers with pattern check, TIME_WAIT lifetime, port reuse, backlog fill, listener-close RST, 1-in-7 data-segment loss, and a host↔guest echo through the QEMU harness. Untested: reordering, duplicate/overlapping segments, RST mid-stream, zero-window/persist, window-edge acceptability, MSS variants, IPv4 options and IHL > 5 quoting (the HIGH leak), concurrent close vs RX (the three UAFs), table-full ARP/ND, IPv6 over a real NIC, multi-CPU contention, hotplug. There are no host tests for the network stack.

---

## 10. Driver / DMA Audit

### 10.1 Structure

Device model (`kernel/device/device.c`): bus/device/driver with resources and matching, one recursive `g_device_mutex` around everything including probe/remove; `device_release` is a no-op (`device.c:45-49`). PCI (`drivers/pci/pci.c`): ECAM from MCFG segment 0 or legacy CF8/CFC (x86 only), depth-first scan, `PCI_MAX_DEVICES` 64, BAR sizing, bounded capability walk, MSI-X (per-entry request with a `cpu` argument) and single-message MSI; INTx is not available to drivers; devices are never freed. Interrupts: `g_slots[1344]` published with release/acquire, `g_irqs[1024]`; x86 vectors 48–238, IOAPIC and MSI in xAPIC physical mode (`apic_id > 0xff` refused, `irqc.c:105-116`); aarch64 GICv2 + GICv2m. VirtIO: modern PCI transport only, split ring ≤ 256, one `dma_alloc` per queue, INDIRECT and EVENT_IDX never negotiated, one MSI-X vector per callback queue, all on CPU 0. DMA (`kernel/device/dma.c`): `dma_addr_t` = physical, `dma_map` = `virt_to_phys` if direct-mapped, contiguous and under the mask, `dma_unmap` no-op, `dma_sync_for_device` = `arch_dma_barrier`; default mask 32-bit (`device.c:102`). Modules (`kernel/module/`): Ed25519 trailer over the whole ELF verified before any parsing, three RW regions in the near arena, resolve, relocate with range checks on both arches, `vm_kernel_protect` to RX/R before `init()`, ~130 `EXPORT_SYMBOL`s, `module_unload` exists.

### 10.2 Findings

| Sev | Finding | Evidence |
|---|---|---|
| CRITICAL | Module signing private key `tools/keys/cosmo-dev.key` is tracked in git and is the default `MODSIGN_KEY`; `cosmo-dev.pub` is what the keyring compiles in. Any build shipping the dev keyring accepts modules signed by anyone with the repository. No revocation or expiry. | `tools/keys/`; `build/module.mk:17`; `kernel/security/keyring.c`; `kernel.mk:112-120` |
| CRITICAL | The virtqueue free list lives in device-shared memory: `virtq_add` walks `d->next` read from the descriptor table and indexes `vq->desc[i]` with it unchecked; `virtq_pop` writes free links back into the table. A device (or DMA bug) that rewrites `desc[].next` to 0xFFFF makes the next `virtq_add` write 16 bytes up to 1 MiB past the table. The used ring is validated; the descriptor table is equally device-reachable. | `drivers/virtio/virtqueue.c:126-137,196` vs comment at `:7` |
| HIGH | Default 32-bit `dma_mask` is never widened (no `dma_set_mask` call in any driver) while kmalloc allocates from ZONE_NORMAL: on hosts with > 4 GiB, `dma_map` of a normal buffer fails and block/net I/O returns `-EINVAL`; all rings are forced into DMA32 and its exhaustion fails probes with `-ENOMEM`. | `device.c:102`; `kmalloc.c:78`; `dma.c:82-101` |
| HIGH | Device and driver removal frees driver containers while kobject references are outstanding: `struct blkdev` is embedded in `struct vblk` and freed by `vblk_remove`, `blkdev_release` is a no-op, and `pool_open`/VFS hold `blkdev_get` references; `virtio_device` is freed in `vpci_remove` while the model holds a reference; `pci_device` is never freed. | `virtio_blk.c:229-246`; `blk.c:19-22`; `pool.c:17`; `vfs.c:199`; `virtio_pci.c:367-375`; `device.c:45-49` |
| HIGH | No IRQ grace period: `pci_msix_release` → `irq_release_msi` → `interrupt_unregister_vector`, then `virtq_free` → `kfree(vq)` while a handler on another CPU may still hold `arg = vq` (`interrupt.c:5-9` acknowledges this). `module_unload` likewise frees module text right after `shutdown()` without quiescing handlers, timers or threads the module created. SMP is live before boot modules load (`main.c:166-170`). | `virtio_pci.c:205-217`; `virtqueue.c:105-113`; `module.c:424-433` |
| MEDIUM | No request timeout anywhere: `sync_io` blocks forever on a silent device; one hung virtio-blk wedges every synchronous caller (and cosmofs holds `cfs->lock` while waiting). | `blk.c:154` |
| MEDIUM | No signature versioning or anti-rollback: `cosmo_module_info.version` is a free string never compared; any previously signed vulnerable `.ko` loads forever. With `MODULE_SIG_ENFORCE=0` unknown-key modules load with the trailer present. | `modelf.c:222`; `module.c:262-272` |
| MEDIUM | `vblk` `inflight[]` is written outside `vb->lock` on failure paths and in the completion handler (benign today with a single vector, not a discipline). | `virtio_blk.c:111,128,143-152` |
| MEDIUM | Symbol export whitelist includes `kmalloc`, `panic`, `thread_create`, VMM/PCI/IRQ APIs, and `SHN_ABS` symbols are accepted, so a module can address anything; capabilities in `cosmo_module_info` are recorded, not enforced. A malicious signed module owns the machine (no sandbox is possible at ring 0; this is stated, not a defect). | `module.c:157-160`; `module.h:32-34` |
| LOW | `dma_unmap`/`dma_sync_for_cpu` are never called by any driver; `vpci_read_config` gives up after 8 generation retries without error and the config-change interrupt only logs; `NEEDS_RESET` is never polled; `PCI_MAX_DEVICES` silently drops devices; `enable_this_cpu` (SVM) allocates with IRQs off. | `virtio_pci.c:138-144,245-251`; `pci.c:260-263`; `svm.c:129-130` |

Verified correct: publish barriers (`virtq_add` release fence, device barrier in `virtq_kick`, acquire + `rmb` on consume: `virtqueue.c:141-166`); used-ring `id` range and cookie validation (`:172-177`); used `len` bounded by consumers where it matters (`virtio_net.c:66`, `virtio_rng.c:46-47`); IRQ handlers only touch irqsave-safe allocators; relocation bounds on both arches (`x86_64/modreloc.c:44-51`, `aarch64/modreloc.c:187-198`); W^X for module regions (`modelf.c:159,185`; `module.c:356-374`); signature verified before parsing (`module.c:258-276`); Ed25519 canonical checks (`ed25519.c:370-397`).

### 10.3 IOMMU readiness

The naming layer exists (`dma_addr_t`, `dma_alloc/map/unmap/sync`) and no driver calls `virt_to_phys` directly (only `dma.c:89`). The mapping layer does not: `dma_map` returns 0 rather than creating a mapping or bouncing; `dma_unmap` is a no-op and no driver calls it, so mapping lifetimes would leak on day one; `blk_submit` uses `dma_map` as a predicate; `struct device` has only `dma_mask`, no domain pointer; `pci_enable_device(MASTER)` is not gated on domain attach (`pci.c:365-372`); MSI-X messages carry raw APIC addresses (`irqc.c:113`) so interrupt remapping needs a compose hook. Positive: descriptor rings already hold bus addresses (`virtqueue.c:76-78`).

### 10.4 NVMe readiness

The block layer offers a single-buffer bio, no scatter-gather or segment limits, no FUA/PREFLUSH flag, no ordering semantics, no queue depth (the driver returns `-EAGAIN` when full and `sync_io` does not retry), sector size ≥ 512 power of two, per-device `max_sectors`. NVMe needs multi-segment bios or PRP construction, admin/IO queue pairs with per-CPU MSI-X (the routing API exists: `pci_msix_request(..., cpu)`), a 64-bit DMA mask (see the HIGH above), completion-queue phase handling, namespaces as multiple `blkdev` per `device`, timeouts and abort, and a `blkdev` with a real refcounted release.

### 10.5 Hotplug readiness

CPU: `CONFIG_MAX_CPUS=64`, `g_cpu_count` only grows, percpu blocks kmalloc'd at `smp_init` and never released, no offline path, nothing migrates vectors from a departed CPU (`percpu.c:27-45`; `smp.c:53`; `irqc.c:90`). PCI: no rescan, `pci_init` once, append-only `g_by_index`. Device removal: `device_unregister`/`driver_unregister` exist but `release` frees nothing and drivers free their own containers (10.2); block handles are `blkdev_get` pointers with no revocation; no chrdev layer beyond `ramfs_mkchr`. NIC: `vnet_remove` leaves dangling `rcvif` pointers (9.2).

### 10.6 Tests

Host: `test_modelf` (36 validator rules), `test_reloc_aarch64` (every encoding and bounds), `test_crypto` (RFC 8032 vectors). No x86-64 `modreloc` host test and no virtqueue host test with a fake transport (both admitted in `virtio/testing.md:46` and `module/invariants.md:153`). Kernel: `device`, `pci`, `dma`, `random`, `blk`, `virtio-console`, `bootarchive`, `ksym`, `modsig`, `module-reject/load/fail`. No fault injection. Untested: hostile used ring or corrupt descriptors, legacy config access, single-message MSI, remove with in-flight I/O, unload under load, enforcement-on rejection of an unsigned module, guests with > 4 GiB.

---

## 11. Virtualization Audit

### 11.1 Structure and locks

Generic manager (`kernel-services/virtualization/`, 1,517 lines) over `kernel/include/arch/hv.h` (123 lines); SVM backend (`svm.c` 631, `svm_npt.c` 142, `svm_run.S` 98); AArch64 `hv.c` is a 64-line `-ENOTSUP` stub. Limits 8 VMs / 4 vCPUs / 64 MiB / GPA < 4 GiB. Locks: hv `g_lock` (M) → `vm->lock` (M); `vcpu->run_lock` (M) → `vm->lock`; `vcpu->irq_lock`, `vm->console.lock`, `g_asid_lock` spin leaves. `vm->lock → run_lock` never occurs. Guest-memory copies hold `vm->lock`; the run loop walks the device list unlocked, legal because the table freezes at first run (`vmdev.c:86-89`). No deadlock found.

### 11.2 Verified correct

Guest memory isolation (NPT leaves point only at region-owned zeroed frames, never user-mapped, freed after the last vCPU reference: `guestmem.c:82-92`; `vm.c:86-94`; `vcpu.c:60-71`); host preemption during `vcpu_run` (`V_INTR_MASKING` + `sti` under `clgi` makes every host tick exit; kill checked per iteration: `svm_run.S:33-35`; `svm.c:242,552`; `vcpu.c:229-232`); vCPU migration safety (`clean_bits=0`, `FLUSH_ALL` every entry, lazy per-CPU SVME); host state save via VMSAVE/VMLOAD of a per-CPU host VMCB with GIF clear until host `vmload` completes (`svm_run.S:33-38,83-88`); guest cannot become a hypervisor (SVME refused, VMRUN/VMLOAD/VMSAVE/STGI/CLGI/SKINIT intercepted → #UD, CPUID hides SVM/VMX and leaf 0x8000000A: `svm.c:343-344,392-393,606-619`; `vcpu.c:109-118`); all ports and MSRs intercepted (all-ones IOPM/MSRPM); interrupt injection and interrupt-shadow clearing (`vcpu.c:233-242`; `svm.c:465-469,527-530`); unknown exits → `HV_EXIT_FAIL`, vCPU dead, never a host fault (`svm.c:620-629`); rights checks per syscall and 0600 `/dev/vmm` (`hvsys.c:42-176`; `vmm.c:115`); cleanup on owner death mid-run.

### 11.3 Findings

| Sev | Finding | Evidence |
|---|---|---|
| CRITICAL | No FPU/SIMD state is saved around `svm_run` and the guest controls CR0.TS/EM (CR intercepts are zero), so the guest reads whatever XMM/x87 contents the last host user thread left (cross-VM information leak) and clobbers the owner's registers on return. Hidden today only because native userland is `-mgeneral-regs-only`; a musl process is exposed. | `svm_run.S:44-80`; `svm.c:vmcb_reset:226-267`; `context.c:40-55` |
| HIGH | XSETBV not intercepted (`SVM_INTERCEPT_XSETBV` defined, absent from `intercept_misc2`; documented as intentional): guest sets CR4.OSXSAVE and XSETBV, #VMEXIT does not restore XCR0. Host CR4.OSXSAVE is never set today so the damage is cross-VM persistence; the moment the host enables XSAVE (needed for the CRITICAL above) it becomes host corruption. | `svm.h:93`; `svm.c:234-236`; `cpu.c:98-108` |
| HIGH | WBINVD, RDPMC and RDTSCP not intercepted: a guest can flush all host caches in a loop (system-wide stall), read host performance counters (side channel), and RDTSCP returns the host `TSC_AUX` while CPUID still advertises it and `wrmsr TSC_AUX` #GPs (a Linux guest panics in `setup_getcpu`). | `svm.c:234-236`; `vcpu.c:150-181` |
| HIGH | String I/O (INS/OUTS/REP) is reported to the owner as a register I/O with RIP already advanced past the whole REP, no RCX/RSI/RDI update, and an IN completion writes RAX not memory: any guest doing `rep outsb` silently loses data. | `vcpu.c:vcpu_run_limited:259-283,221-225` |
| HIGH | Resource limits are global, not per process or uid: 8 × 64 MiB = 512 MiB of pinned, unaccounted memory; mitigated only by `/dev/vmm` being 0600 in a system where everyone is uid 0. | `vmm.c:hv_register_vm:139` |
| MEDIUM | `check_state` tests descriptor-byte-6 bit positions (`SEG_ATTR_L=1<<13`, `DB=1<<14`) but VMCB `attrib` packs AVL/L/DB/G at bits 8–11 (the tests themselves use `0xC9B`); the long-mode L&&DB check is dead and the comment at `svm.h:148` is wrong. Hardware still rejects with `VMEXIT_INVALID`. | `svm.c:41-42,347`; `hvtest.c:249`; `vmm.c:74` |
| MEDIUM | CPUID leaks host features the model does not back: leaf 7 unfiltered; OSXSAVE mirrors host CR4; APIC/x2APIC advertised with no LAPIC and all APIC MSRs #GP; MCA advertised while `MCG_CAP` #GPs; physical-address width is the host's. | `vcpu.c:95-129` |
| MEDIUM | MSR model gaps: `EFER_KNOWN` admits LMSLE/FFXSR/TCE (VMRUN consistency failure kills the vCPU); PAT unvalidated; `guest_efer.LMA` goes stale when the guest toggles CR0.PG; DR0–3 neither intercepted nor saved (host values leak in, guest writes persist on the host CPU), host DR7 cleared by #VMEXIT and never restored; #MC not intercepted. | `svm.c:40,383-386,417`; `intercept_dr=0`, `intercept_exceptions=0` |
| MEDIUM | No TSC virtualisation (`tsc_offset` never set, RDTSC passthrough); vCPU state is not capturable (`cosmo_vcpu_regs` omits STAR/LSTAR/CSTAR/SFMASK/KERNEL_GS_BASE/SYSENTER/PAT, interrupt shadow, `eventinj`, `in_completion`; no way to kick a running vCPU). Snapshot and migration are impossible without UAPI extension. | `vcpu.c:157-162,75`; `uapi/cosmo/syscall.h:253-299` |
| LOW | `paging_off_is_translated` assumes GPA 0x80000000 is outside host RAM (a TCG regression test, not a guarantee); `TLB_CONTROL=1` flushes all ASIDs including the host's every VMRUN (`FLUSH_ASID`=3 is the fix); guest CR0.CD/PAT can make its RAM UC while the host copies through the cached direct map; SVME never disabled on CPU offline. | `vmm.c:58-94`; `svm.c:240`; `guestmem.c:145-149` |

### 11.4 Intel VMX readiness

Naming is vendor-neutral (`HV_EXIT_*`, `arch_hv_*`; `hvtest.c:103` accepts `"vmx"`). What is SVM-shaped and must change: `cosmo_vcpu_seg.attrib` is the 16-bit SVM packing copied verbatim into the UAPI (`syscall.h:253-260`; `svm.c:297-311`), contradicting `arch/hv.h:7-8` ("nothing SVM-specific crosses"); VMX access rights are 32-bit with an unusable bit. `arch_hv_vcpu_msr`'s backend-owned set is the VMSAVE list; on VMX FS/GS/TR bases are VMCS fields and STAR/LSTAR need MSR load/store lists. Real-mode reset state (`svm.c:246-262`, `arch/hv.h:78`) requires EPT + Unrestricted Guest on Intel; make it an `hv_caps` bit. `arch_hv_vm_map` is RWX-only and 4 KiB-only (`arch/hv.h:72-73`; `svm_npt.c:84-101`): no permission argument, no dirty/accessed tracking, no large pages, so ballooning, dirty tracking, snapshot and passthrough need an API change regardless of vendor. The CLGI/STGI + `sti` trick has no VMX analogue (VMX exits on external interrupts via pin-based controls).

### 11.5 AArch64 EL2 readiness

The kernel runs at EL1 and the loader refuses EL2 (`docs/kernel/arch/aarch64/design.md:88-90`). Prerequisites: boot at EL2 with VHE (`HCR_EL2.E2H=1`) so the EL1 kernel becomes an EL2 host with minimal change, or a thin EL2 stub reached by HVC; stage-2 tables via VTTBR/VTCR with VMID as the ASID analogue and `TLBI VMALLS12E1IS` per entry (the `npt_*` shape fits, `svm_npt.c` does not); GICv2 virtualisation via GICH list registers, the GICV alias mapped into the guest and a maintenance IRQ; `CNTVOFF_EL2`/`CNTHCTL_EL2` for timers. The UAPI is x86-only (`cosmo_vcpu_regs` = rax…, segments; `hv_exit.io` port I/O; `HV_GPR_RAX…`): `arch/hv.h` needs a per-arch register block, and the exit kinds need MMIO + HVC + WFI + SMC with ID-register traps replacing CPUID emulation.

### 11.6 Tests

Host `test_hv.c` (UAPI sizes, IOIO decode, NPT map/unmap/rollback/User bit); 8 kernel self-tests (probe, NPT + copies, PIO + IN completion, IRQ + shadow, CPUID/MSR/hypercall, protected-mode MMIO + state refusal, shutdown, spin/tick/limits); boot harness requires `HVTEST: PASS` on x86. Gaps: no string-I/O test; no MSR #GP path test; no multi-CPU migration or concurrent-VM test; no FPU/XCR0/DR clobber test; no test that `vm_release` returns every page (`npt_table_pages` exists, unused); no negative rights test.

---

## 12. Linux ABI Audit

### 12.1 Structure

`compat/linux/syscalls.c` (1,381 lines; table at `:1239-1339`, compiled only under `#if defined(ARCH_X86_64)`, empty table elsewhere `:1366-1381`), `convert.c` (pure conversions), `linux_abi.h`. Personality is selected by the CosmoOS PT_NOTE (`process.c:270`). ELF loader accepts ET_EXEC only and refuses `PT_INTERP` (`elf.c:52-53,69-70`). Auxiliary vector: `AT_PHDR/PHENT/PHNUM/ENTRY/PAGESZ/RANDOM` (`syscalls.c:linux_auxv:75-101`). TLS via `arch_prctl(SET_FS)` → `arch_set_tls_base`, restored per switch (`x86_64/user.c:111-115`; `context.c:53-54`). Errno values are Linux's by construction.

### 12.2 Compatibility matrix

Status: full = Linux semantics for the supported argument space; partial = works with deviations noted; stub = accepted, no effect; missing = `-ENOSYS`.

| Syscall | nr | Status | Notes (`syscalls.c` unless stated) |
|---|---|---|---|
| read, write | 0, 1 | full | read ≤ 1 KiB per call inherited from native (`:176-177`) |
| open, creat, openat | 2, 85, 257 | partial | flags whitelisted; O_CLOEXEC/NONBLOCK/NOCTTY/NOFOLLOW dropped (`convert.c:29-32`); dirfd must be AT_FDCWD or path absolute (`check_dirfd:121-126`) |
| close | 3 | full | |
| stat, lstat, fstat, newfstatat | 4, 6, 5, 262 | partial | lstat = stat; 144 B layout correct; st_dev/rdev 0; atime = mtime (`convert.c:37-62`) |
| poll, select, ppoll, pselect6, epoll_* | 7, 23, 271, 270, 213/232/233/281/291 | missing | musl DNS (`res_msend`) needs poll |
| lseek | 8 | full | ESPIPE on non-files (`:294-308`) |
| mmap | 9 | partial | anonymous only; file-backed → ENODEV (`:664`); MAP_FIXED unmap result ignored (`:684`); MAP_SHARED ≡ PRIVATE; W^X → EINVAL |
| mprotect | 10 | partial | whole-region only (`vmm.c:vm_user_protect`) — breaks RELRO |
| munmap | 11 | partial | whole-region only |
| brk | 12 | partial | 1 GiB cap (`:40`); partial shrink leaves pages mapped, later grow fails (`:644-651`) |
| rt_sigaction, rt_sigprocmask, sigaltstack | 13, 14, 131 | stub | stored, never consulted (`:823-872`) |
| rt_sigreturn, pause, sigpending, sigsuspend, rt_sigtimedwait | 15, 34, 127, 130, 128 | missing | |
| ioctl | 16 | stub | always ENOTTY (`:612-619`) incl. TCGETS, TIOCGWINSZ, FIONBIO |
| pread64, pwrite64, readv, writev | 17, 18, 19, 20 | full | IOV_MAX 1024, short counts handled (`:224-247`) |
| access, faccessat | 21, 269 | partial | existence only, mode ignored (`:405-408,441-452`) |
| pipe, pipe2 | 22, 293 | partial | pipe2 flags dropped (`:567`) |
| sched_yield | 24 | full | |
| mremap, msync, madvise | 25, 26, 28 | missing / missing / stub | |
| dup, dup2, dup3 | 32, 33, 292 | full | dup3 flags ignored |
| nanosleep, clock_nanosleep | 35, 230 | partial | `rem` written as 0 on EINTR (`:903-913`); tv_sec > 1 year → EINVAL (`:162`); ABSTIME against monotonic |
| getpid, gettid, getppid | 39, 186, 110 | partial | gettid = pid (no threads) |
| socket, connect, accept, accept4, sendto, recvfrom, shutdown, bind, listen, getsockname, getpeername | 41–52, 288 | partial | AF_INET/INET6, STREAM/DGRAM; NONBLOCK/CLOEXEC dropped (`:1025-1042`); 64 KiB message cap |
| sendmsg, recvmsg, socketpair | 46, 47, 53 | missing | |
| setsockopt, getsockopt | 54, 55 | stub | SOL_SOCKET accepted silently; getsockopt always ENOPROTOOPT |
| clone, fork, vfork, clone3, execve | 56, 57, 58, 435, 59 | missing | explicit `lx_nosys` |
| exit, exit_group | 60, 231 | full | |
| wait4 | 61 | partial | pid 0 / -N → ECHILD; rusage zeroed; 139 → SIGSEGV encoding |
| kill, tgkill | 62, 234 | **wrong** | any signal < 32 terminates the target (`:812`); tgkill ignores tgid |
| uname | 63 | full | "Linux 6.0.0-cosmo x86_64" |
| fcntl | 72 | partial | GETFL from rights; SETFL/SETFD no-ops; DUPFD works (`:569-610`) |
| fsync, fdatasync, sync | 74, 75, 162 | full (but see 8.2: fsync is not durable) | |
| getcwd, chdir | 79, 80 | full | |
| rename(at), mkdir(at), rmdir, unlink(at) | 82/264, 83/258, 84, 87/263 | partial | dirfd rule |
| readlink, readlinkat | 89, 267 | missing | no symlinks in VFS |
| umask | 95 | stub | returns 022 |
| gettimeofday, time, clock_gettime | 96, 201, 228 | partial | every clock is monotonic since boot; REALTIME epoch is wrong (`:876-901`) |
| getrlimit, setrlimit, prlimit64, sysinfo | 97, 160, 302, 99 | missing | |
| getuid/gid/euid/egid | 102, 104, 107, 108 | full | always 0 |
| setpgid, getpgrp, setsid | 109, 111, 112 | stub | no process groups |
| arch_prctl | 158 | partial | SET/GET_FS only (`:747-764`) |
| futex | 202 | partial | WAIT/WAKE only; REQUEUE/CMP_REQUEUE/WAKE_OP/WAIT_BITSET/PI → ENOSYS (`:968-993`); CLOCK_REALTIME bit ignored |
| sched_getaffinity, sched_* | 204, … | missing | |
| getdents64 | 217 | full | (`convert.c:133-154`) |
| set_tid_address | 218 | stub | stored, never acted on at exit (`:741-745`) |
| set_robust_list | 273 | stub | |
| getrandom | 318 | full | 256 KiB cap |
| rseq | 334 | missing | musl ≥ 1.2.4 tolerates |
| statx, memfd_create, eventfd, timerfd, signalfd, shm*, prctl, getpriority | — | missing | `lx_unknown` → ENOSYS with 8-shot log (`:1231-1237`) |

Static musl startup needs and gets: `arch_prctl(SET_FS)`, `set_tid_address`, `brk`, `mmap(ANON)`, `AT_RANDOM/PHDR/PAGESZ`, `writev`, `exit_group`, `rt_sigprocmask` (stub), `set_robust_list` (stub), `rseq` ENOSYS, `ioctl(TIOCGWINSZ)` → ENOTTY (tolerated). It does not get `poll` (DNS), `readlink("/proc/self/exe")`, or `clone` (pthreads).

### 12.3 Findings

| Sev | Finding | Evidence |
|---|---|---|
| CRITICAL | No per-thread FPU state (shared with 4.2/11.3): two SSE-using Linux processes corrupt each other's XMM registers across context switches, and leak state. `hello_musl` (the only SSE-capable binary in the tree) is optional in CI. | `context.c:40-55`; `linux.mk:12-13,28-32` |
| HIGH | `kill`/`tgkill` with a non-fatal signal terminates the target: `SIGUSR1`, `SIGCHLD`, `SIGWINCH`, `SIGCONT` are all fatal; handlers stored by `rt_sigaction` never run. Until delivery exists, default-ignore signals must be honoured and stop signals refused. | `syscalls.c:lx_kill:805-816` → `process.c:process_kill:591-604` |
| HIGH | Region-granular VM operations make the dynamic-linker path impossible: partial `mprotect` (RELRO), partial `munmap`, `MAP_FIXED` over part of a mapping, multi-step `brk` all fail or misbehave. | `vmm.c:vm_user_unmap`, `vm_user_protect`; `syscalls.c:644-651,684` |
| HIGH | Non-blocking I/O cannot be requested at all: `ioctl` is ENOTTY, `pipe2`/`socket`/`open` drop `O_NONBLOCK`, `fcntl(F_SETFL)` is a no-op. Programs relying on EAGAIN block forever. | `syscalls.c:567,582,612-619`; `convert.c:29-30` |
| MEDIUM | `CLOCK_REALTIME` and `time()` return uptime; `ns_from_timespec` rejects tv_sec > 1 year; `clear_child_tid` never actioned at exit; `nanosleep` writes 0 to `rem`; `access` ignores mode. | `syscalls.c:876-901,162,741-745,909-912,405-408` |

### 12.4 Dynamic linking gap (§30)

Exists: auxv basics, `mprotect`, anonymous `mmap` with whole-region `MAP_FIXED`, `brk`, TLS via `arch_prctl`, PT_GNU_STACK policy. Missing: `PT_INTERP` (refused), **ET_DYN** (refused, which also rejects every PIE and `-static-pie` binary, the default output of modern toolchains), file-backed `mmap` (no file-backed region type in `vmm.c`, only `VM_REGION_ANON`), `MAP_PRIVATE` copy-on-write of file pages, sub-range `mprotect`/`munmap`, `AT_BASE`/`AT_EXECFN`/`AT_PLATFORM`, `readlink` for `$ORIGIN`. The loader change itself is small (map the interpreter at a free base, pass `AT_BASE`, set entry to the interpreter's, keep `AT_ENTRY` = program's); the VMM changes (file-backed regions, split/merge, COW) are the real work and are shared with section 5.3.

### 12.5 Signals gap (§31)

Exists: a single process-wide `kill_sig`/`exit_status` consumed at return-to-user on both arches (`x86_64/trap.c:86-89`; `aarch64/trap.c:64-69`; `syscall.c` around each handler); faults call `process_exit(COSMO_EXIT_FAULT)`; killable waits return `-EINTR` only on kill; Linux state `sigmask`, `act[64]`, `altstack` stored and never read. Needed: per-thread pending/blocked sets plus per-process shared pending; dequeue at `process_return_to_user` **and** on the `sysretq` path, where the syscall frame (`trapframe.h:43`) must become rewritable to redirect rip/rsp/rdi/rsi/rdx and force an `iretq`-style return when a frame is pushed (this is also where the SYSRET canonical guard of 14.2 becomes mandatory); an x86-64 `rt_sigframe` with `sa_restorer` (musl always sets it), `ucontext` GPRs and an **FP state pointer (requires FPU save)**, `siginfo`; `rt_sigreturn`; `-ERESTARTSYS` internally with SA_RESTART rewinding rip by 2 and reloading rax; default actions and SIGCHLD; fault → SIGSEGV/SIGBUS/SIGILL/SIGFPE with `si_addr`; thread-directed vs process-directed once `clone(CLONE_THREAD)` exists. AArch64 has the same shape with `fpsimd_context` and `svc #0` nr 139.

### 12.6 Futex sufficiency (§32)

`kernel/ipc/futex.c`: 64 buckets hashed by `(vm_space*, uaddr)` (effectively always private), compare-and-enqueue under the bucket lock (no lost wake), stack waiter, relative timeout, `-EINTR` only on kill, no spurious wakeups. musl needs and gets `FUTEX_WAIT`/`WAKE` (mutex, cond, barrier, sem). Missing for pthreads: `FUTEX_WAIT` on the tid word cleared by `CLONE_CHILD_CLEARTID` (no `clone`, `clear_child_tid` unused), `FUTEX_REQUEUE`/`CMP_REQUEUE` (musl `pthread_cond_broadcast`), PI and robust lists. Shared futexes across processes are impossible today (no shared memory) and a future shared-memory feature must key by physical page. Defects: copy under spinlock (4.2), and the store-buffer wake ordering (4.2).

### 12.7 Tests

`lxtest.c` covers identity, uname, clocks, TLS survival, brk grow/shrink, mmap/mprotect/munmap/MAP_FIXED, files, getdents64, pipes/dup/fcntl, sigaction/sigprocmask storage, wait4/kill/execve/fork ENOSYS, futex EAGAIN/ETIMEDOUT/WAKE/ENOSYS, getrandom, UDP loopback, unknown numbers; `hello_musl` end to end when `musl-gcc` exists; host `test_linux.c` for conversions. Gaps: no futex wake from another context (no threads); no multi-step brk shrink; no MAP_FIXED over an existing mapping; no partial mprotect; no ET_DYN rejection test; no non-fatal `kill` test (would expose the HIGH); no aarch64 Linux test at all.

---

## 13. AArch64 Audit

### 13.1 What stage 1 delivers

QEMU `virt` with GICv2 + GICv2m, cortex-a72, edk2 firmware, protocol v4 with `boot_pagetable_root_user`, TTBR1/TTBR0 split on VA bit 55, identical VA layout to x86, vector table with a frame per used slot (`vectors.S`), PSCI CPU_ON via HVC/SMC with an identity-mapped trampoline page, absolute CNTP_CVAL timer, PAN via `.inst` encodings when present, semihosting exit, per-arch panic markers, module relocation for the full AArch64 relocation set with a host test, `arch_mmu_near_arena` for CALL26 range, `arch_dma_barrier`. CI runs the full boot test, release build, host tests, analyze, reproducible check and crash test on aarch64.

### 13.2 Findings

| Sev | Finding | Evidence |
|---|---|---|
| MEDIUM | `SCTLR_EL1.WXN` is explicitly cleared by the loader and never set by the kernel; kernel W^X rests solely on per-leaf PXN/UXN. UAO, E0PD, BTI and PAC are unused. On a core without PAN (`has_pan=0`) there is no SMAP equivalent at all. | `boot/uefi/arch/aarch64/cpu.c:77`; `kernel/arch/aarch64/cpu.c:53-59`; `mmu.c:104-106,309-311`; `user.c:30-40` |
| MEDIUM | ASID 0 for every user space and `tlbi vmalle1is` on every user context switch flushes the TLB on **all** CPUs; every page-table page is forced into ZONE_DMA32 forever. | `aarch64/mmu.c:68,377-380` |
| MEDIUM | `gic.g_lock` is taken on every IPI send, under `runqueue.lock`; the GIC is the only vector allocator on this arch and a single lock for the whole machine. GICv3 panics. | `gic.c:55,106-107,364-381` |
| MEDIUM | FIQ and SError from EL0 route to `bad_*` slots that panic then `wfi`-loop; a user-triggerable SError (e.g. a bad device access via a future mapping) is a kernel DoS. | `vectors.S:88-107,150-172`; `trap.c:128-131` |
| LOW | FP/SIMD is never enabled (`CPACR_EL1.FPEN` untouched, no `fpsimd` state anywhere), consistent with `-mgeneral-regs-only` userland; the first FP-using Linux binary will trap. Not a bug today, a documented gap. | grep of `kernel/arch/aarch64` |
| LOW | `arch_dma_barrier` is `dsb sy` and `dma_sync_for_cpu` a seq_cst fence; adequate for coherent QEMU, but no driver calls `dma_sync_for_cpu`/`dma_unmap` (10.2), so the first non-coherent SoC will expose them. | `dma.c`; `virtio_*.c` |

Verified correct: PAN toggling around user copies; BRK advances ELR; AP mailbox fields read before MMU enable; identity-mapped trampoline page only; `map_early_devices` carries PL011/fw_cfg into the kernel root; `arch_mmu_kernel_base` = 0xFFFF800000000000; the vector table builds a frame for every slot so the panic can print it; `save_sp_user` records SP_EL0 for user frames.

### 13.3 Stage 2 prerequisites

- **Linux ABI**: split `linux_abi.h` into common + per-arch (aarch64 uses asm-generic numbers: `openat=56`, `read=63`, no `open`/`stat`/`pipe`/`dup2`/`fork`/`arch_prctl`); a 128 B `struct stat` converter with different field order and widths; O_DIRECTORY/O_NOFOLLOW/O_LARGEFILE octal values (`linux_abi.h:137-140` are x86's); `struct k_sigaction` without `sa_restorer` (24 B); TLS is `TPIDR_EL0` written by user code (already preserved, `aarch64/context.c:26`); `uname.machine`; real `AT_HWCAP`; build `tests/linux` for aarch64 (`linux.mk` currently excluded). Dispatch (`aarch64/trap.c:handle_syscall:71-77`), personality selection, `convert.c` (except stat), auxv, futex, brk/mmap are already generic.
- **EL2 virtualisation**: see 11.5.
- **Hardware beyond QEMU virt**: GICv3 (currently panics), PSCI variations, device tree (only ACPI is parsed), non-coherent DMA, FP/SIMD context, errata table.

---

## 14. Security Audit

### 14.1 Hardening state

| Control | x86-64 | AArch64 | Evidence |
|---|---|---|---|
| Kernel W^X | linker PHDRs, `-z noexecstack -z separate-code`; modules RX/R/RW after relocation | leaf PXN/UXN only; WXN cleared | `linker.ld:14-20`; `module.c:325-329,368-370`; `aarch64/cpu.c:77` |
| NX / PXN | mandatory (panic if absent) | UXN/PXN per leaf | `x86_64/mmu.c:66`; `aarch64/mmu.c:104` |
| SMEP / SMAP / UMIP / PAN | enabled if present, silently absent otherwise | PAN + SPAN if present | `cpu.c:89-109`; `aarch64/cpu.c:53-59` |
| Kernel stack guard pages | yes, every thread and AP #DF stack; boot #DF stack is a static array | yes | `thread.c:146-147`; `gdt.c:57,66,146` |
| Separate IRQ stacks | no (interrupts run on the 16 KiB thread stack; IF re-enabled at `syscall_entry.S:52`) | no | `thread.h:27` |
| IST | #DF only | n/a | `idt.c:50-63` |
| Stack protector | `-fno-stack-protector` everywhere | same | `build/toolchain.mk` |
| KASLR / ASLR | none (static `0xFFFFFFFF80000000`, ET_EXEC only, fixed stack and brk) | none | `linker.ld:12`; `elf.c:52-53`; `process.h:31,85` |
| Speculative-execution mitigations | none; no PTI; policy undocumented | none | grep |
| SYSCALL entry | SFMASK clears IF/TF/DF/AC/NT; `KERNEL_GS_BASE=0`; TSS.rsp0 published per switch | SVC via vector table | `user.c:41,44,58-59`; `context.c:41-42` |
| SYSRET canonical guard | none (safe today, see 4.1) | n/a | `syscall_entry.S:77-83` |
| User `rdtsc`/`rdpmc` | allowed / disabled (PCE=0) | – | `cpu.c` |
| Secure boot | UEFI loader verifies nothing; kernel and boot.tar unsigned | same | `boot/uefi/elf.c` |
| Module signing | Ed25519 over whole ELF, verified before parsing | same | `modsig.c:18-66` |
| Randomness | SHA-512 pool; only real source is virtio-rng; no RDRAND/RDSEED/RNDR, no jitter; never blocks | same | `random.c:28-36`; `virtio_rng.c:49` |
| Crypto | SHA-512/Ed25519 tested against RFC 8032 host vectors; not constant-time (verify-only) | same | `test_crypto.c:31,104` |
| Credentials | `{uid,gid}` inherited, always 0/0; no setuid, no capabilities, no rlimits, no namespaces | same | `process.h:36-39`; `process.c:252,273` |

### 14.2 Findings (security-specific; overlaps with earlier sections are referenced, not repeated)

| Sev | Finding | Evidence |
|---|---|---|
| CRITICAL | NMI/#MC no IST; SWAPGS windows (4.2). | `idt.c:50-63`; `syscall_entry.S:25-28,80-83`; `isr.S:56-58` |
| CRITICAL | Signing private key in the repository (10.2). | `tools/keys/cosmo-dev.key`; `module.mk:17` |
| CRITICAL | No access control anywhere: files unchecked, every process uid 0, no privilege transition (8.2, 4.3). The effective model is "handles as capabilities plus separate address spaces", and handle rights are READ/WRITE only. | `vfs.c:568-651`; `process.c:252,273`; `native.c:382,409,754`; `socket.c:133`; `hvsys.c:52` |
| HIGH | No stack protector, no kernel sanitizer build option, interrupts on 16 KiB thread stacks. | `toolchain.mk`; `thread.h:27` |
| HIGH | Kernel stack disclosure over the network (9.2); unprivileged `klog` exposes kernel VAs (arena addresses, every process's entry address) and `sys_log` is an unrate-limited klog write. | `udp.c:255-263`; `native.c:861-877,202-216`; `vmm.c:242`; `process.c:381` |
| HIGH | Unbounded resource consumption by unprivileged code: ELF `p_memsz` (4.2), ramfs `/tmp` (8.2), lazy kernel-region population (5.2), VM memory pinning (11.3), SYN flood (9.2), unbounded page cache. No rlimit of any kind. | as cited |
| HIGH | No secure or measured boot chain: module signing roots trust in an unverified kernel image. | `boot/uefi/elf.c` |
| MEDIUM | aarch64 WXN cleared; no UAO/E0PD/BTI/PAC; PAN-less core has no SMAP equivalent (13.2). | `boot/uefi/arch/aarch64/cpu.c:77` |
| MEDIUM | SMEP/SMAP/UMIP absence is silently accepted; `arch_user_access_begin` becomes a no-op. | `cpu.c:89-109`; `user.c:99-109` |
| MEDIUM | No speculative-execution mitigation and no documented policy (acceptable for TCG, must be stated). | grep |
| MEDIUM | SYSRET path will need a canonical guard the moment signal delivery rewrites the frame (12.5). | `syscall_entry.S:77-83` |
| MEDIUM | Entropy: without virtio-rng the pool is deterministic yet `random_get_bytes` never blocks or warns; `AT_RANDOM` and any future ASLR would be predictable. | `random.c:28-36` |
| MEDIUM | `PROT_NONE` readable (5.2); `mmap`/`mount`/`umount` accept unknown flag bits (4.2). | `native.c:156,170-171,394,413` |
| LOW | `hook_fatal` logs a kernel `%p` for kernel-mode faults; `#BP` gate is DPL3 by design; Ed25519 `memcmp` is fine for verify-only. | `process.c:83`; `idt.c:57-59`; `ed25519.c:198,264` |

### 14.3 Containers and capabilities readiness (§53)

Nothing exists: no namespaces (mount, pid, net, uts), no cgroups-style limits, no seccomp-style syscall filter, no per-handle rights beyond READ/WRITE, no rights reduction on dup, no uid transitions. The kobject/handle model is the right foundation for a capability design, but needs a rights vocabulary (DUP, TRANSFER, MANAGE, per-type control rights), a rights-reducing dup, an "exiting" gate on lookups, and resource accounting attached to `struct process`. The VFS needs permission checks and per-process roots before mount namespaces are meaningful.

---

## 15. Performance Audit

No benchmark of any kind exists in the tree (grep for bench/latency in `kernel/` and `tests/` is empty), so this section reports structural costs, not measurements.

### 15.1 Per-operation costs by inspection

| Operation | Cost centres | Evidence |
|---|---|---|
| Page fault (demand-zero) | `vm_space.lock` irqsave + linear region scan + `zone.lock` + memset + `arch_mmu_map` | `vmm.c:507-547` |
| `read()`/`write()` on any object | 1 KiB stack bounce per chunk; two range checks per chunk; region list walk per `copy_*_user` | `native.c:36,100-101`; `vmm.c:790-818` |
| Thread exit | reaper wake, `vm_kernel_free` → per-32-page shootdown broadcast to all CPUs under a global lock, wait for all acks | `thread.c:219`; `vmm.c:317,352`; `mmu.c:255-291` |
| Context switch (aarch64) | full `tlbi vmalle1is` (all CPUs) per user switch | `aarch64/mmu.c:377-380` |
| Cross-CPU wake | `rq.lock` remote, IPI; aarch64 adds `gic.g_lock` | `sched.c:299-315`; `gic.c:364-381` |
| TCP packet RX | cluster alloc/free, 2 `mbufq` lock trips, `g_lock` once (plus a mutex under it), linear pcb lookup, byte-at-a-time checksum, 3 copies, one worker thread | `tcp.c:790-806`; `cksum.c:8-19` |
| TCP segment TX | `copy_from_user` + 3 more copies, `m_getcl` + `m_append` per segment, 1500 B stack buffer under IRQ-off lock | `tcp.c:297-302,665` |
| cosmofs metadata op | one `cfs->lock` for the whole filesystem, synchronous one-bio-at-a-time I/O, every directory lookup reads all directory blocks from the pool | `cosmofs.c:289-335`; `blk.c:135-164` |
| cosmofs commit | MRU-order buffer writes + FLUSH + superblock + FLUSH, all under `cfs->lock` and `g_mounts_lock` | `cosmofs_core.c:460-525`; `vfs.c:348-357` |
| VM entry/exit | `FLUSH_ALL` TLB (host included) + full VMCB reload every VMRUN | `svm.c:240` |
| Interrupt dispatch | shared-cache-line atomic count per vector; all device IRQs on CPU 0 | `interrupt.c:119` |
| Timer tick | 250 Hz on every CPU including idle; sorted list O(n) insert | `timer.c` |

### 15.2 Scalability summary

Section 5.3 and 6.3 give the per-CPU-count tables. The short form: the system is designed for 4 CPUs and will run at 16 with visible contention on zone locks, the TCP lock and the RX worker; at 64 the global shootdown/call slots, `gic.g_lock`, CPU-0 interrupt affinity, the missing balancer and the AArch64 full flush dominate; beyond 64 the `cpumask_t` type and xAPIC addressing make it impossible.

### 15.3 What a benchmark suite must measure first (§43)

Context switch and wakeup latency (same-CPU and cross-CPU), IPI round trip, shootdown cost vs CPU count, spinlock acquire under contention, page-fault throughput, `read`/`write` bandwidth at 1 KiB chunking, pipe throughput, TCP loopback and virtio-net throughput with copy counts, cosmofs create/write/commit latency, VM entry/exit round trip, boot time to `boot complete`. Each should run in the boot harness with a `BENCH:` marker so regressions are caught in CI without a separate infrastructure.

---

## 16. Testing / Fuzzing Audit

### 16.1 What exists

- **Boot harness** (`tests/boot/run_boot_test.py`, 329 lines): two independent verdicts must agree (QEMU debug-exit value and serial markers: loader banner, kernel banner, `SELFTEST: PASS`, `USERTEST: PASS`, `SHTEST: PASS`, `LINUXTEST: PASS|skipped`, `HVTEST: PASS|skipped`, `boot complete`); forbidden markers; `--expect-panic` for `test-crash`; drives `shelltest.py` and `nettest.py` on threads; 180 s timeout; per-arch panic markers.
- **Kernel self-tests**: 70 entries (`kernel/core/selftest.c`), debug builds only, across memory, scheduler, SMP, module, device, IPC, process, tty, VFS, cosmofs, network, virtualization.
- **Host tests**: 10 ASan/UBSan binaries (buddy, slab, crypto, modelf, cosmofs, libc, pkg, linux, hv/NPT, reloc_aarch64).
- **User-side**: `init --selftest`, `lxtest`, `hello_musl` (when musl-gcc exists), `shelltest.py`, `nettest.py`.
- **CI**: one job per arch on Debian trixie: check-tools → debug build → boot test → release build + test → host-test → analyze → reproducible → test-crash, artefacts uploaded.
- **Test hooks**: `kernel/include/arch/testhooks.h` provides breakpoint/fault hooks and a periodic IRQ source only.

### 16.2 What is missing

| Gap | Severity | Note |
|---|---|---|
| Fuzzing: none | HIGH | `tests/README.md:2` claims "property and fuzz tests". Ready targets that already compile on the host under ASan: `elf_validate`, `modelf_validate`, cosmofs mount/image parse, mbuf/IP/TCP parsers, `pkg` manifest/tar, `vsnprintf`/`strtol`. A guest-side syscall fuzzer (random `nr`/args from `init`) needs only the existing `USERTEST` plumbing. |
| Fault injection: none | HIGH | No allocation-failure injection, no virtio-blk error injection, no packet-loss knob beyond the deterministic 1-in-7 test. The whole "poison the mount" and "OOM kills the process" model is untested. |
| Crash-consistency harness: none | HIGH | The cosmofs "crash" test is a software discard on unmount plus one torn-superblock flip. Needed: a host-side block device that records the write stream and replays every prefix (and reordered prefixes within a flush epoch), then mounts and checks structural validity. `test_cosmofs.c` already links the on-disk code on the host. |
| Concurrency tests: none | HIGH | Every kernel test is single-threaded from the test's point of view; there is no test with two CPUs racing close vs RX, unlink vs lookup, rename vs rmdir, or `timer_cancel` vs callback. |
| Benchmarks: none | MEDIUM | See 15.3. |
| Coverage: none | MEDIUM | No `-fprofile-instr-generate` host build; no line coverage of self-tests. |
| Statistics transport: none | MEDIUM | `dma_stats`, `kmalloc_stats`, `mbuf_stats`, `netif_stats`, `pagecache_stats`, `pipe_stats`, `cosmofs_stats`, `hv_stats`, scheduler and shootdown stats exist as structs and are printed only by self-tests; `sysctl` has 15 static names; no `/proc`, no `/sys`. |
| Panic quality | MEDIUM | Frame-pointer backtrace of up to 32 PCs with no symbolization (`ksym_lookup` is name→addr for modules only), hard-coded `context: boot (no threads yet)`, no current pid/thread, no per-CPU state. Log ring has no timestamps or sequence numbers. |
| Monolithic verdict | LOW | One `SELFTEST: PASS` for 70 tests; a single failure hides the rest and there is no per-test timing. |
| Missing unit tests | LOW | x86-64 `modreloc` host test; virtqueue with a fake transport; network stack on the host. |

### 16.3 Verification model gap

The constitution's first priority is correctness and the codebase's security argument is "every parser is bounds-checked". That argument is currently supported by inspection and by positive-path tests; the audit found no fuzzer, no negative-path corpus, and no adversarial device model. Sections 9.2 (ICMP stack leak) and 10.2 (descriptor `next`) are exactly the class a fuzzer and a hostile virtio device model find in minutes.

---

## 17. Architectural Debt

Ranked by how many findings trace to each item and how many future subsystems it blocks.

1. **No object-quiescence primitive and refcounts without owning release.** `device`, `blkdev`, `virtio_device`, `netif`, `tcp_pcb`, interrupt slots, timers and module text are all freed while another context may hold a pointer (10.2 F4/F5, 9.2 H2/H3/H4/M4, 4.2 interrupt and timer entries). Blocks safe hot-unplug, module unload, NVMe, multithreading, async I/O. → Section 20.
2. **Single-thread-per-process is a load-bearing, unwritten invariant** in uaccess (no fixup table), handle composites, exit/handle teardown, futex, `clear_child_tid`. Adding `clone` without first fixing these turns latent HIGHs into panics.
3. **Region-granular user VMM** with no split/merge, no file-backed or shared region kind, no COW, no `PROT_NONE`, racy `pmm_page_put`, no rmap. Blocks dynamic linking, RELRO, `mmap(fd)`, ASLR for PIE, fork, shared memory.
4. **Arch entry paths assume benign hardware**: NMI/#MC without IST and non-paranoid SWAPGS logic, non-atomic ICR writes, SYSRET without canonical guard, interrupts on thread stacks, no FPU state ownership model, CR4 differing between BSP and APs.
5. **Global single-slot SMP protocols** (`g_call_*`, `g_shootdown_*`) with 1 s panics, plus `cpumask_t = uint64_t`, xAPIC-only, `gic.g_lock` per IPI, all device IRQs on CPU 0, no migration or balancing. Hard ceiling at 64 CPUs, practical ceiling ~16.
6. **Filesystem semantics that hold only on a lightly used disk**: commit only at sync/umount, no metadata reserve, 264-extent cap without hole support, unchecksummed data and directory blocks, raw block numbers with no DVA, one lock, synchronous one-bio I/O, no fsck/scrub/redundancy, error model "poison the mount".
7. **Network stack lifetimes and hardening**: one lock, one worker, byte rings blocking zero-copy, ad-hoc pcb lifetimes, no SYN cookies/RFC 5961/PMTUD/reassembly/keepalive, blocking-only socket API.
8. **No privilege model**: uid always 0 and immutable, no permission checks in VFS, no rlimits, handle rights READ/WRITE only. Every "uid == 0" check in the tree is dead code.
9. **Verification depth**: no fuzzing, fault injection, crash-consistency harness, concurrency tests, benchmarks or coverage; statistics without a transport; unsymbolized panics.
10. **HV UAPI carries SVM encodings and x86 register names**; `arch_hv_vm_map` has no permission or size parameters. Blocks VMX, AArch64 EL2, dirty tracking, ballooning, snapshot, passthrough.
11. **Trust chain and keys**: dev private key in the repository, unsigned kernel and boot archive, no rollback protection, no key revocation.
12. **Defence in depth**: no stack protector, no KASLR/ASLR, no kernel sanitizer build, no aarch64 WXN/UAO/BTI, single entropy source, undocumented speculative-execution policy.
13. **Documentation drift in exactly the places that matter for concurrency**: three subsystems document a lock order the code does not follow (scheduler S2/S4, VFS V7, network design.md), and there is no lockdep to notice. Plus the smaller drift items in 3.2.
14. **Placeholders and empty directories**: `drivers/{network,nvme,storage}` empty, 52-line storage pool, init as service manager, no `/proc`/`/sys`.

---

## 18. Critical Findings

Consolidated CRITICAL and HIGH findings, ranked by exploitability today, then by blast radius. Section references give the detail and evidence.

| # | Sev | Finding | Reach | Section |
|---|---|---|---|---|
| 1 | CRITICAL | TCP `seg_mss` takes the `g_netif_lock` mutex under the IRQ-off TCP spinlock | remote, every SYN/connect/retransmit | 9.2, 7.3 |
| 2 | CRITICAL | `virtq_add` trusts device-writable `desc[].next`; OOB kernel write | any misbehaving or hostile virtio device | 10.2 |
| 3 | CRITICAL | No FPU/SIMD state save; guest ↔ owner and process ↔ process XMM leak/corruption; CR4.OSFXSR differs per CPU | any VM guest, any SSE-using Linux binary | 4.2, 11.3, 12.3 |
| 4 | CRITICAL | LAPIC ICR_HI/ICR_LO not atomic vs local interrupts; cross-call to wrong CPU then 1 s panic | any `smp_call_function_single` under tick load | 4.2, 6 |
| 5 | CRITICAL | NMI/#MC without IST; SWAPGS windows on SYSCALL entry/exit | latent under QEMU; real on hardware with NMIs | 4.2, 14.2 |
| 6 | CRITICAL | No access control: files unchecked, every process uid 0, no privilege transition | every process | 8.2, 14.2 |
| 7 | CRITICAL | Module signing private key tracked in git and default | anyone with the repo | 10.2, 14.2 |
| 8 | HIGH | Kernel stack bytes leaked into ICMP Port Unreachable (IP options, IHL > 5) | remote | 9.2 |
| 9 | HIGH | `ksock_accept` UAF (RST before attach); UDP close-vs-RX UAF; timer-callback-vs-`pcb_free` UAF | remote timing | 9.2 |
| 10 | HIGH | SYN flood locks out listeners (backlog 16, 128 KiB per SYN, ~4 min) | remote | 9.2 |
| 11 | HIGH | Kernel-mode demand-zero fault under OOM panics instead of `-EFAULT` | local unprivileged | 4.2 |
| 12 | HIGH | ELF `p_memsz` unbounded; populated under IRQ-off lock | local unprivileged | 4.2 |
| 13 | HIGH | User fault on lazy kernel region populates kernel memory | local unprivileged | 5.2 |
| 14 | HIGH | `futex_wait` copies from user under a spinlock; fatal fault leaks the bucket lock | local unprivileged | 4.2, 7.3 |
| 15 | HIGH | Store-buffer lost wakeup in `futex_wake`/`process_kill` on x86 (plausible) | local | 4.2 |
| 16 | HIGH | Cross-call/shootdown 1 s panic when caller holds a spinlock a target spins on; every thread exit broadcasts under a global lock | kernel-internal, load-dependent | 4.2, 6.2 |
| 17 | HIGH | `pmm_page_put` non-atomic (latent primitive for COW/shared memory) | latent | 5.2 |
| 18 | HIGH | Kernel-half PML4 snapshot vs lazy PDPT creation (latent, > 512 GiB arena) | latent | 5.2 |
| 19 | HIGH | uaccess check-then-copy with no fixup table (latent until threads) | latent | 4.2 |
| 20 | HIGH | VFS rename ABBA; racy ancestor-loop check; unlocked `..` | local, two processes | 8.2, 7.3 |
| 21 | HIGH | `vnode_lookup_cached` check-then-get → duplicate vnodes per inode | local timing | 8.2 |
| 22 | HIGH | fsync not durable; commit only at sync/umount; unbounded loss window | any crash | 8.2 |
| 23 | HIGH | cosmofs full-disk deadlock; 264-extent `-EFBIG` cap; sparse-write exhaustion | local | 8.2 |
| 24 | HIGH | Unbounded page cache and ramfs; any process exhausts RAM via `/tmp` | local unprivileged | 8.2 |
| 25 | HIGH | Block `-EAGAIN` treated as I/O error → cosmofs poison under bursts | load | 8.2 |
| 26 | HIGH | Directory and data blocks unchecksummed | disk corruption | 8.2 |
| 27 | HIGH | 32-bit DMA mask never widened; > 4 GiB hosts fail I/O with `-EINVAL` | hardware | 10.2 |
| 28 | HIGH | Device/blkdev/virtio_device freed with outstanding references; no IRQ grace period; module unload frees live text | hot-unplug, unload | 10.2 |
| 29 | HIGH | XSETBV/WBINVD/RDPMC/RDTSCP not intercepted; string I/O mis-reported; VM limits global | VM guest | 11.3 |
| 30 | HIGH | Linux `kill` with non-fatal signal terminates target; region-granular mmap/mprotect/brk; no non-blocking I/O | Linux binaries | 12.3 |
| 31 | HIGH | No stack protector; interrupts on thread stacks; unprivileged `klog` leaks kernel VAs | hardening | 14.2 |
| 32 | HIGH | No secure boot chain | trust | 14.2 |
| 33 | HIGH | Zero fuzzing, fault injection, crash-consistency or concurrency tests | verification | 16.2 |

---

## 19. Recommended Next 10 Milestones

Ordered per the constitution's priorities (correctness, cleanliness, observability, security, portability, performance) and §69's suggested sequence. Each is one PR-sized unit with design docs first, regression tests, and the established verification chain.

| # | Milestone | Scope | Closes |
|---|---|---|---|
| 1 | **Critical-fix pass** (point fixes, no new subsystem; regression test per fix) | IST for NMI/#MC/#DB and paranoid SWAPGS entry; `arch_irq_save` in `icr_send`; `seg_mss` reads netif ownership without the mutex (snapshot or spinlock); `virtq_add` uses a driver-private `next[]` shadow; `udp_input` quotes from the mbuf not the stack copy; `pmm_page_put` via `fetch_sub`; `vm_fault_handler` refuses user faults on kernel addresses; `elf_validate` total `p_memsz` bound; `lx_kill` honours default-ignore signals; rotate the dev signing key out of the tree (keep a documented dev-only path); FPU: enable and save/restore XSAVE state around `svm_run` and per thread (or, as an interim, trap SSE in Linux personality and refuse guests with CR0.EM clear, explicitly documented) | 18: #1–#8, #12, #13, #17, part of #3 |
| 2 | **Kernel object lifetime and quiescence hardening** | Section 20 | 18: #9, #16 (partly), #28, prerequisite for 4–7 |
| 3 | **Lock discipline and lockdep** | Runtime lock-order checker in debug builds (per-lock class, held-lock stack per CPU, sleeping-under-spinlock assertion, IRQ-safety class check); fix documented orders (S2/S4, V7, network design.md); rename lock order (parent → child with a per-mount rename mutex); `vnode_lookup_cached` fix; `futex_wait` copy outside the lock; `vfs_sync` releases `g_mounts_lock` per mount | 18: #14, #20, #21, part of #1 class |
| 4 | **Verification infrastructure** | libFuzzer harnesses for ELF/modelf/cosmofs image/IP-TCP parsers/pkg on the host; guest syscall fuzzer via `USERTEST`; allocation and block-error fault injection behind a debug sysctl; crash-consistency harness (record/replay prefixes of the write stream on the host); per-test markers and timing; hostile virtio device model in a host test | 18: #33 |
| 5 | **uaccess fixups and user VMM regions** | Exception fixup table on both arches (kernel-mode fault at user address → `-EFAULT`); region split/merge; `PROT_NONE`; correct `brk` shrink; `MAP_FIXED` replace; per-space CPU mask for shootdown; pre-populate kernel-half PDPTs at `vmm_init` | 18: #11, #18, #19, #30 (region part) |
| 6 | **Access control and resource limits** | VFS permission checks; creator uid/gid; `setuid`-class or capability transition (decision recorded in a design doc); rlimits for memory, handles, processes, VM memory; ramfs and page-cache caps with reclaim; privilege gate on `klog`/`procinfo` | 18: #6, #24, part of #31 |
| 7 | **Filesystem transaction engine** | Writeback thread and dirty thresholds; `fsync` commits; metadata reserve; hole-capable extents with contiguity-aware allocation; data/directory checksums stored in parent pointers with an algorithm id; older-slot fallback at mount; `bio` FUA/PREFLUSH flags and queueing so `-EAGAIN` never surfaces | 18: #22, #23, #25, #26 |
| 8 | **Network stack hardening and per-connection locking** | Refcounted `netif` and `tcp_pcb`; per-pcb locks with a hashed table; SYN cache/cookies; RFC 5961; FIN_WAIT_2/keepalive timers; ICMP rate limit; PMTUD; out-of-order reassembly; `O_NONBLOCK` and a readiness op on `kobject_io_type` (prerequisite for async I/O) | 18: #9, #10 |
| 9 | **Async I/O and the block layer for NVMe** | Completion-based I/O ring over the readiness op; multi-segment bios; request timeouts; 64-bit DMA masks with a real map/unmap discipline; per-CPU MSI-X routing; NVMe driver in `drivers/nvme/` | 18: #27; §23, §26 |
| 10 | **Linux personality stage 2** | Signals (frames, sigreturn, restart, defaults) with the SYSRET canonical guard; ET_DYN/PIE and `PT_INTERP` with file-backed `mmap`; `poll`; `clone(CLONE_THREAD)` with futex requeue and `clear_child_tid` (after milestones 2, 5); wall clock; aarch64 table split | 18: #30 |

After these: multi-queue networking and zero-copy (§20–21), IOMMU (§25), Intel VMX behind an `hv_caps` bit and a translated `attrib`, AArch64 EL2, snapshots/redundancy/compression/encryption, containers, service manager and `/proc`.

---

## 20. NEXT SUBSYSTEM: Kernel Object Lifetime and Quiescence Hardening

### Problem statement

Objects that are reachable from interrupt handlers, timer callbacks, the network worker and other CPUs are freed by code that cannot know whether such a reader is still active. The kernel has reference counts on some of these objects, but for several of them the count has no owning release (`device_release` and `blkdev_release` are no-ops), for others there is no count at all (`netif`, `tcp_pcb`), and for the two most important asynchronous readers, interrupt handlers and timer callbacks, there is no primitive that says "no invocation is still running". The audit traced eleven HIGH findings across five subsystems to this one gap (18: #9, #16, #28; 9.2 H2/H3/H4/M4; 10.2 F4/F5; 4.2 interrupt and timer entries), and every planned subsystem that removes objects while the system runs (hot-unplug, module unload, multithreading, async I/O, NVMe, containers) depends on it.

### Current implementation

- `struct kobject {type, refcount}` with atomic get/put and `release` on last put (`kernel/object/object.c:15-29`). Correct where the release owns the memory (files, pipes, sockets, VMs, vCPUs, vnodes).
- `struct device`, `struct blkdev`, `struct virtio_device`: kobjects whose `release` frees nothing (`device.c:45-49`; `blk.c:19-22`); drivers embed them in their own containers and `kfree` those on remove (`virtio_blk.c:229-246`; `virtio_pci.c:367-375`) while `pool_open`/VFS still hold `blkdev_get` references (`pool.c:17`; `vfs.c:199`).
- `struct netif`: no refcount; `netif_unregister` unlinks and flushes ARP (`netif.c:130-138`); queued mbufs keep `pkt.rcvif` (`netif.c:232`).
- `struct tcp_pcb`: no refcount; lifetime by state machine plus a `WORK_FREE` deferral and a `sock` back-pointer (`tcp.c:180-229`).
- Interrupt slots: lock-free publish (`interrupt.c:45-127`); `interrupt_unregister` returns immediately; comment at `interrupt.c:5-9` acknowledges the missing grace period.
- Timers: per-CPU queues; `timer_cancel` removes a pending timer but cannot wait for a RUNNING callback (`timer.c:119-148`); safe only because timers run on the arming CPU and threads never migrate.
- Module unload: `shutdown()` then immediate free of text (`module.c:424-433`).
- Quiescent-state infrastructure that already exists and can be reused: `irq_depth`/`preempt_count` per CPU (`percpu.h:34-51`), the IRQ-return preemption check on both arches, `smp_call_function_single`, `cpu_online_mask`.

### Why it matters

Correctness first: these are use-after-free bugs reachable by network timing today (9.2 H2–H4) and by any future unplug or unload. Cleanliness: the constitution's ownership rules (§48 of the master prompt: every object has one owner and a documented lifetime) are currently satisfied by comments, not by mechanism. Security: use-after-free in kernel objects is the primary exploitation primitive class; closing it structurally is worth more than any single fix. Everything in milestones 5–10 removes objects at runtime.

### Proposed design

Three small mechanisms, one rule.

1. **Owning release for every kobject.** `struct kobject_type` gains a mandatory `release(struct kobject *)` that frees the *container* (via `container_of`), and `kobject_put` asserts that the type has one. `device`, `blkdev`, `virtio_device`, `pci_device` (freed only at hot-remove, never at boot), `netif` (becomes a kobject) and `tcp_pcb` (becomes a kobject; `sock` holds a reference; the retransmit timer and the accept queue hold references) get real releases. Drivers stop `kfree`-ing containers on remove; they `kobject_put` and let the last holder free. Handles held by user processes are unaffected (already counted).

2. **A grace-period primitive: `quiesce`.** Epoch-based reclamation. A CPU is quiescent only when it is provably outside every read-side section, and read-side sections are exactly the preempt-disabled regions, so the quiescent state is defined as: (a) the IRQ-return path is about to resume a context with `irq_depth == 0` **and** `preempt_count == 0` **and** the interrupted context had interrupts enabled (the same three-part predicate the preemption check already evaluates at `x86_64/trap.c:81-83` and `aarch64/trap.c:109-110`), or (b) the CPU passes through `schedule()` (which cannot be entered with `preempt_count > 0`). An interrupt that lands inside `quiesce_read_lock()`, inside any spinlock, or inside any other preempt-disabled region therefore does **not** record a quiescent state on return, because `preempt_count` is still non-zero for the interrupted context; that CPU stays non-quiescent until the reader finishes and the next tick or `schedule()` observes `preempt_count == 0`. Nested interrupts are covered by the `irq_depth` term. API:
   - `void quiesce_read_lock(void)` / `quiesce_read_unlock(void)`: mark a read-side section; in this kernel they are `preempt_disable()`/`preempt_enable()` plus, in debug builds, a per-CPU depth counter for assertions. Interrupt handlers are implicitly read-side (IRQs off, `irq_depth > 0`). A read-side section must not block; `quiesce_read_unlock` asserts this the same way `preempt_enable` does.
   - `void synchronize_quiesce(void)`: sleeping; waits until every online CPU has passed a quiescent state since the call began. Implementation: bump a global epoch, then wait for each CPU's per-CPU `last_seen_epoch` (a relaxed store made only at the two quiescent points above) to reach it. An idle CPU is counted as quiescent only when it is in the idle loop's `hlt`/`wfi` with `preempt_count == 0`, published by the idle loop itself before it halts; a CPU that is idle but was interrupted inside a preempt-disabled region is not idle by this definition and is waited for. Bounded by one tick (4 ms) per waiting round in the common case; if a CPU has not reported after two ticks the caller sends it an `IPI_RESCHEDULE` so its IRQ-return path re-evaluates the predicate (the reader itself is never interrupted early).
   - `void call_quiesce(struct quiesce_head *h, void (*fn)(struct quiesce_head *))`: non-sleeping deferral; per-CPU callback lists drained by the reaper thread after `synchronize_quiesce`. Used from IRQ-off contexts (`pcb_free_locked`, `virtq_free` from a remove path holding spinlocks).
   - `void synchronize_irq(unsigned vector)`: `interrupt_unregister_vector` followed by `synchronize_quiesce`; since handlers run with IRQs off, one grace period guarantees no handler holds the old `arg`.
   - `bool timer_cancel_sync(struct timer *t)`: `timer_cancel` plus, if the timer is RUNNING on another CPU, spin (IRQs on, preemption off) on the per-queue `running` pointer until it changes; if RUNNING on this CPU (called from the callback), return false and let the caller defer. Requires `timer_queue` to record `running` under its lock (`timer.c:138-150`).

3. **Module unload uses the above.** `module_unload`: `shutdown()` (drivers unregister devices, which puts references), `synchronize_quiesce()`, verify the module's `refs` and a new `live_objects` counter (incremented by kobjects whose type lives in the module, decremented in release) are zero, then free text. If not zero, return `-EBUSY` and leave the module loaded.

**The rule** (added to `docs/kernel/object/invariants.md` and enforced by a debug assertion in `kfree`): memory reachable from an interrupt handler, a timer callback or another CPU without a lock is freed only from a kobject `release` or a `call_quiesce` callback, never directly.

### Affected files

- `kernel/include/kernel/object.h`, `kernel/object/object.c`: mandatory `release`; `live_objects` hook; debug assertion.
- New `kernel/include/kernel/quiesce.h`, `kernel/core/quiesce.c`: epoch counter, per-CPU `last_seen_epoch`, callback lists, `synchronize_quiesce`, `call_quiesce`.
- `kernel/scheduler/sched.c` (`schedule_internal` quiescent mark), `kernel/arch/x86_64/trap.c:78-83`, `kernel/arch/aarch64/trap.c:105-110` (IRQ-return quiescent mark), `kernel/scheduler/thread.c` (reaper drains callback lists).
- `kernel/interrupt/interrupt.c` (`synchronize_irq`), `kernel/timer/timer.c` (`running` field, `timer_cancel_sync`).
- `kernel/device/device.c`, `kernel/block/blk.c`, `drivers/pci/pci.c`, `drivers/virtio/{virtio.c,virtio_pci.c,virtio_blk.c,virtio_net.c,virtio_rng.c,virtio_console.c}`: real releases; remove paths `put` instead of `kfree`; `virtq_free` deferred.
- `kernel-services/network/{netif.c,tcp.c,udp.c,socket.c}`: `netif` and `tcp_pcb` as kobjects; `udp_input` takes a socket reference before waking; `ksock_accept` holds the child reference across attach.
- `kernel/module/module.c`: unload protocol.
- Docs: `docs/kernel/object/{architecture,design,api,invariants,testing}.md` updated; new `docs/kernel/quiesce/` five-file set; lock-order and lifetime sections of `docs/kernel/interrupt`, `docs/kernel/timer`, `docs/kernel/device`, `docs/drivers/virtio`, `docs/kernel-services/network`, `docs/kernel/module`.

### New APIs

```c
/* kernel/include/kernel/quiesce.h */
void quiesce_init(void);                          /* after smp_init */
void quiesce_read_lock(void);                     /* preempt off; debug depth++ */
void quiesce_read_unlock(void);
void synchronize_quiesce(void);                   /* sleeps; every CPU passed a quiescent state */
struct quiesce_head { struct quiesce_head *next; void (*fn)(struct quiesce_head *); };
void call_quiesce(struct quiesce_head *h, void (*fn)(struct quiesce_head *));  /* any context */
void quiesce_note_quiescent(void);                /* called from schedule() and IRQ return */

/* kernel/include/kernel/interrupt.h */
void synchronize_irq(unsigned vector);            /* unregister + grace period; sleeps */

/* kernel/include/kernel/timer.h */
bool timer_cancel_sync(struct timer *t);          /* false if called from t's own callback */

/* kernel/include/kernel/object.h */
struct kobject_type { const char *name; void (*release)(struct kobject *); /* now mandatory */ ... };
```

No UAPI change. Module ABI version bumps because `kobject_type` gains a mandatory field.

### Migration plan

1. Land `quiesce` with its host test and kernel self-tests; no callers yet.
2. Add `synchronize_irq` and `timer_cancel_sync`; convert `virtq_free`, `pci_msix_release` and the network timers; self-tests for cancel-vs-running-callback across CPUs.
3. Make `release` mandatory; give `device`/`blkdev`/`virtio_device` real releases; convert virtio remove paths; self-test: remove a virtio-blk with an in-flight synchronous read (using the existing test hooks to stall the completion), assert `-EIO` and no UAF under ASan-equivalent poisoning (fill freed slabs with a pattern in debug builds, already available via slab misuse checks).
4. Convert `netif` and `tcp_pcb`; fix `ksock_accept` and `udp_input`; nettest additions for RST-before-attach and close-vs-RX on two CPUs.
5. Module unload protocol; `module-load` self-test extended with unload under I/O.
6. Debug assertion in `kfree` and the invariants document; run the whole verification chain on both arches.

Each step is independently mergeable and leaves the tree green.

### Tests

- Host: `test_quiesce.c` with a simulated multi-CPU epoch model (threads as CPUs, ASan/UBSan) covering grace-period completion, idle CPUs, nested read sections, callback ordering, and the `timer_cancel_sync` self-callback case.
- Kernel self-tests: `quiesce-grace` (synchronize with a reader spinning on another CPU, assert ordering), `quiesce-call` (deferred free observed only after every CPU quiesced), `irq-sync` (unregister a vector whose handler is spinning on another CPU, assert it completes before return), `timer-cancel-sync` (cross-CPU), `device-remove-inflight`, `net-accept-rst-race`, `net-udp-close-race`, `module-unload-busy` (unload refused while an object lives), `object-release-mandatory` (a type without release panics at register time in debug).
- Existing suites unchanged; the boot harness gains no new required markers.

### Benchmarks

Because this adds a per-CPU store on IRQ return and in `schedule()`, measure before/after: context-switch latency, IPI round trip, interrupt dispatch cost, TCP loopback throughput, virtio-blk synchronous IOPS. Target: no measurable change (one relaxed store per quiescent point). Also record `synchronize_quiesce` latency under idle and under a 4-CPU spinner load (expected ≤ 2 ticks).

### Risks

- **Grace-period latency** if a CPU is in a long IRQ-off section (the populate loops of 5.2 hold IRQs off for seconds on large images): `synchronize_quiesce` stalls the caller, not the system. Milestone 5's preemption points shorten those sections; until then the API is sleeping and callers must not hold locks.
- **Reference-count churn on the packet path** if `netif` references are taken per packet; design takes one reference per queued mbuf batch, not per packet, and the worker holds the reference for the drain.
- **Module ABI break** (mandatory `release`): all in-tree modules are rebuilt by the same tree; out-of-tree modules do not exist yet.
- **Behavioural change for drivers**: remove paths become asynchronous with respect to memory; a driver that touches its container after `kobject_put` is a bug the debug `kfree` assertion will catch only if the object was freed; documentation and review must carry the rest.
- **Not addressed here**: uaccess fixups, region split/merge, lockdep, FPU state. They are separate milestones and are listed in section 19 with their order; this subsystem does not depend on them and they do not depend on it, except that lockdep (milestone 3) will later verify the "no sleeping lock under a spinlock" rule that `synchronize_quiesce` relies on.

---

*End of audit. Per the governing prompt, no implementation follows this document; the next step is the user's instruction.*
