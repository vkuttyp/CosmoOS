#!/usr/bin/env python3
"""dist.py -- a source tarball that builds with no CeeSafe compiler (`make dist`; CeeSafe's ADR 0023, roadmap 2.0's CeeSafe
rule, point 7).

The tree holds the kernel's .cee sources and not the C made from them: the build runs the pinned ceesafec
(build/cee.mk). A self-hosted CosmoOS has no ceesafec, so it builds from this tarball, which carries the generated C
beside each source, as the pinned ceesafec makes it now (scripts/cee-gen.py), and two files the tree does not:

  .cee-dist   tells the build its generated C is in the tree (build/cee.mk: no ceesafec is run);
  .build-id    the commit's `git describe --always --dirty`, which the build would otherwise ask git for.

The files are the working tree's, tracked and untracked but not ignored, so a tarball of a tree with changes matches
the build of that tree; `make reproducible` builds both and compares every output (scripts/check-reproducible.sh).
The tarball is deterministic: sorted, owner 0, one modification time.

    python3 scripts/dist.py                  # out/dist/cosmoos-<build id>-src.tar.gz
    python3 scripts/dist.py --dir DIR        # the same tree, unpacked into DIR (no tarball)
"""

from __future__ import annotations

import argparse
import io
import os
import shutil
import subprocess
import sys
import tarfile
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
MTIME = 0   # every entry's modification time: the tarball is a function of its files


def build_id() -> str:
    return subprocess.run(["git", "describe", "--always", "--dirty"], cwd=ROOT, check=True, capture_output=True,
                          text=True).stdout.strip()


def tree_files() -> list[str]:
    out = subprocess.run(["git", "ls-files", "-z", "--cached", "--others", "--exclude-standard"], cwd=ROOT,
                         check=True, capture_output=True).stdout
    return sorted(p for p in set(out.decode().split("\0")) if p and (ROOT / p).is_file())


def stage(into: Path, bid: str):
    for rel in tree_files():
        target = into / rel
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(ROOT / rel, target)
    subprocess.run([sys.executable, str(ROOT / "scripts" / "cee-gen.py"), "--out", str(into), "--no-negative"],
                   cwd=ROOT, check=True)
    (into / ".cee-dist").write_text("The generated C of every .cee source is beside it: this tree builds with no "
                                     "ceesafec (scripts/dist.py).\n")
    (into / ".build-id").write_text(bid + "\n")


def pack(staged: Path, name: str, tarball: Path):
    tarball.parent.mkdir(parents=True, exist_ok=True)
    files = sorted(p for p in staged.rglob("*") if p.is_file())
    raw = io.BytesIO()
    with tarfile.open(fileobj=raw, mode="w", format=tarfile.PAX_FORMAT) as tar:
        for path in files:
            info = tarfile.TarInfo(f"{name}/{path.relative_to(staged).as_posix()}")
            data = path.read_bytes()
            info.size = len(data)
            info.mtime = MTIME
            info.mode = 0o755 if os.access(path, os.X_OK) else 0o644
            info.uid = info.gid = 0
            info.uname = info.gname = ""
            tar.addfile(info, io.BytesIO(data))
    import gzip
    with open(tarball, "wb") as f, gzip.GzipFile(fileobj=f, mode="wb", mtime=MTIME, filename="") as gz:
        gz.write(raw.getvalue())


def main() -> int:
    parser = argparse.ArgumentParser(description="A source tarball that builds with no CeeSafe compiler.")
    parser.add_argument("--out", default="out/dist", help="where the tarball goes (default out/dist)")
    parser.add_argument("--dir", help="unpack the tree into this directory instead of writing a tarball")
    args = parser.parse_args()
    bid = build_id()
    name = f"cosmoos-{bid}"
    if args.dir:
        into = Path(args.dir).resolve()
        if into.exists():
            shutil.rmtree(into)
        into.mkdir(parents=True)
        stage(into, bid)
        print(f"dist: {into} ({name}, the generated C of {len(cee_outputs())} files beside its sources)")
        return 0
    with tempfile.TemporaryDirectory() as temp:
        staged = Path(temp) / name
        staged.mkdir()
        stage(staged, bid)
        tarball = (ROOT / args.out / f"{name}-src.tar.gz").resolve()
        pack(staged, name, tarball)
    print(f"dist: {tarball.relative_to(ROOT) if tarball.is_relative_to(ROOT) else tarball}")
    return 0


def cee_outputs() -> list[str]:
    out = subprocess.run([sys.executable, str(ROOT / "scripts" / "cee-gen.py"), "--list"], cwd=ROOT, check=True,
                         capture_output=True, text=True).stdout
    return out.split()


if __name__ == "__main__":
    sys.exit(main())
