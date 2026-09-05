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
or decode assists, and not VT-x; the backend relies on neither (the I/O
exit's next RIP comes from EXITINFO2, which is architectural). Without
`+svm,+npt` the kernel logs `hv: no hardware virtualization backend on
this CPU`, `/dev/vmm` reads `none asids=0 vms=0`, the guest self-tests
log `selftest: hv: skipped: no backend` and pass, and `rc.test` prints
`HVTEST: skipped`. The harness treats both lines as **forbidden markers**
(`HV_FORBIDDEN_MARKERS` in `tests/boot/run_boot_test.py`) and requires
`HVTEST: PASS`, so a configuration that silently lost the backend fails
the boot test rather than skipping it.

## Host unit test: `tests/host/test_hv.c` (`make host-test`)

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

Eight of the 70 self-tests, run after the Linux ELF test. Each guest test
builds a VM with 1 MiB at guest-physical 0, copies an image from the boot
archive (`tests/hv/<name>.bin`, built by `tests/hv/hv.mk` as flat
binaries linked with `--image-base=0 -Ttext=0x1000 --oformat=binary`) to
0x1000, creates vCPU 0 and sets `rip` = 0x1000 (real mode, `cs` 0).
Console output means bytes the guest wrote to port 0xE9, read back with
`vm_console_read`.

| test | image | what it checks |
|---|---|---|
| `hv-probe` | — | backend name `svm` (or `vmx`), nested paging, ≥ 2 ASIDs; without a backend, `vm_create` is `-ENOTSUP` |
| `hv-npt` | — | `hv_vm_count` up and down; regions at 0 (64 KiB) and 0x200000 (12 KiB); overlap, misalignment, window edge and the 64 MiB limit refused; every page translates to its recorded frame (`arch_hv_vm_query` = `page_to_phys`), holes do not; copies: unbacked range `-EFAULT` before any byte, zeroed memory, a 16-byte round trip straddling a page boundary lands in the right frames |
| `hv-guest-pio` | `guest_pio.S` | `HV` reaches the console without an exit; `outw $0x80` is an `IO` exit (write, size 2, value 0x1234, `rip` after the instruction); `hlt` is `HLT` at the next byte; `inb $0x81` is an `IO` read exit completed with `'Q'` on the next run; the guest echoes it to the console; `rax` low byte is `'Q'`; the reset `cr0` is 0x60000010; exit and entry counters grew |
| `hv-guest-irq` | `guest_irq.S` | installs IVT[0x20]; `cli; hlt` exits with no pending flag; vectors 3 and 256 are `-EINVAL`; after `vcpu_inject(0x20)`, `pending_irq` reads 0x20; `sti; hlt` exits with `F_IRQ_PENDING` and nothing delivered (the STI shadow); the next run delivers it: `HLT` at 0x101A, flag clear, console `I`, `rflags.IF` set; a second injection is delivered on the next run |
| `hv-guest-cpuid` | `guest_cpuid.S` | CPUID 0x40000000 yields `CosmoOSCosmo`; CPUID 1 has the hypervisor bit; `rdmsr EFER` shows SVME clear; `wrmsr EFER` with SCE then `rdmsr` shows it (console `CosmoOSCosmo101`); `vmmcall` is a `HYPERCALL` exit with nr 7 and args 0x11 0x22 0x33 0x44; `efer` reads 1; then `HLT` |
| `hv-guest-pm` | `guest_pm.S` | a 32-bit protected-mode guest entered through `set_regs` (`cr0` PE, flat `cs` 0x0C9B / data 0x0C93); `P` on the console; a store to 0x10000000 is an `MMIO` exit (write, that address) with `rax` 0x5A5A5A5A; the owner skips the 5-byte instruction with `set_regs`; `Q` and `HLT` follow; `set_regs` with PG-without-PE and with EFER.SVME are `-EINVAL` |
| `hv-guest-shutdown` | `guest_shutdown.S` | with `idtr.limit` set to 0, `int $3` triple-faults: `SHUTDOWN` exit after `S` on the console; the next run is `-EIO`; `get_regs` still works |
| `hv-guest-spin` | `guest_spin.S` | a guest that never exits: `vcpu_run_limited(5)` returns `-ETIMEDOUT` after five host-interrupt exits (the tick reaches the guest even with its IF clear); `.` on the console; a second vCPU, index reuse `-EEXIST`, index 4 `-EINVAL`, `nr_vcpus` bookkeeping; the VM count drops when the last references go |
| `hv-guest-fpu` | `guest_fpu.S` | the guest rule of `arch/fpu.h`: the test thread takes ownership of register state (`arch_fpu_alloc`) and puts a pattern in xmm0; the guest enables SSE for itself, stores its initial xmm0 at 0x3000 (must be the reset state, zeros: nothing of the owner leaked in) and loads a pattern the test placed at 0x3010; afterwards the owner's xmm0 must still hold its own pattern (nothing of the guest leaked out), and a second run shows the guest kept running |

Without a backend every guest test and `hv-npt` return true after the
skip line; `hv-probe` then checks the `-ENOTSUP` path instead.

## The guest images (`tests/hv/`)

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
- Nothing runs on hardware SVM or on a VT-x machine (no VMX backend).
- No timer or interrupt controller model exists to test; guests are
  driven by the owner's injections.
