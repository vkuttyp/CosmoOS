#!/usr/bin/env python3
"""Prove a release kernel names no build id but its own.

    release-describe-probe.py [--old] [--arch x86_64|aarch64] [--tree REV] [--fix REV]

The release workflow requires every git-describe-shaped string in
cosmoos-ARCH-kernel.elf to be the tag (scripts/check-release-version.py).
The cosmofs format-12 message said "(v1.0.7 and older)", so the kernel
held two such strings and v1.0.8's first tag failed at "The set says the
tag", in all four builds, after main had passed CI: nothing before a tag
ran the check.

This builds the release kernel of REV (default HEAD) in a throwaway
worktree and runs check-release-version.py --kernel on it against the
worktree's own `git describe --always --dirty`. --old reverse-applies the
fix commit's change to cosmofs_snap.c (the whole of it, nothing else), and
the message says v1.0.7 again.

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
SUBJECT = "cosmofs: the format-12 message names no other build id"
SOURCES = ["kernel-services/filesystem/cosmofs/cosmofs_snap.c"]
OTHER = re.compile(r"describe-shaped (.+), not its build id")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--old", action="store_true")
    ap.add_argument("--arch", choices=("x86_64", "aarch64"), default="x86_64")
    ap.add_argument("--tree", default="HEAD")
    ap.add_argument("--fix", help="the commit that rewords the message (default: found by its subject)")
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
            sha = a.fix or subprocess.check_output(
                ["git", "-C", str(ROOT), "log", "-1", "--format=%H", "--fixed-strings", f"--grep={SUBJECT}", a.tree],
                text=True).strip()
            if not sha:
                sys.exit(f"no commit '{SUBJECT}' reachable from {a.tree}; name it with --fix")
            patch = subprocess.check_output(["git", "-C", str(ROOT), "show", "--format=", sha, "--"] + SOURCES)
            if not patch:
                sys.exit(f"commit {sha[:8]} does not change {SOURCES}")
            subprocess.run(["git", "-C", str(tree), "apply", "-R"], input=patch, check=True)
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
