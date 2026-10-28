#!/usr/bin/env python3
"""Prove ps-nice reads the nice value only once the process is sleep.

    ps-nice-probe.py [--old] [--arch x86_64|aarch64] [--tree REV] [--fix REV]

tests/busybox/ash.sh starts `nice -n 7 sleep 64 &` and reads $!'s nice
value with ps. $! is the forked shell: it is at nice 0 until it has run
nice, which sets the value and execs sleep. CI run 40703059019 (main's
merge of #477, busybox test aarch64) had ps there first: "FAIL ps-nice:
want [5315 7] got [5315 0]".

The adversary widens that window by the mechanism, not by load: in both
modes the background command becomes `{ sleep 0.3; exec nice -n 7 sleep
64; }`, the same process held as the shell for 0.3 s before it runs nice.
With the fix the script waits (up to 1 s) until ps names the process
sleep, and every check passes; --old reverse-applies the fix commit's
change to ash.sh in a throwaway worktree, and ps-nice fails with nice 0.
The check runs in a release `make test-busybox` (as CI runs it) on either
architecture.

Logs stay under out/ps-nice-probe/<arch>-<mode>. Exit 0 when the mode
shows what it should (fixed: test-busybox passes; old: ps-nice is the
only check that fails, with nice 0), else 1.
"""
import argparse
from pathlib import Path
import re
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SUBJECT = "tests/busybox: ps-nice waits for the exec"
SCRIPT = "tests/busybox/ash.sh"
LAUNCH = "nice -n 7 sleep 64 &\n"
ADVERSARY = "{ sleep 0.3; exec nice -n 7 sleep 64; } &\n"
# The harness's verdict joins the scripted test's failure lines with "; ".
VERDICT = re.compile(r"^test-busybox: FAIL: (?:dynamic )?scripted test: (.*) \(log in .*\)$", re.M)
CHECK = re.compile(r"^BBTEST: FAIL (\S+): (.*)$")
NICE0 = re.compile(r"^want \[(\d+) 7\] got \[\1 0\]$")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--old", action="store_true")
    ap.add_argument("--arch", choices=("x86_64", "aarch64"), default="x86_64")
    ap.add_argument("--tree", default="HEAD")
    ap.add_argument("--fix", help="the fix commit (default: found by its subject)")
    a = ap.parse_args()
    mode = "old" if a.old else "fixed"
    work = ROOT / "out" / "ps-nice-probe" / f"{a.arch}-{mode}"
    work.mkdir(parents=True, exist_ok=True)
    if any(work.iterdir()):
        sys.exit(f"refusing to overwrite nonempty probe output {work}")
    # A short path: the boot's sockets live under its output directory, and
    # a unix socket's path is at most 104 bytes on macOS.
    tree = Path(tempfile.mkdtemp(prefix=f"pn-{a.arch[:3]}-{mode}-", dir=ROOT / "out"))
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
            patch = subprocess.check_output(["git", "-C", str(ROOT), "show", "--format=", sha, "--", SCRIPT])
            if not patch:
                sys.exit(f"commit {sha[:8]} does not change {SCRIPT}")
            subprocess.run(["git", "-C", str(tree), "apply", "-R"], input=patch, check=True)
        script = tree / SCRIPT
        text = script.read_text()
        if text.count(LAUNCH) != 1:
            sys.exit(f"{SCRIPT} does not start '{LAUNCH.strip()}' exactly once")
        script.write_text(text.replace(LAUNCH, ADVERSARY))
        make = "gmake" if sys.platform == "darwin" else "make"
        out = tree / "out" / "rel"
        args = [make, "-C", str(tree), "-j8", f"ARCH={a.arch}", "BUILD=release", f"OUT={out}",
                # BusyBox and the pinned sources are the same in either mode.
                f"BB_DIR={ROOT / 'out' / f'busybox-{a.arch}'}", f"PORTS_CACHE={ROOT / '.cache' / 'ports'}"]
        with (work / "make.log").open("w") as log:
            rc = subprocess.run(args + ["test-busybox"], stdin=subprocess.DEVNULL, stdout=log,
                                stderr=subprocess.STDOUT, timeout=2400).returncode
        log = (work / "make.log").read_text(errors="replace")
        v = VERDICT.search(log)
        failed = [m.groups() for m in map(CHECK.match, v.group(1).split("; ")) if m] if v else []
        names = sorted({n for n, _ in failed})
        if a.old:
            ok = rc != 0 and v is not None and names == ["ps-nice"] and all(NICE0.match(w) for _, w in failed)
            seen = (f"ps-nice read nice 0 ({len(failed)} failure lines)" if ok
                    else f"exit {rc}; failed: {failed}")
        else:
            ok = rc == 0 and "test-busybox: PASS" in log and not failed
            seen = "test-busybox passes" if ok else f"exit {rc}; failed: {failed}"
        print(f"PROBE: {'PASS' if ok else 'FAIL'} arch={a.arch} mode={mode}: {seen} ({work})")
        return 0 if ok else 1
    finally:
        subprocess.run(["git", "-C", str(ROOT), "worktree", "remove", "--force", str(tree)])


if __name__ == "__main__":
    sys.exit(main())
