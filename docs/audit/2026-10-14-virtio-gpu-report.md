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
- **After the review fixes**, at `aee66f2e`: the matrix again, 38 of 38
  steps on both architectures (the debug boots run `vm-direct-ranges`,
  the network harness passing in each; reproducible on both), and
  `test-display` and `test-net-login` on both. `make BUILD=release
  DISPLAY_SIZE=3840x2160 test-display` passes on x86-64, read in 16x32
  cells.
- **At `3804c9ad`**, the matrix: 39 of 42 steps passed, the network
  harness ready in each debug boot and reproducible on both
  architectures; host tests and fuzz failed to link (fixed in
  `37ee42e6`, then passing on both). `test-display` and `test-net-login`
  passed on both.
- **At `e7c32a81`**: `test-display` passes on both architectures (check e
  reports owner word 2 for CPU 1), and at 640x480 on x86-64.
- **At `8ea7dc56`**, the matrix: 38 of 38 steps on both architectures,
  the network harness ready in each debug boot (1, 2 and 4 CPUs,
  `test-smp2`, chaos, harness retry), reproducible on both;
  `test-display` and `test-net-login` on both.
- **At `93ccddb8`**: debug `make test` on both architectures (477
  self-tests, `fb-switch`, the network harness ready), and `test-display`
  with check f on both.
- **At `b729944e`** (the fourth round, below): debug `make test` passes
  on both architectures (477 self-tests, `fb-switch` with the late
  notice, the network harness ready) and `test-display` on both.

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

Codex's second review, of `8fca562e`, fixed in `cf18d2b2` and
`06c92421`:

- **The panic flush took tracked locks** (P1). In debug builds every
  spinlock operation takes lockdep's own lock, which a CPU the panic
  stopped may own; `spin_trylock`, `virtq_add`, `virtq_pop` and the
  modern transport's notify (which selected the queue under the
  transport's lock) would wait for it forever, before the serial sink,
  which follows fbcon's on the list, printed the panic. Now
  `spin_trylock_panic` and `virtq_add_panic`/`virtq_pop_panic` take the
  queue's lock word untracked, and virtio-pci reads each queue's notify
  offset at setup, so notify takes no lock. `test-display`'s check e
  boots a debug `CRASH_TEST=3` kernel that leaves lockdep's lock owned by
  another CPU and panics: the panic must be on the serial console and the
  screen. With the driver side of `cf18d2b2` reverted in a worktree, the
  serial log stops at `crash test: panicking with lockdep's lock held
  elsewhere, on purpose` and check e fails (no `halting.`); with it, it
  passes. The first version of the seam read `arch_cpu_id()` from a
  thread and tripped the per-CPU check's own panic, which reached the
  screen and passed check e for the wrong reason; it reads `raw_cpu_id()`
  and check e requires the crash test's own message.
- **The boot test's virtio-gpu marker assumed 8x16 cells** (P2): it
  accepts 16x32 and up, as the firmware framebuffer's marker does.

The rest of that review and Qodo's of `8fca562e` and `3804c9ad`, fixed in
`37ee42e6`, `9286bba2` and `e7c32a81`:

- **The panic flush flushed `g->frame`** (Codex), which names a new frame
  only after `fbcon_attach` returns: a panic during the switch flushed the
  old resource, or nothing. The surface's argument is now its own frame.
- **The panic flush completed the thread's command** (Qodo), taking the
  completion's tracked lock, the hang its first fix removed. A thread's
  command answered meanwhile is dropped; its waiter never runs again.
- **A line logged inside the panic flush re-entered it** (Qodo):
  `virtq_pop_panic` reporting a bad used entry writes the console, which
  called the flush again over the request in flight. It is not re-entered.
- **A failed resize rebuilt a request after a timeout** (Qodo):
  `vgpu_restore_scanout` wrote `g->cmd.req`, which the device may still
  read, before `vgpu_cmd_at` refused to send. Every builder returns `-EIO`
  first once `broken` is set.
- **A resize replaced the screen with old log lines** (Codex). Program
  output reaches the sinks without the log ring, so the shell's screen,
  and a half-typed line, were lost. `fbcon_attach` and `fbcon_detach`
  carry the screen they replace (rows up to the cursor's, cut or padded,
  not reflowed); the log is replayed only for a first console.
- **A resize sent no SIGWINCH** (Codex). A new cell grid calls
  `tty_console_resized`: the console's stored size is dropped, so it
  answers the new geometry, and the foreground group gets SIGWINCH.
  Selftest `fb-switch` attaches 60x17 and then 40x12 surfaces over the
  boot framebuffer (160x50 on x86-64, 100x37 on AArch64) and restores it.
  It fails, on both architectures, with the old log replay ("the lines
  written before the switch are not on the new surface") and without
  `tty_console_resized` ("the console's size is not the new surface's
  40x12"); each run's only failure of 477.
- **Check e could pass without the hold** (Qodo). The panic reports
  lockdep's owner word, read back after the store, and the check requires
  another CPU's. With the seam's call removed (a worktree of `e7c32a81`)
  it fails: `owner word 0, this CPU's 1`.
- **The display marker wrapped on narrow screens** (Codex). It is cut to
  the screen's columns. `DISPLAY_SIZE=640x480` passes on x86-64; below
  that QEMU's firmware does not boot (320x200 on both architectures and
  400x300 on x86-64 print nothing before the loader).
- **`vm-direct-ranges` wrote through a view it had not found mapped**
  (Qodo). The store follows the query.
- **Docs** (Qodo): the virtio module's exports (20 since `virtq_quiesce`), and an inventory line
  that read as if no test mapped a direct range.
- **Found by the matrix**: `test_virtq` and `fuzz_virtq` did not link
  (the host shim lacked the panic pair), and CI's "build, boot, analyze"
  jobs failed on it. The shim has them now (`37ee42e6`). A release build
  failed on `fb-switch`'s query, which is selftest-only; it has a stub
  (`e7c32a81`), and neither release kernel carries a seam symbol
  (`llvm-nm`).

Qodo's and Codex's reviews of `8ea7dc56`, fixed in `b729944e`:

- **A late resize notice dropped a newer size** (Qodo).
  `tty_console_resized` runs after the console lock's release, and a
  `TIOCSWINSZ` in between was cleared with the old one. Each grid change
  is numbered in the hold that makes it (`fbcon_switches`), `TIOCSWINSZ`
  records the number it saw, and the notice drops only an older size.
  `fb-switch` sets 9x4 after the switch and delivers its notice late; with
  the notice dropping every size (a worktree of `b729944e`) it fails on
  both architectures, "a size set after the switch was dropped by its
  late notice", each run's only failure of 477.
- **A line logged inside the panic flush lost its rows** (Qodo): fbcon
  cleared the band before the call, and the re-entered call returned
  without sending it, a scroll's repaint included. `panic_flush` returns
  whether the band reached the device, and fbcon merges an unsent band
  back for the next line's flush. Not reachable with QEMU's device.
- **fbcon's readers raced a switch** (Codex): `fbcon_geometry`,
  `fbcon_cursor` and `fbcon_get_stats` copied `g_fbcon` without the lock
  it is replaced under. They take it. The pixels at `base` stay the
  owner's; their only readers are the self-tests.
- **A refused detach freed attached memory** (Codex): an error response
  to `RESOURCE_DETACH_BACKING` does not set `broken`, and the chunks were
  freed. They are kept until a reset the device acknowledges.
- **The queue was freed before the reset was acknowledged** (Codex).
  `vgpu_teardown` checks the status first; when it is not 0 nothing is
  freed and `device_retain_dma` keeps drivers off the device (U14). Not
  reachable with QEMU's device, which always acknowledges.
- **Docs promised `DISPLAY_SIZE` down to 320x200** (Qodo): the testing
  row now says 640x480 is the smallest tested.

Qodo's review of `d8e77cfc`:

- **The kept queue's interrupt outlived the driver** (Qodo). With the
  reset unacknowledged, `vgpu_teardown` kept the queue and with it its
  vector and irq_poll, and `vgpu_done` could run on the freed `vgpu` or
  an unloaded module. `virtq_quiesce`, `virtq_free`'s first half, stops
  both and keeps the ring; `vdev->priv` is cleared before `kfree`.

Codex's review of `d8e77cfc`:

- **`RESOURCE_UNREF` was built after a timed-out detach** (Codex), over
  the request page the device may still read. No UNREF follows a failed
  detach; the reset drops the resource.
- **The panic flush took an error response as success** (Codex). The
  response is cleared before each command and must read `OK_NODATA`;
  otherwise the band stays pending.
- **The `CRASH_TEST=3` seam built into a release kernel with `LOCKDEP=1`**
  (Codex). `lockdep.c` refuses that configuration (`#error` without
  `CONFIG_DEBUG`); `make test-display` builds it with `BUILD=debug`.

Codex's and CosmoReview's reviews of `a8270905`:

- **No test drove a display change** (Codex): `fb-switch` calls
  `fbcon_attach` itself, so the driver's event polling, clearing and
  retry were checked on Parallels only, and the report said QEMU could
  not resize a headless display. It can: its VNC server takes a client's
  SetDesktopSize and hands it to the device as `VIRTIO_GPU_EVENT_DISPLAY`.
  `test-display`'s check f does that in the `virtio` shape (1920x1080 to
  1024x768) and requires the driver's new scanout line and the marker on
  the new screen. It passes on both architectures; with the event check
  disabled (a worktree of `93ccddb8`) it fails on both, "the driver did
  not follow the display to 1024x768 in 30 s".
- **A panic during a switch could walk a half-copied console** (Codex):
  `fbcon_attach` and `fbcon_detach` assigned the whole structure, the
  sink's list link included, which a CPU in panic mode reads without the
  console lock. The boot console and two surface slots are now whole
  consoles; a switch builds the one not shown and publishes it with one
  store, and the sink and the counters live outside them.
- **The resize retry compared `clock_now_ns() + delay` across a possible
  migration** (Codex): `clock_deadline_ns` and `clock_deadline_passed`.
  The panic flush's 200 ms poll stays on `clock_now_ns`: nothing migrates
  in a panic, and the machine-wide tick the deadline helpers fall back to
  may not run.
- **`ent` leaked on `vgpu_frame_new`'s failure path** (CosmoReview): not
  so. `vgpu_dma_release` hands the entries back before the only `goto
  fail` that follows their allocation.

Qodo's review of `d4b3d0ab`:

- **A panic's writer could draw on a console a switch freed** (Qodo,
  high). `smp_stop_others` gives up on a CPU with interrupts off (one
  holding the console lock in a switch), and lockdep's report enters
  panic mode before any stop, so a lockless writer can load the old
  console just before the publish. A switch now refuses to start in
  panic mode and, after its publish, fences and looks again: a panic
  seen then keeps the old text, and the driver, which reads the same
  flag after `fbcon_attach`/`fbcon_detach`, keeps the old frame (removal
  tears nothing down). The writer fences before loading the pointer, so
  when the switch sees no panic the writer sees the new console. Not
  reachable in a test: panic mode does not end.
- **`QEMU_EXTRA` from the caller was replaced in the `virtio` boot**
  (Qodo): check f's `-vnc` is appended to it.
- **A VNC socket error escaped as a traceback** (Qodo): `OSError` in
  the client is the test's own failure, with the socket's path.

Not changed: CosmoReview's "blocking call under spinlock" in
`vm_map_direct_ranges`. `region_new` allocates from `g_region_cache` with
`kmem_cache_alloc`, which takes only the cache's spinlock and grows from
`pmm_alloc_pages` (`kernel/memory/slab.c`); it does not sleep.
`vm_map_phys` does the same under the same lock.

## Not done here

- Any display interface for programs (a framebuffer device, mode setting
  from user space), a cursor, 3D (virgl), blob resources, more than one
  scanout.
- The panic on the screen under AArch64 QEMU: its exit is semihosting,
  which cannot be left out; the flush code is the same on both.
- The Bochs adapter and `ramfb` mode setting.
