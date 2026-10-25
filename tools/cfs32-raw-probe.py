#!/usr/bin/env python3
"""Prove a compressible file written in order is not stopped by the run
bound: past three quarters of it, new records are written plain and
merge (CFS-32's write policy; cosmofs design.md, "Plain past three
quarters of the bound").

    cfs32-raw-probe.py [--old] [--arch x86_64|aarch64] [--tree REV] [--fix REV]

The probe runs CFS-32's acceptance test, `make BUILD=release test-large`
(tests/boot/large_test.py): the published v1.0.13 upgrades itself to the
tree's build, which writes a 1 GiB compressible file chunk by chunk, each
chunk's dd with an fsync. It needs v1.0.13's release set (fetched, as the
target does; GH_TOKEN where the network asks for one).

  fixed  test-large passes: the 1 GiB file written whole, both pools
         clean, and v1.0.13's own kernel reads it back.
  --old  the policy commit reverse-applied whole in a throwaway worktree:
         a compressed record is a run of its own, so the file meets the
         bound and the write is refused at chunk 37 or so (1a's -EFBIG,
         CF33) with no dirty page lost; the test fails on "every chunk of
         the 1 GiB file".

The self-test cosmofs-extent-raw does not stand in for this: under its
seam bound of 64 runs, the write's charge (CF33) cuts records into plain
blocks that merge from about the same point as the policy's mark, so it
passes without the policy too. Logs stay under
out/cfs32-raw-probe/<arch>-<mode>. Exit 0 when the mode shows what it
should, else 1.
"""
import argparse
from pathlib import Path
import re
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SUBJECT = "cosmofs: past three quarters of the run bound, new records written plain so they merge (CFS-32)"
REFUSED = "every chunk of the 1 GiB file"
# CFS-32's loss is -EFBIG's. A fill to the pool's end loses its last
# chunk's pages with -ENOSPC until MEM-9's charge, a separate defect:
# that is reported, not counted.
LOST = re.compile(r"^.*dirty page\(s\) of inode \d+ on \S+ lost: write-back failed \(-27\).*$", re.M)
NOSPACE = re.compile(r"^.*dirty page\(s\) of inode \d+ on \S+ lost: write-back failed \(-28\).*$", re.M)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--old", action="store_true")
    ap.add_argument("--arch", choices=("x86_64", "aarch64"), default="x86_64")
    ap.add_argument("--tree", default="HEAD")
    ap.add_argument("--fix", help="the fix commit (default: found by its subject)")
    a = ap.parse_args()
    mode = "old" if a.old else "fixed"
    work = ROOT / "out" / "cfs32-raw-probe" / f"{a.arch}-{mode}"
    work.mkdir(parents=True, exist_ok=True)
    if any(work.iterdir()):
        sys.exit(f"refusing to overwrite nonempty probe output {work}")
    # A short path: the boot's sockets live under its output directory.
    tree = Path(tempfile.mkdtemp(prefix=f"c32r-{a.arch[:3]}-{mode}-", dir=ROOT / "out"))
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
            # The whole commit, not chosen hunks.
            # Three-way, so later commits beside its hunks do not stop it. A
            # conflict only in docs/ (later sections beside the fix's) keeps
            # the tree's text, since no document reaches the build; one in
            # code stops the probe.
            if subprocess.run(["git", "-C", str(tree), "revert", "--no-commit", sha],
                              stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode:
                clash = subprocess.check_output(["git", "-C", str(tree), "diff", "--name-only", "--diff-filter=U"],
                                                text=True).split()
                code = [p for p in clash if not p.startswith("docs/")]
                if code:
                    sys.exit(f"the revert of {sha} conflicts in code: {' '.join(code)}")
                subprocess.run(["git", "-C", str(tree), "checkout", "HEAD", "--", *clash], check=True)
        make = "gmake" if sys.platform == "darwin" else "make"
        out = tree / "out" / "o"
        args = [make, "-C", str(tree), "-j8", f"ARCH={a.arch}", "BUILD=release", f"OUT={out}",
                f"BB_DIR={ROOT / 'out' / f'busybox-{a.arch}'}", f"PORTS_CACHE={ROOT / '.cache' / 'ports'}",
                "test-large"]
        with (work / "make.log").open("w") as log:
            rc = subprocess.run(args, stdin=subprocess.DEVNULL, stdout=log, stderr=subprocess.STDOUT,
                                timeout=7200, start_new_session=True).returncode
        text = (work / "make.log").read_text(errors="replace")
        verdict = re.search(r"^test-large: (PASS|FAIL).*$", text, re.M)
        logs = out / "test-large"
        for name in ("boot-write.log", "boot-remount.log", "boot-old.log"):
            if (logs / name).exists():
                subprocess.run(["cp", str(logs / name), str(work)], check=False)
        guest = "".join((work / n).read_text(errors="replace") for n in ("boot-write.log",) if (work / n).exists())
        lost = LOST.search(guest)
        nospace = NOSPACE.findall(guest)
        note = f" (MEM-9's, not counted: {'; '.join(x.strip() for x in nospace)})" if nospace else ""
        if a.old:
            wrote = re.findall(r"^wrote=(\d+)\s*$", guest, re.M)
            ok = (rc != 0 and verdict is not None and verdict.group(1) == "FAIL" and REFUSED in verdict.group(0)
                  and lost is None and wrote)
            seen = (f"{verdict.group(0)}; {int(wrote[-1]) * 4} MiB of the 1 GiB file taken, no page lost" if ok else
                    f"no such failure (make exit {rc}; {verdict.group(0) if verdict else 'no verdict'}; "
                    f"lost: {lost.group(0).strip() if lost else 'none'})")
        else:
            ok = rc == 0 and verdict is not None and verdict.group(1) == "PASS" and lost is None
            lines = re.findall(r"^test-large: (?:write|old): .*$", text, re.M)
            seen = ("; ".join(lines) + f"; {verdict.group(0)}" if ok else
                    f"make exit {rc}; {verdict.group(0) if verdict else 'no verdict'}; "
                    f"lost: {lost.group(0).strip() if lost else 'none'}")
        print(f"PROBE: {'PASS' if ok else 'FAIL'} arch={a.arch} mode={mode}: {seen}{note} ({work})")
        return 0 if ok else 1
    finally:
        subprocess.run(["git", "-C", str(ROOT), "worktree", "remove", "--force", str(tree)])


if __name__ == "__main__":
    sys.exit(main())
