#!/usr/bin/env python3
"""Prove an installed system's /tmp is everyone's (01777).

    tmp-mode-probe.py [--old] [--arch x86_64|aarch64] [--tree REV] [--fix REV]

make test-install's reboot stage boots the installed disk and requires
`stat -c '%a %u' /tmp` to print "1777 0" and uid 1000 (`priv -u 1000 -g
1000 touch /tmp/u1000`) to write there. Before the fix init mounted a
ramfs on /tmp after switching to the disk root and left that ramfs's root
at mode 0755, so no uid but 0 could write in /tmp (inventory section 18,
found in P6's review).

--old reverse-applies the fix commit's userland/etc/rc (its `chmod 1777
/tmp`, and nothing else) in a throwaway worktree, so the write
is refused and the reboot stage fails. The fix commit is found by its
subject, or named with --fix. The check runs in a release test-install on
either architecture.

Logs stay under out/tmp-mode-probe/<arch>-<mode>. Exit 0 when the mode
shows what it should (fixed: test-install PASS; old: the reboot stage
failing on the write in /tmp), else 1.
"""
import argparse
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SUBJECT = "init: an installed system's /tmp is 01777"
SOURCES = ["userland/etc/rc"]
OK = "test-install: PASS"
FAILED = "test-install: FAIL: uid 1000 writing in the installed system's /tmp: no line matching '^tmp=0$'"


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--old", action="store_true")
    ap.add_argument("--arch", choices=("x86_64", "aarch64"), default="x86_64")
    ap.add_argument("--tree", default="HEAD")
    ap.add_argument("--fix", help="the commit that made /tmp 01777 (default: found by its subject)")
    a = ap.parse_args()
    mode = "old" if a.old else "fixed"
    work = ROOT / "out" / "tmp-mode-probe" / f"{a.arch}-{mode}"
    work.mkdir(parents=True, exist_ok=True)
    if any(work.iterdir()):
        sys.exit(f"refusing to overwrite nonempty probe output {work}")
    # A short path: the boot's sockets live under its output directory, and
    # a unix socket's path is at most 104 bytes on macOS.
    tree = Path(tempfile.mkdtemp(prefix=f"tmp-{a.arch[:3]}-{mode}-", dir=ROOT / "out"))
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
        args = [make, "-C", str(tree), "-j8", f"ARCH={a.arch}", "BUILD=release", f"OUT={out}",
                # BusyBox and the pinned sources are the same in either mode.
                f"BB_DIR={ROOT / 'out' / f'busybox-{a.arch}'}", f"PORTS_CACHE={ROOT / '.cache' / 'ports'}"]
        log_path = work / "make.log"
        with log_path.open("w") as log:
            subprocess.run(args + ["test-install"], stdin=subprocess.DEVNULL, stdout=log, stderr=subprocess.STDOUT,
                           timeout=1800)
        text = log_path.read_text(errors="replace")
        if a.old:
            ok = FAILED in text
            seen = "uid 1000 could not write in /tmp (the reboot stage failed)" if ok else "no such failure"
        else:
            ok = OK in text
            seen = "/tmp is 1777 and uid 1000 wrote there (test-install PASS)" if ok else "test-install did not pass"
        print(f"PROBE: {'PASS' if ok else 'FAIL'} arch={a.arch} mode={mode}: {seen} ({work})")
        return 0 if ok else 1
    finally:
        subprocess.run(["git", "-C", str(ROOT), "worktree", "remove", "--force", str(tree)])


if __name__ == "__main__":
    sys.exit(main())
