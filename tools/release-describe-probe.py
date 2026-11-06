#!/usr/bin/env python3
"""Prove a release kernel names no build id but its own.

    release-describe-probe.py [--old] [--arch x86_64|aarch64] [--tree REV]

The release workflow requires every git-describe-shaped string in
cosmoos-ARCH-kernel.elf to be the tag (scripts/check-release-version.py).
The cosmofs format-12 message said "(v1.0.7 and older)", so the kernel
held two such strings and v1.0.8's first tag failed at "The set says the
tag", in all four builds, after main had passed CI: nothing before a tag
ran the check.

This builds the release kernel of REV (default HEAD) in a throwaway
worktree and runs check-release-version.py --kernel on it against the
worktree's own `git describe --always --dirty`. --old puts the message's
old words back in cosmofs_snap.cee, by their text: the snapshots are
CeeSafe now (roadmap 2.0 SA7), so the fix commit's C diff no longer
applies. The message then says v1.0.7 again.

Logs stay under out/release-describe-probe/<arch>-<mode>. Exit 0 when the
mode shows what it should (fixed: the check passes; old: it fails naming
v1.0.7 and only v1.0.7), else 1.
"""
import argparse
from pathlib import Path
import re
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
# The fixed words and the old ones, in the port (cosmofs_snap.cee).
SOURCE = "kernel-services/filesystem/cosmofs/cosmofs_snap.cee"
FIXED_TEXT = "(CosmoOS 1.0.7 and older)"
OLD_TEXT = "(v1.0.7 and older)"
OTHER = re.compile(r"describe-shaped (.+), not its build id")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--old", action="store_true")
    ap.add_argument("--arch", choices=("x86_64", "aarch64"), default="x86_64")
    ap.add_argument("--tree", default="HEAD")
    a = ap.parse_args()
    mode = "old" if a.old else "fixed"
    work = ROOT / "out" / "release-describe-probe" / f"{a.arch}-{mode}"
    work.mkdir(parents=True, exist_ok=True)
    if any(work.iterdir()):
        sys.exit(f"refusing to overwrite nonempty probe output {work}")
    tree = Path(tempfile.mkdtemp(prefix=f"rdp-{a.arch[:3]}-{mode}-", dir=ROOT / "out"))
    tree.rmdir()
    subprocess.run(["git", "-C", str(ROOT), "worktree", "add", "--detach", str(tree), a.tree], check=True,
                   stdout=subprocess.DEVNULL)
    try:
        if a.old:
            src = tree / SOURCE
            body = src.read_text()
            if body.count(FIXED_TEXT) != 1:
                sys.exit(f"{SOURCE}: the fixed text is not there exactly once; the probe is stale")
            src.write_text(body.replace(FIXED_TEXT, OLD_TEXT))
        make = "gmake" if sys.platform == "darwin" else "make"
        out = tree / "out" / "rel"
        with (work / "make.log").open("w") as log:
            rc = subprocess.run([make, "-C", str(tree), "-j8", f"ARCH={a.arch}", "BUILD=release", f"OUT={out}",
                                 "kernel"], stdin=subprocess.DEVNULL, stdout=log, stderr=subprocess.STDOUT,
                                timeout=2400).returncode
        if rc != 0:
            print(f"PROBE: FAIL arch={a.arch} mode={mode}: make kernel failed ({work}/make.log)")
            return 1
        build_id = subprocess.check_output(["git", "-C", str(tree), "describe", "--always", "--dirty"],
                                           text=True).strip()
        r = subprocess.run([sys.executable, str(ROOT / "scripts" / "check-release-version.py"), "--kernel",
                            str(out / "kernel" / "kernel.elf"), "--build-id", build_id],
                           capture_output=True, text=True)
        (work / "probe.txt").write_text(f"build id {build_id}\ncheck rc {r.returncode}: {(r.stdout + r.stderr).strip()}\n")
        m = OTHER.search(r.stderr)
        if a.old:
            ok = r.returncode == 1 and m is not None and m.group(1) == "v1.0.7"
            seen = f"the kernel also says {m.group(1)}" if m else "no other build id"
        else:
            ok = r.returncode == 0
            seen = f"only {build_id}" if ok else f"the check failed ({m.group(1) if m else r.stderr.strip()})"
        print(f"PROBE: {'PASS' if ok else 'FAIL'} arch={a.arch} mode={mode}: {seen} ({work})")
        return 0 if ok else 1
    finally:
        subprocess.run(["git", "-C", str(ROOT), "worktree", "remove", "--force", str(tree)])


if __name__ == "__main__":
    sys.exit(main())
