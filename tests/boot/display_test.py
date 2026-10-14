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
the screen back as text: each 8x16 cell is matched against the kernel's
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


def decode(w, h, px, face):
    """The screen as text lines; '\\0' for a cell that matches no glyph."""
    lines = []
    for row in range(h // 16):
        chars = []
        for col in range(w // 8):
            rows = []
            for y in range(16):
                bits = 0
                base = ((row * 16 + y) * w + col * 8) * 3
                for x in range(8):
                    if max(px[base + 3 * x:base + 3 * x + 3]) > 0x80:
                        bits |= 0x80 >> x
                rows.append(bits)
            chars.append(face.get(tuple(rows), "\0"))
        lines.append("".join(chars).rstrip())
    return lines


def screen(sock, path, face, w, h):
    qmp(sock, "screendump", {"filename": path, "device": "gpu0"})   # the virtio-gpu (qemu-run.sh), not the VGA
    sw, sh, px = read_ppm(path)
    if (sw, sh) != (w, h):
        raise Fail(f"the screen is {sw}x{sh}, not {w}x{h}")
    return decode(sw, sh, px, face)


def panic_screen(args, face):
    w, h = (int(v) for v in args.size.split("x"))
    sock = os.path.join(args.workdir, "qmp-panic.sock")
    b = Boot("panic", args.panic_image, args.workdir,
             {"QEMU_DISPLAY": "virtio", "QEMU_GPU_SIZE": args.size, "QEMU_QMP": sock, "QEMU_DEBUG_EXIT": "0"},
             args.timeout)
    try:
        deadline = time.monotonic() + args.timeout
        while not re.search(r"^halting\.", b.text(), re.M):
            if time.monotonic() > deadline:
                raise Fail("the crash-test kernel did not panic (no `halting.` on the serial console)")
            time.sleep(0.2)
        need(b.text(), r"^\[ INFO\] virtio-gpu: \S+: scanout 0 ", "the virtio-gpu driver before the panic")
        time.sleep(1.0)   # nothing should change now; the panic flushed as it wrote
        lines = screen(sock, os.path.join(args.workdir, "panic.ppm"), face, w, h)
        hit = [ln for ln in lines if "KERNEL PANIC" in ln]
        if not hit:
            raise Fail("the panic is not on the screen; its last rows:\n" + "\n".join(lines[-12:]))
        return f"d. the crash-test kernel's panic is on the screen: {hit[0].strip()[:80]!r}"
    finally:
        b.kill()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--image", required=True)
    ap.add_argument("--workdir", required=True)
    ap.add_argument("--size", default="1920x1080")
    ap.add_argument("--timeout", type=float, default=180.0)
    ap.add_argument("--panic-image", help="a CRASH_TEST=1 image: check the panic reaches the screen")
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
            results.append(panic_screen(args, face))
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

        marker = f"display-check {shape} {os.getpid() % 100000:05d}: the quick brown fox jumps over 0123456789"
        out = b.run(f"echo '{marker}'")
        need(out, "^" + re.escape(marker) + "$", "the marker on the serial console")
        time.sleep(1.0)   # the driver flushes every 16 ms
        lines = screen(sock, shot, face, want_w, want_h)

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
                       f"{want_w}x{want_h} screendump reads back as text ({used} non-blank rows of {len(lines)}), "
                       f"the marker at row {at} under its command, and on the serial console")
        rc = b.finish()
        if rc != install_test.EXIT_SUCCESS:
            raise Fail(f"{shape}: QEMU exit code {rc}")
    except BaseException:
        b.kill()
        raise


if __name__ == "__main__":
    sys.exit(main())
