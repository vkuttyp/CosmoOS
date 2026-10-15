#!/usr/bin/env python3
"""The virtio-gpu unit's acceptance test: what the console draws is what
the display shows, at the size the display asks for
(docs/drivers/virtio/testing.md, "virtio-gpu").

    display_test.py --image cosmoos.img --workdir DIR [--size 1920x1080]

The live image boots twice under QEMU with a virtio-gpu-pci of the given
size (QEMU_GPU_SIZE) and QMP on a socket: alone (QEMU_DISPLAY=virtio),
where edk2 gives the loader no linear framebuffer, so every pixel on the
screen is one the kernel's virtio_gpu module had the device show; and
behind the firmware's own framebuffer (QEMU_DISPLAY=both, Parallels
Desktop's shape), where fbcon starts on that framebuffer and moves. At the shell prompt
the test echoes a marker line, then takes QEMU's `screendump` and reads
the screen back as text: each cell (8x16, or 16x32 at 3840x2160, the
size fbcon reports) is matched against the kernel's
own face (tools/mkfont.py), ink being any channel above half. It requires:

  a. the driver's line `virtio-gpu: ...: scanout 0 WxH` for the size asked,
     on the serial console (printed after the move: the move must leave
     the other console sinks working; in `both`, fbcon's first line on the
     firmware's framebuffer before it);
  b. a screenshot of exactly WxH pixels;
  c. the marker line, and the prompt that echoed it, in the decoded text,
     and no cell that matches no glyph; and the marker on the serial
     console.

With --panic-image (a CRASH_TEST=1 image; x86-64, where QEMU_DEBUG_EXIT=0
leaves the exit device out so a panic halts with QEMU running):

  d. that kernel's deliberate fault reaches the screen: after `halting.`
     on the serial console the decoded screen has the `KERNEL PANIC` line,
     which only fbcon's panic flush can have sent (no thread runs then).

With --panic-lockdep-image (a debug CRASH_TEST=3 image, x86-64):

  e. the same for a panic raised while another CPU owns lockdep's own
     lock: the flush uses no tracked lock (virtq_add_panic), so it neither
     waits for that owner nor keeps the serial sink after it waiting.

Exit 0 PASS, 1 FAIL.
"""

import argparse
import importlib.util
import json
import os
import re
import shutil
import socket
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import install_test  # noqa: E402
from install_test import Boot, Fail, need  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
T0 = time.monotonic()


def load_face():
    """{16 row bytes: character} from tools/mkfont.py, the kernel's face."""
    spec = importlib.util.spec_from_file_location("mkfont", os.path.join(ROOT, "tools", "mkfont.py"))
    mkfont = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mkfont)
    face = {}
    for code, _, rows in mkfont.table(mkfont.parse()):
        if code is not None:
            face[tuple(rows)] = chr(code)
    return face


def qmp(path, command, arguments=None):
    s = socket.socket(socket.AF_UNIX)
    s.settimeout(30)
    s.connect(path)
    f = s.makefile("rw")
    json.loads(f.readline())   # the greeting
    for cmd in ({"execute": "qmp_capabilities"}, {"execute": command, "arguments": arguments or {}}):
        f.write(json.dumps(cmd) + "\n")
        f.flush()
        while True:
            reply = json.loads(f.readline())
            if "return" in reply or "error" in reply:
                break
        if "error" in reply:
            raise Fail(f"QMP {cmd['execute']}: {reply['error']}")
    s.close()


def read_ppm(path):
    data = open(path, "rb").read()
    m = re.match(rb"P6\s+(\d+)\s+(\d+)\s+(\d+)\s", data)
    if not m:
        raise Fail(f"{path}: not a binary PPM")
    w, h = int(m.group(1)), int(m.group(2))
    return w, h, data[m.end():]


def decode(w, h, px, face, scale=1):
    """The screen as text lines; '\\0' for a cell that matches no glyph.
    At `scale` each font pixel is a scale x scale block (fbcon_scale); its
    top-left pixel is read."""
    lines = []
    cw, ch = 8 * scale, 16 * scale
    for row in range(h // ch):
        chars = []
        for col in range(w // cw):
            rows = []
            for y in range(16):
                bits = 0
                base = ((row * ch + y * scale) * w + col * cw) * 3
                for x in range(8):
                    p = base + 3 * x * scale
                    if max(px[p:p + 3]) > 0x80:
                        bits |= 0x80 >> x
                rows.append(bits)
            chars.append(face.get(tuple(rows), "\0"))
        lines.append("".join(chars).rstrip())
    return lines


def cell_scale(log, w, h):
    """fbcon's cell size on the WxH surface, from its own line, as a
    multiple of the 8x16 face (fbcon_scale: 2 at 3840x2160)."""
    cells = re.findall(r"^\[ INFO\] fbcon: \d+x\d+ cells of (\d+)x(\d+) pixels on a (\d+)x(\d+) surface", log, re.M)
    sizes = [(int(cw), int(ch)) for cw, ch, sw, sh in cells if (int(sw), int(sh)) == (w, h)]
    if not sizes:
        raise Fail(f"no fbcon line for a {w}x{h} surface")
    cw, ch = sizes[-1]
    if cw % 8 or ch != 2 * cw:
        raise Fail(f"fbcon's cells are {cw}x{ch}, not a multiple of the 8x16 face")
    return cw // 8


def screen(sock, path, face, w, h, scale):
    qmp(sock, "screendump", {"filename": path, "device": "gpu0"})   # the virtio-gpu (qemu-run.sh), not the VGA
    sw, sh, px = read_ppm(path)
    if (sw, sh) != (w, h):
        raise Fail(f"the screen is {sw}x{sh}, not {w}x{h}")
    return decode(sw, sh, px, face, scale)


def lockdep_held_elsewhere(line):
    """The seam's word, read back after its store: not free, and not this
    CPU's (an owner is its CPU's id + 1). Without the hold the panic is an
    ordinary one and check e would test nothing new."""
    m = re.search(r"\(owner word (\d+), this CPU's (\d+)\)", line)
    if not m:
        raise Fail(f"panic-lockdep: the panic does not report lockdep's owner word: {line!r}")
    owner, this = int(m.group(1)), int(m.group(2))
    if owner == 0 or owner == this:
        raise Fail(f"panic-lockdep: lockdep's lock was not held by another CPU (owner word {owner}, this CPU's {this})")


def panic_screen(args, face, image, name, what, message, check=None):
    w, h = (int(v) for v in args.size.split("x"))
    sock = os.path.join(args.workdir, f"qmp-{name}.sock")
    b = Boot(name, image, args.workdir,
             {"QEMU_DISPLAY": "virtio", "QEMU_GPU_SIZE": args.size, "QEMU_QMP": sock, "QEMU_DEBUG_EXIT": "0"},
             args.timeout)
    try:
        deadline = time.monotonic() + args.timeout
        while not re.search(r"^halting\.", b.text(), re.M):
            if time.monotonic() > deadline:
                raise Fail(f"{name}: the crash-test kernel did not panic (no `halting.` on the serial console)")
            time.sleep(0.2)
        need(b.text(), r"^\[ INFO\] virtio-gpu: \S+: scanout 0 ", "the virtio-gpu driver before the panic")
        time.sleep(1.0)   # nothing should change now; the panic flushed as it wrote
        lines = screen(sock, os.path.join(args.workdir, f"{name}.ppm"), face, w, h, cell_scale(b.text(), w, h))
        line = need(b.text(), r"^KERNEL PANIC: " + re.escape(message) + r".*$", f"{name}: the crash test's own panic")
        if check is not None:
            check(line.group(0))
        hit = [ln for ln in lines if ln.startswith("KERNEL PANIC: " + message)]
        if not hit:
            raise Fail(f"{name}: the panic is not on the screen; its last rows:\n" + "\n".join(lines[-12:]))
        return f"{what} is on the screen: {hit[0].strip()[:80]!r}"
    finally:
        b.kill()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--image", required=True)
    ap.add_argument("--workdir", required=True)
    ap.add_argument("--size", default="1920x1080")
    ap.add_argument("--timeout", type=float, default=180.0)
    ap.add_argument("--panic-image", help="a CRASH_TEST=1 image: check the panic reaches the screen")
    ap.add_argument("--panic-lockdep-image",
                    help="a debug CRASH_TEST=3 image: the same with lockdep's own lock owned by another CPU")
    args = ap.parse_args()
    shutil.rmtree(args.workdir, ignore_errors=True)
    os.makedirs(args.workdir)
    want_w, want_h = (int(v) for v in args.size.split("x"))
    face = load_face()
    results = []
    try:
        for shape in ("virtio", "both"):
            check_shape(args, face, shape, want_w, want_h, results)
        if args.panic_image:
            results.append(panic_screen(args, face, args.panic_image, "panic", "d. the crash-test kernel's panic",
                                        "page fault: kernel write at 0xffff900000000000"))
        if args.panic_lockdep_image:
            results.append(panic_screen(args, face, args.panic_lockdep_image, "panic-lockdep",
                                        "e. with lockdep's lock held by another CPU, the panic",
                                        "crash test: lockdep's lock is held by another CPU",
                                        check=lockdep_held_elsewhere))
    except Fail as e:
        for r in results:
            print(f"test-display: {r}")
        print(f"test-display: FAIL: {e} (logs in {args.workdir})")
        return 1
    for r in results:
        print(f"test-display: {r}")
    print(f"test-display: PASS in {time.monotonic() - T0:.1f}s")
    return 0


def check_shape(args, face, shape, want_w, want_h, results):
    """One boot. `virtio`: the GPU alone (the loader has no framebuffer).
    `both`: the firmware's framebuffer first, the GPU second, as on
    Parallels -- fbcon starts on the first and moves, and the serial
    console must keep working past the move."""
    sock = os.path.join(args.workdir, f"qmp-{shape}.sock")
    shot = os.path.join(args.workdir, f"screen-{shape}.ppm")
    b = Boot(shape, args.image, args.workdir,
             {"QEMU_DISPLAY": shape, "QEMU_GPU_SIZE": args.size, "QEMU_QMP": sock}, args.timeout)
    try:
        b.wait_prompt("the first prompt")
        log = b.text()
        if shape == "both":
            fb = need(log, r"^\[ INFO\] fbcon: (\d+)x(\d+) cells of \d+x\d+ pixels at 0x",
                      "fbcon on the firmware's framebuffer first")
            first = f"{fb.group(1)}x{fb.group(2)} cells on the firmware's framebuffer, then "
        else:
            need(log, r"^\[ INFO\] framebuffer: none; the console is the serial port alone$",
                 "the loader's refusal of the Blt-only mode")
            first = ""
        # Printed after the console moved: on the serial console only if
        # the move kept the sinks after fbcon's on the list.
        m = need(log, r"^\[ INFO\] virtio-gpu: (\S+): scanout 0 (\d+)x(\d+), resource \d+ in (\d+) chunk\(s\)$",
                 "the virtio-gpu driver's scanout")
        if (int(m.group(2)), int(m.group(3))) != (want_w, want_h):
            raise Fail(f"{shape}: the driver set {m.group(2)}x{m.group(3)}, the device offered {args.size}")

        # The marker and its command line each fit one row, so neither
        # wraps at a narrow size (80 columns at 640x480, the smallest
        # QEMU's firmware boots): the prompt, `echo '` and the closing
        # quote take 14 of them.
        scale = cell_scale(log, want_w, want_h)
        cols = want_w // (8 * scale)
        marker = f"display-check {shape} {os.getpid() % 100000:05d}: the quick brown fox jumps over 0123456789"
        marker = marker[:max(16, min(len(marker), cols - 15))].rstrip()
        out = b.run(f"echo '{marker}'")
        need(out, "^" + re.escape(marker) + "$", "the marker on the serial console")
        time.sleep(1.0)   # the driver flushes every 16 ms
        lines = screen(sock, shot, face, want_w, want_h, scale)

        bad = [(i, ln) for i, ln in enumerate(lines) if "\0" in ln]
        if bad:
            raise Fail(f"{shape}: {len(bad)} screen row(s) hold a cell that is no glyph, the first row {bad[0][0]}: "
                       f"{bad[0][1].replace(chr(0), '?')!r}")
        if marker not in lines:
            text = "\n".join(lines[-12:])
            raise Fail(f"{shape}: the marker is not on the screen; its last rows:\n{text}")
        at = lines.index(marker)
        if not any(marker in ln and ln.startswith(("cosmo$ ", "/ # ", "# ")) for ln in lines[max(0, at - 2):at]):
            raise Fail(f"{shape}: the command line is not above the marker: {lines[max(0, at - 2):at]!r}")
        used = sum(1 for ln in lines if ln)
        results.append(f"{shape}: {first}{m.group(1)} scanout 0 {args.size} in {m.group(4)} chunk(s); the "
                       f"{want_w}x{want_h} screendump reads back as text in {8 * scale}x{16 * scale} cells ({used} non-blank rows of {len(lines)}), "
                       f"the marker at row {at} under its command, and on the serial console")
        rc = b.finish()
        if rc != install_test.EXIT_SUCCESS:
            raise Fail(f"{shape}: QEMU exit code {rc}")
    except BaseException:
        b.kill()
        raise


if __name__ == "__main__":
    sys.exit(main())
