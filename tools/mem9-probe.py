#!/usr/bin/env python3
"""Prove a write the pool cannot hold is refused at the write, with no
page lost, and that dirty pages are bounded (MEM-9; vfs design.md,
"Dirty pages, bounded"; cosmofs design.md, "Space, charged at the
write"; invariants V40 and CF34).

    mem9-probe.py [--old] [--arch x86_64|aarch64] [--tree REV] [--fix REV]

The probe boots a debug build for the self-test pagecache-dirty-writer
(write-back off, one writer of 1024 pages, the cache limit lowered to
256: the bound is 128), then runs `make test-fill`
(tests/boot/fill_test.py) in a release build: on a 256 MiB guest with
the build installed on a 256 MiB root, 512 MiB written with no sync to a
data pool, then the root filled by one dd with no sync.

  fixed  pagecache-dirty-writer ok, at most the bound dirty; test-fill
         passes: dd ends ENOSPC, no write-back fails, no page is lost,
         spawns work after it.
  --old  the fix commit reverse-applied whole in a throwaway worktree
         (after the AIO fix and its failing test, which rewrote the code
         it changed in aio.c, also reverted whole, newest first):
         pagecache-dirty-writer fails with every page written dirty; the
         fill's writes are taken, its commits fail -ENOSPC, and test-fill
         fails on `writeback commit failed (-28)` or `dirty page(s) ...
         lost`.

The self-test is judged by its own line, not the boot's verdict. The
writer stage's failure (`sh: cat: Out of memory`, main at 75e1e15f)
depends on whether write-back keeps up and passes on this branch in
either mode; the self-test is the bound's failing test. Logs stay under
out/mem9-probe/<arch>-<mode>. Exit 0 when the mode shows what it should,
else 1.
"""
import argparse
from pathlib import Path
import re
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SUBJECT = ("vfs, cosmofs: dirty pages bounded at an eighth of RAM with the writer asleep holding nothing; "
           "ENOSPC charged at the write (MEM-9)")
# Reverted first, newest first: they rewrite the PWRITE loop the fix
# changed (tools/mem9-aio-probe.py is theirs).
LATER = ("aio: an AIO PWRITE asks about the dirty bound every 64 KiB and lets f->lock go to be throttled (MEM-9)",
         "aio, vfs: aio-pwrite-dirty, one AIO PWRITE past the dirty bound, throttled only before its loop "
         "(MEM-9, failing)")
LOSS = ("writeback commit failed (-28)", "dirty page(s) of inode")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--old", action="store_true")
    ap.add_argument("--arch", choices=("x86_64", "aarch64"), default="x86_64")
    ap.add_argument("--tree", default="HEAD")
    ap.add_argument("--fix", help="the fix commit (default: found by its subject)")
    a = ap.parse_args()
    mode = "old" if a.old else "fixed"
    work = ROOT / "out" / "mem9-probe" / f"{a.arch}-{mode}"
    work.mkdir(parents=True, exist_ok=True)
    if any(work.iterdir()):
        sys.exit(f"refusing to overwrite nonempty probe output {work}")
    # A short path: the boot's sockets live under its output directory.
    tree = Path(tempfile.mkdtemp(prefix=f"m9-{a.arch[:3]}-{mode}-", dir=ROOT / "out"))
    tree.rmdir()
    subprocess.run(["git", "-C", str(ROOT), "worktree", "add", "--detach", str(tree), a.tree], check=True,
                   stdout=subprocess.DEVNULL)
    try:
        if a.old:
            def find(subject):
                return subprocess.check_output(
                    ["git", "-C", str(ROOT), "log", "-1", "--format=%H", "--fixed-strings", f"--grep={subject}",
                     a.tree], text=True).strip()
            shas = [x for x in (find(s) for s in LATER) if x]
            sha = a.fix or find(SUBJECT)
            if not sha:
                sys.exit(f"no commit '{SUBJECT}' reachable from {a.tree}; name it with --fix")
            # Whole commits, not chosen hunks.
            # Three-way, so later commits beside their hunks do not stop it.
            # A conflict only in docs/ (later sections beside the fix's)
            # keeps the tree's text, since no document reaches the build; one
            # in code stops the probe.
            for rev in shas + [sha]:
                if subprocess.run(["git", "-C", str(tree), "revert", "--no-commit", rev],
                                  stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode:
                    clash = subprocess.check_output(
                        ["git", "-C", str(tree), "diff", "--name-only", "--diff-filter=U"], text=True).split()
                    code = [p for p in clash if not p.startswith("docs/")]
                    if code:
                        sys.exit(f"the revert of {rev} conflicts in code: {' '.join(code)}")
                    subprocess.run(["git", "-C", str(tree), "checkout", "HEAD", "--", *clash], check=True)
        make = "gmake" if sys.platform == "darwin" else "make"
        dbg = tree / "out" / "d"
        with (work / "make-debug.log").open("w") as log:
            subprocess.run([make, "-C", str(tree), "-j8", f"ARCH={a.arch}", f"OUT={dbg}", "test"],
                           stdin=subprocess.DEVNULL, stdout=log, stderr=subprocess.STDOUT, timeout=1800,
                           start_new_session=True)
        boot = dbg / "boot-test.log"
        serial = boot.read_text(errors="replace") if boot.exists() else ""
        if boot.exists():
            subprocess.run(["cp", str(boot), str(work)], check=False)
        dw = re.search(r"^SELFTEST: pagecache-dirty-writer\s+\.\.\. (ok|FAIL)\b.*$", serial, re.M)
        dwinfo = re.search(r"^.*selftest: pagecache-dirty-writer: (.*)$", serial, re.M)
        dwseen = f"pagecache-dirty-writer: {dw.group(0).split('... ', 1)[1].strip() if dw else 'no line'}" + (
            f" ({dwinfo.group(1).strip()})" if dwinfo else "")
        out = tree / "out" / "o"
        args = [make, "-C", str(tree), "-j8", f"ARCH={a.arch}", "BUILD=release", f"OUT={out}",
                f"BB_DIR={ROOT / 'out' / f'busybox-{a.arch}'}", f"PORTS_CACHE={ROOT / '.cache' / 'ports'}",
                "test-fill"]
        with (work / "make.log").open("w") as log:
            rc = subprocess.run(args, stdin=subprocess.DEVNULL, stdout=log, stderr=subprocess.STDOUT,
                                timeout=5400, start_new_session=True).returncode
        text = (work / "make.log").read_text(errors="replace")
        verdict = re.search(r"^test-fill: (PASS|FAIL).*$", text, re.M)
        subprocess.run(["cp", "-R", str(out / "test-fill"), str(work)], check=False)
        if a.old:
            dwok = dw is not None and dw.group(1) == "FAIL" and "peak <= bound" in dw.group(0)
            ok = (dwok and rc != 0 and verdict is not None and verdict.group(1) == "FAIL" and
                  any(s in verdict.group(0) for s in LOSS))
            seen = (f"{dwseen}; " + (verdict.group(0) if ok else
                    f"no such failure (make exit {rc}; {verdict.group(0) if verdict else 'no verdict'})"))
        else:
            dwok = dw is not None and dw.group(1) == "ok"
            ok = dwok and rc == 0 and verdict is not None and verdict.group(1) == "PASS"
            lines = re.findall(r"^test-fill: (?:writer|enospc): .*$", text, re.M)
            seen = f"{dwseen}; " + ("; ".join(lines) if ok else
                                    f"make exit {rc}; {verdict.group(0) if verdict else 'no verdict'}")
        print(f"PROBE: {'PASS' if ok else 'FAIL'} arch={a.arch} mode={mode}: {seen} ({work})")
        return 0 if ok else 1
    finally:
        subprocess.run(["git", "-C", str(ROOT), "worktree", "remove", "--force", str(tree)])


if __name__ == "__main__":
    sys.exit(main())
