# Audits and reports

Whole-tree audits, the deferred-work inventory, milestone reports and the
section 68 reports that each proposed one unit of work. A section 68
report is written before its unit is built; the unit's entry in
[`docs/history/`](../history/README.md) records what was actually built.
Remaining work is grouped in [`docs/plan.md`](../plan.md).

## Audits, inventory and milestone reports

| File | Subject |
|---|---|
| [`2026-09-deferred-work-inventory.md`](2026-09-deferred-work-inventory.md) | Deferred work inventory |
| [`2026-09-lifetime-quiesce-report.md`](2026-09-lifetime-quiesce-report.md) | Prompt #3 final report: critical fix pass and kernel object lifetime & quiescence |
| [`2026-09-post-roadmap-audit.md`](2026-09-post-roadmap-audit.md) | CosmoOS Post-Roadmap Architecture Audit |
| [`2026-10-03-lock-discipline-audit.md`](2026-10-03-lock-discipline-audit.md) | Lock discipline audit — 2026-10-03 |
| [`2026-10-03-lock-order.tsv`](2026-10-03-lock-order.tsv) | Observed lock-order edges (data for the lock-discipline audit) |
| [`2026-10-03-lock-sites.tsv`](2026-10-03-lock-sites.tsv) | Lock initialisation and definition sites (data for the lock-discipline audit) |
| [`2026-10-03-lockdep-context-plan.md`](2026-10-03-lockdep-context-plan.md) | Lockdep context validation — continuation after PR #302 |
| [`2026-10-03-lockdep-report.md`](2026-10-03-lockdep-report.md) | Lockdep hardening report — 2026-10-03 |
| [`2026-10-04-spin-contention-report.md`](2026-10-04-spin-contention-report.md) | Observed spin contention — 2026-10-04 |
| [`2026-10-09-m1-randomness-report.md`](2026-10-09-m1-randomness-report.md) | Roadmap M1: sound randomness — 2026-10-09 |
| [`2026-10-10-m2-persistent-system-report.md`](2026-10-10-m2-persistent-system-report.md) | Roadmap M2: persistent system — 2026-10-10 |
| [`2026-10-07-delack-nagle-report.md`](2026-10-07-delack-nagle-report.md) | The delayed acknowledgement against a Nagle peer — 2026-10-07 |
| [`2026-10-07-epoll-close-report.md`](2026-10-07-epoll-close-report.md) | epoll interest removal on the final close — 2026-10-07 |
| [`2026-10-07-epoll-callback-report.md`](2026-10-07-epoll-callback-report.md) | epoll readiness by callback, and nesting — 2026-10-07 |

## Section 68 reports

| File | Subject |
|---|---|
| [`next-subsystem-accept-order.md`](next-subsystem-accept-order.md) | Net-lo-tcp assumes accept returns the first client to connect |
| [`next-subsystem-ahci.md`](next-subsystem-ahci.md) | An AHCI driver: SATA disks through an AHCI host bus adapter |
| [`next-subsystem-aio-timer.md`](next-subsystem-aio-timer.md) | The async I/O ring cannot submit a timer |
| [`next-subsystem-arp-netif-ref.md`](next-subsystem-arp-netif-ref.md) | The interface an ARP retry still points at |
| [`next-subsystem-asid.md`](next-subsystem-asid.md) | Address-space identifiers: ASIDs on AArch64, PCIDs on x86-64 |
| [`next-subsystem-async-error.md`](next-subsystem-async-error.md) | A hardware error one process caused, and everyone pays for |
| [`next-subsystem-balance-movable.md`](next-subsystem-balance-movable.md) | The balancer's test asserts what the balancer promises |
| [`next-subsystem-chrdev-vnode-lock.md`](next-subsystem-chrdev-vnode-lock.md) | A filesystem lock held across a device that sleeps |
| [`next-subsystem-cond-phase.md`](next-subsystem-cond-phase.md) | Thrtest step 25 asserts which of two threads reached the mutex first |
| [`next-subsystem-condvar.md`](next-subsystem-condvar.md) | The wait, written once |
| [`next-subsystem-console-rx.md`](next-subsystem-console-rx.md) | The console's receive interrupt is cleared after the drain |
| [`next-subsystem-console.md`](next-subsystem-console.md) | The machine's own console: the framebuffer and a USB keyboard |
| [`next-subsystem-cosmofs-metadata-csum-id.md`](next-subsystem-cosmofs-metadata-csum-id.md) | Cosmofs's metadata blocks cannot say how they are checksummed |
| [`next-subsystem-cpu-clock.md`](next-subsystem-cpu-clock.md) | Two timestamps and no rule about subtracting them |
| [`next-subsystem-cwd-hold.md`](next-subsystem-cwd-hold.md) | A held walk: the working-directory race becomes a proof |
| [`next-subsystem-cwd-name.md`](next-subsystem-cwd-name.md) | A working directory's name is the path the walk took |
| [`next-subsystem-cwd-ref.md`](next-subsystem-cwd-ref.md) | The reference a path walk never takes |
| [`next-subsystem-device-readiness.md`](next-subsystem-device-readiness.md) | Devices that can be waited on: readiness for the terminal and the tap, and `select` for the Linux door |
| [`next-subsystem-device-reset.md`](next-subsystem-device-reset.md) | The device model has no generic reset operation |
| [`next-subsystem-dhcp-dns.md`](next-subsystem-dhcp-dns.md) | Autoconfiguring the guest: DHCP and a DNS proxy |
| [`next-subsystem-dirfd.md`](next-subsystem-dirfd.md) | A directory descriptor names a directory |
| [`next-subsystem-dnat.md`](next-subsystem-dnat.md) | Reaching the guest from outside: inbound port forwarding (DNAT) |
| [`next-subsystem-elf-shared-text.md`](next-subsystem-elf-shared-text.md) | A program's text belongs to the file, not to each process that runs it |
| [`next-subsystem-epoll.md`](next-subsystem-epoll.md) | The Linux personality has no epoll |
| [`next-subsystem-epollet.md`](next-subsystem-epollet.md) | Epoll is level-triggered only, EPOLLET is refused |
| [`next-subsystem-errno-tls.md`](next-subsystem-errno-tls.md) | A thread pointer, and `errno` per thread |
| [`next-subsystem-eventfd.md`](next-subsystem-eventfd.md) | The Linux personality has no eventfd |
| [`next-subsystem-exit-space.md`](next-subsystem-exit-space.md) | An exited process keeps its memory until its last reference drops |
| [`next-subsystem-file-path.md`](next-subsystem-file-path.md) | A read that fills its buffer, and an error that reaches close |
| [`next-subsystem-file-regions.md`](next-subsystem-file-regions.md) | File-backed regions: the mappings the constitution requires |
| [`next-subsystem-firewall.md`](next-subsystem-firewall.md) | A stateful packet-filter firewall for the guest taps |
| [`next-subsystem-fpsimd.md`](next-subsystem-fpsimd.md) | Floating point and SIMD at EL0 |
| [`next-subsystem-fsck-unchecked.md`](next-subsystem-fsck-unchecked.md) | The invariants the checker takes on trust |
| [`next-subsystem-fsck.md`](next-subsystem-fsck.md) | The blocks nobody can reach |
| [`next-subsystem-fsctl.md`](next-subsystem-fsctl.md) | The pass nobody can run |
| [`next-subsystem-gicv3.md`](next-subsystem-gicv3.md) | GICv3, the interrupt controller of current ARM machines |
| [`next-subsystem-hardening.md`](next-subsystem-hardening.md) | A guard that is proved, not assumed |
| [`next-subsystem-host-input.md`](next-subsystem-host-input.md) | The host chain: what the world may ask of the host |
| [`next-subsystem-host-state.md`](next-subsystem-host-state.md) | The host's own flows: reply state for the host chain |
| [`next-subsystem-hysteresis.md`](next-subsystem-hysteresis.md) | Sched-balance-hysteresis judges pulls by a hint that counts a yielding CPU twice |
| [`next-subsystem-input-chain.md`](next-subsystem-input-chain.md) | The INPUT chain: what a guest may ask of the host |
| [`next-subsystem-irq-order.md`](next-subsystem-irq-order.md) | The vGIC queue test still asserts an order the hypervisor does not promise |
| [`next-subsystem-jobcontrol.md`](next-subsystem-jobcontrol.md) | Job control: `^Z`, stopped processes and the foreground group |
| [`next-subsystem-libc-shared-tables.md`](next-subsystem-libc-shared-tables.md) | The two tables threads left behind |
| [`next-subsystem-lifetime-windows.md`](next-subsystem-lifetime-windows.md) | Four windows nothing has ever raced |
| [`next-subsystem-linux.md`](next-subsystem-linux.md) | Booting Linux: the feature registers a guest reads, and the walls to a kernel's first breath |
| [`next-subsystem-load-balancer.md`](next-subsystem-load-balancer.md) | A balancer that pulls, and a load that can see the thread already running |
| [`next-subsystem-lockup-bound.md`](next-subsystem-lockup-bound.md) | A sampler's bound is a count of waits, not a stopwatch |
| [`next-subsystem-lockup-interrupted.md`](next-subsystem-lockup-interrupted.md) | Lockup-sample asserts a leaf PC on a spinner that can be interrupted |
| [`next-subsystem-lockup.md`](next-subsystem-lockup.md) | A hang that names its program counter |
| [`next-subsystem-machine.md`](next-subsystem-machine.md) | The machine a guest is handed: a device tree, the entry convention, and PSCI |
| [`next-subsystem-map-fixed.md`](next-subsystem-map-fixed.md) | The hole between two syscalls |
| [`next-subsystem-memfd.md`](next-subsystem-memfd.md) | The Linux personality has no memfd_create |
| [`next-subsystem-migrate-stress.md`](next-subsystem-migrate-stress.md) | Sched-migrate-stress asserts a latency the scheduler does not promise |
| [`next-subsystem-mmap-place.md`](next-subsystem-mmap-place.md) | A placement is inserted under the hold that chose it |
| [`next-subsystem-module-zombie-reap.md`](next-subsystem-module-zombie-reap.md) | A zombie nobody will come back for |
| [`next-subsystem-mount-rel.md`](next-subsystem-mount-rel.md) | Mount and unmount name what the caller's path names |
| [`next-subsystem-mprotect.md`](next-subsystem-mprotect.md) | A call one personality has and the other does not |
| [`next-subsystem-mremap.md`](next-subsystem-mremap.md) | The Linux personality has no mremap |
| [`next-subsystem-multiguest.md`](next-subsystem-multiguest.md) | From one guest to many: a tap per guest |
| [`next-subsystem-named-pipes.md`](next-subsystem-named-pipes.md) | Named pipes: a pipe with a name, and files that can be waited on |
| [`next-subsystem-nat.md`](next-subsystem-nat.md) | Reaching beyond the host: IP forwarding and masquerade NAT |
| [`next-subsystem-native-thread-door.md`](next-subsystem-native-thread-door.md) | The two thread calls the native door still lacks |
| [`next-subsystem-net-flows.md`](next-subsystem-net-flows.md) | The operator cannot see why a guest's flows are refused |
| [`next-subsystem-net-leftover.md`](next-subsystem-net-leftover.md) | A failed network test keeps its taps, and the tests after it fail for them |
| [`next-subsystem-net-rx-dup.md`](next-subsystem-net-rx-dup.md) | The network receive path has no duplicate-frame fault injection |
| [`next-subsystem-netctl.md`](next-subsystem-netctl.md) | Configuring the guest's network at runtime: a control channel |
| [`next-subsystem-nettest-accept.md`](next-subsystem-nettest-accept.md) | The connection the harness accepted |
| [`next-subsystem-nettest-deadline.md`](next-subsystem-nettest-deadline.md) | A harness that cannot say why its own exchange failed |
| [`next-subsystem-nettest-probe.md`](next-subsystem-nettest-probe.md) | What the accepted connection was doing |
| [`next-subsystem-nettest-retry.md`](next-subsystem-nettest-retry.md) | A back-connection that survives one reset: the harness stops being a coin flip on someone else's bug |
| [`next-subsystem-nvme-admin.md`](next-subsystem-nvme-admin.md) | The NVMe admin path leaves before complete() has let go |
| [`next-subsystem-orphan.md`](next-subsystem-orphan.md) | The name is gone and the handle is not |
| [`next-subsystem-output-chain.md`](next-subsystem-output-chain.md) | The OUTPUT chain: what the host itself may send |
| [`next-subsystem-percpu-migration.md`](next-subsystem-percpu-migration.md) | A migration that can land: declared per-CPU claims, a lock order lockdep can see, and a migrator that moves one thread |
| [`next-subsystem-placed-running.md`](next-subsystem-placed-running.md) | Three scheduler tests assume a spinner is running when it has only been placed |
| [`next-subsystem-priority-inheritance.md`](next-subsystem-priority-inheritance.md) | The sleeping mutex has no priority inheritance |
| [`next-subsystem-proc-settle.md`](next-subsystem-proc-settle.md) | A test's process outlives the test, and the next test counts it |
| [`next-subsystem-pt-tls.md`](next-subsystem-pt-tls.md) | `__thread`, and the TLS image a program brings with it |
| [`next-subsystem-quiesce-wake.md`](next-subsystem-quiesce-wake.md) | A grace period that ends when it ends, not at the next tick |
| [`next-subsystem-shared-futex.md`](next-subsystem-shared-futex.md) | A futex keyed by what the word maps |
| [`next-subsystem-shm.md`](next-subsystem-shm.md) | The Linux personality has no System V shared memory |
| [`next-subsystem-signalfd.md`](next-subsystem-signalfd.md) | The Linux personality has no signalfd |
| [`next-subsystem-signals.md`](next-subsystem-signals.md) | The signals a person can send: `^C` and a native signal ABI |
| [`next-subsystem-smp-wake.md`](next-subsystem-smp-wake.md) | Smp-wake posts while the waiter is still running |
| [`next-subsystem-snap-deadlist.md`](next-subsystem-snap-deadlist.md) | The list the root does not name |
| [`next-subsystem-socket-verdict.md`](next-subsystem-socket-verdict.md) | The verdict a socket records and nothing can ask for |
| [`next-subsystem-statx.md`](next-subsystem-statx.md) | The Linux personality has no statx |
| [`next-subsystem-straggler-kick.md`](next-subsystem-straggler-kick.md) | What the straggler kick is worth |
| [`next-subsystem-suite-waits.md`](next-subsystem-suite-waits.md) | The suite waits for time instead of for the property |
| [`next-subsystem-symlink.md`](next-subsystem-symlink.md) | A name that points somewhere else |
| [`next-subsystem-sysinfo.md`](next-subsystem-sysinfo.md) | The Linux personality stubs sysinfo |
| [`next-subsystem-tap.md`](next-subsystem-tap.md) | A host bridge: a tap interface and a frame channel, so the guest reaches the host's network |
| [`next-subsystem-tcp-verdict.md`](next-subsystem-tcp-verdict.md) | A verdict TCP's callers can see |
| [`next-subsystem-termios.md`](next-subsystem-termios.md) | A terminal a program can drive: terminal modes and `/dev/tty` |
| [`next-subsystem-thread-migration.md`](next-subsystem-thread-migration.md) | A thread that can never move, on a CPU chosen once |
| [`next-subsystem-threads.md`](next-subsystem-threads.md) | Native threads and a futex |
| [`next-subsystem-timerfd.md`](next-subsystem-timerfd.md) | The Linux personality has no timerfd |
| [`next-subsystem-twelve-bytes.md`](next-subsystem-twelve-bytes.md) | Twelve bytes that never arrive |
| [`next-subsystem-unix-sockets.md`](next-subsystem-unix-sockets.md) | Unix domain sockets: a name in the filesystem, and a handle that rides in a message |
| [`next-subsystem-unmount-leak.md`](next-subsystem-unmount-leak.md) | The commit after the last one |
| [`next-subsystem-usb.md`](next-subsystem-usb.md) | A USB host stack: xHCI, enumeration and class drivers |
| [`next-subsystem-usertest-sections.md`](next-subsystem-usertest-sections.md) | The suite behind one line |
| [`next-subsystem-vblk-rw.md`](next-subsystem-vblk-rw.md) | A writable root: virtio-blk writes and flush, so the guest can persist |
| [`next-subsystem-vblk.md`](next-subsystem-vblk.md) | A root filesystem: virtio-blk, so Linux reaches userspace |
| [`next-subsystem-vcpu-regs-size.md`](next-subsystem-vcpu-regs-size.md) | An invariant written down thirteen times, checked once, and false in half the tree |
| [`next-subsystem-vcpu-threads.md`](next-subsystem-vcpu-threads.md) | A thread per vCPU, and a way to stop one |
| [`next-subsystem-vdist.md`](next-subsystem-vdist.md) | The virtual distributor: a guest that can run a stock GIC driver |
| [`next-subsystem-vgic.md`](next-subsystem-vgic.md) | The vGIC: an AArch64 guest that can be interrupted |
| [`next-subsystem-virtio-remove-inflight.md`](next-subsystem-virtio-remove-inflight.md) | A virtio device dedicated to removal |
| [`next-subsystem-vnet.md`](next-subsystem-vnet.md) | A guest network interface: virtio-net, so the transport carries a second device |
| [`next-subsystem-vtimer.md`](next-subsystem-vtimer.md) | The virtual timer: a guest that can be woken by time |
| [`next-subsystem-vuart.md`](next-subsystem-vuart.md) | The guest's console: a PL011 a stock kernel can print to |
| [`next-subsystem-wake-preempt.md`](next-subsystem-wake-preempt.md) | A wake that preempts |
| [`next-subsystem-watchdog-spent.md`](next-subsystem-watchdog-spent.md) | The self-test hang watchdog is spent on a passing test |
| [`next-subsystem-zero-window.md`](next-subsystem-zero-window.md) | Net-hostinput's window-update check races the zero-window probe |
| [`next-subsystem.md`](next-subsystem.md) | An Intel gigabit NIC driver (`e1000e`) |
