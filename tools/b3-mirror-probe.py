#!/usr/bin/env python3
"""Prove the two defects roadmap 1.2 B3's `make test-mirror` found in the
held devices of a mounted pool (CFS-5, PR #439).

    b3-mirror-probe.py --case NAME --arch ARCH [--old] [--tree REV]

  check  the checker's "another device of this pool is mounted" test
         asked vfs_bdev_mounted, which sees what the checker's own
         detached mount holds: the boot's check of a mirrored root said
         `root not checked: Device or resource busy` and init mounted it
         unchecked
  gone   bdev_related matched a partition to its disk by name: a mount
         still holding the pulled vda2 held the new disk that took the
         name vda, and `cosmo-pool replace 0:1 vda` was refused -EBUSY

--old restores the old behaviour in a throwaway worktree, by the
replacement the case names, and runs `make BUILD=release test-mirror`:
old mode passes when the test fails at the case's step. Fixed mode
passes when the test passes. Logs under
out/b3-mirror-probe/<case>-<arch>-<mode>. Exit 0 when the mode shows what
it should, else 1.
"""
import argparse
from pathlib import Path
import re
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]

# [(file, text, old text)], the test-mirror FAIL line's pattern.
CASES = {
    "check": ([("kernel-services/filesystem/cosmofs/cosmofs_check.c",
                "vfs_bdev_mounted_except(d, mnt)", "vfs_bdev_mounted(d)")],
              r"test-mirror: FAIL: replace: the boot's check of the root"),
    "gone": ([("kernel-services/vfs/vfs.c",
               "    return blk_part_disk(a) == b || blk_part_disk(b) == a;\n",
               "    struct blk_part_info pi;\n"
               "    if (blk_part_info(a, &pi) && strcmp(pi.disk, b->name) == 0)\n"
               "        return true;\n"
               "    return blk_part_info(b, &pi) && strcmp(pi.disk, a->name) == 0;\n")],
             r"test-mirror: FAIL: cosmo-pool replace: no line matching '\^replace-status=0\$'"),
}


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--case", choices=sorted(CASES), required=True)
    ap.add_argument("--arch", choices=("x86_64", "aarch64"), required=True)
    ap.add_argument("--old", action="store_true")
    ap.add_argument("--tree", default="HEAD")
    a = ap.parse_args()
    edits, fail_at = CASES[a.case]
    mode = "old" if a.old else "fixed"
    work = ROOT / "out" / "b3-mirror-probe" / f"{a.case}-{a.arch}-{mode}"
    work.mkdir(parents=True, exist_ok=True)
    if any(work.iterdir()):
        sys.exit(f"refusing to overwrite nonempty probe output {work}")
    tree = Path(tempfile.mkdtemp(prefix=f"b3m-{a.case}-", dir=ROOT / "out"))
    tree.rmdir()
    subprocess.run(["git", "-C", str(ROOT), "worktree", "add", "--detach", str(tree), a.tree], check=True,
                   stdout=subprocess.DEVNULL)
    try:
        if a.old:
            for path, text, old in edits:
                src = tree / path
                body = src.read_text()
                if body.count(text) != 1:
                    sys.exit(f"{path}: the fixed text is not there exactly once; the probe is stale")
                src.write_text(body.replace(text, old))
        make = "gmake" if sys.platform == "darwin" else "make"
        args = [make, "-C", str(tree), "-j8", f"ARCH={a.arch}", "BUILD=release", f"OUT={tree / 'out' / 'rel'}",
                f"BB_DIR={ROOT / 'out' / f'busybox-{a.arch}'}", f"PORTS_CACHE={ROOT / '.cache' / 'ports'}"]
        with (work / "make.log").open("w") as log:
            rc = subprocess.run(args + ["test-mirror"], stdin=subprocess.DEVNULL, stdout=log,
                                stderr=subprocess.STDOUT, timeout=2400).returncode
        text = (work / "make.log").read_text(errors="replace")
        if a.old:
            ok = rc != 0 and re.search(fail_at, text) is not None
            m = re.search(r"test-mirror: FAIL: .*", text)
            seen = (f"`{m.group(0)}`" if m else f"no FAIL line (make exited {rc})")
        else:
            ok = rc == 0 and "test-mirror: PASS" in text
            seen = "test-mirror passed" if ok else f"make exited {rc}"
        print(f"PROBE: {'PASS' if ok else 'FAIL'} case={a.case} arch={a.arch} mode={mode}: {seen} ({work})")
        return 0 if ok else 1
    finally:
        subprocess.run(["git", "-C", str(ROOT), "worktree", "remove", "--force", str(tree)])


if __name__ == "__main__":
    sys.exit(main())
