# Virtualization: testing

**Emulator requirement.** QEMU/TCG before 9.2 does not apply nested
paging to a guest whose own paging is disabled (real mode, flat
protected mode): such a guest reads and writes host physical memory.
`hv_init` runs a one-instruction paging-off guest at guest-physical
0x80000000 (outside the 256 MiB of harness RAM, inside the PCI hole) and
expects an `HLT` exit; anything else disables the backend with a
warning, `/dev/vmm` reports `none`, the guest self-tests log `skipped`
and the harness fails the run (forbidden marker). CI therefore runs in a
Debian trixie container (QEMU 10.0); Ubuntu 24.04's 8.2 is not enough.

Three layers, as everywhere in the project: host unit tests of the pure
code, kernel self-tests that run real guests under the QEMU harness, and
a userland run through `vmctl` from `/etc/rc.test`. All of it executes
under TCG on every developer machine and in CI because the harness's
CPU model carries AMD-V with nested paging.

## The QEMU CPU model

`scripts/qemu-run.sh` passes `-cpu qemu64,+nx,+svm,+npt` (override with
`QEMU_CPU=...`; use `host` with `QEMU_ACCEL=kvm` or `hvf` on a machine
with nested virtualization). TCG emulates SVM and NPT but not NRIP-save
or decode assists, and **not VT-x at all** (`vmx: false` for every model,
which is why the VMX backend is never exercised here); the SVM backend
relies on neither optional feature (the I/O exit's next RIP comes from
EXITINFO2, which is architectural). Without
`+svm,+npt` the kernel logs `hv: no hardware virtualization backend on
this CPU`, `/dev/vmm` reads `none asids=0 vms=0`, the guest self-tests
log `selftest: hv: skipped: no backend` and pass, and `rc.test` prints
`HVTEST: skipped`. The harness treats both lines as **forbidden markers**
(`HV_FORBIDDEN_MARKERS` in `tests/boot/run_boot_test.py`) and requires
`HVTEST: PASS`, so a configuration that silently lost the backend fails
the boot test rather than skipping it.

## Host unit tests: `tests/host/test_hv.c` and `test_vmx.c` (`make host-test`)

`test_vmx.c` is the VMX backend's only executable evidence in this
repository (see "What is not tested yet"): `vmx_fix_ctls` against
synthetic capability MSRs (a required bit is added, a forbidden one is
dropped, and `vmx_ctls_ok` reports the difference), `vmx_decode_io` on
the exit qualifications the SDM tabulates, `vmx_eptp`'s memory type and
level count, and the EPT builder over the harness arena — mappings,
per-page permissions in the leaf, 2 MiB leaves with their collision and
splitting refusals, rollback of a partially failed map, and every table
page returned at destroy. `test_hv.c` covers the same ground for NPT
plus the segment translation both backends share.


Compiled natively with ASan/UBSan against `kernel/arch/x86_64/svm_npt.c`
and `x86/svm.h`, over the harness arena (`tests/host/harness.c`
provides `pmm_alloc_pages` and an identity direct map):

- **Layouts**: `sizeof(struct cosmo_vcpu_regs) == 448`, `struct
  cosmo_vm_exit == 64`, `struct cosmo_vcpu_seg == 16`, `struct vmcb ==
  4096`, `EXITCODE` at 0x70, `save.rip` at 0x578 (the header's own
  `STATIC_ASSERT`s pin the rest).
- **IOIO decoder**: port, size 1/2/4, IN/OUT, string and REP bits from
  EXITINFO1.
- **Nested page tables**: a fresh root is one page; mapping three pages
  at 0x1000 costs root + PDPT + PD + PT (4 pages); every level of every
  walked entry carries P, RW and **US**; queries return the host address
  with the offset preserved and fail on unmapped pages; a distant mapping
  in the same PML4 slot adds a PD and a PT (6 pages); mapping an already
  mapped page is `-EEXIST`, misaligned arguments `-EINVAL`, and a map
  that fails half-way is rolled back (the earlier pages of that call are
  unmapped, existing mappings untouched); unmap clears leaves, ignores
  unmapped pages and rejects misalignment; destroy returns every table
  page to the arena (`host_arena_free_pages` equals the value before
  `npt_create`); `npt_destroy(0)` is tolerated.

## Kernel self-tests (`kernel-services/virtualization/hvtest.c`)

Each guest test builds a VM with 1 MiB at guest-physical 0, copies an
image from the boot archive (`tests/hv/<name>.bin`, built by
`tests/hv/hv.mk` from `tests/hv/$(ARCH)/` as flat binaries linked with
`--image-base=0 -Ttext=0x1000 --oformat=binary`) to 0x1000, creates vCPU
0 and points it there: `rip` in real mode with `cs` 0 on x86-64, `pc`
at EL1 with the MMU off on AArch64. Console output means bytes the guest
wrote to port 0xE9, read back with `vm_console_read` (x86 only: there is
no port space on AArch64, and its guests report through their exits).
The `hv-guest-*` tests are x86's and skip elsewhere; the `el2-guest-*`
tests are AArch64's and skip on x86.

| test | image | what it checks |
|---|---|---|
| `hv-probe` | — | backend name `svm` (or `vmx`), nested paging, ≥ 2 ASIDs, and a backend that claims it can run the reset state; without a backend, `vm_create` is `-ENOTSUP` |
| `hv-caps` | — | the capabilities reported are the ones honoured: `arch_hv_vm_map` refuses `prot` 0 and unknown bits, and with `map_prot` a read-only mapping is accepted, queried back and unmapped |
| `hv-npt` | — | `hv_vm_count` up and down; regions at 0 (64 KiB) and 0x200000 (12 KiB); overlap, misalignment, window edge and the 64 MiB limit refused; a VM created with an 8 KiB cap refuses 12 KiB and takes 8 KiB; every page translates to its recorded frame (`arch_hv_vm_query` = `page_to_phys`), holes do not; copies: unbacked range `-EFAULT` before any byte, zeroed memory, a 16-byte round trip straddling a page boundary lands in the right frames |
| `hv-guest-pio` | `guest_pio.S` | `HV` reaches the console without an exit; `outw $0x80` is an `IO` exit (write, size 2, value 0x1234, `rip` after the instruction); `hlt` is `HLT` at the next byte; `inb $0x81` is an `IO` read exit completed with `'Q'` on the next run; the guest echoes it to the console; `rax` low byte is `'Q'`; the reset `cr0` is 0x60000010; exit and entry counters grew |
| `hv-guest-irq` | `guest_irq.S` | installs IVT[0x20]; `cli; hlt` exits with no pending flag; vectors 3 and 256 are `-EINVAL`; after `vcpu_inject(0x20)`, `pending_irq` reads 0x20; `sti; hlt` exits with `F_IRQ_PENDING` and nothing delivered (the STI shadow); the next run delivers it: `HLT` at 0x101A, flag clear, console `I`, `rflags.IF` set; a second injection is delivered on the next run |
| `hv-guest-cpuid` | `guest_cpuid.S` | CPUID 0x40000000 yields `CosmoOSCosmo`; CPUID 1 has the hypervisor bit; `rdmsr EFER` shows SVME clear; `wrmsr EFER` with SCE then `rdmsr` shows it (console `CosmoOSCosmo101`); `vmmcall` is a `HYPERCALL` exit with nr 7 and args 0x11 0x22 0x33 0x44; `efer` reads 1; then `HLT` |
| `hv-guest-pm` | `guest_pm.S` | a 32-bit protected-mode guest entered through `set_regs` (`cr0` PE, flat `cs` 0x0C9B / data 0x0C93); `P` on the console; a store to 0x10000000 is an `MMIO` exit (write, that address) with `rax` 0x5A5A5A5A; the owner skips the 5-byte instruction with `set_regs`; `Q` and `HLT` follow; `set_regs` with PG-without-PE and with EFER.SVME are `-EINVAL` |
| `hv-guest-shutdown` | `guest_shutdown.S` | with `idtr.limit` set to 0, `int $3` triple-faults: `SHUTDOWN` exit after `S` on the console; the next run is `-EIO`; `get_regs` still works |
| `hv-guest-spin` | `guest_spin.S` | a guest that never exits: `vcpu_run_limited(5)` returns `-ETIMEDOUT` after five host-interrupt exits (the tick reaches the guest even with its IF clear); `.` on the console; a second vCPU, index reuse `-EEXIST`, index 4 `-EINVAL`, `nr_vcpus` bookkeeping; the VM count drops when the last references go |
| `hv-guest-fpu` | `guest_fpu.S` | the guest rule of `arch/fpu.h`: the test thread takes ownership of register state (`arch_fpu_alloc`) and puts a pattern in xmm0; the guest enables SSE for itself, stores its initial xmm0 at 0x3000 (must be the reset state, zeros: nothing of the owner leaked in) and loads a pattern the test placed at 0x3010; afterwards the owner's xmm0 must still hold its own pattern (nothing of the guest leaked out), and a second run shows the guest kept running |

| `el2-guest-wfi` | `aarch64/guest_wfi.S` | the first `WFI` is a `WFI` exit with the PC past it; a second run reaches the second `WFI`; the guest's PSTATE still reads EL1h, so the switch put it back where it was |
| `el2-guest-hvc` | `aarch64/guest_hvc.S` | `HVC` is a `HYPERCALL` exit carrying this architecture's convention (`x0` = 0x2A the number, `x1`–`x4` = 1..4 the arguments), with the PC already past the instruction as the architecture defines; the guest runs on afterwards (`x0` becomes 0x2B) and reaches its `WFI` |
| `el2-guest-mmio` | `aarch64/guest_mmio.S` | a store to 0x4000_0000, which the VM has no memory at, is a stage-2 fault reported as an `MMIO` exit with that address and `write` set; after the owner steps over it the load is reported as a read — the direction comes from `ESR_EL2`, not a guess |
| `el2-guest-sysreg` | `aarch64/guest_sysreg.S` | `HCR_EL2.TID3` traps `mrs x5, id_aa64pfr0_el1`: a `SYSREG` exit naming register 5 and a read, which is what lets a model answer; the owner writes the answer and steps over it |
| `el2-vgic-roundtrip` | `aarch64/guest_wfi.S` | the guest's interrupt state crosses EL2 and comes back read from hardware: after a run, list register 0 is empty and `ICH_ELRSR_EL2` says it is free. On a machine with no virtual GIC there is no such state and the switch leaves those registers alone |
| `el2-guest-irq` | `aarch64/guest_irq.S` | **the unit's point**: the guest enables its own CPU interface, the owner injects INTID 42, and the handler acknowledges and calls out with the number it was given. The hypercall is between `ICC_IAR1_EL1` and `ICC_EOIR1_EL1`, so the list register reads **Active** -- delivery, not completion -- and the pending bit must already be clear. A second injection (43) is delivered after the first completes, so the register was released and not merely emptied once |
| `el2-guest-irq-masked` | `aarch64/guest_irq.S` | injected while the guest has `PSTATE.I` set, it stays Pending in the list register and pending in the owner's set; it arrives when the mask clears. This is what tells "delivered" apart from "the guest happened to call out for another reason", and it is the test the active-priority leak broke |
| `el2-guest-irq-private` | `aarch64/guest_irq.S` | PPI 27 and SGI 0 -- the private interrupts the old x86-shaped range refused -- are delivered, and 1020 and 8192 (an LPI) are refused |
| `el2-guest-timer-isolated` | `aarch64/guest_timer.S` | the guest arms `CNTV` and exits; the host's own `CNTV_CTL_EL0` reads exactly what it read before the run, and the guest's `ENABLE` is in the guest's saved state and nowhere else. This is the measurement that opened the virtual-timer report -- `0x1` in the host where `0x2` had been -- as a test |
| `el2-guest-timer-offset` | `aarch64/guest_timer.S` | two VMs made at different times read different `CNTVCT_EL0`s, both a few milliseconds after their own creation rather than the host's uptime; two vCPUs of one VM made 20 ms apart read the *same* clock, the later one later by the 20 ms and nothing else. The second half is what a per-vCPU offset would fail |
| `el2-guest-phys-timer` | `aarch64/guest_ptimer.S` | `mrs x3, CNTPCT_EL0` and `msr CNTP_CTL_EL0, x4` are both `SYSREG` exits naming CRn 14 and the right CRm/Op2; the owner answers the read and steps over both, and the guest ends with the owner's answer, not the host's clock -- and the host's tick untouched |
| `el2-guest-timer` | `aarch64/guest_timer.S` | the guest arms its timer for ~15 ms and heartbeats; its handler eventually calls out with INTID 27, `CNTV_CTL` read in the handler showing `ENABLE|ISTATUS` before it masked, the pending bit already clear, and the timer -- masked -- not firing again. A backend that injected on every host interrupt would deliver on the host's tick and the handler's `CNTV_CTL` would not read `ISTATUS` |
| `el2-guest-timer-ontime` | `aarch64/guest_timer_wfi.S` | the idle loop every guest kernel has: arm, `WFI`, be woken. The `WFI` run must hold for most of the ~15 ms the guest asked for (measured on the host's clock), and the handler's own `CNTVCT` minus its `CVAL` -- lateness, in the guest's ticks -- must be less than the interval asked for. Measured: asked 15.6 ms, held 17 ms, 2.6 ms late |
| `el2-guest-gicd-probe` | `aarch64/guest_gicd.S` | the first thing every GIC driver does: read `GICD_TYPER`, `IIDR`, `PIDR2`, then find the redistributor whose frame is this CPU's by its own MPIDR. The run ends in a hypercall, **not** an `MMIO` exit -- something answered; 288 lines, a GICv3; two vCPUs each find a frame carrying their own Aff0, `Last` on the higher one and not the lower; each reads an MPIDR that is its index |
| `el2-guest-gic-config` | `aarch64/guest_gicc.S`, `guest_gicd.S` | the register file returns what was written through each access size a driver uses: a 64-bit route, 32-bit words, a single priority byte. vCPU 1 configures, so the route it writes -- its own affinity -- reads back as 1 and not the zero an unimplemented register would give; vCPU 0 sees the SPI's enable (the VM's) and not the PPI's (frame 1's); `ICENABLER` clears what `ISENABLER` set and only that |
| `el2-guest-gic-timer` | `aarch64/guest_gic.S` | **the unit's point**: distributor on, redistributor found by MPIDR and woken, the timer PPI grouped, prioritised and enabled there, an SPI routed to itself, then the CPU interface -- and the timer arrives through the controller so configured. Then the phase only a distributor passes: with the PPI *disabled* in the redistributor the next expiry is held past its deadline (the guest counts zero handler runs); re-enabling releases it. Direct injection would deliver it disabled. Last, an SPI the guest makes pending through `ISPENDR` arrives at the vCPU its route names. Measured: held through 81 heartbeats while disabled, released after 1 |
| `el2-guest-sgi` | `aarch64/guest_sgi.S` | a guest can be SMP: both vCPUs run one image and each finds its own redistributor; vCPU 0 writes `ICC_SGI1R_EL1` naming SGI 3 for Aff0 = 1 and its owner sees "sent", not a `SYSREG` exit; vCPU 1's handler runs with INTID 3 on a CPU whose MPIDR says 1; vCPU 0, not in the target list, heartbeats twenty times and never sees it. Routed, not broadcast |
| `el2-guest-gicd-isolated` | `aarch64/guest_gicc.S`, `guest_gicd.S` | a guest enables the host's *spare* SPI in its distributor; the host's enable bit for that line (`arch_test_irq_is_enabled`) reads the same before and after -- a model that wrote through to hardware would fail here -- and a second VM reads a fresh distributor with nothing of the first's in it |
| `el2-mmio-device` | `aarch64/guest_mmio_widths.S` | the device seam, tested before the first device leans on it: a word the test registers at `0x0A001000` answers `ldrb`, `ldrh`, `ldr w`, `ldr x`, `ldrsb w`, `ldrsh x` and `ldrsw x`, and each register holds what the architecture says that load leaves in it -- the `ldrsb w` reads `0xFFFFFFF3`, not `0xF3` and not `0xFFFFFFFFFFFFFFF3` -- with no exit; a halfword store reports size 2 and `0xBEEF`; a load no device claims goes to the owner with size 8 and register 9, and the owner's answer, left in the exit, lands in `x9` |
| `el2-guest-uart` | `aarch64/guest_uart.S` | the report's opening measurement as a test: six stores to `DR`, the way an `earlycon` prints, and the VM's console holds exactly `"hello\n"`; `FR` read `TXFE|RXFE` throughout; PeriphID0/1 and PCellID3 say a PL011 |
| `el2-guest-uart-rx` | `aarch64/guest_uart_rx.S` | the first device interrupt the distributor routes that a guest did not fake through `ISPENDR`: with SPI 33 routed to itself and `RXIM` unmasked, the guest is interrupted when the test writes `'x'` to the VM, its handler reads `RXMIS` set, `'x'` from `DR`, and `RXFE` after; ten heartbeats follow with no second interrupt |
| `el2-guest-uart-race` | `aarch64/guest_uart_poll.S` | two phases, two claims. One vCPU, a kernel thread typing 200 bytes at a guest that polls `DR` (interrupts masked around the poll) and takes SPI 33: **no interrupt with `MIS` zero** -- with no sibling to drain the byte, one can only come from the hypervisor's own ordering (a transition applied after the UART's lock was dropped; "the line is up" returned for the run loop to act on -- both proved by forcing their windows). Then two vCPUs each on its own thread, both polling and both able to take the interrupt, 300 bytes: **none lost**. `MIS`-zero interrupts are counted and reported in phase two, not asserted absent: a sibling that drains the byte between the GIC forwarding the interrupt and the handler reading `MIS` makes it zero on real hardware too, and one list register adds a window hardware does not have (a line that drops after the interrupt was placed is still delivered, where a GIC would return 1023 at `IAR`). The first version asserted zero in phase two and failed once in ten runs, on exactly that interleaving |
| `el2-guest-dtb` | `aarch64/guest_dtb.c`, `tests/hv/virt.dtb` | a VM laid out as machine mode lays one out (RAM at the uapi's base, the image where its Image header's `text_offset` says -- read, not assumed -- the tree at the first 2 MiB boundary past it, `x0` = the tree); the C guest reads the tree and prints what it found through the UART the tree named; every field of the line is compared to the uapi constant it came from |
| `el2-guest-psci` | `aarch64/guest_dtb.c` | the owner is the firmware: the test answers `PSCI_VERSION` with 1.0 and the guest prints it; `CPU_ON` for vCPU 1 creates it after the VM has started, at the guest's entry with the guest's context, and its own first line carries that context (`cpu1: up ctx=1234cafe`), so a vCPU that merely existed would not pass; `SYSTEM_OFF` is the last hypercall |
| `el2-vcpu-run-tick` | `aarch64/guest_spin.S` | the bounded run: a spinning guest run with `COSMO_VCPU_RUN_ONE_TICK` returns `PREEMPTED` within a tick (measured: ~2 ms), runs again in between, is still in its loop; without the flag only the tests' own bound stops it; unknown flag bits are ignored |
| `test_fdt` (host) | -- | the writer's blob has a sound header and is the size it says; every node and property the machine description promises reads back through the reader (both virtio-mmio windows, block and net, at their addresses and SPIs), each equal to the uapi constant it came from; a truncated blob, a blob that does not fit, a node left open and a property outside any node are refused |
| `el2-guest-idreg` | `aarch64/guest_idreg.S` | the feature model, checked by property not by a constant a wrong model could also return: the guest reads each register the model shapes and reports it, and the test knows the host's own value (an inline `mrs` of the same registers) and asserts the policy -- EL2/EL3/SVE/PMU/pointer-auth hidden, FP/GIC/PARange kept from the host, and a register in the ID space the model does not name (`ID_AA64ZFR0`, SVE) answered as zero rather than sent to the owner. Runs in CI without any Linux image |
| Linux boot (userland, demonstration) | a raw arm64 `Image` at `tests/hv/aarch64/Image` (gitignored) via `vmctl run --machine` in `rc.test` | a stock Linux kernel, when a developer has provided one: `vmctl` builds the device tree, loads the Image by its header, answers PSCI and runs it; a diskless kernel comes up and prints its banner and `Machine model: cosmo,virt` (our device tree's compatible, read back by Linux through our PL011) and panics on the root filesystem it cannot find (`Unable to mount root fs`). It is a **demonstration in the log, not a harness gate**: the guest needs 256 MiB, so the milestone appears only under `QEMU_MEM=2G` (at the boot-test's default 256M the allocation fails harmlessly and the boot goes on). There is no kernel self-test for it either -- a Linux boot is seconds long and would blow the harness's 8 s per-test budget. The automated gate is `el2-guest-idreg`, the feature model's own test, which needs no Image; the Linux boot is the milestone a human confirms in the log |
| `el2-vm-raise-spi` | `aarch64/guest_spi.S` | a level SPI through the distributor, from the owner: the guest enables SPI 50 and waits; the test raises it and the guest takes it after one beat; SPI 51, left disabled, is raised and gated -- the guest runs on and never sees it. The line is raised and lowered through `vm_raise_spi`/`vm_lower_spi`, the same path the owner's device model uses, not by the guest faking it through `ISPENDR` |
| `el2-virtq-device` | `aarch64/guest_vblk.c` | the virtio-blk mechanism end to end with a real guest driver: the test models the virtio-mmio block transport inline (MagicValue/Version/DeviceID, status handshake, feature negotiation, the queue the guest sets up) over an in-test disk, direction-aware and offering flush; the guest probes the node, negotiates, builds a ring, reads sector 1, then writes a known pattern to sector 2, flushes, and reads sector 2 back. The test requires the read to return the disk's bytes and the read-back to return exactly what was written (not the disk's initial sector-2 bytes) with the write, flush and read-back all OK and the model having seen one flush -- so a device that completed a request without serving it, or dropped the write, would not pass |
| `test_vblk_dev` (host) | -- | the device-side virtqueue walk (`vblk.c`) with no guest and no QEMU: an in-memory guest RAM holds a ring a driver would build and an in-memory disk holds per-sector bytes. A read returns the requested sector's bytes (a per-sector pattern, so serving the wrong sector fails the compare); a write drains a device-readable buffer to the disk, a flush is observed, and the written sector reads back byte for byte; a write's data buffer marked device-writable is refused, a write past the disk is a completed I/O error, a write buffer outside guest RAM is refused, and a read-only device refuses a write and a flush as `UNSUPP`; and the hostile read rings are refused with no read out of bounds -- a head index past the ring (large enough to leave guest RAM), a chain that loops, a data buffer outside guest RAM, a read past the disk, a backlog past the ring depth, an over-large request, a fault after a served request (reported as failure, not progress), and a partial used-ring publish (uncounted, no phantom interrupt) |
| `el2-virtq-net` | `aarch64/guest_vnet.c` | the virtio-net mechanism end to end with a real guest driver: the test models the second virtio-mmio window inline with a loopback wire (DeviceID 1, a MAC, two queues); the guest negotiates, sets up the receive and transmit queues, posts a receive buffer, transmits a known frame, and polls the receive used ring. The test requires the frame to come back -- the received used length is header + frame and the bytes are the ones transmitted, not a stale buffer -- and checks the transport identity and the MAC, so a device that dropped the frame or looped nothing would not pass |
| `test_vnet_dev` (host) | -- | the device-side net queues (`vnet.c`) over the shared walk (`vq.c`) with no guest and no QEMU: an in-memory guest RAM and a small loopback wire that drops when full. A transmitted frame reaches the wire; a wire frame fills a posted receive buffer with a zeroed header and the bytes intact; an empty wire leaves receive buffers available (a pull queue); the wire drops past its depth rather than overflowing; and hostile rings are refused with no out-of-bounds access -- a transmit buffer marked device-writable, a frame past the maximum, a receive buffer marked device-readable, a receive buffer too small for a header, and buffers outside guest RAM |
| machine mode (userland) | `guest_dtb.bin` via `vmctl run --machine -c 2` in `rc.test` | the whole path through the real owner: `vmctl` builds the tree, loads by the header, answers PSCI, runs the second vCPU `CPU_ON` created in turn with the first, and stops at `SYSTEM_OFF`. The boot harness (`run_boot_test.py`, `HVTEST_MARKERS`) requires each guest line -- the `dtb:` line, the version, `cpu1: up ctx=1234cafe`, `cpu_on 1 -> 0` and `vmctl: guest powered off` -- because the userland shell has no `grep` to check them with in `rc.test` |
| `el2-guest-uart-level` | `aarch64/guest_uart_rx.S`, `guest_uart_wfi.S` | two bytes written before the guest handles: one interrupt drains `'a'` and `FR` says a byte still waits, a second interrupt drains `'b'` and `FR` says empty, no third -- an edge-triggered model would deliver one for two. Then a guest whose `WFI` has a two-second deadline is woken by a byte a kernel timer writes 20 ms in: the `WFI` run returns in well under a second with an interrupt pending, and the handler reads `'w'` |
| `el2-guest-spin` | `aarch64/guest_spin.S` | a guest in a one-instruction loop: `vcpu_run_limited(5)` returns `-ETIMEDOUT` after five host-interrupt exits (the tick is taken to EL2 through `HCR_EL2.IMO`), and the guest's PC never left the loop |

Without a backend every guest test and `hv-npt` return true after the
skip line; `hv-probe` then checks the `-ENOTSUP` path instead. On
AArch64 that is what `QEMU_EL2=0` produces.

## The guest images (`tests/hv/<arch>/`)

Each architecture has its own, built for its own target: x86-64's are
the real-mode and protected-mode guests below, AArch64's are
`guest_wfi`, `guest_hvc`, `guest_mmio`, `guest_sysreg`, `guest_spin`,
`guest_irq`, `guest_timer`, `guest_ctimer`, `guest_ptimer`,
`guest_timer_wfi`, `guest_gicd`, `guest_gicc`, `guest_gic` and
`guest_sgi`, `guest_mmio_widths`, `guest_uart`, `guest_uart_rx`,
`guest_uart_wfi`, `guest_uart_poll`, `guest_dtb` (the first written in
C, with an arm64 Image header, linked at `COSMO_HVM_RAM_BASE + text_offset`
and loaded there) and `guest_idreg` -- one per exit the EL2 switch
decodes, then one per
thing a guest does with its interrupt controller, then one per thing it
does with its console. Both sets are flat binaries linked
at guest-physical 0x1000 and carried in the boot archive as
`tests/hv/<name>.bin`; the self-tests and `vmctl` load them from there.
The `el2-guest-*` self-tests are AArch64's, the `hv-guest-*` tests are
x86's, and each set skips with a note on the other architecture.

### `guest_irq.S` waits in a hypercall, not a `WFI`

Worth recording, because the first version did the obvious thing and
hung the kernel for the watchdog's eight seconds. `HCR_EL2.TWI` traps a
`WFI` only when it would actually **wait**: with an interrupt already
pending -- the masked case, where the guest cannot take it -- the `WFI`
completes as a no-op and traps nothing, so a guest built around one
spins forever and `vcpu_run` never returns. Its heartbeat is `hvc`
instead, which always exits.

The handler also leaves the acknowledged INTID in `x0` rather than
restoring it: the number the guest saw is the evidence the test wants.

`guest_timer_wfi.S` *does* wait in `WFI`, and may: nothing in it is
pending-but-masked, so the `WFI` either traps (a `WFI` exit, which
`vcpu_run` now holds until the guest's deadline) or the timer interrupt
is taken first. Both timer guests report their own clock in `x1` on every
hypercall, so lateness is measured in units the guest controls and the
owner never has to trust the host's clock to judge it.

### The timer guests enable their PPI in their redistributor

`guest_timer.S` and `guest_timer_wfi.S` now begin the way a kernel does:
`GICD_CTLR` on, their redistributor found by MPIDR and woken, PPI 27
grouped and enabled in `GICR_ISENABLER0`. Since the virtual distributor,
an expiry is a pending PPI in the redistributor and reaches the guest
only if the guest enabled it there -- a guest that never did gets no
timer, which is the behaviour and not a gap. `guest_ctimer.S`, which the
timer *state* tests use and which must run on a GICv2 host, touches no
GIC at all and is unchanged.

`guest_gicd.S`, `guest_gicc.S` (the prober and the configurer) are pure
MMIO -- no `ICC_*`, no vectors -- so they too would run on any host; the
tests that use them skip on GICv2 only because there is no distributor
there to probe. `guest_gic.S` and `guest_sgi.S` bring up a CPU interface
and are GICv3-only like `guest_irq.S`.

### The `irq-route` source moved

`arch_test_periodic_irq_start` was the virtual timer -- the one hardware
periodic source to hand -- and is not now: `CNTV` is a guest's and its
PPI is the hypervisor's to bind. The hook raises the distributor's spare
SPI (the `irq-affinity` line) from a kernel timer at the requested rate,
at tick granularity. The line `irq-route` requests, enables, counts, masks
and releases is as real as before; what asserts it moved from a compare
register to a callback, and the test passes unchanged.

## The x86 guest images

All are position-dependent flat binaries for 0x1000. `guest_pio.S`,
`guest_irq.S`, `guest_cpuid.S`, `guest_shutdown.S`, `guest_spin.S` and
`guest_fpu.S` (which enables SSE for itself with CR0/CR4 writes)
are `.code16` real-mode programs; `guest_pm.S` is `.code32` and expects
the owner to have entered protected mode for it. They use only port
0xE9 (the console), `hlt`, `in`/`out`, `cpuid`, `rdmsr`/`wrmsr`,
`vmmcall`, and a real-mode IVT; none needs a BIOS. Byte offsets the tests
assert (`LOAD_GPA + 13`, `+ 0x1A`, the 5-byte `mov %eax, 0x10000000`)
are the encodings of these sources; changing an image means updating the
test.

## Userland: `vmctl` in `/etc/rc.test`

```sh
vmctl probe || echo "HVTEST: skipped"
vmctl probe && vmctl run /boot/tests/hv/guest_pio.bin > /tmp/shtest/hv.out && cat /tmp/shtest/hv.out && echo "HVTEST: PASS"
vmctl info
```

`vmctl run` opens `/dev/vmm`, creates the VM (1 MiB), loads the image,
runs until `HLT`, echoing the console (`HV`), printing the `IO` exit for
port 0x80 and `halted at 0x100e`, and exits 0; the shell then prints
`HVTEST: PASS`. `run_boot_test.py` requires `^HVTEST: PASS$`
(`HVTEST_MARKERS`) in debug and release runs. `vmctl info` prints the
four `hv.*` values. The shell test (`tests/boot/shelltest.py`) does not
drive `vmctl`; the interactive path is covered by `rc.test`.

## Boot-test log lines to look for

```text
[ INFO] svm: AMD-V with nested paging, 15 ASIDs usable
[ INFO] hv: backend svm, nested paging yes, 16 ASIDs
SELFTEST: hv-probe         ... ok
SELFTEST: hv-npt           ... ok
SELFTEST: hv-guest-pio     ... ok
...
SELFTEST: hv-guest-spin    ... ok
SELFTEST: PASS (70 tests)
vmctl: /boot/tests/hv/guest_pio.bin: 22 bytes at 0x1000, 1024 KiB, entry 0x1000
HVvmctl: io out port 0x80 size 2 value 0x1234 at 0x100d
vmctl: halted at 0x100e
HVTEST: PASS
```

`[DEBUG] hv: vmN created (uid U)` / `released` bracket every VM in debug
builds; `svm: asid A: exit code 0x...` at warning level means an exit the
decoder does not know (a `FAIL` exit followed).

## Debugging

- `make test QEMU_EXTRA="-d int,guest_errors -D /tmp/qemu-int.log"`
  logs every interrupt TCG services; a virtual interrupt delivered to a
  guest appears as `Servicing virtual hardware INT=0x20`, a host tick
  taken while a guest ran as an `INTR` exit followed by `Servicing
  hardware INT=...`. The log is large (100 MB for a boot test).
- `QEMU_EXTRA="-d cpu_reset"` and `-d guest_errors` help when a guest
  triple-faults unexpectedly.
- The debug `CHECK` messages name the line; the `hv-guest-irq` history
  is the worked example: a third `HLT` exit with the vector still
  pending at `rip 0x101a` meant the interrupt shadow was being restored
  on every entry, fixed by clearing it when skipping an instruction.

## The page-poison check as the world switch's watchdog

The memory manager's page-poison check (`docs/kernel/memory/testing.md`)
is what found the world switch leaving `SP_EL2` in a freed vCPU context
page, and it remains the check on that rule: with the restore removed,
the `TLBI` hypercall in `el2_vm_destroy` writes its four registers into
the page `el2_vcpu_destroy` has just freed, and the next allocation of
that frame panics with `last freed from el2_vcpu_destroy+0x44` and a dump
in which `x0` is `0x12` and `x1` is the VM's `VTTBR`. Ten seconds into
every debug boot, during `hv-*`, before any userland runs. The gap it
does not cover is a frame that has been *reallocated* before the stray
push -- but the push happens on the very next hypercall, and the
`el2_vm_destroy` that issues it follows the free immediately, so the
window is short and always exercised.

## What is not tested yet

- The owner-kill path (`process_kill_pending` in the run loop returning
  `-EINTR`) has no automated test; it needs a shell with job control or a
  spawned helper to kill `vmctl run guest_spin.bin`.
- `vm_device_register` beyond the built-in console (`-EBUSY` after the
  first run, `-EEXIST` on overlap) is unexercised.
- The `-EPERM` refusal of `vm_create` with a handle that is not
  `/dev/vmm`, and a non-root caller (rc.test runs as uid 0).
- String I/O (`INS`/`OUTS`) exits, `vm_mem_rw` across several regions,
  the 16-region and 8-VM limits, and concurrent runs of two vCPUs of one
  VM on different host CPUs.
- **The VMX backend is never executed here.** QEMU's TCG emulates AMD-V
  and reports `vmx: false` for every CPU model
  (`query-cpu-model-expansion` on `max`), and the development host is
  AArch64, so VMXON, the VMCS writes, `VMLAUNCH` and the exit path have
  run nowhere. What carries evidence is the pure logic
  (`tests/host/test_vmx.c`: control fixing against capability MSR
  values, the I/O exit qualification, the EPT pointer and builder) and
  the fact that adding the backend left every SVM test passing; the
  rest is compiled, statically analysed, and reviewed against the SDM.
  A green chain says nothing about VMX working, and invariant V17 says
  so as well. First contact with real hardware should expect the usual
  bring-up failures — a control the CPU refuses, a VMCS field written in
  the wrong order, a host-state field that does not describe the CPU.
- Nothing runs on hardware SVM either.
- No timer or interrupt controller model exists to test; guests are
  driven by the owner's injections.
