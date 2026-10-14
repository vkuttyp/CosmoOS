# virtio-gpu — 2026-10-14

The fifth of the owner's post-1.0 units, after the everyday commands,
`ping`, `scp`/`sftp` and HTTPS. It was approved with them on 2026-10-14.
Base: main `6011a5ec` (v1.0.3's docs, PR #386).

**Goal:** on Parallels Desktop, the screen console fills the window at
the size Parallels gives the display, instead of the firmware's single
1024x768 mode. QEMU's `virtio-gpu-pci`, which gives the loader no linear
framebuffer at all, gets a console too.

## Why a driver

- **Parallels:** the firmware's GOP offers one mode, 1024x768, and no
  EDID (the console unit, 2026-10-13). Without a driver the console is
  128x48 cells whatever the window's size.
- **QEMU:** `virtio-gpu-pci`'s edk2 driver offers a `PixelBltOnly` mode,
  which the loader refuses (`boot/uefi/display.c`), so
  `QEMU_DISPLAY=virtio` booted with the serial console alone.
- **Scope:** constitution §60 says "GPU later", and the roadmap left GPU
  and display out of 1.0. This unit is 2D only: one scanout, the console
  drawn into guest memory. There is no 3D, no blob resources, no cursor,
  and no display interface for programs.

| | Parallels Desktop 27 (Apple Silicon) | QEMU `virtio-gpu-pci` |
|---|---|---|
| PCI id | 1af4:1050, modern (virtio device 16), `pci:00:0a.0` | 1af4:1050 |
| MSI-X vectors granted | 2: the configuration's and `controlq`'s | more |
| scanouts | 16 | 1 |
| firmware framebuffer | GOP 1024x768, linear | Blt-only: the loader passes none |
| first size offered | 1024x768 | `xres` x `yres` (default 1280x800) |

## Changes

| Area | Change | Files |
|---|---|---|
| driver | `virtio_gpu` module: `GET_DISPLAY_INFO`, a B8G8R8X8 resource of the scanout's size (capped at 3840x2160), backing in `dma_alloc` chunks of at most 4 MiB described a page per entry, `SET_SCANOUT`, `TRANSFER_TO_HOST_2D` and `RESOURCE_FLUSH`. The `vgpu` thread flushes what fbcon drew every 16 ms and follows `VIRTIO_GPU_EVENT_DISPLAY` to a frame of the new size; the panic flush polls | `drivers/virtio/virtio_gpu.c`, `build/module.mk` |
| fbcon | `fbcon_attach`/`fbcon_detach`/`fbcon_take_damage` (exported): a driver's write-back surface, the drawn rows recorded as one damaged band, `panic_flush` called by the sink in panic mode | `kernel/core/fbcon.c`, `kernel/include/kernel/fbcon.h` |
| mappings | `vm_map_direct_ranges` (exported, with `vm_unmap_phys`): direct-map buffers back to back as one view, so a frame in 4 MiB chunks is one linear framebuffer | `kernel/memory/vmm.c`, `kernel/include/kernel/vmm.h` |
| transport | `write_config32` in both transports and `virtio_write_config32` (exported by `virtio`): clearing `events_clear`; `VIRTIO_ID_GPU` | `drivers/virtio/` |
| harness | `QEMU_GPU_SIZE`, `QEMU_DISPLAY=both` (the firmware's framebuffer and a virtio-gpu, Parallels' shape), the GPU's QOM id `gpu0`, `QEMU_DEBUG_EXIT=0` (x86-64: a panic halts with QEMU running); the boot test's markers for the virtio shapes | `scripts/qemu-run.sh`, `tests/boot/run_boot_test.py` |

## Found on the way

- **Parallels refuses the backing entries inside the command's
  descriptor.** `RESOURCE_ATTACH_BACKING` with its entries in the same
  descriptor as the 32-byte command was answered 0x1205 (invalid
  parameter) on Parallels and accepted by QEMU. Entries of a page each
  did not change it. Sending the entries as a second descriptor, as
  Linux's driver does, did.
- **The first `fbcon_attach` cut the serial console off.** It copied the
  whole console structure, and with it the sink's `next` link (sinks are
  a singly linked list, newest first), so every sink after fbcon was
  dropped. On QEMU's virtio-only shape fbcon registers fresh at attach
  and the bug hid; on Parallels fbcon was already registered on the
  firmware framebuffer, and the serial console went silent. The sink is
  now kept across attach and detach. `make test-display`'s `both` shape
  boots that order; with the old copy it fails (no prompt on the serial
  console), and passes with the fix.
- **fbcon's drawing loops were inside a page by luck.** They were aligned
  to 256 bytes but are 124 to 457 long. This branch's AArch64 release
  link put `fbcon_draw_clear_rows` (272 bytes) at 0x...2f00, and
  `check-kernel-elf.sh` refused the kernel. Make had kept the refused ELF,
  so a second `make` booted it: an image check ran on a kernel that had
  failed its own link check. They are aligned to 512 now; checked on
  x86-64 and AArch64, debug and release.

## Tests

- **`make BUILD=release test-display`** (`tests/boot/display_test.py`;
  CI, in the net-login job, both architectures). A `virtio-gpu-pci` at
  1920x1080 with QMP, booted in two shapes:
  - `virtio`: the GPU alone;
  - `both`: the firmware's framebuffer first, then the GPU.

  In each, the driver's `scanout 0 1920x1080` line must be on the serial
  console (printed after the move), and QEMU's `screendump` of the GPU
  must be 1920x1080. The screen is read back as text by matching every
  8x16 cell against the kernel's own face (`tools/mkfont.py`), and must
  hold no unmatched cell and the echoed marker under its command line.
  On x86-64 it also boots the crash-test kernel (`CRASH_TEST=1`) without
  the exit device and requires the `KERNEL PANIC` line on the screen.
- **Mutations**, each failing the test:
  - no periodic flush: the marker is not on the screen;
  - `vgpu_panic_flush` returning at once: the panic is not on the screen;
  - the old sink copy in `fbcon_attach`: `both` gets no prompt.
- **Parallels Desktop 27** (manual, the live image): the console moved
  from the firmware's 1024x768 to the GPU at 1024x768. Resizing the VM's
  window through macOS (System Events) moved it to 1407x768 (175x48
  cells, resource 2), then 1129x768 and back to 1024x768. Parallels kept
  the height at 768. The serial console kept working throughout.
- The debug boot with `QEMU_DISPLAY=virtio` passes every self-test with
  the console on the GPU (x86-64).

## Validation

- **Matrix**, local, at `af33b68f`: 38 of 38 steps pass on x86-64 and
  AArch64 (host tests, fuzz, analyze, debug boots at 1, 2 and 4 CPUs with
  the network harness passing in each, `test-smp2`, chaos, harness retry,
  crash, wxn, guard, entropy, security, release boot, install, platforms,
  gic, and `make BUILD=release reproducible`, where `cosmoos.img`, which
  carries `virtio_gpu.ko`, is the same in both trees).
- **`make BUILD=release test-display`** passes on both architectures:
  `virtio` and `both` at 1920x1080 in 2 chunks each, the marker on the
  screen under its command; on x86-64 the crash-test kernel's
  `KERNEL PANIC: page fault: ...` line is on the screen.
- **`make BUILD=release test-net-login`** passes on both architectures.

## Review (PR #387)

Qodo, Codex and CosmoReview reviewed `b776cb2e`. Fixed in `de63fd92`:

- **A frame could be asked for in 2 MiB blocks** (Qodo, high).
  `vm_map_direct_ranges` passed `ARCH_MMU_MAP_LARGE`. For a view placed
  2 MiB-aligned over 2 MiB-aligned chunks, both MMUs make a block where
  the slot has no page table, and `vm_unmap_phys`, which unmaps 128 KiB
  at a time under a `KASSERT`, would panic on the old frame after a
  resize (neither MMU splits a block). Where the slot keeps a table from
  an earlier mapping (M19), the map fails with `-EEXIST` and the frame is
  refused. It maps pages now. The selftest `vm-direct-ranges` places such
  a view (filling the arena until it lands aligned), requires a 4 KiB
  leaf and unmaps it. With the old flag (`vmm.c` reverted in a worktree
  of `de63fd92`) it fails on both architectures at the map, the
  `-EEXIST` case: `check failed: view != 0`. The panic case needs a slot
  never mapped, which the test cannot choose; it follows from the two
  refusals above.
- **A display change not followed was dropped** (Qodo). It stays pending
  and is retried every second, with one warning.
- **Rows whose flush failed were dropped** (Qodo). They stay pending,
  merged with the next band.
- **A new frame given up after `SET_SCANOUT` left the scanout on a freed
  resource** (Qodo, Codex). The old frame goes back on the scanout and is
  flushed whole before the new resource is dropped.
- **A panic flush that timed out was tried again for every line**
  (Qodo), delaying the serial sink behind it. It is not tried again.
- **Memory a timed-out command may own was never freed** (Qodo). It is
  kept on a list and freed after the reset at removal, when the status
  reads 0.
- **The probe cleared display events after its size query** (Codex),
  so a change in between was lost. It clears them first.
- **`fbcon_attach` copied the log before the switch and registered the
  sink after the replay** (Qodo, Codex): a line in either window could
  miss the new screen. Copy, switch, replay and registration are one hold
  of the console lock now.
- **`struct virtio_transport` changed layout without an ABI bump**
  (Qodo, Codex). `COSMO_MODULE_ABI_VERSION` is 11.
- **`test-display` assumed 8x16 cells** (Codex); fbcon draws 16x32 at
  3840x2160. The test reads the cell size from fbcon's line.

Not changed: CosmoReview's "blocking call under spinlock" in
`vm_map_direct_ranges`. `region_new` allocates from `g_region_cache` with
`kmem_cache_alloc`, which takes only the cache's spinlock and grows from
`pmm_alloc_pages` (`kernel/memory/slab.c`); it does not sleep.
`vm_map_phys` does the same under the same lock.

## Not done here

- Any display interface for programs (a framebuffer device, mode setting
  from user space), a cursor, 3D (virgl), blob resources, more than one
  scanout.
- A resize test under QEMU, which has no way to resize a headless
  display: Parallels is the only check of the display-change path.
- The panic on the screen under AArch64 QEMU: its exit is semihosting,
  which cannot be left out; the flush code is the same on both.
- The Bochs adapter and `ramfb` mode setting.
