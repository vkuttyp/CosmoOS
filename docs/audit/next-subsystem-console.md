# NEXT SUBSYSTEM

Constitution §68: after the audit, name the next subsystem in this shape
and wait for the instruction to build it. This is the fourth such report
(`next-subsystem.md` named the Intel NIC, `next-subsystem-usb.md` the
USB host stack, `next-subsystem-ahci.md` AHCI; all three record their
outcomes). Nothing in this one is implemented.

**Subsystem: the machine's own console — the framebuffer the firmware
has already lit, and a keyboard on the bus §60 made the tree drive, so
that a CosmoOS machine can be looked at and typed on without a serial
cable.**

## Problem

CosmoOS speaks to exactly one kind of console device: a UART. On x86-64
a 16550 at `0x3f8` (`kernel/arch/x86_64/serial.c`), on AArch64 a PL011
(`kernel/arch/aarch64/pl011.c`). Those two files register the only
console sinks a real machine has, and they are the only two callers of
`tty_input` in the tree — the whole input path, for every shell session
the harness runs, is a byte arriving in a UART interrupt.

That is a complete console for the harness, which pipes the serial line
to a file, and it is no console at all for §61's hardware matrix: AMD
real hardware, Intel real hardware, Apple Silicon where feasible. A
desktop or laptop built in the last decade has no serial port. Booted on
one today, CosmoOS would light no pixel and read no key: the kernel's
only output sink cannot be reached, and its only input path does not
exist. The panic report — the thing one most wants from a first boot on
a new machine — would be written to a port that is not there.

Two pieces are missing, and they are not one piece. Output wants the
linear framebuffer the UEFI firmware has already configured and told
nobody about: `console.h` has said "serial now, framebuffer later" since
it was written, and `cosmoboot.h:144` has reserved the fields for it
since version 3. Input wants a USB keyboard, which is the first class
driver §60's USB item makes reachable and the first user the xHCI
driver's interrupt-endpoint code has ever had.

## Current implementation

- **Console** (`kernel/core/console.c`, 81 lines): a fan-out of
  registered sinks with an IRQ-safe lock and an irreversible panic mode
  that stops taking it. Three sinks exist: the x86 serial port, the
  PL011, and `virtio_console` (a module, and a file in the harness, not
  a device on a machine). Sinks may not sleep, allocate, or take a
  sleeping lock; `console_write` runs from interrupt and panic context.
- **TTY** (`kernel/tty/tty.c`, 168 lines): one console tty, canonical
  line editing with echo, a ring of completed lines, readers waiting on
  a waitqueue. `tty_input` takes bytes in interrupt context under the
  tty lock and echoes through `console_write`; lock order is
  `tty.lock → console.lock`. The shell reads this tty, so anything that
  calls `tty_input` is a keyboard as far as userland is concerned.
- **The loader** (`boot/uefi/`): `LocateProtocol` and `HandleProtocol`
  are already used (loaded image, simple filesystem, ACPI tables from
  the configuration table), the memory map is walked and handed on, and
  there is no mention of the Graphics Output Protocol anywhere. Output
  goes to firmware `ConOut` until `ExitBootServices` and to COM1
  afterwards. The protocol is at `COSMOBOOT_VERSION 5`; the kernel
  carries an ELF note with the version it was built for and the loader
  checks it, so the two move together.
- **USB** (`drivers/usb/`, `docs/drivers/usb/`): a core that matches
  class drivers on interface class/subclass/protocol (`usb_bus.match`),
  an xHCI controller driver, and one class driver (`usb_storage`). The
  core's topology is a root-hub port: `hcd->port_dev[]` indexed by port,
  names `usb<hcd>-<port>`, every device a child of the controller.
  `USB_CLASS_HUB` is defined in the header and used nowhere.
  `usb_submit` takes any endpoint, and `xhci.c:813-833` already writes
  interrupt-endpoint contexts and computes the Interval field from
  `bInterval` for all four speeds — **code that has never run**, because
  a mass-storage device uses control and bulk endpoints only.
- **MMIO mapping**: `device_map_mmio(dev, res)` maps a device's resource
  uncached and sleeps (`kernel/include/kernel/device.h:120`). There is
  no write-combining anywhere in the tree, and no way to map a physical
  range that belongs to no `struct device`.
- **The log ring** (`kernel/include/kernel/log.h`): every line the
  kernel prints is kept, and `dmesg` reads it. A sink that registers
  late can therefore show what it missed.
- **The harness** (`scripts/qemu-run.sh`, `tests/boot/run_boot_test.py`):
  `-serial stdio`, `-display none`, `-monitor none`. The boot test
  decides PASS from the debug-exit status and required markers, and its
  shell test drives the guest by writing bytes into QEMU's stdin — the
  serial line. There is no monitor and no QMP socket, which is why the
  AHCI unit's hotplug tests had to be driven from inside the guest.

## Why it matters

1. **§61 is blocked on it.** "Never assume QEMU behavior exactly matches
   physical hardware" is advice one can only take on a machine that can
   report what it did. The first real-hardware boot of this kernel will
   either print its banner on the screen or tell us nothing at all.
2. **A panic must be visible.** `panic()` prints through
   `console_write`; a framebuffer sink is the difference between a
   report and a black screen. It also means the sink is held to the
   serial sink's rules — no allocation, no sleeping, bounded work — and
   the fan-out gets its first test with two sinks of very different
   speed.
3. **The interrupt-endpoint path has never run.** A keyboard is a device
   that answers on an interval forever; it is the natural first user of
   the periodic path, of a request resubmitted from its own completion,
   and of an endpoint that must be stopped cleanly when the device is
   pulled. Whatever is wrong in that code has never been in a position
   to show itself.
4. **The architectural question: topology.** The USB core equates a
   device with a root-hub port — an index, a name, a parent that is
   always the controller. A hub makes a device whose parent is a device
   on the same bus, at a route the controller has to be told (the slot
   context's Route String, and for a full- or low-speed device behind a
   high-speed hub, the transaction translator's hub and port). The
   question this unit answers is where that topology lives: in the USB
   core as a parent pointer, a depth and a route on `struct usb_device`;
   or in the HCD, private, with the core still seeing a flat list. The
   AHCI unit answered the DMA-parent question with "no, the model gains
   nothing" — a port had no identity of its own. A hub's children do
   have identities, so the answer may go the other way, and that is
   worth knowing before a second bus with real hierarchy (PCIe bridges,
   a second USB controller behind a dock) arrives.
5. **It is the last thing between the tree and a machine a person can
   use.** Everything a user-facing system needs — a shell, a
   filesystem, packages, a service manager, containers — exists and is
   reachable only through a cable that modern hardware does not have.

## Proposed design

Four pieces, in the order they should be built, each observable before
the next begins.

### 1. The framebuffer, through the boot protocol (`COSMOBOOT_VERSION 6`)

The loader locates `EFI_GRAPHICS_OUTPUT_PROTOCOL` and **takes the mode
the firmware has already set**. It does not call `SetMode`: the
firmware's mode is known to work, and choosing modes is the beginning of
the GPU driver §60 defers. From `Mode->Info` it records the frame buffer
base and size, the resolution, the pixels per scan line, and the pixel
format reduced to three (shift, width) pairs for red, green and blue —
so the kernel never needs the EFI enumeration, and a bit-mask format is
described by the same three pairs as the two common ones.

`struct cosmoboot_info` gains those fields in the space
`cosmoboot.h:144` reserved for them, and `COSMOBOOT_VERSION` becomes 6.
All zero means "no framebuffer", which is what a firmware without a GOP,
or a machine whose display device the firmware did not drive, produces;
the kernel then behaves exactly as it does today.

The range is not RAM. It appears in the EFI memory map as
`EfiMemoryMappedIO`, the loader must not fold it into the free lists it
hands over, and the kernel's memory code must keep treating it as
outside RAM — the check is that a boot with a framebuffer has the same
free-page count as one without.

### 2. A framebuffer console sink (`kernel/console/fbcon.c`)

A text console on a linear framebuffer: an 8×16 cell grid, one glyph
per cell, no cursor, scrolling by a line when the last one fills. Its
`write` walks bytes, handling `\n`, `\r`, `\b` (which the tty's echo
uses as `"\b \b"`) and tab, drawing everything else through a font
table; anything unprintable is a box.

The font is drawn in-tree. A small generator script turns a
hand-written glyph description into a C table of the 96 printable ASCII
characters plus a replacement box — about 1.6 KiB. Copying a font table
from elsewhere is exactly the borrowed artifact "a new operating system
from scratch" excludes, and 96 glyphs of 8×16 is an afternoon.

Two constraints shape the rest. It runs on the panic path, so it
allocates nothing, sleeps never, and holds only an IRQ-safe spinlock
that panic mode drops like the console's own. And it cannot exist until
something can map the framebuffer, which is after the VMM is up and
therefore well after the first hundred lines of the boot — so when it
registers it **replays the log ring**, and the screen shows the boot
from its first line rather than from the middle.

Whether it keeps a shadow copy of the text plane in RAM and blits, or
writes glyphs straight into uncached MMIO, is left to the benchmark
(below): a scroll of a 1024×768×32 framebuffer moves 3 MiB, and what
that costs uncached is a measurement, not a guess (§21).

### 3. A HID boot-protocol keyboard (`drivers/usb/usb_hid.c`)

A module `usb_hid` binding interface class `03`, subclass `01`,
protocol `01`. It issues `SET_PROTOCOL(0)` — the boot protocol — so the
device sends the fixed 8-byte report of modifiers plus six keycodes, and
**there is no report-descriptor parser**. A device that speaks only the
report protocol gets one log line and no driver; the parser waits for a
device that needs it (§21), and the docs say so, so the next unit does
not have to re-argue it.

One interrupt IN endpoint, one `struct usb_request` resubmitted from its
own completion — the first periodic transfer in the tree. Each report is
diffed against the previous one to turn the rollover array into press
events; modifiers give shift and control; a US layout table gives
characters, and control-letter gives the control bytes the tty already
understands (`^D`, `^U`, backspace). Keys with no mapping are dropped,
as the tty drops what it does not know. Delivery is
`tty_input(tty_console(), …)` from interrupt context, which is precisely
what the two UARTs do — so the shell reads a key and a serial byte
through the same door, and nothing in userland changes.

Disconnect is the USB unit's protocol: the endpoint is stopped, the
request completes `-ENODEV`, the driver's `remove` runs, and the tty is
untouched.

### 4. A hub (`drivers/usb/usb_hub.c`)

A module binding class `09`. It reads the hub descriptor, powers the
ports, and watches the status-change endpoint — a second interrupt
endpoint, and one whose reports are a bitmap of ports rather than a
stream. On a connect it resets the port, reads the speed from the port
status, and hands the core a device whose parent is the hub; on a
disconnect it removes that device, children first.

The core gains what it needs to say where a device is: a parent pointer,
a depth, a route string, and a name that is a path (`usb0-1.2`) rather
than a root-port number. The xHCI driver gains the two slot-context
fields it has never had a reason to set: Route String, and — for a full-
or low-speed device behind a high-speed hub — the TT's slot and port.
QEMU's `usb-hub` is a full-speed hub, so a keyboard behind it exercises
exactly the fields a root-port device leaves zero.

**This step is the droppable one.** It goes last because if it turns out
to fight the core — if a hub's children want to be something the device
model cannot express cheaply — the finding is the answer to question 4
above, written down, and the keyboard keeps working on a root port. That
is a result, not a failure.

## Affected files

| File | Change |
| --- | --- |
| `boot/protocol/cosmoboot.h` | framebuffer fields in the reserved space; `COSMOBOOT_VERSION` 6 |
| `boot/uefi/efi.h` | the Graphics Output Protocol GUID and its structures |
| `boot/uefi/main.c`, `boot/uefi/loader.h` | locate the GOP, record the mode, fill the new fields |
| `kernel/include/kernel/bootinfo.h`, the boot-info reader | carry the framebuffer description |
| `kernel/console/fbcon.c` (new), `kernel/console/font8x16.c` (generated), `tools/mkfont.py` (new) | the sink and its font |
| `kernel/include/kernel/console.h` | one line: `fbcon_init()`, called once the VMM is up |
| `kernel/memory/*` | keep the framebuffer range out of the free lists; a mapping for a physical range with no device |
| `drivers/usb/usb_hid.c` (new), `drivers/usb/usb_hub.c` (new) | the two class drivers |
| `drivers/include/drivers/usb.h`, `drivers/usb/usb.c` | parent, depth, route; a connect entry for a hub port |
| `drivers/usb/xhci.c`, `xhci.h` | Route String and TT fields in the slot context |
| `build/module.mk` | the `usb_hid` and `usb_hub` modules |
| `kernel/device/devtest.c` (or a new `kernel/console/fbtest.c`) | the self-tests |
| `scripts/qemu-run.sh` | `QEMU_DISPLAY`, `QEMU_KBD`, a QMP socket |
| `tests/boot/run_boot_test.py` | markers, a QMP client, a key-injection mode |
| `docs/drivers/usb/*`, `docs/kernel/tty/*`, `docs/boot/*`, `docs/drivers/console/` (new) | design, api, invariants, testing |
| `README.md`, `docs/README.md` | the map |

## New APIs

- **Boot protocol v6**: `fb_phys`, `fb_size`, `fb_width`, `fb_height`,
  `fb_pitch`, `fb_bpp`, and three (shift, width) pairs. Zero means none.
  The version bump is the whole compatibility story: the ELF note pairs
  a kernel with a loader, so a v5 kernel refuses a v6 loader and the
  pair moves in one commit.
- **A mapping for a range with no device.** This is the one existing
  interface that is likely to change shape: `device_map_mmio` takes a
  `struct device` and a resource, and a firmware framebuffer has
  neither. Either it learns to take `NULL`, or a plain
  `vmm_map_phys(paddr, size, cacheability)` appears beneath it and
  `device_map_mmio` becomes its caller. The second is the honest one if
  write-combining is worth having (the benchmark decides), because a
  cacheability argument is exactly what the framebuffer needs and no
  driver has ever wanted.
- **`fbcon_init(void)`**: called once from the boot sequence after the
  VMM; registers a sink and replays the log. No other console interface
  changes.
- **USB core**: `struct usb_device` gains `parent`, `depth` and `route`;
  `usb_hub_port_connected(struct usb_device *hub, unsigned port, enum
  usb_speed)` and `usb_hub_port_disconnected(…)` beside the existing
  root-hub pair; one HCD op to update a slot context when the device is
  behind a hub. All of it is step 4's, and step 4 may report that the
  core should not have it.
- **Nothing userland sees changes.** No syscall, no device file, no
  input event nodes. The shell reads the tty it already reads.
- Possibly one fault-injection kind: a keyboard whose interrupt endpoint
  stops answering, so the driver's own recovery is a test and not a
  review.

## Migration plan

1. **The framebuffer through the protocol.** Loader and kernel in one
   commit (the version note pairs them). The kernel prints the mode it
   was handed and does nothing with it. Check: both architectures print
   a plausible framebuffer, or "none", and the free-page count is
   unchanged.
2. **The sink.** Map it, draw, register, replay. Check: the boot log is
   on the screen, the readback self-test passes, and a boot with the
   sink registered is not measurably slower than one without (or it is,
   which is what the benchmark is for).
3. **The keyboard.** `usb_hid` binds QEMU's `usb-kbd` on a root port and
   feeds the tty; the harness gains QMP and types a line. Check: the
   line arrives, echoed, and the shell test runs from the keyboard.
4. **The hub.** The core learns topology; the keyboard moves behind
   `-device usb-hub` and everything above still passes. Check: the same
   key test, one level down, and an unplug of the hub that takes its
   child with it.

Nothing is removed at any step. The serial sink stays registered, stays
first, and stays the harness's channel; the framebuffer is a second
sink, and a machine without one boots exactly as it does today.

## Tests

**Boot markers.** The framebuffer line (`fbcon: 1280x800, 32 bpp, …`)
in the default shape; the keyboard's enumeration and binding; the hub's,
in the shape that has one.

**`fb-console`** (self-test, both architectures). Render a known string
into a known cell, then **read the pixels back through the mapping** and
compare them against the font table's bits — this is what makes a
display testable with `-display none` and no screenshot. Then: a `\b`
erases one cell; a write that fills the last line scrolls exactly once
and the top line is gone; a string longer than the row wraps; a
non-printable byte draws the box. Foreground and background pixels are
both checked, so a wrong pixel format fails rather than drawing
something illegible that a marker would still match.

**`fb-geometry`** (host test and fuzz target). The kernel must refuse a
framebuffer description that does not describe memory it may write:
pitch smaller than width × bytes-per-pixel, size smaller than height ×
pitch, a zero dimension, a bit field that runs off the pixel, absurd
resolutions. The validator is a pure function over the boot fields, so
it is a host test and a cheap fuzz target, and the failure mode it
prevents — writing outside the mapping on someone else's firmware — is
the one that matters on real hardware.

**`hid-keyboard`** (self-test plus harness). The harness opens a QMP
socket (`-qmp unix:…`, alongside the serial stdio it already uses),
waits for the driver's binding marker, and sends key events
(`input-send-event`). In the guest the self-test waits for the line,
checks the bytes reached the tty with the right characters, that shift
gives capitals, that `^U` clears the line and `^D` ends it, and that the
echo went to both sinks. Then the existing shell test is run again with
its script typed on the keyboard instead of written to the serial line —
the same expectations, a different door.

**`hid-unplug`**. The keyboard is removed with a report in flight: the
interrupt request completes `-ENODEV` and not never, `remove` runs, the
tty keeps working from the serial line, and plugging it back in binds a
new device (the USB unit's shape, and the AHCI unit's: the old object is
never re-registered).

**`usb-hub`** (the `QEMU_KBD=hub` shape). The hub enumerates, its child
is named for its route, the whole `hid-keyboard` test passes one level
down, and unplugging the hub removes the child before the hub.

**Shapes.** `QEMU_DISPLAY={bochs (default), virtio, 0}` and
`QEMU_KBD={root (default), hub, 0}`, each on both architectures, plus
the usual chain (`QEMU_IOMMU=0`, `QEMU_SMP=1`, release, `test-crash`,
`analyze`, `reproducible`). The verification chain grows by about six
steps, from 27 to 33.

**Reintroduction.** Each bug found gets its test, and each test is
proved by putting the bug back — as every unit since the audit has done.

## Benchmarks

Two questions, both of which decide code:

1. **What does a scroll cost?** Time a full-screen scroll and a single
   glyph, written straight into uncached MMIO and again through a RAM
   shadow blitted forward, at the default resolution on both
   architectures; and time a whole boot's console output with the
   framebuffer sink registered and without it. The shadow buffer is
   written only if the numbers say so (§21), and either way the figures
   go beside the decision in `docs/drivers/console/design.md`. If the
   fan-out cost is large enough to change the harness's timing, the
   default shape may register the sink only in a dedicated boot, and
   that too is a measurement.
2. **What does an idle keyboard cost?** Interrupts per second from a
   keyboard nobody is typing on, at the `bInterval` QEMU's device asks
   for and at a longer one, with the latency each gives. It is the first
   periodic endpoint in the tree, and "a device that interrupts forever"
   is a shape the tree has never had — a wrong answer here is a tax on
   every boot.

Key-to-tty latency is reported, not optimised: under TCG it measures
QEMU.

## Risks

- **The firmware may not give a GOP for the display device we pick**,
  particularly on AArch64, where the edk2 build the harness uses may
  carry a virtio-GPU driver and not a Bochs one. Step 1's first act is
  to find out on both architectures; the fallbacks are
  `virtio-gpu-pci`, and `ramfb`, which the kernel could program itself
  through the `fw_cfg` interface it already has. If neither works on
  AArch64 the framebuffer piece is x86-first with the reason written
  down, and steps 3 and 4 are unaffected.
- **`-display none`.** QEMU still models the display device with no UI,
  and the firmware should still bind a GOP to it; the readback test
  needs no window. If it turns out a display backend is required, the
  harness needs a headless one, which is a harness change and not a
  design change — but it is measured in step 1, not assumed.
- **A sink on the panic path.** Bounded work, no allocation, no sleeping
  lock, and panic mode drops the lock exactly as the console's own does.
  A scroll during a panic is a `memcpy`, which is acceptable; anything
  cleverer is not.
- **Two sinks change every `printk`.** A slow second sink slows every
  CPU that prints. This is what benchmark 1 exists for, and the answer
  may be that the framebuffer sink is not registered in the harness's
  default shape.
- **QMP in the harness** is another socket and another failure mode, in
  a CI that already needed a retry for a firmware handover that
  sometimes never happens. The key test waits for the driver's marker
  before sending anything, and a missing QMP socket fails that one test
  rather than the boot.
- **The hub's fields.** Route strings and TT are the part of xHCI most
  easily got wrong and least visible when wrong (a device that enumerates
  and then answers nothing). QEMU's `usb-hub` is a full-speed device, so
  the shape is at least exercised; if QEMU's xHCI will not take it, step
  4 becomes review-only and is dropped.
- **Font provenance.** Drawn in-tree from a generator script, for the
  reason given above; no table is copied in.
- **Size**: about 250 lines of loader and protocol, 450 of `fbcon`
  including a 1.6 KiB generated table, 300 of `usb_hid`, 450 of
  `usb_hub` with the core and xHCI changes, and 400 of tests and harness
  — roughly 1 850, the largest unit since USB. The four steps are
  independent, each is observable on its own, and the last is droppable.

## Alternatives considered

- **A PS/2 keyboard (i8042)** — 200 lines, works on `q35` today, and
  wrong: the machines §61 names do not have one, AArch64 has none at
  all, and it would leave the xHCI interrupt path still unexercised. It
  is the legacy interface §60 means when it says not to implement every
  historical one.
- **VGA text mode** — x86-only, and UEFI hands the loader a machine in a
  graphics mode. A text-mode console would work only on the machine that
  needs it least.
- **virtio-gpu with mode setting** — the GPU work §60 defers. Nothing
  here sets a mode, allocates a scanout, or touches a command queue; it
  writes pixels to the buffer the firmware is already scanning out.
- **A full input subsystem** (event nodes, a device file per device, a
  mux) — §21. One keyboard, one consumer, one call to `tty_input`. The
  second input device is what makes an input layer worth its interface,
  and a mouse has nothing yet to be a mouse for.
- **The AArch64 follow-ups** (GICv3, ASID allocation instead of a full
  invalidate per switch, FP/SIMD at EL0) — still worth doing, still
  improvements to things that work; none of them makes a machine without
  a serial port reachable.
- **NCQ** — measured and refused by the AHCI unit; nothing has changed.
- **Nothing at all: keep the serial console.** Defensible right up to
  the first real machine, which is the next thing §61 asks for.
