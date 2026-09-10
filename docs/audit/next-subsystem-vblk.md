# NEXT SUBSYSTEM — a root filesystem: virtio-blk, so Linux reaches userspace

## Problem

A stock Linux boots on CosmoOS and stops where a diskless machine stops:

```text
[    2.310878] Kernel panic - not syncing: VFS: Unable to mount root fs on unknown-block(0,0)
```

`No filesystem could mount root, tried:` — the list is empty, because the
guest has no block device. The kernel came up, drove the GIC, the timer
and the console this hypervisor built, read its feature registers and its
device tree, and found nothing to mount. The panic is the milestone the
Linux unit aimed at; this unit is what takes the kernel past it, into the
program a root filesystem carries — the first userspace that ever ran on
CosmoOS under a kernel not its own.

## Current implementation

**There is no device with a data plane.** The guest's devices are the
GICv3 distributor, the PL011 and the timer — each a register file the
kernel completes in place, with no payload larger than a byte. A disk is
different: a `read` moves a block between a file the owner holds and guest
memory, and that is neither a register nor a byte.

**The virtqueue code is the driver side.** `drivers/virtio/virtqueue.c`
is what the *host* kernel uses to talk to real virtio devices: it
*produces* the available ring and *consumes* the used ring
(`virtq_add`, `virtq_kick`, `virtq_get_used`). A guest's virtio-blk needs
the opposite end — *consume* available, *produce* used — which does not
exist in the tree. It is not large (`tests/host/test_virtq.c` already
models a device end, consuming `avail` and writing `used`, as its hostile
peer), and the ring layout structs are shared, but the device-side logic
is new.

**The MMIO seam already reaches the owner.** The machine unit made an
unclaimed guest MMIO access an exit to the owner carrying the size, the
register and the value (`HV_EXIT_MMIO`, completed on the next `vcpu_run`
for a read). So `vmctl` can model a device's registers in userland
without any new kernel mechanism — which is the right place for a device
whose backing store is a file the owner opens.

**The owner can read guest memory, and inject — but not through the
distributor.** `cosmo_vm_mem_read`/`_write` (SYS_vm_mem_rw) let the owner
walk a virtqueue's descriptors in guest RAM and move disk blocks in and
out. `cosmo_vcpu_irq` injects an interrupt — but through the vGIC's
pending set directly, which ignores the guest's distributor: whether the
guest enabled, grouped or routed that interrupt. A device's interrupt
must go through the distributor, as the console UART's SPI 33 does
(`vm_raise_spi`, in the kernel). There is no syscall for an owner-side
device to raise an SPI that way. That is the one kernel-side gap this
unit fills.

**No disk, no image, no virtio-mmio node.** `fdt_cosmo_virt` writes no
`virtio_mmio` node, and nothing carries a disk image. The guest is told
of no block device, which is why its root list is empty.

## Why it matters

- **It is userspace, the thing a kernel exists to run.** Linux booting to
  a panic proves the kernel came up; Linux running `/init` proves the
  machine is a machine. Every hypervisor unit so far has been toward a
  kernel; this is the first toward a program under that kernel.
- **It is the device seam's first real load.** The console proved the
  seam with a byte; the distributor proved interrupts with an SGI. A
  block device is the first to move a payload, through a virtqueue, with
  an interrupt on completion — the shape every later device (a network
  card, a console with a data plane) will reuse.
- **It is where the owner becomes a device model, not just a loader.**
  `vmctl` today loads an image and pumps exits. A virtio-blk device makes
  it the thing KVM's userland (QEMU) is: a process that holds a file,
  answers a guest's register accesses, walks its queues and completes its
  I/O. The seam for that — MMIO to the owner, guest-memory access, an SPI
  the owner can raise — is most of this unit.
- **It closes the arc honestly.** The Linux unit's milestone was a panic;
  a reader could fairly ask whether a kernel that only panics has "run".
  A kernel that mounts a root and runs a program has, by any measure.

## Proposed design

### 1. The device is the owner's, because the disk is the owner's

Unlike the distributor (whose output is a list-register write the owner
cannot reach) and the console (a byte per exit, too costly to round-trip),
a block device's backing store is a file, which only the owner holds, and
its traffic is batched — a queue-notify kicks many descriptors at once, so
one exit per notify is cheap. So virtio-blk lives in **`vmctl`**, not the
kernel. This is where KVM draws the same line, and what the machine and
console reports both said virtio would be.

### 2. A virtio-mmio transport, modelled in the owner

`fdt_cosmo_virt` gains one (or a few) `virtio_mmio@...` nodes at fixed
guest addresses the hypervisor defines (the uapi header, beside the GIC
and UART), each with an SPI the distributor can route. Linux's
virtio-mmio driver reads the transport's register window — `MagicValue`
(`0x74726976`, "virt"), `Version` (2), `DeviceID` (2 for block),
`VendorID`, the feature negotiation registers, the queue selection and
sizing registers, `QueueNotify`, `InterruptStatus`/`InterruptACK`,
`Status`, and the device-specific config space past `0x100` (for block:
the capacity). The window is unclaimed in the kernel, so every access is
an `HV_EXIT_MMIO` to `vmctl`, which models the register file — the
machine unit's seam already delivers these with size and value, and
completes a read on the next run.

The transport registers are a small state machine (feature bits offered
and acked, a queue's address and size written by the guest, the ready
flag); `vmctl` keeps it per device.

### 3. The device side of a virtqueue

When the guest writes `QueueNotify`, the owner walks the queue the guest
has set up in its own RAM: read `avail->idx`, and for each new entry read
the descriptor chain (`vm_mem_read` of the `virtq_desc` array the guest
placed at the address it wrote to the transport), gather the guest-physical
buffers, do the block I/O against the disk file, write the result back
into the guest's buffers (`vm_mem_write`), append to the `used` ring, and
raise the device's interrupt. The ring-walk is the device mirror of
`virtqueue.c` and of what `test_virtq.c` already does as a test device;
it is written once, for block, and is the reusable part.

Hostile-input discipline, from `test_virtq.c`'s own reason for existing:
the guest owns the descriptor ring, so every index, length and `next`
link the owner reads from guest memory is untrusted — a chain that loops,
a length past the buffer, a descriptor id out of range must be refused,
not followed. The owner validates exactly as the driver-side virtqueue
validates a hostile device.

### 4. The interrupt the owner raises, through the distributor

A new syscall, `cosmo_vm_raise_spi(vm, intid)` (and a lower), lets the
owner assert the device's SPI *through the guest's distributor* — the
same `vm_raise_spi` the console UART uses in the kernel, now reachable
from userland. Going through the distributor (not `cosmo_vcpu_irq`'s
direct pending-set injection) is what makes the guest's enable, group and
route state apply, so a guest that has configured its GIC for the device's
SPI takes the interrupt, and one that has not does not. The device is
level-triggered like the UART: the owner raises while a used entry is
unconsumed and lowers when the guest has acknowledged, reusing the
distributor's level handling the console established.

### 5. The disk, and the milestone

`vmctl run --machine --disk FILE IMAGE` opens `FILE` as the block
device's backing store. The milestone is the guest mounting it and
running a program: the device tree (or `root=/dev/vda` on the command
line) points Linux at the virtio-blk device, it mounts the filesystem,
and `/init` runs and prints a line the test recognises. The filesystem
can be the simplest that Linux mounts without modules in the boot path —
a squashfs or ext4 image with a single static `/init` that writes
`cosmo-guest: userspace` to the console and powers off via a `reboot`
syscall (which Linux turns into PSCI `SYSTEM_OFF`, which `vmctl` already
answers). That line, printed by a program, under a stock kernel, on a
filesystem served by the owner over virtio — is the unit, the way the
Linux unit's panic line was its predecessor's.

### 6. Deliberately out of scope

- **Writes, and a writable root.** A read-only root is enough to reach
  userspace; `VIRTIO_BLK_T_OUT` and flush come when a guest needs to
  persist, which the milestone does not. The device **sets
  `VIRTIO_BLK_F_RO`** so the driver knows the disk is read-only and never
  submits a write -- without that bit Linux exposes `/dev/vda` as
  writable and a mount, or the kernel's own write-back, submits a
  `VIRTIO_BLK_T_OUT` the device does not implement, failing the root.
- **Indirect and chained descriptors beyond the simple case, multiple
  queues, `VIRTIO_F_RING_EVENT_IDX`.** Block uses one queue; the owner
  offers the minimal set -- `VIRTIO_F_VERSION_1` and `VIRTIO_BLK_F_RO`,
  nothing else optional -- so the guest's driver takes the simple path
  and treats the disk as read-only.
- **virtio-net, virtio-console-with-dataplane, a PCI transport.** Each is
  a later unit on this same seam; block is the first because a root
  filesystem is what userspace needs.
- **Committing a disk image.** Like the kernel Image, the filesystem is
  someone's binary and is not committed; the test skips without it, and
  `el2-virtq-device` (below) is the CI gate that needs neither image nor
  disk.
- **An in-kernel virtio-blk.** The backing store is a file; the kernel
  has no business holding a guest's disk. The console and distributor are
  in the kernel because their output is kernel state; this is not.

## Affected files

| file | change |
|---|---|
| `kernel/include/uapi/cosmo/hv_machine.h` | the virtio-mmio window(s) and their SPIs |
| `kernel/include/uapi/cosmo/syscall.h`, `libc/include/cosmo/hv.h` | `SYS_vm_raise_spi` / `_lower_spi`; `cosmo_vm_raise_spi` |
| `kernel-services/virtualization/hvsys.c`, `vmdev.c` | the syscall → `vm_raise_spi` (the op the console already uses) |
| `tools/fdt/fdt.c` | a `virtio_mmio` node per device |
| `userland/system/vmctl.c` | `--disk`; the virtio-mmio transport register model; the block config |
| `userland/system/virtio_blk_dev.c` (or in `vmctl`) | **new**: the device-side virtqueue walk and the block I/O |
| `tests/host/test_virtq.c` | extended, or a new `test_vblk_dev`, for the device-side ring walk and its hostile-input refusals |
| `kernel-services/virtualization/hvtest.c` | `el2-virtq-device` (a guest driving a test virtio-mmio device the test models), `el2-vm-raise-spi` |
| `userland/etc/rc.test`, `run_boot_test.py` | the Linux-to-userspace run when an Image and a disk are provided |
| docs | `virtualization/design.md`, `testing.md`, README |

## New APIs

```c
/* uapi: the owner raises a device's shared interrupt through the guest's
 * distributor -- honouring its enable/group/route, unlike cosmo_vcpu_irq. */
#define SYS_vm_raise_spi  <n>
#define SYS_vm_lower_spi  <n+1>
int cosmo_vm_raise_spi(int vm, unsigned intid);
int cosmo_vm_lower_spi(int vm, unsigned intid);

/* uapi cosmo/hv_machine.h: the virtio-mmio window(s) the owner models. */
#define COSMO_HVM_VIRTIO0_BASE   0x0A000000ull   /* where virt puts its virtio-mmio bank */
#define COSMO_HVM_VIRTIO0_SIZE   0x200ull
#define COSMO_HVM_VIRTIO0_INTID  48u             /* an SPI, routed by the distributor */
```

The kernel gains no device: `vm_raise_spi` already exists for the console;
the syscall is a thin wrapper with the same VM-handle rights check the
other VM syscalls use. Everything else is in the owner.

## Migration plan

1. **The owner can raise an SPI.** `SYS_vm_raise_spi`/`_lower_spi`,
   `cosmo_vm_raise_spi`. `el2-vm-raise-spi`: the test, as owner, raises an
   SPI the guest has routed to itself and the guest takes it; raised while
   the guest has it masked, it waits; this is the console's level path
   reached from userland. No virtio yet.
2. **The device-side virtqueue walk, on the host.** The consume-avail /
   produce-used logic and its hostile-input refusals, as a host test
   (`test_vblk_dev` or an extension of `test_virtq`), over an in-memory
   ring — no guest, no QEMU. This is the reusable core, proved before it
   is wired to a guest.
3. **The virtio-mmio transport in `vmctl`.** The register model; a guest
   that probes the node negotiates features, sets up its queue, and reads
   the capacity. `el2-virtq-device`: a C guest brings up a virtio-mmio
   device the *test* backs (a tiny in-test block device), submits a read,
   and gets the bytes — the whole path in the kernel's own harness,
   without Linux.
4. **`vmctl --disk`, and Linux.** The block I/O against a real file; the
   device tree's `virtio_mmio` node; `root=/dev/vda`. Linux mounts the
   disk and runs `/init`; `rc.test` runs it when an Image and a disk are
   present.
5. **The milestone.** `/init` prints `cosmo-guest: userspace` and powers
   off; the demonstration is that line in the log under `QEMU_MEM=2G`.
6. Docs, README.

## Tests

- **`test_vblk_dev`** (host) — the device-side ring walk: a read request
  across a descriptor chain is gathered, served and completed into the
  used ring; a chain that loops, a length past the buffer, a descriptor
  id out of range, a zero-length queue are each refused without a
  read out of bounds. This is the core, proved on the host in a second,
  and it is the CI gate — no Image, no disk, no QEMU.
- **`el2-vm-raise-spi`** — the owner-raised SPI through the distributor: a
  guest routes an SPI to itself and takes it when the test raises it;
  masked, it waits; `cosmo_vcpu_irq`'s direct path is *not* what the guest
  sees (the distributor's enable gates it). Distinguishes the new syscall
  from the old injection.
- **`el2-virtq-device`** — a C guest drives a virtio-mmio device the test
  models: `MagicValue`/`Version`/`DeviceID` read as a block device,
  feature negotiation completes, a queue is set up, a read of a known
  sector returns the test's known bytes, and the completion interrupt
  arrives through the distributor. The whole transport and device path in
  the kernel harness, no Linux.
- **Linux to userspace** (userland, demonstration, `QEMU_MEM=2G`) — with
  an Image and a disk present, Linux mounts `/dev/vda` and `/init` prints
  `cosmo-guest: userspace`. The milestone a human confirms; skipped
  without the artifacts, like the Linux boot.

The two vacuity traps this unit's history warns of: the host ring-walk
test must assert a *served* read returns the device's known bytes (not
merely that it completes), and the hostile-input cases must confirm no
out-of-bounds guest read happened (a bounds check that is never reached
is not a bounds check); and `el2-virtq-device` must read back a sector's
content, not just that an interrupt fired.

## Benchmarks

Counted:

- **Does userspace run: no today, yes after.** The headline.
- **Exits per block request.** One MMIO exit for the `QueueNotify`, then
  the owner serves N descriptors with no further exit until it raises the
  completion SPI; the count that says batching works and a disk is not a
  byte-at-a-time console.
- **Hostile descriptors refused vs. followed**, from `test_vblk_dev`: all
  refused, none followed — the number that says the owner does not trust
  the ring.

## Risks

- **The owner now reads and writes guest memory on the guest's
  instructions.** A descriptor points where the guest says; the owner
  must treat every address and length as hostile and clamp it to the
  guest's own regions (`vm_mem_rw` already bounds to mapped regions and
  returns `-EFAULT` past them, which is the backstop), and must never let
  a descriptor chain loop it forever. This is the security core of the
  unit, and `test_vblk_dev`'s refusals are where it is proved.
- **An owner-raised SPI must go through the distributor, or the guest's
  GIC configuration is bypassed.** The temptation is `cosmo_vcpu_irq`,
  which is simpler and wrong: it injects past the distributor's enable and
  route state, so a guest that has not set up the device's interrupt would
  take it anyway, or take it on the wrong vCPU. The new syscall goes
  through `vm_raise_spi`; `el2-vm-raise-spi` is the test that the
  distributor's state actually gates it.
- **The feature negotiation is a handshake with a real driver.** Linux's
  virtio-mmio and virtio-blk drivers expect `VIRTIO_F_VERSION_1` and a
  precise status-bit dance (ACKNOWLEDGE, DRIVER, FEATURES_OK, DRIVER_OK);
  a transport that accepts the wrong order or offers a feature it does not
  implement hangs the driver. The owner offers the minimal set
  (`VIRTIO_F_VERSION_1` and, for block, `VIRTIO_BLK_F_RO`) and follows the
  status machine exactly; `el2-virtq-device` drives it with a C guest that
  does the same dance, so the handshake is tested before Linux.
- **A wrong `virtio_mmio` node is a driver that does not probe, silently.**
  As with the PL011, the device tree's `reg`, `interrupts` and
  `compatible` must be what Linux's driver matches; the proof is whether
  the driver probes, visible because a probed device reads the capacity
  and an unprobed one does not.
- **The milestone needs an Image and a disk, and neither is committed.**
  The CI gate is `test_vblk_dev` and `el2-virtq-device`, which need
  neither; the Linux-to-userspace run is a documented demonstration under
  `QEMU_MEM=2G`, skipped without the artifacts, exactly as the Linux boot
  is. A reader reproducing it needs a kernel Image and a small root image,
  both documented by source, not shipped.

## Alternatives considered

- **An initramfs instead of a block device.** Linux can run userspace
  from an initramfs the bootloader loads into memory, no disk at all —
  simpler, and it would reach userspace sooner. But it proves no device:
  the initramfs is just guest memory the owner filled. The point of the
  unit is the first device with a data plane; a root on virtio-blk is
  that, and an initramfs is a shortcut past it. (An initramfs may still
  be the quickest way to *get* a userspace to run while developing the
  device — used as a scaffold, not the deliverable.)
- **virtio-blk over the existing PCI transport.** The tree has a
  virtio-pci *driver*; a guest virtio-pci *device* would mean emulating a
  PCIe root complex, config space and MSI-X for the guest — far more than
  virtio-mmio, which is a flat register window and a wired SPI. `virt`
  offers both and Linux drives either; mmio is the smaller machine.
- **In-kernel virtio-blk, like the console.** The console is in the kernel
  because its output is a kernel ring; a disk's backing is a file in the
  owner's filesystem, which the kernel cannot and should not hold. The
  one-exit-per-notify cost that justified the console's placement argues
  the other way here: batched I/O makes the round trip cheap.
- **Wait for a writable, journaled filesystem.** A read-only root reaches
  userspace, which is the claim; write support, flush and a real
  filesystem are what a guest that *does* something needs, and are the
  unit after, driven by a guest that needs to persist.
