#!/usr/bin/env python3
"""
lz4-stack-probe.py -- does a cosmofs write-back under a system call still
overflow the kernel stack in lz4_compress? (roadmap M2 report, "Two stack
overflows on the installer's path")

    python3 tools/lz4-stack-probe.py [--old] [--arch x86_64|aarch64]

Builds HEAD in a throwaway worktree (git worktree add --detach) and runs
the debug boot test there. With --old, the whole commit that moved the
compressor's table off the stack is reverted first (`git show SHA |
git apply -R`), and nothing else: the user-mode check that exposes the
overflow stays (init's fs section writes 64 KiB of a repeated line to
the scratch cosmofs and fsyncs it).

Verdict, from the boot's own lines:
  old   -- the boot must panic with a double fault whose PC symbolises to
           lz4_compress, during the user-mode self-test;
  fixed -- the boot must PASS and print
           "usertest: cosmofs compressed a file committed from user mode".
Exit 0 when the mode's expectation held, 1 when it did not.
"""

import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FIX_SUBJECT = "lz4: the compressor's table is the caller's, off the kernel stack"
MARKER = "usertest: cosmofs compressed a file committed from user mode"


def git(*args, cwd=ROOT, **kw):
    return subprocess.run(["git", *args], cwd=cwd, check=True, capture_output=True, text=True, **kw).stdout


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--old", action="store_true")
    ap.add_argument("--arch", default="x86_64")
    args = ap.parse_args()
    sha = git("log", "--format=%H", "--fixed-strings", f"--grep={FIX_SUBJECT}", "-1").strip()
    if not sha:
        sys.exit("lz4-stack-probe: the fix commit is not in this history")
    tmp = tempfile.mkdtemp(prefix="lz4-stack-probe-")
    wt = os.path.join(tmp, "tree")
    try:
        git("worktree", "add", "--detach", wt, "HEAD")
        if args.old:
            patch = git("show", sha)
            subprocess.run(["git", "apply", "-R"], cwd=wt, input=patch, text=True, check=True)
            if "uint32_t table[LZ4_HASH_SIZE];" not in open(os.path.join(wt, "kernel/core/lz4.c")).read():
                sys.exit("lz4-stack-probe: the revert did not restore the on-stack table")
        make = shutil.which("gmake") or "make"
        run = subprocess.run([make, f"ARCH={args.arch}", "test"], cwd=wt, capture_output=True, text=True)
        log_path = os.path.join(wt, "out", f"{args.arch}-debug", "boot-test.log")
        log = open(log_path, errors="replace").read().replace("\r", "") if os.path.exists(log_path) else ""
        verdict = [l for l in run.stdout.splitlines() if l.startswith("boot-test: ")][-1:] or ["(no verdict)"]
        print(f"lz4-stack-probe: {args.arch} {'old' if args.old else 'fixed'} ({sha[:8]}): {verdict[0]}")
        if args.old:
            m = re.search(r"unhandled exception \S+ \(#DF double fault\)|exception.*stack overflow", log)
            pc = re.search(r"^\s*#0\s+(0x[0-9a-f]+)", log, re.M) or re.search(r"(?:RIP|PC|ELR)=([0-9a-f]{16})", log)
            where = ""
            if pc:
                addr = pc.group(1) if pc.group(1).startswith("0x") else "0x" + pc.group(1)
                sym = shutil.which("llvm-symbolizer") or os.path.expanduser("~/.swiftly/bin/llvm-symbolizer")
                where = subprocess.run([sym, f"--obj={os.path.join(wt, 'out', args.arch + '-debug', 'kernel', 'kernel.elf')}",
                                        addr], capture_output=True, text=True).stdout.split("\n")[0]
            panic_line = re.search(r"^KERNEL PANIC: .*$", log, re.M)
            print(f"lz4-stack-probe: panic: {panic_line.group(0) if panic_line else 'none'}; at {where or '?'}")
            ok = panic_line is not None and "lz4_compress" in where and MARKER not in log
        else:
            ok = run.returncode == 0 and MARKER in log
        print(f"lz4-stack-probe: expectation {'held' if ok else 'FAILED'}")
        return 0 if ok else 1
    finally:
        subprocess.run(["git", "worktree", "remove", "--force", wt], cwd=ROOT, capture_output=True)
        shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
