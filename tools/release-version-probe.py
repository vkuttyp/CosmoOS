#!/usr/bin/env python3
"""Prove a release build names its tag (docs/audit/2026-10-18-release-version-report.md).

    release-version-probe.py [--old] [--arch x86_64|aarch64] [--tag vX.Y.Z[-rcN]]

The release workflow's checkout leaves the pushed annotated tag as a
lightweight tag (actions/checkout fetches it by its commit), and `git
describe` -- the build id -- skips it: v1.0.4 was built as
v1.0.3-123-g1ec37307. This makes the same clone: the tag's commit checked
out, the tag re-created lightweight. Fixed mode then runs
scripts/release-tag.sh TAG, as the workflow now does, which fetches the
annotated tag from origin (this repository); --old does not, as the
workflow did not. Either way it runs `make release` for one architecture
and scripts/check-release-version.py on the set.

The tag (default v1.0.5-rc1) must be annotated in this repository and its
tree must have both scripts. Logs stay under
out/release-version-probe/<arch>-<mode>. Exit 0 when the mode shows what it
should (fixed: the check passes, the kernel says the tag; old: the check
fails naming another build id), else 1.
"""
import argparse
from pathlib import Path
import re
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def git(cwd, *args):
    return subprocess.run(["git", "-C", str(cwd), *args], check=True, capture_output=True, text=True).stdout.strip()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--old", action="store_true")
    ap.add_argument("--arch", choices=("x86_64", "aarch64"), default="x86_64")
    ap.add_argument("--tag", default="v1.0.5-rc1")
    a = ap.parse_args()
    if git(ROOT, "cat-file", "-t", f"refs/tags/{a.tag}") != "tag":
        sys.exit(f"{a.tag} is not an annotated tag in {ROOT}")
    mode = "old" if a.old else "fixed"
    work = ROOT / "out" / "release-version-probe" / f"{a.arch}-{mode}"
    work.mkdir(parents=True, exist_ok=True)
    if any(work.iterdir()):
        sys.exit(f"refusing to overwrite nonempty probe output {work}")
    clone = Path(tempfile.mkdtemp(prefix=f"rvp-{a.arch[:3]}-", dir=ROOT / "out"))
    try:
        subprocess.run(["git", "clone", "-q", "--no-checkout", "--no-tags", str(ROOT), str(clone)], check=True)
        commit = git(ROOT, "rev-parse", f"{a.tag}^{{commit}}")
        git(clone, "fetch", "-q", "origin", commit)
        git(clone, "checkout", "-q", "--detach", commit)
        # What actions/checkout leaves: the tag as the bare commit, and the
        # older tags as they are (annotated) for git describe to fall back on.
        git(clone, "fetch", "-q", "origin", "refs/tags/v1*:refs/tags/v1*")
        git(clone, "tag", "-f", a.tag, commit)
        before = git(clone, "describe", "--always", "--dirty")
        lines = [f"describe after the checkout: {before} ({git(clone, 'cat-file', '-t', f'refs/tags/{a.tag}')})"]
        if not a.old:
            r = subprocess.run(["sh", "scripts/release-tag.sh", a.tag], cwd=clone, capture_output=True, text=True)
            lines.append(f"release-tag.sh: rc {r.returncode}: {(r.stdout + r.stderr).strip()}")
            if r.returncode != 0:
                (work / "probe.txt").write_text("\n".join(lines) + "\n")
                print(f"PROBE: FAIL arch={a.arch} mode={mode}: release-tag.sh failed ({work})")
                return 1
        make = "gmake" if sys.platform == "darwin" else "make"
        with (work / "make.log").open("w") as log:
            rc = subprocess.run([make, "-C", str(clone), "-j8", f"ARCH={a.arch}",
                                 f"BB_DIR={ROOT / 'out' / f'busybox-{a.arch}'}",
                                 f"PORTS_CACHE={ROOT / '.cache' / 'ports'}", "release"],
                                stdin=subprocess.DEVNULL, stdout=log, stderr=subprocess.STDOUT,
                                timeout=2400).returncode
        if rc != 0:
            print(f"PROBE: FAIL arch={a.arch} mode={mode}: make release failed ({work}/make.log)")
            return 1
        r = subprocess.run([sys.executable, "scripts/check-release-version.py", "--tag", a.tag,
                            f"out/release/{a.arch}"], cwd=clone, capture_output=True, text=True)
        lines.append(f"check-release-version: rc {r.returncode}: {(r.stdout + r.stderr).strip()}")
        (work / "probe.txt").write_text("\n".join(lines) + "\n")
        if a.old:
            m = re.search(r"build id (\S+), not " + re.escape(a.tag), r.stderr)
            ok = r.returncode == 1 and m is not None and m.group(1) == before
            seen = f"the kernel says {m.group(1)}" if m else "no build-id mismatch"
        else:
            ok = r.returncode == 0
            seen = "the set says the tag" if ok else "the check failed"
        print(f"PROBE: {'PASS' if ok else 'FAIL'} arch={a.arch} mode={mode}: {seen} ({work})")
        return 0 if ok else 1
    finally:
        subprocess.run(["rm", "-rf", str(clone)])


if __name__ == "__main__":
    sys.exit(main())
