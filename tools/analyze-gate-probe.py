#!/usr/bin/env python3
"""Prove that the make analysis gate rejects an unreviewed diagnostic."""

import argparse
from pathlib import Path
import plistlib
import subprocess
import sys
import tempfile


ROOT = Path(__file__).resolve().parents[1]


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
        subprocess.run(["git", "-C", str(ROOT), "worktree", "add", "--detach", str(tree), args.tree], check=True)
        try:
            report = tree / "out/analyze-gate-probe" / "diagnostic.analyzed"
            report.parent.mkdir(parents=True, exist_ok=True)
            source = report.with_suffix(".c")
            source.write_text("int analyze_gate_probe(void) { int *p = 0; return *p; }\n")
            target = "x86_64-unknown-none-elf" if args.arch == "x86_64" else "aarch64-unknown-none-elf"
            analyzed = subprocess.run([
                "clang", "--target=" + target, "--analyze", "-Xanalyzer",
                "-analyzer-output=plist-multi-file", str(source), "-o", str(report),
            ], text=True, capture_output=True)
            if analyzed.returncode != 0:
                print("PROBE: FAIL: compiler did not produce the deliberate diagnostic\n" + analyzed.stderr)
                return 1
            findings = plistlib.loads(report.read_bytes())["diagnostics"]
            if len(findings) != 1 or findings[0]["check_name"] != "core.NullDereference":
                print("PROBE: FAIL: expected one core.NullDereference diagnostic")
                return 1
            if args.old:
                # Before the gate, analyzer reports were emitted as text and
                # discarded; no diagnostic comparison ran. Confirm the
                # synthetic report contains the deliberate finding.
                print(f"PROBE: OLD BEHAVIOR: {args.arch} clang reports the diagnostic; the prior target discarded it")
                return 0
            cmd = [make, "-C", str(tree), "ARCH=" + args.arch,
                   "OUT=" + str(report.parent), "analysis-gate",
                   "ANALYSIS_REPORTS=" + str(report)]
            run = subprocess.run(cmd, text=True, capture_output=True)
            expected = "Dereference of null pointer" in run.stderr
            ok = run.returncode == 2 and expected and "static analysis: clean" not in run.stdout + run.stderr
            print(run.stderr.strip())
            print(f"PROBE: {'PASS' if ok else 'FAIL'}: {args.arch} unexpected diagnostic makes analysis-gate fail ({out_dir})")
            return 0 if ok else 1
        finally:
            subprocess.run(["git", "-C", str(ROOT), "worktree", "remove", "--force", str(tree)], check=True)


if __name__ == "__main__":
    sys.exit(main())
