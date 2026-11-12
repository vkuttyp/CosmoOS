# T2: a faster place to boot, 2026-11-12

Roadmap 1.3's T2 names two candidates for a faster debug boot, each to be measured before it is chosen: KVM on a
Linux host for x86-64, and QEMU's `hvf` accelerator on the Mac for AArch64. Both were measured on main at
`ded65c04`. Neither is chosen. This PR changes documents only.

## What was run

| Place | Machine | QEMU |
|---|---|---|
| Mac | Apple M1, 8 CPUs | 11.1.3, accelerators `tcg` and `hvf` |
| Linux host | Xeon E-2136, 12 threads, Linux 6.8.0-146, `/dev/kvm` | 10.0.13 in the CI image, accelerators `tcg` and `kvm` |

On the Linux host every command ran in one container of the CI image limited to 4 CPUs and 6 GiB, one at a time,
with ceesafec 0.37.0's published Linux binary (SHA-256 `ce9289c4…a072c3`) mounted beside the tree. The KVM runs
passed `/dev/kvm` to that container.

## Results

| Run | Verdict | Where it stopped |
|---|---|---|
| Mac, AArch64, `QEMU_ACCEL=hvf QEMU_CPU=host QEMU_EL2=0` | FAIL after 0.5 s | QEMU: "HVF does not support GICv2 emulation" |
| Mac, the same with `QEMU_GIC=3` | FAIL after 6.9 s | QEMU aborts, `Assertion failed: (isv), function hvf_handle_exception`, after the kernel's `platform:` line |
| Linux host, x86-64, TCG, 4 CPUs | PASS in 211.7 s | |
| Linux host, x86-64, TCG, 1 CPU, `QEMU_IOMMU=0` | PASS in 199.3 s | |
| Linux host, x86-64, `QEMU_ACCEL=kvm QEMU_CPU=host`, 4 CPUs | FAIL after 3.0 s | QEMU exits 0 (a reset) after `serial: console input on IRQ 4`; the next line under TCG is `smp: CPU 1 (APIC 1) up` |
| the same, 1 CPU | FAIL, 300 s timeout | after `virtio: virtio0: queue 0, 256 entries, interrupt driven` |
| the same, 1 CPU, `QEMU_IOMMU=0` | FAIL, 300 s timeout | 372 self-tests pass, `vm-replace-race` fails, the watchdog reports no progress in the test after `net-neigh-down-race` |

For comparison, the Mac's TCG debug boots in today's matrix took 179.0 s to 208.0 s.

## HVF

- HVF has no GICv2, which `make test` uses by default, and on this M1 no EL2 (`virtualization=on`), which the
  hypervisor's self-tests need.
- With a GICv3, the kernel's first GIC access stops QEMU. `gicv3_init` in the debug object stores to the
  distributor with post-index addressing (`str w12, [x10], #0x4`, `str wzr, [x10], #0x4`, `str x8, [x9], #0x8`).
  Such an access traps without an instruction syndrome, and QEMU's HVF code asserts on it. This is backlog DRV-15.
- No boot time under HVF exists: the kernel does not reach its self-tests.

## KVM

- **Four CPUs.** The machine resets where the application processors start. Cause not established (backlog SMP-7).
- **The emulated VT-d.** With one CPU the virtio block driver's first interrupt-driven queue never completes.
  `QEMU_IOMMU=0` removes the stop. Cause not established (backlog DRV-18).
- **The hypervisor's tests.** The kernel finds VT-x, and its own self-check disables the backend: "a guest with
  paging off exited with rc 0 kind 3 pc 0x80000000 (wanted kind 1)".
- **One CPU, no IOMMU.** `vm-replace-race` fails (`e.probes > 0`, line 832). The boot then stops in
  `net-accept-race`, the test that follows `net-neigh-down-race` under TCG, with `kmain` blocked on a socket.
  Neither is diagnosed (backlog TEST-25).

## What KVM would buy

The 372 self-tests that passed under KVM took 71.4 s there and 116.6 s under TCG on the same host, with one CPU and
no IOMMU in both: 1.6 times faster.

Under TCG the self-tests are 155.6 s of the 199.3 s boot. Twenty tests are 55% of that and fifty are 75%. Of the
tests that take 500 ms or more under TCG and also ran under KVM, 24 take at least 80% as long under KVM (40.5 s
together): they wait on the clock.

| Test | TCG, ms | KVM, ms |
|---|---|---|
| `cosmofs-replay` | 24,218 | 7,426 |
| `process-user` | 7,302 | not reached |
| `lockdep-graph-bench` | 6,037 | 1,463 |
| `net-harness` | 5,238 | not reached |
| `module-cycle` | 4,054 | 3,634 |
| `net-hostinput` | 3,960 | 3,933 |
| `syscall-fuzz` | 3,702 | not reached |
| `net-lo-tcp` | 2,813 | 2,784 |
| `sched-fair` | 2,636 | 2,611 |
| `usb-storage-timeout` | 2,590 | 2,670 |
| `ahci-timeout` | 2,570 | 2,714 |
| `blk-timeout-skew` | 2,508 | 2,532 |

## The choice

Neither. The matrix stays on TCG.

- HVF needs DRV-15 fixed across the drivers, and then covers neither GICv2 nor EL2.
- KVM needs three defects found and fixed before one boot finishes, covers no hypervisor self-test on this host,
  and would then save well under half the boot.
- The Linux host's TCG boot (199.3 s to 211.7 s in a 4-CPU container) is as fast as the Mac's. Running the x86-64
  half of the matrix there while the Mac runs AArch64 would halve the matrix's wall time with no kernel change.
  The host runs production services, so whether to use it that way is the owner's decision.
- The waits inside the self-tests are the larger lever (backlog TEST-26).

## What this does not do

- It does not diagnose the three KVM failures; each has a backlog line.
- It did not measure AArch64 under KVM (no AArch64 Linux host) or x86-64 under HVF (the Mac is AArch64).
