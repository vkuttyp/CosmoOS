# The screen console, RSA login keys and file times — 2026-10-13

Three changes the owner asked for after using v1.0.1 and the Parallels
devices unit on Parallels Desktop 27 (Apple Silicon), released together
as v1.0.2. Base: main `74537ed4` (PR #379). None is a roadmap milestone;
the roadmap's M1–M6 are done (v1.0.0).

## What changed

| Area | Change | Files |
|---|---|---|
| console face | an 8x16 face drawn in-tree (`tools/mkfont.py`, glyphs 7 wide in rows 2–14) replaces the 5x7 glyphs in 8x8 cells | `kernel/core/font8x16.c`, `kernel/include/kernel/font.h` |
| console scale | the face one-to-one up to 1920x1080 (Parallels' 1024x768: 128x48 cells, the owner's choice), scaled only above | `kernel/core/fbcon.c` |
| console scroll | a scroll moves a quarter of the screen; 32-bit pixels are written as rows (a scroll 13.4 → 6.6 ms on x86-64) | `kernel/core/fbcon.c` |
| display mode | the loader reads the EDID and sets the preferred mode when the firmware offers it, and reports what it saw on serial (Parallels offers one mode and no EDID) | `boot/uefi/display.c` |
| ssh | RSA user keys with rsa-sha2-256/512 (`DROPBEAR_RSA 1`, `DROPBEAR_RSA_SHA1 0`); the service still names only its Ed25519 host key | `ports/dropbear/localoptions.h` |
| file times | `vfs_now_ns` returns `clock_realtime_ns`, not `clock_now_ns` | `kernel-services/vfs/vfs.c` |

## Tests

- `fb-console` checks the scaled pixels.
- `test-net-login` logs in with a 3072-bit RSA key as well as Ed25519,
  on both architectures.
- `lxtest` requires a new file's mtime within 5 s of `CLOCK_REALTIME`.
  `tools/file-times-probe.py`: fixed mode `LINUXTEST: PASS`, `--old` fails
  that check (mtime 139 s, the uptime), on x86-64 and AArch64.

## Validation

The full matrix on `fbcon-scale` 6d7fac74, locally: every step passed on
AArch64 (host-test, analyze, debug boots at 1, 2 and 4 CPUs, smp2t,
chaos, harness-retry, crash, wxn, guard, entropy, security, release,
install, platforms) and on x86-64 (the same, plus fuzz and the
reproducible release), except x86-64 `test-smp2` and `test-crash`, which
failed in `irqpoll-boost` (longest gap 290 ms, bound 100) while other
work ran on the host. Rerun twice each on an idle host, all four passed.
Both sightings are in `docs/testing/flakes.md` (`irqpoll-boost` over its
gap bound): the same 290 ms as the two earlier CI sightings, not
attributed. Nothing on this branch touches the poll, its worker or the
timer path.

## Not done here

- Parallels' firmware offers only 1024x768 and no EDID; a larger screen
  needs a virtio-gpu driver (a later unit).
- scp, sftp and port forwarding stay compiled out of dropbear.
