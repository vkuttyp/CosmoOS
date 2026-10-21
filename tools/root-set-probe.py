#!/usr/bin/env python3
"""Prove an upgrade is not refused on a root's record an older tool left.

    root-set-probe.py [--old] [--arch x86_64|aarch64] [--tree REV] [--fix REV]

v1.0.8's cosmo-upgrade trusted /var/lib/cosmo-upgrade/root-set: an
upgrade was refused whenever it named another set than the current one.
Its --rollback to v1.0.7 wrote it naming v1.0.7's set; v1.0.7's own
cosmo-upgrade, rolling forward again, keeps no such record, so it stayed
behind, and every later upgrade was refused (`the root holds another
set's base system than the current set's (a rollback half done):
cosmo-upgrade --repair first`), with --repair finding no intent and
nothing to do. The fix tells what the root holds from its files and
corrects the record from that.

test-be's forward stage (tests/boot/be_test.py, `make test-be
BE_ONLY=forward`) is that sequence on the published v1.0.7 and this
build's sets: v1.0.7 -> A, A's --rollback, v1.0.7's --rollback forward,
then the upgrade to B on A. With the fix it passes; without it the
upgrade to B exits 3 at that refusal. --old reverse-applies the fix
commit's change to the C sources and userland.mk (the whole commit's
change to them, nothing else) in a throwaway worktree, keeping the test.
The published v1.0.7 comes from this checkout's .cache/releases.

Logs stay under out/root-set-probe/<arch>-<mode>. Exit 0 when the mode
shows what it should (fixed: the stage passes; old: it fails at the
upgrade to B, exit 3, with that refusal), else 1.
"""
import argparse
from pathlib import Path
import re
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SUBJECT = "cosmo-upgrade: the root's record is checked against the root"
SOURCES = ["userland/system/cosmo-upgrade.c", "userland/system/cosmo-install.c", "userland/system/upgrade_be.c",
           "userland/system/upgrade_be.h", "userland/userland.mk"]
PASSED = re.compile(r"^test-be: PASS in ", re.M)
REFUSED = re.compile(r"^test-be: FAIL: 'cosmo-upgrade /tmp/b': exit 3, want 0; output .*the root holds another "
                     r"set's base system than the current set's \(a rollback half done\): cosmo-upgrade --repair "
                     r"first; nothing written", re.M)
REACHED = re.compile(r"^test-be: install: v1\.0\.7 .* installed itself", re.M)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--old", action="store_true")
    ap.add_argument("--arch", choices=("x86_64", "aarch64"), default="x86_64")
    ap.add_argument("--tree", default="HEAD")
    ap.add_argument("--fix", help="the fix commit (default: found by its subject)")
    a = ap.parse_args()
    mode = "old" if a.old else "fixed"
    work = ROOT / "out" / "root-set-probe" / f"{a.arch}-{mode}"
    work.mkdir(parents=True, exist_ok=True)
    if any(work.iterdir()):
        sys.exit(f"refusing to overwrite nonempty probe output {work}")
    # A short path: the boots' sockets live under the output directory.
    tree = Path(tempfile.mkdtemp(prefix=f"rs-{a.arch[:3]}-{mode}-", dir=ROOT / "out"))
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
        args = [make, "-C", str(tree), "-j8", f"ARCH={a.arch}", "BUILD=release", f"OUT={out}", "BE_ONLY=forward",
                f"BE_OLDER_DIR={ROOT / '.cache' / 'releases' / 'v1.0.7'}",
                # BusyBox and the pinned sources are the same in either mode.
                f"BB_DIR={ROOT / 'out' / f'busybox-{a.arch}'}", f"PORTS_CACHE={ROOT / '.cache' / 'ports'}"]
        with (work / "make.log").open("w") as log:
            rc = subprocess.run(args + ["test-be"], stdin=subprocess.DEVNULL, stdout=log, stderr=subprocess.STDOUT,
                                timeout=3600).returncode
        text = (work / "make.log").read_text(errors="replace")
        for f in (out / "test-be" / "forward").glob("*.log"):
            (work / f.name).write_text(f.read_text(errors="replace"))
        if a.old:
            ok = rc != 0 and REACHED.search(text) is not None and REFUSED.search(text) is not None
            seen = ("the upgrade to B refused at the gate on the record v1.0.7 left (exit 3)" if ok else
                    f"no such refusal (make exited {rc})")
        else:
            ok = rc == 0 and PASSED.search(text) is not None
            seen = "the forward stage passed" if ok else f"make test-be exited {rc}"
        print(f"PROBE: {'PASS' if ok else 'FAIL'} arch={a.arch} mode={mode}: {seen} ({work})")
        return 0 if ok else 1
    finally:
        subprocess.run(["git", "-C", str(ROOT), "worktree", "remove", "--force", str(tree)])


if __name__ == "__main__":
    sys.exit(main())
