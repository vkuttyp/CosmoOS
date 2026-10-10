#!/usr/bin/env python3
"""check-frame-baseline.py ROOT -- the kernel-stack guard's baseline
(kernel/include/kernel/compiler.h, FRAME_EXEMPT_BEGIN).

Every FRAME_EXEMPT_BEGIN(fn) in the kernel, kernel-services, compat and
drivers sources must be named in scripts/frame-baseline.txt with its
file, and every baseline line must have its wrapper: an exemption is
never added, or left behind, silently. Exit 1 with the differences.
"""
import os
import re
import sys

root = sys.argv[1]
want = set()
for line in open(os.path.join(root, "scripts", "frame-baseline.txt")):
    if line.strip() and not line.startswith("#"):
        path, fn, _size = line.split()
        want.add((path, fn))
have = set()
for top in ("kernel", "kernel-services", "compat", "drivers"):
    for d, _subdirs, files in os.walk(os.path.join(root, top)):
        for f in files:
            if not f.endswith(".c"):
                continue
            p = os.path.join(d, f)
            for m in re.finditer(r"^FRAME_EXEMPT_BEGIN\((\w+)\)", open(p, errors="replace").read(), re.M):
                have.add((os.path.relpath(p, root), m.group(1)))
bad = 0
for path, fn in sorted(have - want):
    print(f"check-frame-baseline: {path}: {fn} is exempt from the stack-frame guard but not in scripts/frame-baseline.txt")
    bad = 1
for path, fn in sorted(want - have):
    print(f"check-frame-baseline: scripts/frame-baseline.txt names {fn} in {path}, which has no FRAME_EXEMPT_BEGIN")
    bad = 1
sys.exit(bad)
