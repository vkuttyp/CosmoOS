# Roadmap M3: real process model — 2026-10-10

Milestone M3 of the [1.0 roadmap](../roadmap-1.0.md): static musl BusyBox
`ash` is the shell; a fixed list of applets passes a scripted test; a
subset of BusyBox's own testsuite passes; both architectures, in CI. Base:
main `3dcd7436` (M1 and M2 done, CI green). Decisions 1-8 of the task are
the owner's and were not reopened; this report records how each was built.

## Plan as worked

| PR | Content |
|---|---|
| 1 | Personality lifecycle and initial-stack hooks; the kernel-stack guard (`-Wframe-larger-than=2048` with a checked baseline); `vm_space_fork`, the copy-on-write duplication of a user space, with its fault and the `vm-fork` self-test. No new system call. |
| 2 | Linux `fork`, fork-like `clone`, `CLONE_VM|CLONE_VFORK`, `wait4` for children, handle-table duplication. |
| 3 | `execve` (Linux) and `SYS_exec` (native), close-on-exec, `#!`, personality switching on exec, vfork release. |
| 4 | The BusyBox port, the image layout, `make test-busybox` and its CI job; M3 ticked. |

## PR 1 (as built)

| Area | Change | Files |
|---|---|---|
| Personalities | `struct personality` gains `claims_elf`, `init`, `release`, `platform`, `auxv`, `fork`, `exec`; `personality_for_elf` chooses (kernel-created: native; the CosmoOS note: native; otherwise Linux); `process.c` names no personality. The native auxiliary vector moved into the native personality. Only change: a native initial stack no longer carries the unused AT_PLATFORM string. | `kernel/include/kernel/process.h`, `kernel/syscall/personality.c`, `kernel/syscall/native.c`, `compat/linux/syscalls.c`, `kernel/process/process.c` |
| Stack guard | `-Wframe-larger-than=2048` on kernel and modules, an error. Measured on all four builds: ~135 functions over 1 KiB, 33 over 2 KiB, the largest 8.6 KiB. The 33 are wrapped in `FRAME_EXEMPT_BEGIN/END` and listed in `scripts/frame-baseline.txt`; `scripts/check-frame-baseline.py` at the kernel link fails when the two differ. | `build/toolchain.mk`, `kernel/include/kernel/compiler.h`, `scripts/`, 18 source files |
| fork primitive | `vm_space_fork`: phase A links a child mapping record per parent record (parent's tag), phase B copies regions and shares every present page under both space locks (child nested), private pages read-only in both, then shoots the parent's lowered ranges down. Limits inherited, `-ENOMEM` when the child would start over them. | `kernel/memory/vmm.c`, `kernel/include/kernel/vmm.h` |
| Copy-on-write fault | `cow_write_locked`: a write to a present read-only anonymous frame in a private region copies it while shared, takes it back writable when this mapping is the last user. `vm_user_protect` granting write keeps a shared frame read-only. | `kernel/memory/vmm.c` |
| Tests | `vm-fork` (design in `docs/kernel/memory/testing.md`) | `kernel/memory/forktest.c` |

Design: `docs/kernel/memory/design.md` §8; invariants M48-M50;
`docs/kernel/process/design.md`, "Personalities"; `docs/build/design.md`,
"Stack frames".

### The TLB check, twice vacuous before it was a check

`vm-fork`'s TLB check was mutation-tested by removing the parent's
shootdown in `vm_space_fork`:

| Version | x86-64 | AArch64 |
|---|---|---|
| this CPU caches a translation, switches away and back, writes after the fork | passed with the shootdown removed: qemu64 has no PCID, so the switch flushes | — |
| the same where address-space tags exist | — | passed with the shootdown removed: `arch_mmu_protect` invalidates the local entry itself |
| a thread on another CPU keeps the space active (preemption off, interrupts on) | **fails** with the shootdown removed (`t.after == 1`), passes with it | passes either way, correctly: `tlbi vaae1is` is broadcast to every CPU by the hardware, so the protect already reached the other CPU |

The kept version is the third. Probe convention: M3 is a new feature; the
mutation runs above are the evidence the test can fail, not a defect proof.

The guard was also built with CI's own compiler (Debian clang 19.1.7, the
`debian:trixie` container): kernel and modules of the debug, release,
crash, chaos and harness-break builds of both architectures, no frame-size
error and the exemption pragma honoured.
