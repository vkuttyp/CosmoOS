#!/usr/bin/env python3
"""
nova-gen.py - the C that CosmoOS's Nova sources generate (docs/build/design.md, "Nova sources").

Some kernel code is written in Nova, a memory-safe language compiled to C
(https://github.com/vkuttyp/Nova). Each NAME.nova sits beside the NAME.c it
generates, and the generated C is checked in, so building CosmoOS needs no
Nova compiler; only changing a .nova file does. Nova's runtime, which the
generated C needs, is generated too.

  kernel/block/gpt.nova        -> kernel/block/gpt.c
  (every port)                 -> kernel/include/nova/rt.h, kernel/nova/nova_rt_core.c

The compiler is pinned (NOVAC_VERSION): generated C is a pure function of
the source and the compiler, so a regeneration with the pinned novac
reproduces the checked-in files byte for byte, and --check (CI's "Nova
sources" job) fails when they differ. Moving to another novac is a change
of its own: bump the pin and regenerate.

Usage:
    python3 scripts/nova-gen.py           # regenerate (make nova)
    python3 scripts/nova-gen.py --check   # compare only (make nova-check)
    python3 scripts/nova-gen.py --pinned  # print the pinned version
The novac used is $NOVAC, else `novac` on PATH. To install the pinned one:
    curl -fsSL https://vkuttyp.github.io/Nova/install.sh | NOVA_VERSION=v<pinned> sh
"""

import argparse
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

NOVAC_VERSION = "0.1.1"

# Each port: its Nova source, the C it generates, and what that C is.
PORTS = [
    ("kernel/block/gpt.nova", "kernel/block/gpt.c", "The GUID Partition Table (gpt.h)"),
]
RUNTIME = [("nova/rt.h", "kernel/include/nova/rt.h"), ("nova_rt_core.c", "kernel/nova/nova_rt_core.c")]

BANNER = """/*
 * {name} - {title}, generated from {source}.
 *
 * GENERATED: do not edit. Change {source} and run `make nova`
 * (scripts/nova-gen.py), which compiles it with the pinned novac. It needs
 * Nova's runtime (kernel/include/nova/rt.h, kernel/nova/nova_rt_core.c, also
 * generated) and a nova_panic for the environment it is linked into
 * (kernel/nova/nova_panic.c in the kernel).
 */
"""


def novac() -> str:
    exe = os.environ.get("NOVAC") or shutil.which("novac")
    if exe is None:
        sys.exit(f"nova-gen: no novac: set NOVAC or put novac {NOVAC_VERSION} on PATH (see the usage in {__file__})")
    out = subprocess.run([exe, "--version"], capture_output=True, text=True).stdout.strip()
    if out != f"novac {NOVAC_VERSION}":
        sys.exit(f"nova-gen: {exe} is '{out}', not the pinned novac {NOVAC_VERSION}")
    return exe


def generated(exe: str, work: Path) -> dict[str, str]:
    """Every generated file, by its path in the tree, as the pinned novac makes it now."""
    files: dict[str, str] = {}
    for i, (source, target, title) in enumerate(PORTS):
        out = work / str(i)
        subprocess.run([exe, "build", "--profile", "freestanding", "--no-float", "--emit", "c", "-o", out, source],
                       cwd=ROOT, check=True)
        main = (out / "main.c").read_text()
        files[target] = BANNER.format(name=Path(target).name, title=title, source=source) + main
        for made, path in RUNTIME:
            text = (out / made).read_text()
            if files.setdefault(path, text) != text:
                sys.exit(f"nova-gen: {source}'s {path} differs from the other ports'")
    return files


def main() -> int:
    parser = argparse.ArgumentParser(description="Generate the C of CosmoOS's Nova sources.")
    parser.add_argument("--check", action="store_true", help="compare with the checked-in files instead of writing")
    parser.add_argument("--pinned", action="store_true", help="print the pinned novac version and exit")
    args = parser.parse_args()
    if args.pinned:
        print(NOVAC_VERSION)
        return 0
    exe = novac()
    with tempfile.TemporaryDirectory() as temp:
        files = generated(exe, Path(temp))
    stale = []
    for path, text in files.items():
        target = ROOT / path
        if target.exists() and target.read_text() == text:
            continue
        stale.append(path)
        if not args.check:
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_text(text)
    for path in stale:
        print(f"nova-gen: {'differs from its Nova source' if args.check else 'wrote'}: {path}")
    if not stale:
        print(f"nova-gen: {len(files)} generated files match their Nova sources (novac {NOVAC_VERSION})")
    elif args.check:
        print("nova-gen: run `make nova` with the pinned novac and commit the result")
    return 1 if args.check and stale else 0


if __name__ == "__main__":
    sys.exit(main())
