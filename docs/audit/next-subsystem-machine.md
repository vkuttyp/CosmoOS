# NEXT SUBSYSTEM — the machine a guest is handed: a device tree, the entry convention, and PSCI

## Problem

An AArch64 guest now has an interrupt controller it can drive the
ordinary way, a timer of its own, and a console it can print to and be
typed at. Every one of those sits at an address and an interrupt number
the hypervisor chose -- and **nothing tells the guest what they are.** A
guest enters with `x0 = 0`, as every register is, and the tree contains
no code that writes a device tree: `grep -rli fdt kernel boot userland
libc` finds only the word in comments.

The fixtures that exercise those units know the layout because they were
written against it: `movz x10, #0x0900, lsl #16` is the UART because the
author of `guest_uart.S` read `gicv3_vdist.h`. A kernel compiled for the
architecture cannot be asked to do that. An arm64 kernel's entry contract
is `x0 = the physical address of a device tree blob`, and its first act is
to walk it for `/memory`, `/cpus`, the interrupt controller, the timer and
`stdout-path`; handed `0` it stops -- Linux's `setup_machine_fdt` panics
on an invalid blob before a single line is printed, and that line would
have gone to the UART the blob was supposed to name.

Two more things such a kernel does before it is useful, and both go to
the owner today as exits nobody answers. It calls **PSCI** -- the ARM
power-state interface, `PSCI_VERSION` first, then `CPU_ON` to bring its
secondary CPUs up, and `SYSTEM_OFF` to power down -- through `hvc`, which
here is a `HYPERCALL` exit that `vmctl` prints and continues from with
`x0` unchanged, so `PSCI_VERSION` would return its own function id
(`0x84000000`) as the version. And it expects to be loaded where its
**Image header** says: `text_offset` from a 2 MiB-aligned base, not at
`0x1000`.

So the last gating item before software written for the architecture can
run here is not a device. It is the description of the machine, handed
over the way the architecture's boot protocol hands it.

## Current implementation

**The layout exists as constants, in three headers, in the kernel.**
`gicv3_vdist.h` (`VDIST_GICD_BASE 0x08000000`, `VDIST_GICR_BASE
0x080A0000`, `VDIST_GICR_STRIDE`, 288 lines), `kernel/hv.h` (`VUART_BASE
0x09000000`, `VUART_INTID 33`), and the timer's PPI 27 in `hv_el2.c`. Each
unit said "the hypervisor's constant, like the GIC's" and pointed at "the
device tree a guest is eventually handed" as the place they would be read
from. Nothing outside the kernel can read them: they are not in the uapi.

**A guest enters with what `vmctl run` sets: `pc`.** `regs.pc = entry`
(`vmctl.c:147`) and nothing else; `x0..x30` are zero, `sp_el1` is zero,
the image is loaded at `0x1000` in a VM whose memory starts at `0`. That
is right for a flat test binary and wrong for every arm64 kernel image
there is.

**`HVC` reaches the owner and is not answered.** `COSMO_VM_EXIT_HYPERCALL`
carries `x0..x4`; `vmctl` prints them and runs again (`vmctl.c:179`). The
guest sees its own argument registers unchanged. A PSCI call is an SMCCC
call -- function id in `x0`, result in `x0` -- so this is not "PSCI
unimplemented", it is "PSCI answers with garbage".

**One vCPU per `vmctl`.** `vmctl` creates vCPU 0 and runs it in a loop.
The kernel supports four per VM and the distributor routes SGIs between
them (`el2-guest-sgi`), but the only owner that exists runs one. The
native libc has no threads, so a second vCPU has no thread to run on, and
`SYS_vcpu_run` has no budget: a vCPU that never exits keeps its caller
until the guest does something. (The kernel's own tests use an internal
`vcpu_run_limited(max_intr)` for exactly this; it is not in the uapi.)

**The guest images are flat binaries linked at `0x1000`**, all assembly,
built by `tests/hv/hv.mk` and carried in the boot archive; the userland
harness (`rc.test`) runs one and checks `HVTEST: PASS`.

## Why it matters

- **It is what turns four units into one machine.** The vGIC, the timer,
  the distributor and the console each work for a guest written to know
  where they are. A device tree is the one artefact that makes them work
  for a guest that does not -- which is every guest anyone else wrote.
- **It is the arm64 boot protocol, and there is only one.** `x0` is the
  DTB, the image goes at `text_offset`, PSCI brings up the other CPUs.
  There is no design freedom to spend here; there is a contract to meet,
  and meeting it is what makes "boot a stock kernel" a test rather than
  an aspiration.
- **The layout becomes a contract instead of a convention.** Today it is
  three headers that agree because the same person wrote them. In the
  uapi, read by the device-tree writer and the kernel alike, it is one
  source with two readers, and a change to either side that the other
  did not see fails a test.
- **PSCI is where SMP begins for a real guest.** The distributor unit
  made a guest SMP-capable; a stock kernel gets there through `CPU_ON`,
  and today `CPU_ON` is a printed hypercall. Answering it is small, and it
  is the difference between "the kernel could run on four vCPUs" and
  "a kernel does".
- **It gives the tree its first C guest.** A guest that parses a device
  tree is not a thing to write in assembly. The fixture for this unit is
  written in C, and every fixture after it can be.

## Proposed design

### 1. The layout moves into the uapi, once

`kernel/include/uapi/cosmo/hv_machine.h`: the guest machine as the
hypervisor defines it -- `COSMO_HVM_GICD_BASE`, `_GICD_SIZE`,
`_GICR_BASE`, `_GICR_STRIDE`, `_GICR_FRAMES`, `_UART_BASE`, `_UART_INTID`,
the timer PPIs (`_TIMER_PPI_VIRT 27`, `_PHYS 30`, `_HYP 26`, `_SEC 29`),
the number of SPIs, `COSMO_HVM_RAM_BASE 0x40000000` (where `virt` puts
RAM and where the machine mode below places it), and `COSMO_HV_VCPUS_MAX`
which is already there. The kernel's `VDIST_*` and `VUART_*` become
aliases of these; the device-tree writer reads them; nothing else defines
an address. The header is the contract the last four units promised.

### 2. A device-tree writer, shared by the owner and the build

`tools/fdt/fdt.c`, a flattened-device-tree *writer* -- not libfdt: a
writer needs no parsing, no overlays, no reallocation, and is about three
hundred lines: a header, a memory-reservation block, a structure block
built by `begin_node`/`prop`/`end_node` calls, a strings block, and
`totalsize`. It is compiled into `vmctl` and into a host tool (`mkdtb`),
so the same code produces the blob a running owner hands a guest and the
blob the build puts in the boot archive for the kernel's own tests.

The machine it describes is the one the kernel implements, from the uapi
header, and nothing else:

| node | content |
|---|---|
| `/` | `compatible = "cosmo,virt"`, `#address-cells = <2>`, `#size-cells = <2>`, `interrupt-parent = <&gic>` |
| `/cpus/cpu@N` | one per vCPU, `compatible = "arm,armv8"`, `reg = <N>`, `enable-method = "psci"` |
| `/memory@40000000` | `device_type = "memory"`, `reg = <base size>` from the VM's regions |
| `/psci` | `compatible = "arm,psci-1.0", "arm,psci-0.2"`, `method = "hvc"` |
| `/timer` | `compatible = "arm,armv8-timer"`, the four PPIs as `<1 ppi flags>` |
| `/intc@8000000` | `compatible = "arm,gic-v3"`, `#interrupt-cells = <3>`, `interrupt-controller`, `reg = <GICD 0x10000>, <GICR stride*frames>` |
| `/pl011@9000000` | `compatible = "arm,pl011", "arm,primecell"`, `reg`, `interrupts = <0 1 4>` (SPI 33, level-high), `clocks`, `clock-names`, `current-speed` |
| `/apb-pclk` | a `fixed-clock`, because the PL011 driver insists on one |
| `/chosen` | `stdout-path = "/pl011@9000000"`, `bootargs` from the command line |
| `/aliases` | `serial0` |

Node names, `phandle`s and the GIC interrupt-cell encoding (`<type
number flags>`, PPIs numbered from 16, SPIs from 32) are the parts a
stock kernel is unforgiving about, and they are the parts the C guest in
§6 checks by reading them back.

### 3. The entry convention: a machine mode for `vmctl`

`vmctl run --machine [--kernel Image] [--append CMDLINE] [-c N]` builds
the machine rather than a flat test: RAM at `COSMO_HVM_RAM_BASE`, the
image loaded by its **arm64 Image header** -- magic `ARM\x64` at 56,
`text_offset` at 8, `image_size` at 16 -- at `RAM_BASE + text_offset`
(or at `RAM_BASE` for a flat binary with no header), the DTB written into
guest memory after the image on an 8-byte boundary and below 512 MiB, and
vCPU 0 started with `x0 = DTB`, `x1 = x2 = x3 = 0`, `pc = entry`, EL1h
with interrupts masked, MMU off -- which is what `ctx_reset` already
gives it. The existing `vmctl run IMAGE` is unchanged: the flat tests
keep their `0x1000`.

### 4. PSCI, answered by the owner

The SMCCC range is the owner's to answer and `vmctl` answers it, in the
`HYPERCALL` case, by `cosmo_vcpu_set_regs` of `x0` (and stepping is not
needed: `hvc` already advanced the PC):

| function | answer |
|---|---|
| `PSCI_VERSION` (`0x84000000`) | `0x00010000` (1.0) |
| `PSCI_FEATURES` (`0x8400000A`) | 0 for the functions below, `NOT_SUPPORTED` otherwise |
| `CPU_ON` (`0xC4000003`): `x1` target MPIDR, `x2` entry, `x3` context | create vCPU `Aff0(x1)` if it does not exist, `pc = x2`, `x0 = x3`, add it to the run set; `SUCCESS`, or `ALREADY_ON`, or `INVALID_PARAMETERS` above the VM's limit |
| `CPU_OFF` (`0x84000002`) | the vCPU leaves the run set |
| `AFFINITY_INFO` (`0xC4000004`) | `ON`/`OFF` from the run set |
| `MIGRATE_INFO_TYPE` (`0x84000006`) | 2 (no migration) |
| `SYSTEM_OFF` (`0x84000008`) | `vmctl` prints "guest powered off" and exits 0 |
| `SYSTEM_RESET` (`0x84000009`) | printed and exit 0, as `virt` without reboot would |
| anything else in `0x8400_0000..0xC400_FFFF` | `NOT_SUPPORTED` (`-1`) |

Hypercalls outside the SMCCC range keep today's behaviour: printed, `x0`
untouched -- the fixtures' own protocol.

### 5. More than one vCPU in one thread

The native libc has no threads, and the unit does not wait for them: a
single-threaded owner can run several vCPUs if a run can be **bounded**.
`SYS_vcpu_run` gains an optional third argument, `flags`, with
`COSMO_VCPU_RUN_ONE_TICK`: the run returns after the first host-interrupt
exit with a new exit kind, `COSMO_VM_EXIT_PREEMPTED` -- "nothing happened;
your other vCPUs may want a turn". A missing argument is 0 and today's
behaviour. `vmctl` round-robins its run set with the flag, so `CPU_ON`
produces a vCPU that actually runs, interleaved at the host's tick rate
(250 Hz here: a 4 ms slice, which is what a guest kernel's scheduler
expects to see). The kernel side is small -- the loop already has
`max_intr` for its own tests -- and it is the right primitive regardless
of threads: an owner with threads still wants to bound a run.

### 6. The first guest written in C

`tests/hv/aarch64/guest_dtb.c` with a four-instruction assembly entry
(`_start`: stack, then `main(x0)`), built `-ffreestanding -nostdlib` to
the same flat binary the assembly guests are. It walks the DTB in `x0`
-- a reader of about a hundred lines, structure block only -- finds
`/chosen/stdout-path`, follows it to the `pl011` node, reads `reg` and
`interrupts`, counts `/cpus/cpu@*`, reads `/memory`'s `reg`, and prints
what it found *through the UART the tree named*:

```text
dtb: uart@9000000 irq 33 cpus 2 mem 40000000+4000000 psci hvc
```

then calls `PSCI_VERSION` and prints it, `CPU_ON`s vCPU 1 with an entry
that prints `cpu1: up` through the same UART and `CPU_OFF`s, waits for
that line's side effect, and `SYSTEM_OFF`s. Every number in that line came
from the blob, and the line arrived through the device the blob
described. That is the whole unit in one output.

### 7. Deliberately out of scope

- **Booting Linux.** The test that would settle whether the blob is
  right by every stock driver's standards is a Linux boot, and it needs
  an image the tree does not carry and a virtio disk to do anything after
  `init`. This unit builds the machine description Linux needs and proves
  it with a guest that reads it the same way; the boot is the next report,
  and it will find what this one got wrong.
- **virtio-mmio.** The DTB will carry `virtio_mmio@...` nodes when there
  is a device behind them; there is not yet.
- **An initrd, `/chosen/linux,initrd-*`.** Nothing to load one into.
- **ACPI for the guest.** arm64 kernels take a DTB even when they will
  use ACPI; the DTB is the universal contract.
- **Threads in the native libc.** Wanted, unrelated, not this unit's to
  build; §5 is what makes that not matter here.

## Affected files

| file | change |
|---|---|
| `kernel/include/uapi/cosmo/hv_machine.h` | **new**: the guest machine's layout, the one source |
| `kernel/include/uapi/cosmo/syscall.h` | `SYS_vcpu_run` gains `flags`; `COSMO_VCPU_RUN_ONE_TICK`; `COSMO_VM_EXIT_PREEMPTED` |
| `kernel/arch/aarch64/include/aarch64/gicv3_vdist.h`, `kernel/include/kernel/hv.h`, `hv_el2.c` | `VDIST_*`, `VUART_*`, the timer PPI become aliases of the uapi constants |
| `kernel-services/virtualization/hvsys.c`, `vcpu.c` | the bounded run: `max_intr` reached becomes `PREEMPTED`, not `-ETIMEDOUT`, when asked for |
| `libc/include/cosmo/hv.h` | `cosmo_vcpu_run_flags` |
| `tools/fdt/fdt.c`, `fdt.h` | **new**: the writer, and the machine description built from the uapi header |
| `tools/fdt/mkdtb.c` | **new**: host tool; the build writes `tests/hv/virt.dtb` into the boot archive |
| `userland/system/vmctl.c` | `--machine`, the Image header, the DTB placement and `x0`, PSCI, the run set and round-robin |
| `tests/hv/aarch64/guest_dtb.c`, `guest_dtb_start.S` | **new**: the first C guest |
| `tests/hv/hv.mk` | a rule for C guests |
| `tests/host/test_fdt.c`, `host.mk` | the writer's own test |
| `kernel-services/virtualization/hvtest.c` | `el2-guest-dtb`, `el2-vcpu-run-tick` |
| `userland/etc/rc.test` | the machine-mode HVTEST |
| docs | `virtualization/design.md` ("The machine a guest is handed"), `testing.md`, README |

## New APIs

```c
/* uapi cosmo/hv_machine.h -- the guest machine, as the hypervisor defines it */
#define COSMO_HVM_RAM_BASE      0x40000000ull
#define COSMO_HVM_GICD_BASE     0x08000000ull
#define COSMO_HVM_GICD_SIZE     0x00010000ull
#define COSMO_HVM_GICR_BASE     0x080A0000ull
#define COSMO_HVM_GICR_STRIDE   0x00020000ull
#define COSMO_HVM_GICR_FRAMES   COSMO_HV_VCPUS_MAX
#define COSMO_HVM_NR_SPIS       256u
#define COSMO_HVM_UART_BASE     0x09000000ull
#define COSMO_HVM_UART_INTID    33u
#define COSMO_HVM_TIMER_PPI_VIRT 27u   /* and PHYS 30, HYP 26, SEC 29 */

/* uapi: a bounded run */
#define COSMO_VCPU_RUN_ONE_TICK 1u          /* return PREEMPTED at the first host-interrupt exit */
#define COSMO_VM_EXIT_PREEMPTED 10u         /* nothing happened; run again when you like */
static inline int cosmo_vcpu_run_flags(int vcpu, struct cosmo_vm_exit *x, unsigned flags);

/* tools/fdt/fdt.h -- the writer */
struct fdt_writer;
int  fdt_begin(struct fdt_writer *w, void *buf, size_t cap);
int  fdt_begin_node(struct fdt_writer *w, const char *name);
int  fdt_prop(struct fdt_writer *w, const char *name, const void *val, uint32_t len);
int  fdt_prop_u32(struct fdt_writer *w, const char *name, uint32_t v);
int  fdt_prop_u64(struct fdt_writer *w, const char *name, uint64_t v);
int  fdt_prop_str(struct fdt_writer *w, const char *name, const char *s);
int  fdt_end_node(struct fdt_writer *w);
int  fdt_finish(struct fdt_writer *w, size_t *size);
/* The machine: `nr_cpus` vCPUs, `ram_bytes` at COSMO_HVM_RAM_BASE, `bootargs`. */
int  fdt_cosmo_virt(void *buf, size_t cap, unsigned nr_cpus, uint64_t ram_bytes, const char *bootargs, size_t *size);
```

The kernel's own interfaces do not change shape: `vcpu_run_limited`
exists; it gains a caller that asked for it.

## Migration plan

1. **The header.** `hv_machine.h`; the kernel's constants become its
   aliases; a static assertion in each says they agree. No behaviour
   change. Verifiable by the build and by `el2-guest-gicd-probe` still
   reading 288 lines.
2. **The writer and its test.** `tools/fdt/fdt.c`, `test_fdt` on the
   host: build the machine blob, check the header's magic and
   `totalsize`, walk the structure block, find `/pl011@9000000` and read
   `reg` back as `<0 0x9000000 0 0x1000>`, `interrupts` as `<0 1 4>`, the
   GIC's two `reg` ranges, the `cpu@N` count, `stdout-path`. `mkdtb` puts
   `tests/hv/virt.dtb` in the boot archive.
3. **The C guest, run by the kernel's test.** The `hv.mk` rule for a C
   guest; `guest_dtb.c`; `el2-guest-dtb` loads the guest and the archive's
   blob into a VM, sets `x0`, and reads the line back from the console:
   every field equal to the uapi constant it came from. The kernel test
   stops at the PSCI calls (it is the owner; it answers them as `vmctl`
   will, minimally) -- so this step also proves the guest's parser
   against the blob the build made.
4. **The bounded run.** `COSMO_VCPU_RUN_ONE_TICK`, `PREEMPTED`;
   `el2-vcpu-run-tick`: a spinning guest run with the flag returns
   `PREEMPTED` within a few ticks and its PC has moved; without the flag
   the same guest still needs `-ETIMEDOUT` from the internal bound.
5. **`vmctl --machine` and PSCI.** Image header, RAM base, DTB
   placement, `x0`; the PSCI table; the run set. `rc.test` runs
   `vmctl run --machine /boot/tests/hv/guest_dtb.bin` and requires the
   `dtb:` line, the version line, `cpu1: up`, and "guest powered off" --
   that is `CPU_ON` producing a running second vCPU in a single-threaded
   owner, end to end, from userland.
6. Docs, README.

## Tests

- **`test_fdt`** (host) -- the writer's blob has a valid header and is
  the size it says; every node and property above reads back through an
  independent walk of the structure block; `phandle`s resolve;
  `stdout-path` names a node that exists. Fails before step 2 trivially;
  the point is that it keeps failing when a later change breaks the
  encoding, on the host, in a second.
- **`el2-guest-dtb`** -- the C guest finds the UART where the blob says,
  and the line it prints through it carries `9000000`, `33`, the vCPU
  count and the memory range -- each compared to the uapi constant, not
  to a literal, so the test is the header's and the blob's agreement.
  Fails on a tree where `x0` is not set (the guest prints nothing: no
  UART to print to), and on a tree where the blob's UART address is wrong
  (the guest prints to the wrong place: the console stays empty).
- **`el2-vcpu-run-tick`** -- a guest that never exits, run with
  `ONE_TICK`, returns `PREEMPTED` and has made progress; run again it
  makes more; the flag is per call. Fails where the flag is ignored
  (the run never returns: the internal bound catches it with
  `-ETIMEDOUT`, which the test refuses).
- **`el2-guest-psci`** -- the C guest's PSCI calls answered by the test
  as the owner: `PSCI_VERSION` reads `0x10000`; `CPU_ON` for vCPU 1 with
  the guest's entry and context produces a second vCPU whose first
  hypercall carries that context in `x0`; `CPU_ON` for a vCPU above the
  limit is `INVALID_PARAMETERS`; `SYSTEM_OFF` is reported. This is the
  owner-side table, tested from the kernel before `vmctl` carries it.
- **`HVTEST` machine mode** (userland, `rc.test`) -- `vmctl run
  --machine guest_dtb.bin` prints the `dtb:` line, the version, `cpu1:
  up` and "guest powered off": the whole path through the real owner, the
  real blob, two vCPUs in one thread.

Each with the bug-proof this project expects. The two the previous units
would warn about: the `dtb:` line must be compared field by field to the
uapi constants (a test that only checks "a line arrived" passes with a
wrong blob that happens to name the right UART), and `el2-guest-psci`
must check the second vCPU's context register, not merely that a second
vCPU exists.

## Benchmarks

Counted, not timed:

- **What a guest knows at entry: nothing today; the machine after.** The
  headline.
- **Blob size and node count** -- a few hundred bytes, ten nodes; the
  number that says a device tree is not a cost worth designing around.
- **PSCI calls answered versus printed** -- for the C guest, four and
  zero; today zero and four.
- **Hypercall exits per second under `ONE_TICK`** -- the round-robin's
  overhead: one `PREEMPTED` per tick per vCPU, 250 per second here. The
  count that says a single-threaded owner is viable, and the one that
  will justify threads when it is not.

## Risks

- **The blob is right by this tree's reader and may be wrong by
  Linux's.** The C guest and `test_fdt` check what the writer wrote
  against what the writer meant; a stock kernel checks against thirty
  years of convention (`#address-cells` inheritance, the exact
  `compatible` strings its drivers match, `clock-names` order). The
  mitigation is to copy `virt`'s shape node for node -- it is the most
  booted device tree there is -- and to name the Linux boot as the next
  unit, where the remaining errors will be found by the only reader that
  matters. Recorded here so that unit's findings are expected, not
  surprising.
- **A single-threaded owner is a scheduler.** Round-robin at the tick is
  fair and simple, and it is also what makes a guest's spinlock between
  two vCPUs take up to a slice to resolve. Fine for a test guest and for
  a first Linux boot; a real owner wants a thread per vCPU, and §5's flag
  is what such an owner would still use to stay responsive. The unit
  does not pretend otherwise; it records the number (§Benchmarks).
- **`CPU_ON` creates a vCPU after the VM has started.** `vcpu_create`
  allows it (the distributor learns the frame, `GICR_TYPER.Last` moves),
  but nothing has done it to a VM with a running sibling before. The
  distributor's `vdist_vcpu_present` takes its lock; the sibling's entry
  reads the same state under it. `el2-guest-psci` is the test that a
  vCPU created mid-flight is routable and runnable.
- **The Image header is a contract with a version.** `text_offset`
  semantics changed in Linux 3.17 (a flag bit says whether the offset is
  from a 2 MiB base). Read the flag; place accordingly; a flat binary with
  no header goes at `RAM_BASE`.
- **Two memory layouts.** The flat tests keep RAM at `0` and the image at
  `0x1000`; machine mode puts RAM at `0x40000000`. Both are the VM's
  owner's choice through `vm_mem_add`, and the kernel does not care; but
  a fixture written for one will fault under the other, so the C guest
  reads its memory range from the blob and assumes nothing.
- **A third argument to `SYS_vcpu_run`.** Old userland passes two;
  the kernel reads a garbage third from the register file unless the
  syscall entry zeroes unused arguments -- check the dispatcher, and
  mask the flags to the defined bits regardless.

## Alternatives considered

- **Import libfdt.** Well-tested and the standard; also a parser, an
  overlay engine and a tree editor this unit does not need, under a
  licence to record, at ten times the size of a writer. The reader the C
  guest needs is a hundred lines and reading is the guest's problem in
  any case. A writer this tree owns is the smaller thing.
- **Hand the guest ACPI tables instead**, as the host itself consumes. An
  arm64 kernel needs the DTB first regardless -- ACPI systems pass a
  minimal DTB pointing at the tables -- so this does not remove the DTB,
  it adds ACPI. Later, if a guest wants it; not instead.
- **Answer PSCI in the kernel.** Tempting, since `CPU_ON` touches vCPU
  creation, which is the kernel's. But PSCI is the *machine's* firmware
  interface and the owner is the machine; `SYSTEM_OFF` has to end the
  owner's loop, `CPU_ON` has to add to the owner's run set. The kernel
  gives the owner what it needs -- the exit, `set_regs`, `vcpu_create`
  mid-flight, a bounded run -- and the owner is the firmware. This is
  also where KVM draws the line (`KVM_CAP_ARM_PSCI` is the exception, made
  for performance a test owner does not need).
- **Wait for threads in the native libc** before multi-vCPU owners. The
  bounded run is needed anyway, is a few lines in a loop that already has
  the bound, and lets this unit prove `CPU_ON` end to end now. Threads
  make the owner faster, not possible.
- **Keep hand-written guests and skip the description.** Every unit so
  far proved a mechanism with a fixture that knew the answer. The claim
  the hypervisor now wants to make -- that it runs software written for
  the architecture -- cannot be made by a fixture written for the
  hypervisor. This unit is what lets the next one try.
