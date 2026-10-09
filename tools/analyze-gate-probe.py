#!/usr/bin/env python3
"""Prove that the make analysis gate rejects an unreviewed diagnostic."""

import argparse
from pathlib import Path
import subprocess
import sys
import tempfile


ROOT = Path(__file__).resolve().parents[1]
BASELINE = "55b1825ebc28c7f5cdb948477019808dc8ed5517"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--arch", choices=("x86_64", "aarch64"), required=True)
    parser.add_argument("--old", action="store_true",
                        help="show that the old analyzer path ignored this diagnostic")
    parser.add_argument("--tree", default="HEAD")
    args = parser.parse_args()
    out_dir = ROOT / "out/analyze-gate-probe" / args.arch / ("old" if args.old else "fixed")
    out_dir.mkdir(parents=True, exist_ok=True)
    make = "gmake" if sys.platform == "darwin" else "make"
    with tempfile.TemporaryDirectory(prefix="tree-", dir=out_dir) as temp:
        tree = Path(temp) / "tree"
        revision = BASELINE if args.old else args.tree
        subprocess.run(["git", "-C", str(ROOT), "worktree", "add", "--detach", str(tree), revision], check=True)
        try:
            main_c = tree / "kernel/core/main.c"
            main_c.write_text(main_c.read_text() +
                              "\nint analyze_gate_probe(void) { int *p = 0; return *p; }\n")
            output = tree / "out/analyze-gate-probe"
            log = out_dir / "analyze.log"
            with log.open("w") as stream:
                run = subprocess.run([
                    make, "-j4", "-C", str(tree), "ARCH=" + args.arch,
                    "OUT=" + str(output), "analyze",
                ], text=True, stdout=stream, stderr=subprocess.STDOUT)
            result = log.read_text()
            if args.old:
                ok = (run.returncode == 0
                      and "Dereference of null pointer" in result
                      and "static analysis: clean" in result)
            else:
                ok = (run.returncode == 2
                      and "main.c: analyze_gate_probe: Dereference of null pointer" in result
                      and "static analysis: FAIL (1 unexpected diagnostics)" in result
                      and "static analysis: clean" not in result)
            mode = "old false success" if args.old else "new diagnostic rejection"
            print(f"PROBE: {'PASS' if ok else 'FAIL'}: {args.arch} make analyze {mode} ({log})")
            return 0 if ok else 1
        finally:
            subprocess.run(["git", "-C", str(ROOT), "worktree", "remove", "--force", str(tree)], check=True)


if __name__ == "__main__":
    sys.exit(main())
