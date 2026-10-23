#!/usr/bin/env python3
"""Prove an rc's release run checks the final tag's notes (BUILD-7).

    build7-probe.py [--old] [--bad REV] [--good REV]

v1.0.11-rc1's run (39938572061) passed on 569d0905, whose
docs/releases/v1.0.11.md said "CosmoOS 1.0.10"; the v1.0.11 tag's run on
the same commit then failed check-release-version.py on it (run
39942569981). An rc's notes are built from docs/releases/vX.Y.Z-rcN.md,
which no release has, so that file was first read by the final tag.

The probe runs the publish step's checks for the tag v1.0.11-rc1 on the
notes file of two commits: --bad (569d0905, the rc1 commit) and --good
(af381311, the commit v1.0.11 was released from). release-notes.py gets
placeholders for what only the workflow has (image digests, the CI
image's package list, the sums).

  fixed  the step as release.yml has it now: the rc's own notes, then the
         final tag's from docs/releases/v1.0.11.md -- the bad commit
         fails ("CosmoOS 1.0.10"), the good one passes.
  --old  the step before: the rc's own notes only -- both pass, so the
         bad commit's rc would have gone through.

Exit 0 when the mode shows what it should, else 1.
"""
import argparse
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
RC, FINAL = "v1.0.11-rc1", "v1.0.11"


def checks(tmp, notes_file, old):
    """The publish step's checks of the notes for RC; True when all pass."""
    pub = str(next((ROOT / "tools" / "keys").glob("*.pub")))
    common = ["--toolchain", "probe", "--litmus", "probe", "--packages", str(tmp / "packages.txt"), "--pub", pub,
              "--sums", str(tmp / "SHA256SUMS")]
    runs = [(RC, None)]
    if not old:
        runs.append((FINAL, notes_file))
    for tag, lead in runs:
        out = tmp / f"{tag}.md"
        args = [sys.executable, str(ROOT / "scripts" / "release-notes.py"), "--tag", tag] + common + ["--out", str(out)]
        if lead:
            args += ["--lead", str(lead)]
        subprocess.run(args, check=True)
        r = subprocess.run([sys.executable, str(ROOT / "scripts" / "check-release-version.py"), "--tag", tag,
                            "--notes", str(out)], capture_output=True, text=True)
        if r.returncode != 0:
            print(f"  {tag}: {r.stderr.strip()}")
            return False
    return True


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--old", action="store_true")
    ap.add_argument("--bad", default="569d0905")
    ap.add_argument("--good", default="af381311")
    a = ap.parse_args()
    with tempfile.TemporaryDirectory() as t:
        tmp = Path(t)
        (tmp / "packages.txt").write_text("")
        (tmp / "SHA256SUMS").write_text("")
        verdicts = {}
        for name, rev in (("bad", a.bad), ("good", a.good)):
            text = subprocess.check_output(["git", "-C", str(ROOT), "show", f"{rev}:docs/releases/{FINAL}.md"])
            notes = tmp / f"{name}-{FINAL}.md"
            notes.write_bytes(text)
            verdicts[name] = checks(tmp, notes, a.old)
            print(f"  {name} ({rev}): {'passes' if verdicts[name] else 'fails'}")
    mode = "old" if a.old else "fixed"
    ok = verdicts["good"] and verdicts["bad"] == a.old
    seen = ("the rc's run passes the commit whose final notes name another release" if a.old else
            "the rc's run fails the commit whose final notes name another release, passes the released one")
    print(f"PROBE: {'PASS' if ok else 'FAIL'} mode={mode}: {seen if ok else verdicts}")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
