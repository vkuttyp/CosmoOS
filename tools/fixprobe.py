"""Shared runner for probes whose --old mode reverses one fix commit.

A probe names the fix commit by subject, the source files it may revert,
the self-tests that must fail without it and the harness failures that
follow. --old reverse-applies the whole commit (never a subset of its
hunks) in a throwaway worktree; both modes build, boot once at QEMU_SMP
and check the harness verdict. Logs stay under out/<probe>/<arch>-<mode>.
"""
import argparse
from pathlib import Path
import re
import subprocess
import sys
import tempfile


ROOT = Path(__file__).resolve().parents[1]
COMMON_FAILURES = {"kernel reported failure via debug-exit", "no 'SELFTEST: PASS' line"}


def fix_commit(tree_rev, subject):
    sha = subprocess.check_output(["git", "-C", str(ROOT), "log", "-1", "--format=%H", "--fixed-strings",
                                   f"--grep={subject}", tree_rev], text=True).strip()
    if not sha:
        raise RuntimeError(f"no commit '{subject}' reachable from {tree_rev}")
    return sha


def run(name, doc, subject, sources, tests, missing, old_markers, fixed_markers, smp=1):
    parser = argparse.ArgumentParser(description=doc)
    parser.add_argument("--arch", required=True, choices=("x86_64", "aarch64"))
    parser.add_argument("--old", action="store_true")
    parser.add_argument("--tree", default="HEAD", help="commit with the proof and fix")
    args = parser.parse_args()

    mode = "old" if args.old else "fixed"
    work = ROOT / "out" / name / f"{args.arch}-{mode}"
    work.mkdir(parents=True, exist_ok=True)
    if any(work.iterdir()):
        raise SystemExit(f"refusing to overwrite nonempty probe output {work}")
    sha = fix_commit(args.tree, subject) if args.old else None
    tree = Path(tempfile.mkdtemp(prefix="worktree-", dir=work))
    tree.rmdir()
    subprocess.run(["git", "-C", str(ROOT), "worktree", "add", "--detach", str(tree), args.tree], check=True)
    try:
        if args.old:
            patch = subprocess.check_output(["git", "-C", str(ROOT), "show", "--format=", sha, "--"] + sources)
            if not patch.strip():
                raise RuntimeError(f"fix commit {sha} changes none of {sources}")
            subprocess.run(["git", "-C", str(tree), "apply", "-R", "--index"], input=patch, check=True)
        make = "gmake" if sys.platform == "darwin" else "make"
        command = [make, "-C", str(tree), f"ARCH={args.arch}", f"QEMU_SMP={smp}"]
        build_log = work / "build.log"
        with build_log.open("w") as output:
            rc = subprocess.run(command + ["-j6", "image"], stdout=output, stderr=subprocess.STDOUT).returncode
        if rc:
            print(f"PROBE: FAIL (build; {build_log})")
            return 1
        serial = work / "boot.serial"
        result_path = work / "boot.result"
        with result_path.open("w") as output:
            rc = subprocess.run(command + [f"BOOT_LOG={serial}", "test"], stdout=output,
                                stderr=subprocess.STDOUT).returncode
        log = serial.read_text(errors="replace") if serial.exists() else ""
        result = result_path.read_text(errors="replace")
        failures = set(re.findall(r"^  - (.+)$", result, re.M))
        markers = old_markers if args.old else fixed_markers
        proof = all(m in log for m in markers) and "SNAPTEST: PASS" in log and "NETTEST: done " in log and \
            "served 100 of 100" in result
        if args.old:
            forbidden = [f for f in failures if re.fullmatch(
                r"forbidden marker /SELFTEST: FAIL/: SELFTEST: FAIL \(\d+ of \d+\)", f)]
            proof = proof and rc != 0 and len(forbidden) == 1 and \
                failures - set(forbidden) == COMMON_FAILURES | set(missing)
            proof = proof and all(re.search(rf"SELFTEST: {t}\s+\.\.\. FAIL:", log) for t in tests)
            proof = proof and re.search(rf"SELFTEST: FAIL \({len(tests)} of \d+\)", log) is not None
        else:
            proof = proof and rc == 0 and not failures
            proof = proof and re.search(r"SELFTEST: PASS \(\d+ tests\)$", log, re.M) is not None
            proof = proof and all(re.search(rf"SELFTEST: {t}\s+\.\.\. ok", log) for t in tests)
        print(f"PROBE: {'PASS' if proof else 'FAIL'} arch={args.arch} mode={mode} ({work})")
        if not proof:
            print(f"  make exit={rc}; harness failures={sorted(failures)}; logs={result_path},{serial}")
        return 0 if proof else 1
    finally:
        subprocess.run(["git", "-C", str(ROOT), "worktree", "remove", "--force", str(tree)], check=True)
