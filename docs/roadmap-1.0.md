# CosmoOS 1.0 roadmap

Adopted 2026-10-09. This document replaces open-ended hardening as the
source of the next unit of work. [`plan.md`](plan.md) and the
[deferred-work inventory](audit/2026-09-deferred-work-inventory.md) remain
the record of everything else; work from them is taken only when the
current milestone's acceptance test needs it. The scope rules that govern
this roadmap are in [`AGENTS.md`](../AGENTS.md#scope-rules).

## Definition of done

CosmoOS can be installed to a disk, rebooted into, used through a real
shell with standard tools, keeps files across reboots, reaches the network
by name, accepts an ssh login, and has a verified boot chain and sound
randomness, on x86-64 and AArch64, all checked by CI.

## Milestones

Milestones are worked in order. Each is done when its acceptance test is
green on main; its box is ticked in the same PR that makes it green.

### M1 Sound randomness

Entropy readiness; hardware sources (x86 `RDRAND`/`RDSEED`, AArch64
`RNDR`) and virtio-rng; Linux `getrandom` semantics (block until seeded;
`GRND_NONBLOCK` gives `-EAGAIN`; `GRND_INSECURE` never blocks); key
generation refuses an unseeded pool.

- [x] **Acceptance:** a boot with no entropy source shows those
  behaviours; a boot with a source works. Done 2026-10-09: `make
  test-entropy` in CI on both architectures
  ([report](audit/2026-10-09-m1-randomness-report.md)).

### M2 Persistent system

cosmofs as the root filesystem, an installer, init mounts the disk root;
filesystem registration and root choice move out of `vfs_init` into boot
composition.

- [ ] **Acceptance:** install to a blank disk image, reboot from it, write
  a file, reboot, read it.

### M3 Real process model

Linux `fork` (copy-on-write address space), `vfork`, `execve`; native
exec; close-on-exec; personality lifecycle and initial-stack hooks so
`process.c` no longer calls `linux_*` directly.

- [ ] **Acceptance:** static musl BusyBox `ash` is the shell; a fixed list
  of about 40 applets passes a scripted test; a subset of BusyBox's own
  testsuite passes.

### M4 Interactive and networked

PTYs (`/dev/ptmx`, `/dev/pts`) and job control sufficient for `ash`; a
DHCP client so networking does not depend on QEMU `fw_cfg`; DNS for
userland.

- [ ] **Acceptance:** from the installed system, BusyBox `wget` fetches a
  file by hostname through QEMU user networking; ssh login from the host
  with static dropbear.

### M5 Security baseline

The UEFI loader verifies Ed25519 signatures of `kernel.elf` and
`boot.tar` with a compiled-in key (documented developer override);
user-space ASLR (stack, heap, mmap, PIE base); a small fixed set of
operation-specific privileges replacing the single `cred_privileged()`.

- [ ] **Acceptance:** a tampered kernel or archive is refused; layouts
  differ across runs; an unprivileged process cannot mount and a granted
  one can.

### M6 Release

Architecture-specific code moved behind arch interfaces
(`native_signal.c`, the AArch64 assembly in `main.c`); five-file docs
completed for ACPI, objects, syscalls and cosmofs; a user guide (install,
use, network, ssh); README status made accurate; tagged reproducible
release images for both architectures.

- [ ] **Acceptance:** the tagged release images for x86-64 and AArch64
  rebuild bit-for-bit from the tag, boot, and pass the M1-M5 acceptance
  tests in CI; the docs and user guide listed above exist.

## Out of scope for 1.0

These remain in [`plan.md`](plan.md) and are not worked before 1.0:

- VMX validation on hardware
- NUMA
- huge pages
- live migration
- GPU and display
- RAID-Z
- hotplug
- eBPF
- more than 64 CPUs
- a physical hardware matrix
