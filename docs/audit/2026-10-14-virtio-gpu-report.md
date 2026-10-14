# virtio-gpu — 2026-10-14

> §68 report, written before the implementation: it records the plan,
> and every API, file and test below is planned unless it says
> otherwise. The implementation PR updates it to what was built.

The fifth of the owner's post-1.0 units (after the everyday commands,
`ping`, `scp`/`sftp` and HTTPS). It was approved with the others on
2026-10-14. Base: main `6011a5ec` (v1.0.3's docs, PR #386).

**Goal:** on Parallels Desktop, the screen console fills the window at
the size Parallels asks for, instead of the firmware's single 1024x768
mode. QEMU's `virtio-gpu-pci`, which gives the loader no linear
framebuffer at all, gets a console too.

## Why a driver

- **Parallels:** the firmware's GOP offers one mode, 1024x768, and no
  EDID (`docs/guide/platforms.md`; the console unit, 2026-10-13). The
  console is 128x48 cells, whatever the window's size.
- **QEMU:** `virtio-gpu-pci`'s edk2 driver offers a `PixelBltOnly` mode
  with no linear buffer, which the loader refuses (`boot/uefi/display.c`).
  `QEMU_DISPLAY=virtio` boots with the serial console alone.
- **Scope:** constitution §60 says "GPU later", and the roadmap left GPU
  and display out of 1.0. This unit is 2D only:
  - one scanout, one resource, the console drawn into guest memory;
  - no 3D (virgl), no blob resources, no cursor, no user-visible display
    device and no mode-setting interface for programs.

## The device on both targets

| | Parallels Desktop 27 (Apple Silicon) | QEMU `virtio-gpu-pci` |
|---|---|---|
| PCI id | 1af4:1050, modern (virtio device 16), `pci:00:0a.0` | 1af4:1050 |
| MSI-X vectors granted | 2 (boot log, 2026-10-14) | per QEMU |
| firmware framebuffer | GOP 1024x768, linear | Blt-only: the loader passes none |

With 2 vectors, entry 0 is the configuration vector and entry 1 serves
`controlq` (queue 0) (`drivers/virtio/virtio_pci.c`, `vpci_setup_queue`).
`cursorq` (queue 1) is not set up: the spec lets a driver leave a queue
unused, and this driver draws no cursor.

## Design

1. **`drivers/virtio/virtio_gpu.c`**, a module on the virtio bus (id 16),
   on the pattern of `virtio_rng`. Probe:
   - `GET_DISPLAY_INFO`: scanout 0's enabled rectangle is the size, capped
     at 3840x2160. With none enabled, the size is 1024x768.
   - `RESOURCE_CREATE_2D`, B8G8R8X8.
   - Backing from `dma_alloc` in chunks of at most 4 MiB, the page
     allocator's largest block (an 8 MiB 1920x1080 frame is two chunks):
     `RESOURCE_ATTACH_BACKING` with one entry per chunk.
   - `SET_SCANOUT`, then a full `TRANSFER_TO_HOST_2D` and `RESOURCE_FLUSH`.
   - Every command waits for its response on `controlq`, and a response
     other than `OK_NODATA` or `OK_DISPLAY_INFO` fails the probe with its
     code.
2. **A contiguous view of the chunks.** fbcon draws rows across the frame,
   so the chunks are mapped back to back in kernel virtual space. This
   needs an exported mapping call: `vm_map_phys` is not exported, and a
   page list is the shape needed (`vm_map_pages`, planned). Write-back
   memory, not uncached: the device reads guest RAM. Both architectures'
   DMA is cache-coherent here (`kernel/device/dma.c`).
3. **fbcon gains a surface it can be moved to** (`kernel/core/fbcon.c`,
   built in). It exports:
   - `fbcon_attach(const struct fbcon_surface *)`: base, width, height,
     pitch, pixel format, and `panic_flush`.
   - `fbcon_detach()`.
   - `fbcon_take_damage(&y0, &y1)`.

   Attach allocates the new text buffer first (thread context), then under
   the console lock (`console_hold`) swaps the target, clears it and
   repaints the newest screenful of the log. The sink path stays as it is:
   no allocation, no sleep, no lock of its own. It now records the rows
   it drew as one damaged band, `[y0, y1)`.
4. **Flushing from a thread.** The sink runs in interrupt and panic
   context, so it never talks to the device. A driver thread, `vgpu-flush`:
   - takes the damage under the console lock, every 16 ms while there is
     any;
   - sends `TRANSFER_TO_HOST_2D` and `RESOURCE_FLUSH` for that band.

   A burst of output therefore costs one transfer per tick, not one per
   glyph.
5. **Panic.** In panic mode console.c drops its lock and no thread runs,
   so fbcon calls the surface's `panic_flush` after each write. The driver
   sends the transfer and flush on `controlq` with interrupts not needed:
   it adds, kicks, and spins on `virtq_pop` for a bounded time. The
   screen then shows the panic, as the firmware framebuffer does today.
6. **Display changes.** Parallels resizes the scanout when the window
   changes and raises `VIRTIO_GPU_EVENT_DISPLAY` through the
   configuration interrupt. The transport's `vpci_config_irq` only logs
   today, so `struct virtio_driver` gains `config_changed` (module ABI
   bump). The driver's thread then re-reads the display info, builds a
   resource at the new size, attaches fbcon to it, and only then frees
   the old one.
7. **Remove.** `fbcon_detach` first, back to the firmware framebuffer if
   there was one, else the serial console. Then `SET_SCANOUT` with
   resource 0, `RESOURCE_UNREF`, and the backing freed.

## Tests (planned)

- **`tests/boot/display_test.py`** (`make test-display`, both
  architectures, in CI): QEMU with `virtio-gpu-pci` at a set `xres`/`yres`.
  The harness waits for the driver's line
  (`virtio-gpu: scanout 0 WxH, ...`), then takes QEMU's `screendump`. It
  decodes the screen into characters by matching each cell against the
  kernel's own 8x16 face (`tools/mkfont.py`), and requires the boot's
  marker lines on the screen at the requested size. This is the end-to-end
  claim: what the guest drew is what the device shows. A second size,
  through a resize at QEMU's monitor if QEMU offers one, checks the
  display-change path.
- **A self-test**, `virtio-gpu` (debug boots with the device): the
  display info is sane, fbcon is attached at the scanout's size, a
  drawn row is flushed (damage taken, response `OK_NODATA`), and a second
  attach at another size leaves no damage pending against the old one.
- **`fb-console`, unchanged:** it runs against whichever surface fbcon has.
- **`run_boot_test.py`:** `QEMU_DISPLAY=virtio` now expects the console on
  the GPU instead of `framebuffer: none`.
- **Parallels:** the console fills a resized window. This is a manual
  check, recorded in the implementation report.

## Not in this unit

- Any user-visible display interface (a framebuffer device, mode setting
  from user space), cursor, 3D, blob resources, more than one scanout.
- The Bochs/`bochs-display` adapter, `ramfb` mode setting.
