#!/usr/bin/env python3
"""Prove the Linux personality's writev fix.

    writev-probe.py [--old] [--arch x86_64|aarch64] [--tree REV] [--fix REV]

The Linux personality's writev made one write per iovec, where Linux
makes the vector one write. musl's line-buffered stdout flushes a line
as two iovecs, so a kernel log line could land inside it on the console
(test-pkg-net's "serial: 11" check failed on main 08f97a91, run
39315782811), and a datagram socket sent one datagram per iovec. lxtest
now writes two iovecs to an AF_UNIX datagram socketpair and requires one
12-byte datagram.

--old reverse-applies the fix commit's change to compat/linux/syscalls.c
and nothing else (the lxtest check stays), in a throwaway worktree, so
the check fails. The commit is found by its subject, or named with
--fix. The check runs in the debug boot (`make test`, where lxtest runs)
on either architecture.

Logs stay under out/writev-probe/<arch>-<mode>. Exit 0 when the mode
shows what it should (fixed: the boot PASSes; old: the check fails), else
1.
"""
import argparse
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SUBJECT = "linux: writev is one write"
SOURCES = ["compat/linux/syscalls.c"]
OK = "boot-test: PASS"
RAN = "LINUXTEST: PASS"
FAILED = 'LINUXTEST: FAIL dn == 12 && memeq(buf, "second line\\n", 12) (7)'


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--old", action="store_true")
    ap.add_argument("--arch", choices=("x86_64", "aarch64"), default="x86_64")
    ap.add_argument("--tree", default="HEAD")
    ap.add_argument("--fix", help="the fix commit (default: found by subject)")
    a = ap.parse_args()
    mode = "old" if a.old else "fixed"
    work = ROOT / "out" / "writev-probe" / f"{a.arch}-{mode}"
    work.mkdir(parents=True, exist_ok=True)
    if any(work.iterdir()):
        sys.exit(f"refusing to overwrite nonempty probe output {work}")
    # A short path: the boot's sockets live under its output directory, and
    # a unix socket's path is at most 104 bytes on macOS.
    tree = Path(tempfile.mkdtemp(prefix=f"wv-{a.arch[:3]}-{mode}-", dir=ROOT / "out"))
    tree.rmdir()
    subprocess.run(["git", "-C", str(ROOT), "worktree", "add", "--detach", str(tree), a.tree], check=True,
                   stdout=subprocess.DEVNULL)
    try:
        if a.old:
            sha = a.fix or subprocess.check_output(
                ["git", "-C", str(ROOT), "log", "-1", "--format=%H", "--fixed-strings", f"--grep={SUBJECT}",
                 a.tree], text=True).strip()
            if not sha:
                sys.exit(f"no commit '{SUBJECT}' reachable from {a.tree}; name it with --fix")
            patch = subprocess.check_output(["git", "-C", str(ROOT), "show", "--format=", sha, "--"] + SOURCES)
            if not patch:
                sys.exit(f"commit {sha[:8]} does not change {SOURCES}")
            subprocess.run(["git", "-C", str(tree), "apply", "-R"], input=patch, check=True)
        make = "gmake" if sys.platform == "darwin" else "make"
        out = tree / "out" / "dbg"
        # The debug build, named: lxtest runs only with the self-tests, and a
        # BUILD=release inherited from the environment would pass with
        # the check never run. The fixed mode also needs lxtest's line.
        args = [make, "-C", str(tree), "-j8", f"ARCH={a.arch}", "BUILD=debug", f"OUT={out}",
                f"PORTS_CACHE={ROOT / '.cache' / 'ports'}", "test"]
        log_path = work / "make.log"
        with log_path.open("w") as log:
            rc = subprocess.run(args, stdin=subprocess.DEVNULL, stdout=log, stderr=subprocess.STDOUT,
                                timeout=3600).returncode
        text = log_path.read_text(errors="replace")
        for f in out.glob("boot-test*.log"):
            (work / f.name).write_text(f.read_text(errors="replace"))
            text += f.read_text(errors="replace")
        # Fixed: the boot's own verdict and exit status both. Old: the
        # check's failure (the boot then exits non-zero too).
        ok = (rc != 0 and FAILED in text) if a.old else (rc == 0 and OK in text and RAN in text)
        seen = ("the check failed as the defect predicts" if a.old else "the boot passed") if ok else \
            "not what the mode predicts"
        print(f"PROBE: {'PASS' if ok else 'FAIL'} arch={a.arch} mode={mode}: {seen} ({work})")
        return 0 if ok else 1
    finally:
        subprocess.run(["git", "-C", str(ROOT), "worktree", "remove", "--force", str(tree)])


if __name__ == "__main__":
    sys.exit(main())
