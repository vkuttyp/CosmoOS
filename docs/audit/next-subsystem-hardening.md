# NEXT SUBSYSTEM — a guard that is proved, not assumed

Date: 2026-09-15. Tree: `main` at 1dfbd74 (after PR #138, the
file-path unit). Chosen from
`docs/audit/2026-09-deferred-work-inventory.md` §3.

**Subsystem: the kernel's guard on its own access to user memory, run
and asserted on CPUs that have it; the hardening the loader turned off,
turned on; and the flag bits the ABI accepts without knowing them,
refused.** **Built: PR #140 (2026-09-15).** The design below is as
proposed; the sections "As built" and "As run" record what the build
changed and measured. Differences from the plan, each found by building
rather than reading:

1. **The switch needed a hand-back call of its own.** The plan had
   `arch_hv_disable` use the stub's set-vectors call. Once the switch
   owns EL2 on a CPU the stub's calls are not answered there (the `el2`
   self-test's own comment says so), so the first build's hand-back
   failed, cleared the ready flag anyway, and the next guest run asked
   the switch to be installed over itself: every `el2-*` test failed on
   both cores. The switch gained `HV_EL2_CALL_HANDBACK` (installs the
   vectors in `x1`, version 3), and the flag is cleared only when the
   call succeeded.
2. **WXN is set at kernel-table activation, not in CPU init.** The
   loader's tables map RAM writable and executable (this kernel's own
   early code runs from them), so a bit set in `aarch64_cpu_init` would
   have denied the next fetch. `arch_mmu_activate` sets it when it
   installs the kernel root on a CPU, and the hardening line is printed
   by `arch_hardening_report` after `vmm_init`, reading `SCTLR_EL1` and
   `CR4` as they are rather than what the plan intended.
3. **`arch_user_guard_present`.** Generic code cannot reach the
   arch-private CPU-info headers, so the guard test asks a hook in
   `arch/user.h` (SMAP on x86-64, PAN on AArch64) instead.
4. **`hv-disabled` restores the caps instead of re-probing.** Whether
   the x86-64 backends' probes are idempotent was not established, so
   the cycle saves `hv_caps`, disables, checks, restores, and proves the
   hand-back by running the self-check again (the switch re-installs on
   use). The plan's "fresh probe" is not exercised; the flag reset that
   a fresh probe relies on is.
5. **The `hv` suite ran on `cortex-a76` and passed whole.** After the
   stage-2 fix the boot self-check passed there and all 38 `hv-*` and
   `el2-*` tests followed; the plan's risk of "more than three faults"
   did not materialise.
6. **The flags test's `mmap` bit is bit 30**, not 31: the flags word is
   an `int` and bit 31 is its sign.
7. **The stage-2 input is capped at 48 bits.** A core reporting a
   52-bit range (FEAT_LPA) would have had `T0SZ` set for a 52-bit input
   over tables that index bits 47:12. The layout rule reports the input
   size it serves (`input_bits`, `HV_S2_INPUT_MAX`) and the VTCR takes
   `T0SZ` from that while `PS` keeps what the CPU reported; a wider
   input needs FEAT_LPA2 or concatenated level-0 tables, neither built.
   No machine here reports 52 bits, so this is reasoning and a host
   test, not a run.
8. **`hardening: absent` is forbidden in the guard boot**, not merely
   contradicted by a required line: a boot printing both would
   otherwise have passed.
9. **The flag sweep found four calls to change and two already strict.**
   Every native call taking a flags word: `mmap`, `mount`, `umount` and
   `open` gained the check; `wait` (`native.c`, the `COSMO_W*` mask) and
   `spawn` (the `COSMO_SPAWN_*` mask) already had it; `mmap`'s `prot`
   word was already exhaustive. No other native call takes one.

This report is to close two rows of the
inventory's §3: the row "SMEP/SMAP/UMIP absence silently accepted;
`mmap`/`mount`/`umount` accept unknown flag bits" (audit 14.2) in full,
and the `SCTLR_EL1.WXN` part of the row "AArch64 hardening" (audit
13.2, 13.3); what that row keeps is named at the end.

The kernel brackets every deliberate access to user memory with
`arch_user_access_begin` / `arch_user_access_end`, which is `stac` /
`clac` on x86-64 and `msr pan, #0` / `msr pan, #1` on AArch64. The
bracket is correct code. It is also, on every machine this tree has
ever been tested on, a no-op: the x86-64 boot runs on QEMU's `qemu64`
model, which has no SMEP, SMAP or UMIP, so `has_smap` is false and the
bracket compiles to a test-and-skip; the AArch64 boot runs on
`cortex-a72`, which has no PAN, so `has_pan` is false and the bracket
does nothing either. An access to user memory *outside* the bracket
therefore behaves exactly like one inside it, on both architectures, in
every boot the continuous integration has run. The guard has never been
seen guarding.

Two probes settle whether that matters. Booting the unchanged image on
`qemu64,+nx,+svm,+npt,+smep,+smap,+umip` passes all 265 self-tests: the
x86-64 tree's accesses are all bracketed. Booting it on `cortex-a76`, a
core with PAN and a 40-bit physical address range, fails 5 of 265:
four ASID tests, each reading a user page from the kernel with
`arch_copy_user_raw` and no bracket (the raw copy's own contract says
"call inside `arch_user_access_begin/end`"; nothing enforced it because
nothing could), and the `el2` test, for a reason that has nothing to do
with PAN and everything to do with a tree that has only ever run on
one core: the hypervisor's stage-2 walk starts at a level the ARM
architecture forbids below a 43-bit address range, the first guest
instruction faults at level 0, the boot self-check disables the
backend, and the disabled backend keeps EL2's vectors so the stub the
test talks to is gone. The second core found three bugs in one boot,
one of them in the guard's own tests.

This unit makes the second core a permanent part of the test: a
`test-guard` target, modelled on `test-gic`, boots the same image on the
protection-capable model of each architecture, the harness requires the
kernel's own line saying the guard is live, and the three faults the
core found are fixed. A self-test proves the guard on the core that has
it (an unbracketed probe faults, a bracketed one does not) and says so
when the core lacks it; a user program proves UMIP the same way. The
kernel's boot line says which protections it has and warns, at `WARN`,
when a guard is absent, instead of a `DEBUG` line in debug builds only.
The native ABI's `mmap`, `mount`, `umount` and `open` return `-EINVAL`
for a flag bit they do not know, so a program can learn what the kernel
it runs on supports, and a future flag cannot be silently ignored. And
`SCTLR_EL1.WXN`, which the loader clears, is set by the kernel on every
CPU, with a crash-test variant that executes a deliberately writable
and executable page to prove the bit denies what the leaf allowed.

## Problem

Three facts, each verified in this tree, each a hole in what the tests
can say.

**The guard is compiled out of every tested boot.** `x86_cpu_enable_features`
sets `CR4.SMEP`, `CR4.SMAP` and `CR4.UMIP` when `cpuid` reports them
(`kernel/arch/x86_64/cpu.c:104-112`); `arch_user_access_begin` executes
`stac` only when `has_smap` (`kernel/arch/x86_64/user.c:104-108`). On
AArch64, `aarch64_cpu_init` sets `SCTLR_EL1.SPAN` and `PSTATE.PAN` when
`ID_AA64MMFR1_EL1.PAN` is non-zero (`kernel/arch/aarch64/cpu.c:71-77`),
and the bracket toggles PAN only then
(`kernel/arch/aarch64/user.c:31-40`). Both are right. Both are dead on
the models CI boots: `scripts/qemu-run.sh:249` gives x86-64
`qemu64,+nx,+svm,+npt` and `scripts/qemu-run.sh:213` gives AArch64
`cortex-a72`. QEMU 11.1.1 has both protection-capable models
(`qemu64` accepts `+smep,+smap,+umip`; `cortex-a76` has PAN), and the
tree has never been booted on either by anything but this report's two
probes.

**What the probes found.** The x86-64 image on
`qemu64,+nx,+svm,+npt,+smep,+smap,+umip`: the kernel's debug line reads
`x86: nx=1 smep=1 smap=1 umip=1`, and the run passes 265 of 265. The
AArch64 image on `cortex-a76`: the debug line reads `aarch64:
Cortex-A76 r4p1, EL1, MPIDR 0x80000000, PAN 1, PARange 2, GIC sysregs 0,
ASID 16-bit`, and the run fails 5 of 265:

```
SELFTEST: asid-isolation   ... FAIL: a user read faulted with the space active (33 ms)
SELFTEST: asid-rollover    ... FAIL: check failed: fa == 0 && fb == 0 at line 782 (4 ms)
SELFTEST: asid-paranoid    ... FAIL: a user read faulted with the space active (59 ms)
SELFTEST: asid-destroy-reuse ... FAIL: check failed: f1 == 0 && v1 == 0xAA at line 872 (2 ms)
SELFTEST: el2              ... FAIL: check failed: el2_call_raw(EL2_STUB_VERSION_CALL, 0) == EL2_STUB_VERSION at line 669 (0 ms)
```

The four ASID tests switch to a private space with interrupts off and
read one byte at a user address through `arch_copy_user_raw`
(`kernel/memory/memtest.c:691,697,767,773,840,863`) with no
`arch_user_access_begin` around it. With PAN set, a kernel access to a
page that user mode may access is a permission fault; the exception
table resolves it (the copy returns "1 byte not copied") and the test
reads that as a broken ASID. The ASID code is fine; the test is the
kind of code the guard exists to catch, and it has passed every boot
since the ASID unit (PR #68) because the guard was never on.

The `el2` failure has two causes, both outside PAN. First, the boot log
on `cortex-a76` reads:

```
[ INFO] hv: EL2 with stage-2 translation, 40-bit addresses, 15 VMIDs, guest interrupts unavailable (no GICv3 virtual interface)
[ WARN] hv: self-check: a guest with paging off exited with rc 0 kind 3 pc 0x80000000 (wanted kind 7)
[ WARN] hv: backend el2 disabled: nested paging does not confine a guest with paging off (QEMU/TCG before 9.2 has this bug)
```

Exit kind 3 is `COSMO_VM_EXIT_MMIO`, which `hv_el2.c:997-1002` reports
for any abort from the guest; a temporary print of the exit's registers
(this report's probe, not committed) gave `esr 0x82000004 hpfar 0x800000
far 0x80000000`: an instruction abort from a lower exception level
(`EC` 0x20) with fault status 0b000100, a translation fault at level 0,
at the guest's first instruction. The stage-2 walk never resolved
anything. `hv_s2_vtcr` (`kernel/arch/aarch64/hv_s2.c:207-212`) sets
`VTCR_EL2.SL0 = 2`, which for the 4 KiB granule means "start at level
0" in the architecture's numbering (its comment calls that "level 1 of
four": the first of the walk's four levels). The architecture
allows a level-0 start for the 4 KiB granule only when the physical
address size exceeds 42 bits (the `SL0` table in the ARM ARM's
description of `VTCR_EL2`; QEMU's `check_s2_mmu_setup` refuses "level 0
with 4 KiB pages when PAMax <= 42" with exactly this fault). `cortex-a72`
reports a 44-bit range (the control boot's line: `aarch64: Cortex-A72
r0p3, EL1, MPIDR 0x80000000, PAN 0, PARange 4, ...`, and `hv: EL2 with
stage-2 translation, 44-bit addresses`) and the start is legal;
`cortex-a76` reports 40 bits (`PARange 2`) and every walk faults at its
first step. The SMMU
driver derives its stage-2 configuration "the same way"
(`drivers/iommu/arm_smmuv3.c:79-81`, `SL0 2`) and builds its tables
with the generic IOMMU walker, which always starts from one four-level
root (`kernel/iommu/pt.c`: `iommu_pt_alloc_table`, `iommu_pt_map`,
`iommu_pt_unmap`, `iommu_pt_lookup`, `iommu_pt_free`); it carries the
same assumption. QEMU's SMMUv3 reports 44 bits, so it has never fired
there, and it cannot be exercised here.

Second, the self-check's failure disables the backend by clearing
`hv_caps` (`kernel-services/virtualization/vmm.c`, `hv_init`), but the
self-check's own run had already taken EL2 on its CPU: the first time a
CPU runs a guest, `el2_ready_here` installs the switch's stack and
vectors there (`kernel/arch/aarch64/hv_el2.c:243-262`,
`el2_set_vectors(kernel_va_to_pa(hv_el2_vectors))`) and marks the CPU
ready, and the disable path never hands them back. The `el2` self-test sees `hv_caps()->present ==
false`, asks the stub for its version
(`kernel-services/virtualization/hvtest.c:669`), and the switch, which
owns EL2 and does not know `EL2_STUB_VERSION_CALL`, answers `-1`. The
whole `hv` suite then prints `selftest: hv: skipped: no backend`, which
the harness forbids, so the boot fails on that too.

**Absence is a debug line.** The only record of which protections a
boot has is `kdebug("x86: nx=%d smep=%d smap=%d umip=%d ...")`
(`kernel/arch/x86_64/start.c:46-48`) and `kdebug("aarch64: %s, EL%u,
..., PAN %d, ...")` (`kernel/arch/aarch64/start.c:25`): `DEBUG` level,
so absent from a release boot, and never asserted by the harness.
`docs/kernel/arch/invariants.md` I-ARCH-9 says "automated assertion is
future work". A kernel running with no guard on its user-memory accesses
says nothing about it.

**Unknown flag bits are accepted.** `sys_mmap` tests `COSMO_MAP_ANONYMOUS`
and `COSMO_MAP_FIXED` and ignores every other bit
(`kernel/syscall/native.c:334,351`); `sys_mount` masks its flags with
`COSMO_MOUNT_RDONLY` (`native.c:613`) and `sys_umount` with
`COSMO_UMOUNT_FORCE` (`native.c:632`); `sys_open` passes its flags to
`vfs_open`, which reads `COSMO_O_ACCMODE`, `CREAT`, `EXCL`, `TRUNC`,
`APPEND` and `DIRECTORY` and ignores the rest
(`kernel-services/vfs/vfs.c:937-951`). A program that passes a flag
this kernel does not implement gets success and not the flag. The
audit's 14.2 row records it; `sys_wait` already shows the rule the rest
should follow: `flags & ~(COSMO_WNOHANG | COSMO_WUNTRACED |
COSMO_WCONTINUED)` is `-EINVAL` (`native.c:1046-1048`).

**WXN is cleared and never set.** The loader clears `SCTLR_EL1.WXN`
together with alignment checking on its way to the kernel
(`boot/uefi/arch/aarch64/cpu.c:177`, `sctlr &= ~(SCTLR_WXN | SCTLR_A)`);
the kernel defines the bit (`kernel/arch/aarch64/include/aarch64/sysreg.h:54`)
and never writes it. Kernel W^X (`docs/kernel/memory/invariants.md`
M13) rests on every leaf's PXN/UXN being right; WXN would make a
writable page non-executable whatever its leaf said. Nothing in the
kernel executes from a writable page: modules are written while RW and
remapped RX before they run (`docs/kernel/arch/design.md`, "Phase 3
additions"), the AP trampoline is identity-mapped `VM_PROT_RX`
(`kernel/arch/aarch64/smp.c:94-106`), user W+X is refused by `mmap`
(`native.c:338`), by the ELF loader (`kernel/process/elf.c:125-126`)
and by the Linux personality (`compat/linux/syscalls.c:837`), and EL2
runs under its own `SCTLR_EL2`. The bit costs nothing and is off.

## Current implementation

- **The bracket.** `arch_user_access_begin/end` in
  `kernel/include/arch/user.h:109-111`, implemented per architecture as
  above; used by `copy_from_user`, `copy_to_user`,
  `strncpy_from_user` and the raw copy's callers. `arch_copy_user_raw`
  (`user.h:116-121`) lists every access in the exception table and
  requires the bracket from its caller.
- **The fixup test.** `kernel/syscall/uaccesstest.c` (`uaccess`) runs
  with no process, so every user address is unmapped and every copy
  faults through the table to `-EFAULT`; it proves the table and the
  range checks, and cannot prove the guard, because on its CPU there is
  none and because it maps nothing.
- **Feature detection and enabling.** x86-64: `x86_cpu_init` /
  `x86_cpu_enable_features` (`cpu.c:95-118`), every CPU. AArch64:
  `aarch64_cpu_init` (`cpu.c:41,71-77`), every CPU.
- **The boots.** `make test` (`Makefile:66-69`) boots the default model;
  `make test-gic` (`Makefile:77-90`) reboots the same image with
  `QEMU_GIC=3` twice on AArch64 and is a no-op elsewhere; CI runs both
  (`.github/workflows/ci.yml:69-76`). `make test-crash` (`Makefile:93-99`)
  builds with `CRASH_TEST=1` into a sibling tree and expects the panic
  (`kernel/core/main.c:208-220`). The harness takes the machine's shape
  from the environment (`tests/boot/run_boot_test.py:93-99,150-198`) and
  builds `REQUIRED_MARKERS` from it (`run_boot_test.py:113-165`).
- **Flags.** As cited under Problem. The Linux personality translates
  its own flag words (`lx_open_flags`, `lx_prot`, the `LX_MAP_*` tests
  at `compat/linux/syscalls.c:850-861`) and, like Linux, ignores bits it
  does not know.
- **Stage 2.** `hv_s2.c`: a four-level walk from a single root page
  (`hv_s2_create`, `walk_to` from internal level 3 down), `hv_s2_vtcr`
  with `SL0 = 2`; the SMMU's `STE_S2VTCR_FIXED` likewise.

## Why it matters

A guard that is never on is a guard whose failures are invisible. The
tree has an unbracketed user access in four tests today and passed every
CI run with it; the next one will be in a syscall, and the CI CPU will
pass that too, until someone boots on hardware. Every x86-64 CPU
since Broadwell (2014) has SMAP; every ARMv8.1 core has PAN. The tests run on the
one kind of machine that hides the bug.

The second core found two hypervisor bugs that are not about the guard
at all, and that is the general point: a tree tested on one CPU model
per architecture has that model's parameters baked in as invariants.
A 44-bit physical range is not an invariant. A cheap second boot
(the same image, one QEMU flag) is the only test that can tell.

The unknown-bit rule is what lets an ABI grow. A future `COSMO_O_NONBLOCK`
or `COSMO_MAP_SHARED` can be probed by a program (`-EINVAL` means "not
here") only if today's kernel refuses it; today it succeeds silently,
and the program cannot tell a kernel that implemented the flag from one
that dropped it.

WXN is one bit, on every CPU, that turns a per-leaf policy into an
architectural one. The kernel's own tables are already W^X (M13); this
makes the CPU enforce it whatever a future leaf says.

## Design

### The second boot

A `test-guard` target, one per architecture, boots the image `make test`
built on the protection-capable model, with everything else the same as
`test`:

- x86-64: `QEMU_CPU='qemu64,+nx,+svm,+npt,+smep,+smap,+umip'`.
- AArch64: `QEMU_CPU=cortex-a76` (PAN, 40-bit physical range, VHE
  present but unused: the kernel runs at EL1 and the switch keeps
  `HCR_EL2.E2H` clear).

The target sets `QEMU_GUARD=1` for the harness, which then adds two
required markers and one forbidden marker (below). CI runs it as a
step after `test-gic` in both jobs ("Boot test on a protection-capable
CPU (debug)"), about ninety seconds per architecture. The default
boot is unchanged: `qemu64` and `cortex-a72` stay the control, on
which the bracket is a no-op and every fixup path still runs. The two
boots differ in one thing, the CPU model; a failure in one and not the
other names the guard.

### What the second core found, fixed

1. **The ASID tests take the bracket.** The six raw reads in
   `kernel/memory/memtest.c` are wrapped in `arch_user_access_begin()` /
   `arch_user_access_end()`, inside the interrupts-off window they
   already hold (the bracket is per-CPU state, PSTATE or `EFLAGS.AC`,
   and the window keeps the CPU). Nothing else in the tree reads user
   memory outside the bracket: the x86-64 guard boot proves it for the
   whole suite, and the AArch64 one will, once these six are fixed.

2. **The stage-2 start level follows the address range.** A pure rule,
   `hv_s2_layout(unsigned pa_bits, struct hv_s2_layout *out)`, in a
   header with no kernel dependencies (`kernel/include/arch/hv_s2_core.h`,
   after `lockup_core.h`'s pattern), gives the walk's starting level and
   the number of concatenated root pages: for `pa_bits > 42`, a level-0
   start (`SL0 = 2`) from one root page; for `40 <= pa_bits <= 42`, a
   level-1 start (`SL0 = 1`) from `1 << (pa_bits - 39)` contiguous root
   pages aligned to their own size, the level-1 index widened by the same
   bits; for `pa_bits < 40`, a level-1 start from one page. `hv_s2_create`
   allocates the root with `pmm_alloc_pages(order, PMM_FLAGS_ZERO)` at
   the layout's order (the allocator returns naturally aligned blocks);
   `walk_to`, `destroy_level`, `count_level` and `hv_s2_vtcr` take the
   start level from the layout computed once at probe. The SMMU driver
   asks the same rule about `IDR5.OAS` at probe and, when the rule says
   a level-1 start (an output size of 42 bits or less), refuses stage-2
   with `-ENOTSUP` and a message naming the width, instead of
   programming `S2SL0 = 2` over tables the walker built for level 0.
   The walker itself is not changed by this unit: a concatenated root
   in `kernel/iommu/pt.c` (its indexing, mapping, unmapping, lookup and
   destruction) cannot be tested on QEMU's 44-bit SMMU, and a host
   lacking the width cannot test the code for it; the refusal is what
   can be built honestly, and the walker change goes to the inventory
   as follow-up work with this report's evidence.

3. **A disabled backend hands EL2 back.** `arch_hv_disable(void)` in
   `kernel/include/arch/hv.h`, implemented on AArch64 as
   `el2_set_vectors(el2_stub_phys())` on every CPU whose `g_el2_ready`
   flag `el2_ready_here` set, clearing the flag, and a no-op on x86-64
   (`kernel/arch/x86_64/hv.c`). `hv_init` calls it
   on the self-check's failure path before clearing `hv_caps`. The `el2`
   self-test's stub branch then holds whether the backend was never
   present, or was present and disabled.

With the first two fixed the self-check passes on `cortex-a76` and the
`hv` suite runs there; what it finds is the implementation's to find,
and the target's requirement is the whole suite passing. This report
commits to that, not to "only these three".

### The guard self-test

`uaccess-guard`, in `kernel/syscall/uaccesstest.c` after `uaccess`,
registered after it in `selftest.c`. Like the ASID tests it creates a
private `vm_space`, maps one user page and fills it, switches to the
space with interrupts off, and reads one byte at the user address with
`arch_copy_user_raw` twice: once inside `arch_user_access_begin/end`,
which must succeed with the byte, and once outside, whose outcome is
the guard:

- the read faults (the copy returns 1): the guard is live; the test
  records `guard = live` and asserts the byte was not delivered;
- the read succeeds: the guard is absent on this CPU; the test records
  `guard = absent` and asserts the byte *was* delivered (the page is
  mapped; a fault here would be a broken table, not a guard).

Either way the bracketed read must succeed: a bracket that does not
open is the bug the test exists for on a guard CPU, and a table bug on
any CPU. The test prints `selftest: uaccess-guard: guard live` or
`... guard absent (no smap|no pan)`, and the harness in a guard boot
requires the first form, so on the guard CPU the fault assertion is
live and asserted, and on the control CPU the test asserts the mapping
and says what it could not assert. The two runs together are the proof;
neither alone is.

UMIP, on x86-64, is proved from user mode: `init --trap umip` executes
`sgdt` (a user-legal instruction without UMIP, `#GP` with it) and the
`trap_selftest` in `userland/init/init.c` spawns it and accepts either
exit: status `128 + 11` (killed by SIGSEGV, UMIP enforced) or 0 (the
instruction ran, UMIP absent), printing `usertest: umip: enforced` or
`usertest: umip: absent`. The x86-64 guard boot requires `enforced`.
SMEP is set and logged but not exercised: its test would be the kernel
jumping to user memory, an instruction fetch the exception table has no
fixup for, so its only form is a crash test, and this report does not
add one (see Alternatives).

### The line that says so

Every boot, both builds, on every CPU, after the boot CPU's features
are enabled:

```
[ INFO] hardening: x86-64: nx smep smap umip
[ WARN] hardening: absent: smep smap umip -- kernel access to user memory is unguarded
[ INFO] hardening: aarch64: pan wxn
[ WARN] hardening: absent: pan -- kernel access to user memory is unguarded
```

The `INFO` line lists what is on; the `WARN` line, printed only when
something is off, lists what is not, and names the consequence when
the missing feature is the guard (`smap` or `pan`); `nx` on x86-64 is
already mandatory (`x86_64/mmu.c:66` panics without it) and `wxn` on
AArch64 is set unconditionally by this unit, so neither appears under
`absent`. `kdebug` lines stay. The harness: with `QEMU_GUARD=1` the
`INFO` line with every feature named is required and
`hardening: absent` is forbidden; without it, nothing changes, and the
control boots carry the `WARN` from now on, which is the truth about
them. I-ARCH-9's "automated assertion is future work" becomes "checked
by the guard boot's required marker".

### Unknown bits

The native ABI refuses a flag bit it does not define, with `-EINVAL`,
at the four calls the audit and this report name:

| call | accepted bits | site |
| --- | --- | --- |
| `mmap` | `COSMO_MAP_ANONYMOUS`, `COSMO_MAP_FIXED` | `sys_mmap` |
| `mount` | `COSMO_MOUNT_RDONLY` | `sys_mount` |
| `umount` | `COSMO_UMOUNT_FORCE` | `sys_umount` |
| `open` | `COSMO_O_ACCMODE`, `CREAT`, `EXCL`, `TRUNC`, `APPEND`, `DIRECTORY` | `sys_open` |

The check is at the syscall, before the path or the space is touched,
as `sys_wait`'s is; `vfs_open` keeps its own tolerance because the
kernel's internal callers add bits (`COSMO_O_DIRECTORY` for a trailing
slash). The implementation sweeps every other native call that takes a
flags word and either finds it already strict or adds the check, and
lists the result in "As built". The
Linux personality is a second door to the same objects and keeps
Linux's own rule, which is to ignore unknown `MAP_`, `MS_` and `O_`
bits; a Linux program that relies on that is correct on Linux and stays
correct here. The rule is per ABI: ours is strict because it is ours to
grow; Linux's is Linux's.

### WXN

`aarch64_cpu_init` sets `SCTLR_EL1.WXN` on every CPU, after the page
tables it runs on are the kernel's (the loader's are gone by then;
`SPAN` is set at the same place). The bit makes any writable page in
the EL1&0 regime execute-never regardless of its leaf, so it changes
nothing for a correct tree and denies exactly one thing: a future W+X
leaf. User mappings are already W^X at every door (native `mmap`, the ELF
loader, the Linux personality's `mmap` and `mprotect`), so no user
program can notice.

A crash-test variant proves the bit is what denies: `CRASH_TEST=2`
(`make test-wxn`, AArch64 only, a no-op elsewhere like `test-gic`)
maps one kernel page `VM_PROT_READ | VM_PROT_WRITE | VM_PROT_EXEC` on
purpose through `arch_mmu_map`, writes a `ret` into it, and calls it.
Without WXN the leaf allows the fetch and the call returns, which the
variant reports as `crash test: a writable page executed; WXN is off`
and counts as a failure; with WXN the fetch is an instruction abort at
EL1 and the kernel panics. The harness's panic run today requires the
unmapped-write crash's own signature (`run_boot_test.py:326-354`: the
`crash test: writing to an unmapped address` line, the page-fault panic
line, `trap 1029`, `FAR=ffff900000000000`), which an instruction abort
cannot satisfy, and accepting any panic would not prove WXN caused it.
So `--expect-panic` takes a kind: `fault` (today's markers, the
default) or `wxn`, whose required markers use the page-fault path's
existing wording, so no diagnostic code changes: the variant's own line
`crash test: executing a writable page on purpose`; the panic line
`KERNEL PANIC: page fault: kernel execute at <page> (protection): ...`
(`kernel/memory/vmm.c:608-612` already says `execute` for
`VM_FAULT_EXEC` and `protection` for a present page); the trap's
`FAR=<page> (protection read kernel instruction-fetch)` line
(`kernel/arch/aarch64/trap.c:201-204`); the stack trace and `halting.`.
Its forbidden markers are `boot complete`, the `WXN is off` line and a
recursive panic. The page's address is what the variant chose, so the
markers can name it exactly, as the `fault` kind names
`ffff900000000000`. The test exists so that the bit's absence has a symptom; it is
the only path in the tree that maps W+X, and only in that build.

### The §70 gate

*Ownership and lifetime.* The stage-2 root becomes an order-`n`
allocation owned exactly as the single page was: `hv_s2_destroy` frees
the block at the same order it was allocated, and the layout that
chose the order is a per-boot constant read at probe, so create and
destroy cannot disagree. `arch_hv_disable` hands EL2 to the stub on the
CPUs that took it, and nothing references the switch's vectors after;
the probe's other state (`g_vtcr`, VMID bookkeeping) is inert with
`present == false`. The guard test's `vm_space` is destroyed before the
test returns, as the ASID tests' are.

*Concurrency.* The guard test's read sequence runs with interrupts off
on one CPU; PAN and `EFLAGS.AC` are per-CPU state that an interrupt
would not change (the trap entry masks `AC` via `SFMASK` on x86-64; on
AArch64 `SPAN` set means an exception entry leaves PAN alone) and the
window forbids the interrupt anyway. The hardening line is printed by
the boot CPU once; the APs enable the same features silently and the
existing per-CPU debug lines remain. WXN is per-CPU `SCTLR_EL1` state
set by the CPU it affects, before it schedules.

*Memory.* Concatenated roots: 2 pages on a 40-bit machine, 4 at 41, 8
at 42, per VM; today one page. Nothing else allocates.

*Error handling.* An unknown flag bit is `-EINVAL` before any side
effect. A guard boot on a QEMU without the model (`cortex-a76` arrived
in QEMU 7.1; the `+smap` feature names are old) fails at QEMU
start with QEMU's own message, and the target says which model it asked
for. The SMMU driver refuses an output size of 42 bits or less with
`-ENOTSUP` at probe, naming the width, rather than programming a start
level the architecture forbids over tables built for another. A `hardening: absent` `WARN` is a warning, not a refusal: a
machine without SMAP boots, as it always has.

*Security.* The unit's whole point. What it does not change: SMEP is
still not exercised; UAO, E0PD, BTI and PAC remain unused; a
user-triggerable SError still panics (see Alternatives for each).

*Performance.* `stac`/`clac` and `msr pan` are serialising-free
single instructions; the bounce copies in the syscall path execute two
per chunk. The benchmarks section measures the difference on the guard
CPU against the control CPU with the same image. WXN has no runtime
cost. The two extra boots cost CI about three minutes.

*Future extensibility.* `hv_s2_layout` is where a 16 KiB or 64 KiB
granule, or FEAT_LPA2, would enter. The hardening line is where the
next feature (UAO, E0PD, BTI) is listed when it is used. `QEMU_GUARD`
is a harness knob any future boot variant can reuse. The strict flag
rule is the ABI's growth path: a new flag is a new accepted bit.

## Affected files

| file | change |
| --- | --- |
| `Makefile` | `test-guard` target (both architectures); `test-wxn` target (AArch64; a no-op elsewhere) building with `CRASH_TEST=2` and running the harness with `--expect-panic wxn`; `test-crash` passes `--expect-panic fault`; help lines |
| `.github/workflows/ci.yml` | the two steps, after `test-gic` |
| `tests/boot/run_boot_test.py` | `GUARD = os.environ.get("QEMU_GUARD", "0") != "0"`; required markers `hardening: (x86-64|aarch64): ...` with every feature, `uaccess-guard: guard live`, `umip: enforced` (x86-64); forbidden `hardening: absent`; `--expect-panic` takes a kind (`fault`, `wxn`) with the `wxn` marker sets |
| `scripts/qemu-run.sh` | unchanged: `QEMU_CPU` is already honoured |
| `kernel/include/arch/cpu.h` | `arch_hardening_report` |
| `kernel/arch/x86_64/cpu.c` | `arch_hardening_report`: the `INFO`/`WARN` lines from `CR4` |
| `kernel/arch/aarch64/cpu.c` | the same from `SCTLR_EL1` |
| `kernel/arch/aarch64/mmu.c` | `SCTLR_EL1.WXN` set in `arch_mmu_activate` when the kernel root goes in (as-built 2) |
| `kernel/include/arch/user.h`, `kernel/arch/x86_64/user.c`, `kernel/arch/aarch64/user.c` | `arch_user_guard_present` (as-built 3) |
| `kernel/memory/memtest.c` | the six raw reads bracketed |
| `kernel/syscall/uaccesstest.c` | `uaccess-guard` |
| `kernel/core/selftest.c` | registration after `uaccess` |
| `kernel/core/main.c` | `CRASH_TEST == 2`: the W+X page executed on purpose |
| `build/toolchain.mk` | unchanged: `CRASH_TEST` is already passed as a number |
| `kernel/syscall/native.c` | unknown bits refused in `sys_mmap`, `sys_mount`, `sys_umount`, `sys_open`, and the sweep |
| `kernel/include/arch/hv_s2_core.h` | new: `struct hv_s2_layout`, `hv_s2_layout` |
| `kernel/arch/aarch64/hv_s2.c` | the layout-driven root, walk, destroy, count, VTCR |
| `kernel/arch/aarch64/hv_el2.c` | layout computed at probe; `arch_hv_disable` through the switch |
| `kernel/arch/aarch64/hv_el2_switch.S`, `kernel/include/arch/el2.h` | `HV_EL2_CALL_HANDBACK`, switch version 3 (as-built 1) |
| `kernel/arch/aarch64/include/aarch64/hv_s2.h` | `hv_s2_configure`, `hv_s2_current_layout` |
| `kernel/include/arch/hv.h` | `arch_hv_disable` |
| `kernel/arch/x86_64/hv.c` | `arch_hv_disable` no-op |
| `kernel-services/virtualization/vmm.c` | `hv_init` calls `arch_hv_disable` on the self-check's failure path |
| `kernel-services/virtualization/hvtest.c` | `el2` unchanged; `hv-disabled` (fault-injected self-check failure) |
| `kernel/include/kernel/hv.h` | `hv_selftest_disable_cycle` (debug builds) |
| `kernel/include/kernel/selftest.h` | the two new self-tests |
| `kernel/include/kernel/faultinject.h`, `kernel/core/faultinject.c` | `FI_HV_SELFCHECK` |
| `drivers/iommu/arm_smmuv3.c` | asks `hv_s2_layout` about `IDR5.OAS` at probe; `-ENOTSUP` with a message at 42 bits or less (`kernel/iommu/pt.c` unchanged: the concatenated root there is follow-up work, untestable on QEMU's 44-bit SMMU) |
| `tests/host/test_hv_s2.c`, `tests/host/host.mk` | the layout rule on the host |
| `userland/init/init.c` | `--trap umip`; the flags test in `fs_selftest`/`proc_selftest` |
| docs | `docs/kernel/arch/invariants.md` (I-ARCH-9 checked), `docs/kernel/arch/testing.md` (the guard boot, `test-wxn`), `docs/kernel/arch/aarch64/design.md` (WXN; the stage-2 start level), `docs/kernel/security/design.md` (a "Hardening" section: what is on, where it is asserted), `docs/kernel/syscall/api.md` (the unknown-bit rule on the four calls), `docs/verification/design.md` (the guard boot as a verification step), `docs/kernel-services/virtualization/` (the layout rule; `arch_hv_disable`), `README.md` Status, `docs/README.md`, the inventory |

## New APIs

```c
/* kernel/include/arch/hv_s2_core.h -- pure, host-testable */
struct hv_s2_layout {
    unsigned start_level;   /* internal: 3 = a level-0 start, 2 = level-1 */
    unsigned root_order;    /* log2 of the root's page count: 0..3 */
    unsigned sl0;           /* the VTCR_EL2.SL0 / STE.S2SL0 encoding: 2 or 1 */
};
void hv_s2_layout(unsigned pa_bits, struct hv_s2_layout *out);

/* kernel/include/arch/hv.h */
void arch_hv_disable(void);   /* the backend is not to be used: give the hardware back */

/* kernel/include/kernel/faultinject.h */
FI_HV_SELFCHECK,              /* the boot self-check reports failure once: the disable path */
```

`hv_s2_vtcr(unsigned pa_bits, unsigned ps_field)` keeps its signature
and takes `SL0` from the layout. `hv_s2_create(void)` keeps its
signature; the root's order comes from the layout computed at probe.
No user-visible API changes: the four calls return an existing errno
for a new reason, documented.

## Migration plan

1. **The second boot, failing.** `test-guard`, `QEMU_GUARD`, the
   harness's `hardening:` markers, the CI steps. On this step's tree the
   AArch64 boot fails 5 of 265 and the x86-64 one fails on the missing
   `hardening:` line; the step is committed with the failing state
   recorded in "As run", so the fixes that follow have a test that
   fails first.
2. **The three faults.** The bracket in `memtest.c`; the layout rule,
   its host test, and the stage-2 root, walk and VTCR that follow it
   (the SMMU driver's probe-time refusal with it); `arch_hv_disable` and the
   self-check's failure path. The AArch64 guard boot passes the whole
   suite, or "As run" says what it found next and the step is not done.
3. **The lines.** The hardening `INFO`/`WARN` on both architectures;
   the markers of step 1 now pass on x86-64.
4. **The guard tests.** `uaccess-guard`; `--trap umip` and its two
   outcomes; their required markers in the guard boot (`guard live`,
   `umip: enforced`); `hv-disabled`.
5. **Unknown bits.** The four checks, the sweep, the user-side test.
6. **WXN.** The bit on every CPU; `CRASH_TEST=2`; the harness's
   `--expect-panic` kind and the `wxn` marker sets; `test-wxn`.
7. **Docs, README Status, inventory, the report's as-built sections.**

Each step boots both architectures on both CPU models; steps 4 and 5
also run the release build and the host tests; step 6 runs `test-wxn`
and `test-crash`.

## Tests

| test | what it asserts | bug-proof (what makes it fail for the stated reason) |
| --- | --- | --- |
| `uaccess-guard` (guard boot) | with a user page mapped and the space active, the unbracketed raw read returns 1 and the byte is not delivered; the bracketed read returns 0 with the byte; prints `guard live` | remove the `msr pan, #0` / `stac` from `arch_user_access_begin`: the bracketed read faults; make `aarch64_cpu_init` skip `msr pan, #1`: the unbracketed read succeeds and the harness fails on the required marker |
| `uaccess-guard` (control boot) | both reads return 0 with the byte; prints `guard absent (no pan)` / `(no smap)` | unmap the page before the reads: both fault, the test fails on the bracketed one |
| `--trap umip` (x86-64 guard boot) | the child's `sgdt` ends it with status 139; `usertest: umip: enforced` | clear `CR4_UMIP` in `x86_cpu_enable_features`: status 0, the marker missing |
| `test_hv_s2` (host) | `hv_s2_layout` for 32..52 bits: `sl0 = 2` and one page only above 42; `sl0 = 1` with `1 << (bits - 39)` pages at 40, 41, 42; `sl0 = 1` and one page below 40; the root order never exceeds 3 | return `sl0 = 2` for every width: the 40-bit case fails |
| the AArch64 guard boot's `hv` suite | every `hv` and `el2` test passes on `cortex-a76`; `hv: backend el2` with nested paging in the log; no `selftest: hv: skipped` | revert the layout in `hv_s2_vtcr` to `SL0 = 2`: the self-check fails with the level-0 translation fault, exactly as the probe did |
| `hv-disabled` | with `FI_HV_SELFCHECK` armed for one hit, `hv_init`'s path is re-run through a test hook: `hv_caps()->present` is false and `el2_call_raw(EL2_STUB_VERSION_CALL, 0) == EL2_STUB_VERSION` still holds; hits asserted equal to the budget | remove `arch_hv_disable` from the failure path: the stub does not answer |
| `flags` (user-side, in `fs_selftest` and `proc_selftest`) | `mmap` with bit 31 set, `mount` with `1u << 5`, `umount` with `1u << 5`, `open` with `0x8000000`: each `-COSMO_EINVAL`; the same calls without the bit succeed as before | remove any one check: that call succeeds |
| `test-wxn` (AArch64) | the crash-test build's W+X page traps on execution; the harness's `wxn` marker set sees the variant's own line, the `page fault: kernel execute at <page> (protection)` panic, the `FAR=<page> (... instruction-fetch)` line, the stack trace and `halting.`, and none of the forbidden lines | do not set `WXN` in `aarch64_cpu_init`: the page executes, the variant logs `WXN is off` (a forbidden marker), and no panic follows |
| the guard boot itself | required markers `hardening: x86-64: nx smep smap umip` / `hardening: aarch64: pan wxn`; forbidden `hardening: absent` | boot the guard target with `QEMU_CPU` forced to the control model: the `WARN` appears and the run fails |
| the ASID tests on `cortex-a76` | pass, with the bracket | drop the bracket from one read: that test fails as the probe showed |

**Vacuity, named in advance.** `uaccess-guard` on the control CPU
cannot assert a fault; it asserts the mapping instead and *says* the
guard is absent, and the guard boot requires the other sentence. A
guard test that passed because the space switch never happened would
read the kernel's own mapping at the user address (unmapped: a fault
in the bracketed read), so the bracketed read's success is also the
proof that the space was active. `hv-disabled` asserts the fault
injection's hit count equals its budget, so a self-check that was never
reached fails it. `test-wxn` without WXN produces a log line and a
failed run, not a pass. The flags test runs the same calls without the
bogus bit so a kernel that refused everything would fail it too.

### As run

The probes that motivated the unit, on the unchanged tree: x86-64 on
`qemu64,+nx,+svm,+npt,+smep,+smap,+umip`, 265 of 265 passed; AArch64 on
`cortex-a76`, 5 of 265 failed, named under Problem, with the
hypervisor's exit registers read through a temporary print that was not
committed.

The unit, on both architectures (PR #140, 2026-09-15, QEMU 11.1.1):

| run | x86-64 | AArch64 |
| --- | --- | --- |
| `make test` (control) | 267 of 267, `hardening: x86-64: nx` + `WARN absent: smep smap umip -- kernel access to user memory is unguarded`, `uaccess-guard: guard absent (no smap)`, `umip: absent`; 79.0 s | 267 of 267, `hardening: aarch64: wxn` + `WARN absent: pan -- …`, `guard absent (no pan)`; 85.1 s |
| `make test-guard` | 267 of 267, `hardening: x86-64: nx smep smap umip`, `guard live`, `umip: enforced`; 78.2 s | 267 of 267 on `cortex-a76`, `hardening: aarch64: pan wxn`, `guard live`, `hv: EL2 with stage-2 translation, 40-bit addresses (level-1 start, 2 root pages)`, backend enabled, the 38 `hv-*`/`el2-*` tests passing; 87.8 s |
| `make test-wxn` | no-op | PASS: `page fault: kernel execute at 0xffff900000000000 (protection): no region`, `trap 1029 … ESR=0x000000008600000f EC=0x21`, `FAR=ffff900000000000 (protection read kernel instruction-fetch)`; 74.5 s |
| `make test-crash`, `test-gic`, release `test`, host tests, fuzz, analyzer | all pass | all pass |

The first AArch64 run of the unit failed 22 of 267 (`hv-disabled` and
every `el2-*`/`hv-*` test after it, on both cores): the hand-back
through the stub's call, as-built difference 1. With the switch's call
the runs above followed, and they were run again after the review
round's changes (x86-64 control and guard, AArch64 control, guard and
`test-wxn`: all pass).

One lesson from the proofs themselves, since it cost three runs: the
injection script reverted `kernel`, `drivers`, `tests` and `userland`
but not `kernel-services`, so an injection there survived its proof and
failed `hv-disabled` in every later run until the tree was restored.
The revert must name every directory an injection can touch.

**Bug-proofs, as run** (each injection applied alone, the run named,
then reverted):

| injection | run | result |
| --- | --- | --- |
| `layout-always-l0`: the rule says a level-0 start for every width | `make host-test` | `hv-s2 FAIL (20)`, the 40..42-bit cases. The first attempt was vacuous: the host binary did not rebuild for a header-only change, so the rule's make prerequisite now names the header |
| `vtcr-l0`: the VTCR says `SL0 = 2` whatever the layout | `make ARCH=aarch64 test-guard` | FAIL on `cortex-a76`: every `hv` marker missing (`HVTEST: PASS`, `psci version`, `cpu1: up`, …) -- the unchanged tree's fault, reproduced |
| `no-hand-back`: the disable path does not call `arch_hv_disable` | `make ARCH=aarch64 test` | FAIL: `hv-disabled … check failed: rc == 0`, step -2 (the stub did not answer while the backend was disabled) |
| `asid-no-bracket`: one ASID read outside the bracket | `make ARCH=aarch64 test-guard` | FAIL 4 of 267: `asid-isolation`, `asid-rollover`, `asid-paranoid`, `asid-destroy-reuse`, the exact failures the unchanged tree gave on `cortex-a76` |
| `bracket-closed-a64` / `bracket-closed-x86`: `arch_user_access_begin` never opens | both guard boots | FAIL: `selftest: uaccess-guard: guard live` missing, and userland never starts (every user copy faults) |
| `pan-not-detected`: the core's PAN is not seen | `make ARCH=aarch64 test-guard` | FAIL: `hardening: aarch64: pan wxn` and `guard live` missing, `hardening: absent` present (forbidden). Removing only the init-time `msr pan, #1` on every CPU does **not** fail it: every `arch_user_access_end` sets PAN again |
| `space-not-active`: the private space is never made active | `make test` | FAIL: `uaccess-guard … check failed: left_in == 0 && in == 0x5A` -- the vacuity guard, since the kernel's tables have nothing at that address |
| `umip-off`: `CR4.UMIP` never set | `make test-guard` (x86-64) | FAIL: `hardening: x86-64: nx smep smap umip` and `usertest: umip: enforced` missing |
| `mmap-flags`: the `mmap` bit check removed | `make test` | FAIL: `USERTEST: check failed: cosmo_mmap(…, COSMO_MAP_ANONYMOUS \| (1 << 30)) == -COSMO_EINVAL` |
| `umount-flags`: the `umount` bit check removed | `make test` | FAIL: `USERTEST: check failed: cosmo_umount2("/tmp/flagm", 1u << 5) == -COSMO_EINVAL`. The first attempt passed: the test unmounted `/tmp`, which is not a mount point, so `-EINVAL` came back for the wrong reason; `mount` and `umount` now use a mount point of their own and the same calls without the bit must succeed |
| `wxn-off`: `SCTLR_EL1.WXN` not set | `make ARCH=aarch64 test-wxn` | FAIL: every `wxn` marker missing and the forbidden `crash test: a writable page executed; WXN is off` present |


## Benchmarks

As run (PR #140), the same image on the control and the guard CPU
model, one boot each:

| | x86-64 `qemu64` | x86-64 `+smep,+smap,+umip` | AArch64 `cortex-a72` | AArch64 `cortex-a76` |
| --- | --- | --- | --- | --- |
| `USERBENCH` read 200 KiB at 1 KiB requests | 46 MiB/s | 47 MiB/s | 36 MiB/s | 37 MiB/s |
| at 4 KiB | 103 | 108 | 85 | 99 |
| at 64 KiB | 112 | 115 | 140 | 144 |
| the 38 `hv-*`/`el2-*` tests | 159 ms | 147 ms | 1273 ms | 419 ms |
| boot to verdict | 79.0 s | 78.2 s | 85.1 s | 87.8 s |

The guard instructions cost nothing measurable under TCG (the guard
runs are within noise of the control, and slightly faster, which is the
noise). The a76 `hv` suite is three times faster than the a72's, a
property of QEMU's models, not of the concatenated root. None of these
gates the unit; they are the baseline the plan asked for:

- The `USERBENCH` read line the file-path unit added (`read` of
  64 KiB through the bounce, in calls, microseconds and MiB/s): the
  guard CPU pays two guard instructions per bounce chunk; the expectation is a difference
  under the run-to-run noise on TCG, and the number is recorded either
  way.
- The `hv` suite's time on `cortex-a76` against `cortex-a72`: the
  concatenated root costs one extra page per VM and nothing per walk.
- Boot-to-`boot complete` on each model: the guard boots add nothing
  to the kernel; the number says what the second CPU model costs QEMU.

None of these gates the unit; they are recorded so a later change has a
baseline.

## Risks

- **The AArch64 guard boot finds more than three faults.** Likely: the
  `hv` suite has never run on a core with VHE, 16-bit VMIDs (`15 VMIDs`
  in the a76 line is the switch's own limit, not the core's) or a
  40-bit range. The plan's step 2 is not done until the whole suite
  passes there, and "As run" records what it found. The risk is to the
  unit's size, not its shape.
- **The stage-2 rewrite regresses the control CPU.** Every step boots
  both models; the 44-bit path (one page, level 0) is the existing
  behaviour and stays the CI default's.
- **A `WARN` on every control boot.** It is new noise in every CI log,
  and it is true. A reader who wants it gone changes the CI CPU, which
  this unit deliberately does not (the control is the point).
- **WXN denies something the tree does that this report missed.** The
  audit and this report found no writable executable page; the crash
  test's is the only one made on purpose. WXN is ARMv8.0, so both
  `cortex-a72` and `cortex-a76` enforce it and every AArch64 boot from
  step 6 on is the test: a fetch that faults after the bit is set names
  the page, and the mapping that made it W+X is the bug.
- **The strict flag rule breaks a native program.** The native programs
  are this tree's (`userland/`); the sweep greps every caller of the
  four calls for the flags it passes. A program that passed garbage
  would already be wrong.
- **QEMU's `cortex-a76` model changes.** The target names one model;
  the harness's markers name features, not the model, so a move to
  another PAN core is a one-line change.

## Alternatives considered

- **Make the guard CPU the default and drop the control.** Rejected:
  the control is where `has_smap == false` and `has_pan == false` paths
  run, and where the guard's absence is proved to be handled, not just
  its presence.
- **Assert the guard from the harness by grepping the debug line.**
  Rejected: the line is debug-only and asserted nothing about the
  kernel's own view; the `INFO`/`WARN` pair is the kernel saying it,
  in every build.
- **Test SMEP with a crash-test variant.** Deferred: it needs the
  kernel to jump to a user page on purpose, which needs a mapped user
  page in the crash-test path (the ASID tests' fixture) and a third
  crash-test variant; the value is low next to SMAP's, since the kernel
  has no indirect jump that user data can steer. Named, not built.
- **Wrap `arch_copy_user_raw` so it brackets itself.** Rejected: the
  bracket is deliberately the caller's, so a copy loop pays it once,
  not per chunk; the guard boot now enforces the contract the comment
  states.
- **Reject unknown Linux flag bits too.** Rejected: the Linux
  personality's contract is Linux's, and Linux ignores them.
- **Limit the stage-2 input range to 39 bits instead of concatenating
  roots.** Rejected: a guest's physical map is the owner's to lay out,
  and `SELFCHECK_GPA` at 2 GiB is already above what a smaller range
  would cover on some layouts; the concatenated root is the
  architecture's own answer to this width.
- **The IOMMU walker's concatenated root.** Left out: the SMMU driver
  refuses a 42-bit-or-smaller output size at probe rather than carrying
  a walker change no machine here can run; the inventory gets a row
  for it (`kernel/iommu/pt.c`, with this report's stage-2 evidence).
- **UAO, E0PD, BTI, PAC; the EL0 SError.** Left out, and the inventory
  row keeps them: UAO is moot while every user access goes through the
  bracket; E0PD is a kernel-address-space separation the tree does not
  have; BTI and PAC are a toolchain and user-ABI change; an SError from
  EL0 has no deterministic trigger under TCG, so its test cannot be
  written here. The row after this unit reads: "UAO, E0PD, BTI, PAC
  unused; a user-triggerable SError panics the kernel; no device-tree
  parsing for the host (ACPI only); PSCI variations untested".
