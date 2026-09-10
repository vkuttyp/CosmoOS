# NEXT SUBSYSTEM — the guest's console: a PL011 a stock kernel can print to

## Problem

An AArch64 guest can be interrupted, keep time, and drive its interrupt
controller the ordinary way, and it still **cannot say a single
character**. Measured, with a guest whose second instruction stores `'A'`
to `UARTDR` of the UART every `virt` guest kernel prints to first:

```text
EXPERIMENT: guest wrote 'A' to UARTDR -> exit kind 3 gpa 0x9000000 write 1; console pending 0 bytes
```

Exit kind 3 is `COSMO_VM_EXIT_MMIO`. The store reached the owner as "a
write happened at this address"; `vmctl` prints `mmio write at
0x9000000: no device; stopping` and exits. The guest's console ring
(`vm->console`, which x86 guests fill through port `0xE9`) has nothing
in it, because nothing on this architecture puts anything there. And the
`'A'` itself is gone: the exit carries the address and the direction and
**not the value** -- `struct cosmo_vm_exit.mmio` is `{ gpa, write }` --
so even an owner that wanted to be the UART could not be.

A stock kernel's first act after the GIC is `earlycon`. On this
hypervisor that is where it stops.

## Current implementation

**Devices exist as a seam and one x86 instance.** `struct vm_device`
(`kernel/include/kernel/hv.h`) has a port range with a real handler --
`pio()` returns 0 for handled or `-ENODEV` to hand the exit to the owner,
and `vmdev_pio` is what the x86 debug console at port `0xE9` hangs off,
feeding `vm_console_put` -- and a memory range whose handler is
**notification only**: `mmio(d, gpa, write)` is called and *the exit
still reaches the owner* (`vmdev.c:121`, the comment says "Stage 1"). No
device registers an MMIO range. On AArch64, which has no port space, that
means no device at all.

**The MMIO exit does not describe the access.** `decode_exit` in
`hv_el2.c` reports the faulting IPA from `HPFAR_EL2` and the direction
from `ESR_EL2.WnR`, and nothing else. The size (`SAS`), the register
(`SRT`) and, for a write, its value are all in `ESR_EL2.ISS` and the
guest's registers, and the distributor unit already decodes exactly those
-- but inside the backend, for itself (`el2_vdist_access`). The generic
layer and the owner never see them. A read cannot be completed at all:
there is no MMIO analogue of the `in_completion` path that lets an owner
answer a port `IN` on the next `vcpu_run`.

**The guest console exists and is empty.** `vm->console` is a ring with
a lock, `vm_console_put`/`vm_console_read`/`vm_console_pending`, read
through the VM's file descriptor (`vm.c:18`) and drained by `vmctl` after
every run. It works; on AArch64 it has no producer.

**The distributor can deliver an SPI that nothing raises.** The virtual
distributor accepts a pending SPI and routes it (`el2-guest-gic-timer`
proves it, with an SPI the guest itself makes pending through `ISPENDR`),
and named "device SPIs" out of scope because no device existed to be the
source. `virt`'s UART is SPI 33, and a guest kernel's serial driver
expects RX interrupts on it.

**The host has a PL011 driver** (`kernel/arch/aarch64/pl011.c`): `DR`,
`FR`, `IBRD`, `FBRD`, `LCR_H`, `CR`, `IMSC`, `MIS`, `ICR`, at
`VIRT_PL011_BASE` with `VIRT_PL011_INTID` 33. The register set a guest
needs modelled is the one the host already drives.

## Why it matters

- **It is the first thing a real guest does that fails.** GIC
  initialisation now succeeds; the next line of any kernel's boot is a
  `printk`, and its `earlycon` is a `str` to `0x0900_0000`. Until that
  works, "a guest can run a stock GIC driver" is a claim only a
  hand-written fixture can exercise, because nothing else can report
  what happened.
- **It is the first device, and the seam for every device after it.**
  The `vm_device` MMIO hook is a stub. Making it real -- an MMIO access
  the kernel completes, with size, value and a register to write a read's
  result into -- is what a virtio-mmio transport, a disk, a network
  device will each need, and it is better designed once for the simplest
  device than retrofitted for a complicated one.
- **It closes the distributor unit's named gap.** A UART with an RX FIFO
  is a device that raises an SPI when a byte arrives: the first source
  the distributor routes that the guest did not fake through `ISPENDR`.
  That is the end-to-end path -- owner writes a byte, device raises SPI
  33, distributor forwards it, guest's handler reads `DR` -- and no test
  has it yet.
- **It gives every future test a channel.** Today a guest reports through
  `hvc` registers, one word at a time, and every fixture has a bespoke
  protocol. A guest that can `printk` into a ring the test reads makes
  the next units' fixtures shorter and their failures legible.

## Proposed design

### 1. The MMIO exit describes the access, and a device may complete it

`struct hv_exit.mmio` grows `size` (1, 2, 4, 8), `reg` (the guest GPR,
31 for `XZR`), `value` (for a write, the register's contents masked to
the size), and the two bits that say how a read's result lands: `sse`
(`ISS.SSE`: the load sign-extends -- `ldrsb`, `ldrsh`, `ldrsw`) and `sf`
(`ISS.SF`: the destination is `Xt`, 64 bits wide; clear, it is `Wt` and
the upper 32 bits become zero). `decode_exit` fills them from
`ESR_EL2.ISS` when `ISV` is set -- the same fields `el2_vdist_access`
reads, which ignores `SSE` because no GIC register is signed; a general
seam may not -- and the backend's distributor decode keeps its first
refusal on the access, in the order it already has: **distributor first,
then devices, then the owner**.

**Completing a read is defined, not assumed.** The device produces up to
`size` bytes; the completion zero-extends them to `size`, then, if `sse`,
sign-extends from bit `8*size-1` to the destination width -- 64 bits if
`sf`, 32 if not -- and if not `sf` clears bits 63:32, because a write to
`Wt` does. `reg` 31 discards the result. That is one function,
`hv_mmio_complete_read(ctx, size, sse, sf, reg, value)`, used by the
in-kernel path and the owner-answered path alike, so the two cannot
disagree; `arch_hv_vcpu_write_gpr` on its own is not the contract. An
`ldrsh` of `0x8001` from a device must read `0xFFFFFFFFFFFF8001` in an
`Xt` and `0xFFFF8001` in a `Wt`, and an `ldrb` must read `0x01` with no
high bits from before; `el2-mmio-device` checks each width and both
extensions. On x86
the `mmio` exit from NPT/EPT gains the same fields where the backend can
supply them (it decodes the instruction today only for I/O; MMIO stays
owner-handled there and `size` reads 0: "unknown"), so the generic layer
is one shape.

`vm_device.mmio` becomes `int (*mmio)(struct vm_device *d, uint64_t gpa,
bool write, unsigned size, uint64_t *value)` -- the port handler's
contract, in memory: 0 handled, `-ENODEV` to the owner. `vmdev_mmio`
returns that. In `vcpu_run`, a handled read has its value written into
the guest's register (`arch_hv_vcpu_write_gpr`) and the instruction
stepped over (`arch_hv_vcpu_advance_rip` by the instruction length the
exit now also carries); a handled write is stepped over; either way the
loop continues without an exit. An unhandled access reaches the owner as
it does today, **now carrying size and value**, so an owner-side model
becomes possible too.

The uapi `cosmo_vm_exit.mmio` gains `size`, `reg`, `sse` and `sf` in
the four bytes of padding it has, and a 64-bit `value` after them, which
grows the member from 16 to 24 bytes. That is within the union, whose
size is fixed at 48 bytes by `raw[6]` (`struct cosmo_vm_exit` stays 64
bytes), so existing userland is unaffected; but it is a new field, not a
use of padding, and the implementation must add it, not squeeze `value`
into 32 bits. A read the owner answers completes the way an `IN` does:
`x->mmio.value` on the next `vcpu_run`, through the same
`hv_mmio_complete_read` as an in-kernel device's answer.

### 2. A PL011 model, in the kernel, per VM

`kernel-services/virtualization/vuart.c`: one `struct vm_device` per VM
at `0x0900_0000`, 4 KiB, registered by `vmdev_init` on AArch64 the way
the debug console is on x86 -- the guest's console is part of the machine
this hypervisor defines, at the address `virt` puts it and a stock
kernel's device tree names. Its state is the register file the host
driver drives plus a receive FIFO:

| register | model |
|---|---|
| `DR` (0x000) | write: the byte goes to `vm_console_put`. read: pops the RX FIFO, or 0 with `FR.RXFE` set |
| `FR` (0x018) | `TXFE`/`TXFF` say the transmitter is always empty and never full (the ring absorbs everything); `RXFE`/`RXFF` follow the FIFO |
| `IBRD`, `FBRD`, `LCR_H`, `CR` | stored and returned; nothing here has a baud rate |
| `IMSC` (0x038) | the interrupt mask, stored |
| `RIS` (0x03C), `MIS` (0x040) | `RXRIS` while the FIFO is non-empty; `MIS = RIS & IMSC` |
| `ICR` (0x044) | write-1-to-clear the raw bits that are clearable (RX is level: cleared by draining, not by `ICR`) |
| `PeriphID0..3`, `PCellID0..3` (0xFE0..0xFFC) | the PL011's constants, so a driver that checks finds a PL011 |

Everything else in the page RAZ/WI, for the reason the distributor gives:
a driver touches registers this model has no state for.

### 3. Input: the owner writes, the device interrupts

The VM's file descriptor already reads the console; it gains **write**:
bytes written land in the UART's RX FIFO (a small fixed ring, 64 bytes,
dropping the oldest as the console ring does). When the FIFO goes from
empty to non-empty and `IMSC.RXIM` is set, the device raises SPI 33 in
the VM's distributor -- a new `vm_raise_spi(vm, intid)` in the generic
layer, forwarded to a backend op `vm_raise_spi` that on AArch64 calls
`vdist_raise_spi` (the SPI twin of `vdist_raise_private`, marking the
shared pending bit; the distributor's routing does the rest, to whichever
vCPU `IROUTER` names). The SPI is level-triggered as the PL011's is: the
device keeps `RIS.RXRIS` up while bytes remain, and a guest that reads
`DR` until `FR.RXFE` sees it drop. In the distributor's terms that is
"pending while asserted": the device re-raises after an acknowledgement
if the FIFO is still non-empty, at the next entry, through the same
routing.

A WFI-waiting vCPU is woken by this the way it is by an SGI: the
distributor is the second source `arch_hv_vcpu_irq_waiting` already
asks, so an owner's `write()` on the VM descriptor wakes a guest blocked
in `WFI` waiting for input. That is a guest shell's read loop, end to
end.

### 4. Where it lives and why

In the kernel, like the debug console and unlike a virtio disk would be.
The argument is the cost of an exit per character: a console is written
a byte at a time by an `earlycon`, and a round trip to userland per byte
is the one thing the distributor unit's design ruled out for the same
reason. The device is small (a ring, a dozen words, one interrupt line),
and it is the one every guest has. Complex devices with data planes --
virtio -- are for a later unit and may well be owner-side; this unit
makes the seam that lets either kind exist.

### 5. Deliberately out of scope

- **A device tree.** A stock kernel learns the UART's address from a DT
  or ACPI the hypervisor hands it; producing that blob is the owner's
  (`vmctl`'s) and is its own unit. This unit puts the UART where the DT
  will say it is, which is where `virt` puts it.
- **virtio, of any kind.** The MMIO seam here is what virtio-mmio will
  use; the transport and any device behind it are separate.
- **Modem lines, DMA, the TX FIFO as a FIFO, baud rates.** A PL011 has
  them; a console does not need them; they are RAZ/WI or stored.
- **x86 MMIO emulation.** The x86 backends do not decode MMIO
  instructions; their exit gains the fields with `size = 0`, and the
  generic device seam works for them the day a backend fills them in.

## Affected files

| file | change |
|---|---|
| `kernel/include/arch/hv.h` | `hv_exit.mmio` gains `size`, `reg`, `value`, `insn_len`; backend op `vm_raise_spi` |
| `kernel/arch/aarch64/hv_el2.c` | `decode_exit` fills them from `ISS` (the distributor keeps first refusal); `el2_vm_raise_spi` → `vdist_raise_spi` |
| `kernel/arch/aarch64/gicv3_vdist.c`, `.h` | `vdist_raise_spi`; level semantics -- a source that is still asserted after an ack is pending again |
| `kernel/arch/x86_64/{svm,vmx}.c` | `size = 0` in the MMIO exit; `vm_raise_spi` returns `-ENOTSUP` |
| `kernel/include/kernel/hv.h` | `vm_device.mmio` returns int and takes size/value; `struct vm` gains the UART; uapi `cosmo_vm_exit.mmio` gains size/reg/value |
| `kernel-services/virtualization/vmdev.c` | `vmdev_mmio` returns the handler's verdict; registers the UART on AArch64 |
| `kernel-services/virtualization/vuart.c` | **new**: the PL011 model, the RX FIFO, the interrupt |
| `kernel-services/virtualization/vcpu.c` | a handled MMIO completes the access and runs on; an owner-answered read is written back on the next run |
| `kernel-services/virtualization/vm.c` | `write()` on the VM descriptor feeds the UART's RX |
| `userland/system/vmctl.c` | prints size and value on an unhandled MMIO; forwards its own stdin to the guest |
| `tests/hv/aarch64/guest_uart.S`, `guest_uart_rx.S` | **new** fixtures |
| `kernel-services/virtualization/hvtest.c` | the tests below |
| docs | `virtualization/design.md` (devices, the MMIO contract, the console), `testing.md`, `aarch64/design.md` (SPI sources), invariant |

## New APIs

```c
/* kernel/include/kernel/hv.h */
struct vm_device {
    ...
    /* 0: handled (a read's result in *value); -ENODEV: hand the exit to the owner. */
    int (*mmio)(struct vm_device *d, uint64_t gpa, bool write, unsigned size, uint64_t *value);
};
int vm_raise_spi(struct vm *vm, unsigned intid);          /* a device asserts a shared interrupt */
int vm_console_write(struct vm *vm, const void *buf, size_t len);   /* the owner's input to the guest */

/* kernel/include/arch/hv.h */
struct hv_exit { ... struct { uint64_t gpa, value; uint8_t size, reg, insn_len; bool write, sse, sf; } mmio; ... };
/* One place a read's result becomes a register: zero-extend to size,
 * sign-extend to the destination width if sse, clear 63:32 if !sf. */
void hv_mmio_complete_read(struct arch_hv_vcpu *v, unsigned size, bool sse, bool sf, unsigned reg, uint64_t value);
int arch_hv_vm_raise_spi(struct arch_hv_vm *vm, unsigned intid);

/* kernel/arch/aarch64/include/aarch64/gicv3_vdist.h */
void vdist_raise_spi(struct gicv3_vdist *d, unsigned intid);
void vdist_lower_spi(struct gicv3_vdist *d, unsigned intid);   /* the level dropped */

/* uapi */
struct cosmo_vm_exit { ... struct { uint64_t gpa; uint32_t write; uint8_t size, reg, sse, sf; uint64_t value; } mmio; ... };   /* 24 of the union's 48 bytes */
```

`vcpu_inject` and the owner's pending set are untouched; a device's
interrupt goes through the guest's distributor, as the timer's does.

## Migration plan

1. **The exit describes the access.** `hv_exit.mmio` and the uapi struct
   gain size, register and value, filled on AArch64 from `ISS`; `vmctl`
   prints them. No behaviour change beyond the extra fields. Verifiable
   by `el2-guest-mmio`, which now also asserts the size and value of the
   store it already checks.
2. **A device may complete an access.** `vm_device.mmio` returns a
   verdict; `vcpu_run` completes a handled read into the guest's register
   and steps over; an owner-answered read completes on the next run. A
   test device registered by the test itself (a word of memory at an
   address) proves the seam before any real device uses it.
3. **The PL011, output.** `vuart.c` with `DR` writes to the console ring
   and the identification, flag and control registers; registered per
   VM on AArch64. A guest that stores a string to `DR` appears on the
   VM's descriptor -- the measurement that opened this report, as a test.
4. **The PL011, input.** The RX FIFO, `write()` on the descriptor,
   `FR.RXFE`, `RIS`/`MIS`/`IMSC`/`ICR`, and SPI 33 through the
   distributor via `vm_raise_spi`. A guest with its GIC up and `RXIM` set
   takes SPI 33 when the owner writes a byte and reads it back from `DR`.
5. **Level, and WFI.** A second byte written while the first is unread
   keeps the line up; draining `DR` to `RXFE` drops it; a guest waiting
   in `WFI` for input is woken by the owner's write. The device is
   level-triggered and the test says so.
6. Docs.

Steps 1 and 2 are the seam and could be one commit; 3, 4 and 5 are
separate claims.

## Tests

- **`el2-guest-mmio`** (existing, extended) -- the store to
  `0x4000_0000` now reports size 4 and the value the guest wrote; the
  load reports its register. Fails on the tree before step 1, where
  `size` is 0 and `value` is 0.
- **`el2-mmio-device`** -- a test-registered device at an unused address
  answers reads with a known pattern (`0x8001_8081_F0F1_F2F3`) and records
  a write's size and value; the guest reads it back through every width
  and extension a driver can use -- `ldrb`, `ldrh`, `ldr w`, `ldr x`,
  `ldrsb w`, `ldrsh x`, `ldrsw x` -- and reports each register, which
  must be `0xF3`, `0xF2F3`, `0xF0F1F2F3`, the whole word,
  `0xFFFFFFF3`, `0xFFFFFFFFFFFFF2F3` and `0xFFFFFFFFF0F1F2F3`
  respectively, with no exit to the owner; a store of a halfword reports
  size 2 and the low sixteen bits; an access outside the device still
  reaches the owner, now with size and value. This is the seam, tested
  before the UART depends on it, and the widths and extensions are the
  part a completion that merely wrote the value would fail.
- **`el2-guest-uart`** -- the guest stores `"hello\n"` to `DR` a byte at a
  time and calls out; the VM's console reads back exactly `"hello\n"`;
  `FR` read `TXFE` throughout; the PL011 identification registers read
  as a PL011. The opening measurement as a test: `console pending`
  becomes 6.
- **`el2-guest-uart-rx`** -- the guest brings up its GIC, enables SPI 33
  in the distributor routed to itself, sets `IMSC.RXIM`, and heartbeats;
  the test writes `'x'` to the VM; the guest's handler takes INTID 33,
  reads `DR` and reports `'x'`, and `MIS` read `RXMIS` in the handler
  and reads clear after the drain. Fails on a tree where the device
  does not raise, or raises the wrong line.
- **`el2-guest-uart-level`** -- two bytes written before the guest
  handles; the handler drains one and returns; the line is still up and
  the handler runs again for the second; after draining to `RXFE` no
  third interrupt comes. Then the guest waits in `WFI`; the test writes a
  byte; the `WFI` run returns and the byte is taken. Level semantics and
  the wake-up, which an edge-triggered model would fail (one interrupt
  for two bytes) and an unwired `irq_waiting` would hang.

Each with the bug-proof this project expects. Per the last two units'
lesson: the output test needs the ring non-empty *before* asserting
nothing leaked elsewhere, and the level test needs two bytes, not one,
or an edge-triggered model passes it.

## Benchmarks

Counted, not timed:

- **Characters a guest can print: none today; all of them after.** The
  headline.
- **Exits per printed character.** With the device in the kernel, an
  `earlycon` line of 80 characters is 80 trapped stores and 0 exits to
  the owner; the count of `HV_EXIT_EMULATED`-style completions versus
  `COSMO_VM_EXIT_MMIO` to the owner says the model is answering. A
  userland UART would be 80 round trips per line; this is the number
  that justifies the placement.
- **Bytes in, interrupts out.** For a burst of N bytes written before
  the guest runs, one RX interrupt (level) not N; for N bytes each
  drained before the next, N interrupts. The counts confirm the model is
  level-triggered and not lossy.

## Risks

- **The seam change touches the x86 backends and the uapi.** Growing
  `cosmo_vm_exit.mmio` uses padding and stays inside the union's size,
  so existing userland is unaffected; but `vm_device.mmio`'s signature
  changes and every caller (one, `vmdev_mmio`) with it. Small, and step
  2's test device proves the seam on its own before the UART leans on it.
- **A read the owner answers needs a completion path that does not exist
  yet.** `in_completion` is port-shaped (`rax`, a size). Generalising it
  to "write this value into register `reg`" is the design; getting it
  wrong corrupts a guest register silently. The `el2-mmio-device` test
  covers the in-kernel path; an owner-side read test needs `vmctl` and
  is the `HVTEST` harness's job.
- **Level-triggered interrupts are new to the distributor.** Everything
  it forwards today is edge (an SGI, a timer expiry latched once). A
  level source must be re-raised after acknowledgement while still
  asserted, and lowered when not, or the guest gets one interrupt for
  two bytes -- the failure `el2-guest-uart-level` is written for. The
  distributor's "pending clears on ack" stays; the *device* re-asserts,
  which keeps level a property of the source, not a mode of the router.
- **The console ring drops the oldest byte when full.** A guest that
  prints faster than its owner reads loses output silently; that is the
  existing x86 behaviour and this unit inherits it rather than adding
  flow control. Recorded so it is not a surprise; `dropped` is counted.
- **Writes from the owner race the guest's reads.** The FIFO gets a lock
  (the console ring already has one); the interrupt raise is a
  distributor operation under the distributor's lock; the two are never
  held together. Decided up front, per this project's own lesson on
  shared state.

## Alternatives considered

- **Emulate the UART in `vmctl`.** Where a device model "belongs", and
  where the distributor's argument applies in reverse: a console is one
  exit per byte, and a round trip to userland per byte is the cost the
  in-kernel distributor was built to avoid. The debug console on x86 is
  in-kernel for the same reason. The seam this unit builds lets an owner
  model exist later; the first device should not pay for it.
- **A hypercall console** -- a `hvc` the guest calls with a byte. Every
  fixture already does this in effect, and it is exactly what a stock
  kernel cannot be asked to do. The point is a device at the address the
  device tree names.
- **virtio-console first.** The device a "real" VMM would give; but it
  needs the virtio-mmio transport, virtqueues in guest memory, and a
  guest driver that has already initialised -- none of which help a
  kernel print its first line. `earlycon` is a PL011 on every `virt`
  guest kernel there is, and it is also the one the host already has a
  driver for.
- **Skip input; output only.** Half the device, and the half that does
  not exercise the distributor as a device's interrupt sink -- which is
  the gap the last report named. A console that cannot be typed at is
  not a console.
